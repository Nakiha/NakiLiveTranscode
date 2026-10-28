#include "pipeline_internal.h"
#include "rendition_runtime.h"
#include "decode_output_policy.h"
#include "decode_backend.h"

int pipeline_stamp_valid(const AVBufferRef *ref) {
    if(!ref||ref->size<sizeof(Stamp))return 0;
    const Stamp *s=(const Stamp *)ref->data;
    if(ref->size!=sizeof(*s)+s->sei_size)return 0;
    if(!s->sei_size)return 1;
    if(s->sei_size<sizeof(NakiSeiBundle))return 0;
    const NakiSeiBundle *b=(const NakiSeiBundle *)(ref->data+sizeof(*s));
    return b->count>0&&b->count<=NAKI_SEI_MESSAGES&&b->bytes<=NAKI_SEI_PAYLOAD_BYTES&&s->sei_size==naki_sei_bundle_size(b);
}
const NakiSeiBundle *pipeline_stamp_sei(const AVBufferRef *ref) {
    return ((const Stamp *)ref->data)->sei_size?(const NakiSeiBundle *)(ref->data+sizeof(Stamp)):NULL;
}
void pipeline_trace_pts(yyjson_mut_doc *doc, const char *key, int64_t pts) {
    if(pts==AV_NOPTS_VALUE)yyjson_mut_obj_add_null(doc,yyjson_mut_doc_get_root(doc),key);
    else json_int(doc,key,pts);
}
static void media_phase(Pipeline *p,NakiMediaPhase phase) {
    naki_job_media_phase(p->job,phase);
}
/* These references stay inside the worker. Frame, encoder, ledger and mux
 * packet clones share one lease; only the final actual reference closes it. */
static void delivery_stamp_free(void *opaque,uint8_t *data) {
    (void)opaque;Stamp *stamp=(Stamp *)data;
    if(stamp->delivery_lease&&naki_watermark_release_frame(stamp->delivery_owner,stamp->delivery_lease))abort();
    av_free(data);
}
NakiFrameIdentity pipeline_frame_identity(Pipeline *p,const Stamp *stamp,int64_t output_pts,AVRational output_tb) {
    return (NakiFrameIdentity){.frame_id=(uint64_t)stamp->frame_id,.source_pts=stamp->source_pts,.source_us=stamp->source_us,
        .source_tb_num=p->input_tb.num,.source_tb_den=p->input_tb.den,.output_pts=output_pts,.output_tb_num=output_tb.num,.output_tb_den=output_tb.den};
}

int pipeline_interrupted(void *opaque) {
    Interrupt *io=opaque;
    int64_t deadline=atomic_load(&io->deadline);
    return (!atomic_load(&io->cleanup)&&naki_job_cancelled(io->job)) || (deadline && now_us()>deadline);
}

void pipeline_refresh_auto(Pipeline *p) {
    AVCodecAsyncAutoStats stats={0};
    AVCodecEarlyOutputStats early={0};
    int early_ret=AVERROR(EINVAL);
    if (p->async) {
        avcodec_async_get_auto_stats(p->async,&stats);
        early_ret=avcodec_async_get_early_output_stats(p->async,&early);
    } else if (p->decoder)early_ret=avcodec_get_early_output_stats(p->decoder,&early);
    naki_job_decoder_observation(p->job,&stats,&early,early_ret>=0,now_us());
}
static int event_log(Pipeline *p, const char *event) {
    logger_submit(naki_job_logger(p->job),NULL,new_event(event,p->job)); return 0;
}

int pipeline_filter_output(Pipeline *p) {
    AVFrame *frame=av_frame_alloc();
    if (!frame) return AVERROR(ENOMEM);
    int ret;
    while ((ret=av_buffersink_get_frame(p->sink,frame))>=0) {
        frame->sample_aspect_ratio=p->encoder->sample_aspect_ratio;
        if(p->filter_generation>1) {frame->colorspace=p->encoder->colorspace;frame->color_range=p->encoder->color_range;}
        if(!pipeline_stamp_valid(frame->opaque_ref)) {ret=AVERROR_INVALIDDATA;break;}
        uint64_t watermark_revision=0;
        Stamp stamp=*(Stamp *)frame->opaque_ref->data;NakiOperationHandle lease=0;
        AVRational time_base=av_buffersink_get_time_base(p->sink);
        NakiFrameIdentity identity=pipeline_frame_identity(p,&stamp,frame->pts,time_base);
        p->stage="watermark composition";
        ret=naki_watermark_frame_tracked(naki_job_watermark(p->job),&p->watermark_compositor,frame,
            time_base,&identity,&watermark_revision,&lease);
        if(ret>=0) {
            stamp.watermark_revision=watermark_revision;stamp.delivery_owner=naki_job_watermark(p->job);stamp.delivery_lease=lease;
            size_t owned_size=frame->opaque_ref->size;
            Stamp *owned=av_memdup(frame->opaque_ref->data,owned_size);
            if(owned)*owned=stamp;
            AVBufferRef *reference=owned?av_buffer_create((uint8_t *)owned,owned_size,delivery_stamp_free,NULL,0):NULL;
            if(!reference) {
                av_free(owned);if(naki_watermark_release_frame(naki_job_watermark(p->job),lease))abort();ret=AVERROR(ENOMEM);
            } else {av_buffer_unref(&frame->opaque_ref);frame->opaque_ref=reference;}
        }
        if(ret>=0) {
            if(p->watermark_revision!=watermark_revision) {
                yyjson_mut_doc *event=new_event("watermark_applied",p->job);
                naki_watermark_status(naki_job_watermark(p->job),event);logger_submit(naki_job_logger(p->job),NULL,event);
                p->watermark_revision=watermark_revision;
            }
        }
        if(ret>=0)ret=pipeline_encode_frame(p,frame);
        av_frame_unref(frame);
        /* Only EAGAIN/EOF from the main sink below are normal draining.
         * An inner compositor/encoder error must never silently drop a frame. */
        if(ret<0) {av_frame_free(&frame);return ret;}
    }
    av_frame_free(&frame);
    return ret==AVERROR(EAGAIN) || ret==AVERROR_EOF ? 0 : ret;
}
int pipeline_prepare_decoded_frame(Pipeline *p, AVFrame *frame) {
    if (!pipeline_stamp_valid(frame->opaque_ref)) return AVERROR_INVALIDDATA;
    Stamp *input_stamp=(Stamp *)frame->opaque_ref->data;
    if(input_stamp->sei_size) {
        if(!naki_job_config(p->job)->decoder_pump)p->stage="selected SEI picture association";
        if(frame->flags&AV_FRAME_FLAG_INTERLACED)return AVERROR(ENOSYS);
        int claim=pipeline_reader_sei_obligation(p,input_stamp->sequence,0);if(claim<0)return claim;
    }
    naki_policy_untrack(p->job,NAKI_TRACK_PACKET,(uint64_t)((Stamp *)frame->opaque_ref->data)->sequence);
    /* Stamp is a private packet property propagated by AV_CODEC_FLAG_COPY_OPAQUE. */
    int ret=av_buffer_make_writable(&frame->opaque_ref);
    if (ret<0) return ret;
    Stamp *stamp=(Stamp *)frame->opaque_ref->data;
    stamp->decoder_receive_us=p->decoder_receive_us;
    stamp->decoded_us=now_us();
    if(naki_job_config(p->job)->codec_trace) {
        AVFrameSideData *sd=av_frame_get_side_data(frame,AV_FRAME_DATA_LIVE_DECODE_TRACE);
        if(!sd||sd->size!=sizeof(AVLiveDecodeTrace))return AVERROR_INVALIDDATA;
        const AVLiveDecodeTrace *tr=(const AVLiveDecodeTrace*)sd->data;
        stamp->decode_trace.version=tr->version;
#define COPY_TRACE(n) stamp->decode_trace.n=__atomic_load_n(&tr->n,__ATOMIC_ACQUIRE);
        AV_LIVE_DECODE_TRACE_FIELDS(COPY_TRACE)
#undef COPY_TRACE
        if(tr->version!=AV_LIVE_DECODE_TRACE_VERSION||stamp->decode_trace.source_pts!=frame->pts||
           !stamp->decode_trace.reconstructed_us||!stamp->decode_trace.frontend_us)return AVERROR_INVALIDDATA;
    }
    naki_job_decoded(p->job,stamp,0);
    naki_job_checkpoint(p->job,NAKI_CP_FIRST_DECODED,0);
    stamp->picture_type=frame->pict_type;
    stamp->first_i=frame->pict_type==AV_PICTURE_TYPE_I && !p->seen_input_i;
    if (frame->pict_type==AV_PICTURE_TYPE_I) p->seen_input_i=1;
    stamp->decoded_key=!!(frame->flags&AV_FRAME_FLAG_KEY);
    if(p->next_frame_id==INT64_MAX)return AVERROR(ERANGE);
    stamp->frame_id=++p->next_frame_id; stamp->raw_pts=frame->pts;
    if(naki_job_config(p->job)->policy.timestamps==NAKI_TS_SOURCE_ALL) {
        if(!naki_job_config(p->job)->decoder_pump)p->stage="strict source PTS";
        if((ret=naki_timeline_resolve(&p->timeline,frame->pts,p->input_wrap_bits,&frame->pts))<0)return ret;
        stamp->timestamp_source=1;
    } else {
        if(frame->best_effort_timestamp!=AV_NOPTS_VALUE)frame->pts=frame->best_effort_timestamp;
        if(frame->pts==AV_NOPTS_VALUE) {frame->pts=p->synthetic_pts; stamp->timestamp_source=3;}
        else stamp->timestamp_source=2;
        p->synthetic_pts=frame->pts+av_rescale_q(1,av_inv_q(p->frame_rate),p->input_tb);
    }
    stamp->source_pts=frame->pts;
    stamp->source_us=av_rescale_q(frame->pts,p->input_tb,AV_TIME_BASE_Q);
    stamp->timestamp_wraps=p->timeline.wraps;
    if(naki_job_config(p->job)->codec_trace) {
        yyjson_mut_doc *event=new_event("decoder_frame",p->job);
        json_int(event,"frame_id",stamp->frame_id);json_int(event,"input_sequence",stamp->sequence);
        json_int(event,"pts_us",stamp->source_us);json_int(event,"decoder_submit_monotonic_us",stamp->submitted_us);
        json_int(event,"decoded_monotonic_us",stamp->decoded_us);
        naki_decode_trace_json(event,&stamp->decode_trace);logger_submit(naki_job_logger(p->job),naki_job_trace(p->job),event);
    }
    if(stamp->source_us==INT64_MIN || stamp->source_us==INT64_MAX)return AVERROR(ERANGE);
    return 0;
}
static int rebuild_input_filter(Pipeline *p,const AVFrame *frame) {
    if(frame->width==p->input_width&&frame->height==p->input_height&&frame->format==p->input_format&&
       frame->colorspace==p->input_colorspace&&frame->color_range==p->input_range&&
       frame->color_primaries==p->encoder->color_primaries&&frame->color_trc==p->encoder->color_trc)return 0;
    p->stage="dynamic input validation";
    int ret=naki_watermark_video(naki_job_watermark(p->job),frame,p->encoder->width,p->encoder->height);if(ret<0)return ret;
    if(frame->color_primaries!=p->encoder->color_primaries||frame->color_trc!=p->encoder->color_trc) {
        p->stage="input primaries/transfer changed; tone mapping unsupported";return AVERROR(ENOSYS);
    }
    if(frame->format!=AV_PIX_FMT_YUV420P&&frame->format!=AV_PIX_FMT_YUV420P10LE)return AVERROR(ENOSYS);
    AVFilterGraph *graph=NULL;AVFilterContext *source=NULL,*sink=NULL;
    p->stage="dynamic filter prepare";
    ret=naki_video_filter_prepare_fixed(&graph,&source,&sink,frame,p->input_tb,p->encoder->width,p->encoder->height,
        p->encoder->pix_fmt,p->encoder->colorspace,p->encoder->color_range);
    if(ret<0)return ret;
    p->stage="old filter generation drain";
    ret=av_buffersrc_add_frame_flags(p->source,NULL,0);if(ret>=0)ret=pipeline_filter_output(p);
    if(ret<0) {avfilter_graph_free(&graph);return ret;}
    avfilter_graph_free(&p->graph);p->graph=graph;p->source=source;p->sink=sink;
    p->input_width=frame->width;p->input_height=frame->height;p->input_format=frame->format;
    p->input_colorspace=frame->colorspace;p->input_range=frame->color_range;
    uint64_t generation=p->filter_generation=naki_job_filter_generation(p->job,0);
    yyjson_mut_doc *event=new_event("input_filter_rebuilt",p->job);json_int(event,"generation",generation);
    json_int(event,"input_width",frame->width);json_int(event,"input_height",frame->height);json_int(event,"input_format",frame->format);
    json_int(event,"output_width",p->encoder->width);json_int(event,"output_height",p->encoder->height);
    json_str(event,"encoder_action","unchanged");logger_submit(naki_job_logger(p->job),NULL,event);return 0;
}
int pipeline_process_frame(Pipeline *p,AVFrame *frame) {
    int ret=naki_job_parent(p->job)?av_buffer_make_writable(&frame->opaque_ref):naki_job_config(p->job)->decoder_pump?0:pipeline_prepare_decoded_frame(p,frame);
    if(ret<0)return ret;
    Stamp *stamp=(Stamp *)frame->opaque_ref->data;
    if(naki_job_config(p->job)->decoder_pump&&!naki_job_parent(p->job))stamp->decoder_owner_us=now_us();
    if(naki_job_parent(p->job)) {
        stamp->branch_started_us=now_us();
        naki_job_decoded(p->job,stamp,1);
    }
    if(!p->rate_divisor) {
        p->stage="explicit output rate requires a known integer input/output ratio";
        ret=naki_video_rate_output(&p->video_rate,naki_job_config(p->job)->output_fps,&p->output_rate,&p->rate_divisor);
        if(ret<0)return ret;
    }
    if(!stamp->decoder_tracked&&(ret=naki_policy_track(p->job,NAKI_TRACK_FRAME,(uint64_t)stamp->frame_id,stamp->ingress_us))<0)return ret;
    if(p->graph&&(ret=rebuild_input_filter(p,frame))<0)return ret;
    stamp->intra_reason=naki_intra_decide(&naki_job_config(p->job)->policy,&p->intra,stamp->source_us,stamp->picture_type==AV_PICTURE_TYPE_I);
    int sei_messages=naki_sei_count(pipeline_stamp_sei(frame->opaque_ref),&naki_job_config(p->job)->sei);
    int sei_protected=sei_messages>0;
    int first=naki_job_parent(p->job)?p->rendition_frames++==0:stamp->frame_id==1;
    int protect=stamp->intra_reason || stamp->picture_type==AV_PICTURE_TYPE_I || first || sei_protected;
    int level=naki_job_frame_deciding(p->job,stamp,(unsigned)sei_messages);
    int keep=naki_frame_keep(&naki_job_config(p->job)->policy,level,(uint64_t)stamp->frame_id,protect);
    if(keep<0)return keep;
    int rate_drop=keep && !naki_video_rate_keep((uint64_t)stamp->frame_id,p->rate_divisor,protect);
    if(rate_drop)keep=0;
    naki_job_frame_selected(p->job,protect,sei_protected,keep,rate_drop);
    if(!keep) {
        if(!naki_policy_untrack(p->job,NAKI_TRACK_FRAME,(uint64_t)stamp->frame_id))return AVERROR_INVALIDDATA;
        naki_branch_release(p->job,(uint64_t)stamp->frame_id,stamp->branch_charge);
        if(naki_job_config(p->job)->frame_log && (stamp->frame_id-1)%naki_job_config(p->job)->frame_log_every==0) {
            yyjson_mut_doc *event=new_event("frame_drop",p->job);
            json_int(event,"frame_id",stamp->frame_id); json_int(event,"input_sequence",stamp->sequence);
            json_int(event,"source_pts",stamp->source_pts); json_int(event,"overload_level",level);
            json_str(event,"reason",rate_drop?"output_rate":"overload"); logger_submit(naki_job_logger(p->job),naki_job_trace(p->job),event);
        }
        return 0;
    }
    if(!p->graph) {
        p->stage="watermark video validation";
        if((ret=naki_watermark_video(naki_job_watermark(p->job),frame,naki_job_config(p->job)->width?naki_job_config(p->job)->width:frame->width,
              naki_job_config(p->job)->height?naki_job_config(p->job)->height:frame->height))<0)return ret;
        if((ret=pipeline_open_output(p,frame))<0)return ret;
    }
    stamp->generation=(unsigned)p->filter_generation;
    p->stage="filter";
    stamp->filter_started_us=now_us();
    ret=av_buffersrc_add_frame_flags(p->source,frame,AV_BUFFERSRC_FLAG_KEEP_REF);
    if (ret<0) return ret;
    return pipeline_filter_output(p);
}
static int initialize_decoder(Pipeline *p) {
    media_phase(p,NAKI_MEDIA_PHASE_OPENING_INPUT);
    p->stage="input open/probe";
    p->input=avformat_alloc_context();
    if (!p->input) return AVERROR(ENOMEM);
    if(naki_job_config(p->job)->policy.timestamps==NAKI_TS_SOURCE_ALL)p->input->flags|=AVFMT_FLAG_NOFILLIN;
    p->input->interrupt_callback=(AVIOInterruptCB){pipeline_interrupted,&p->read_interrupt};
    p->input->probesize=1024*1024; p->input->max_analyze_duration=1000000;
    p->input->max_streams=64;
    atomic_store(&p->read_interrupt.deadline,now_us()+IO_TIMEOUT_US);
    naki_job_checkpoint(p->job,NAKI_CP_INPUT_OPEN_STARTED,0);
    AVDictionary *input_options=NULL;
    if(naki_job_config(p->job)->rtmp_tcp_nodelay&&!strncmp(naki_job_config(p->job)->input,"rtmp://",7))av_dict_set(&input_options,"tcp_nodelay","1",0);
    if(naki_job_config(p->job)->network_trace) {
        if(avformat_version()<AV_VERSION_INT(63,3,105))return AVERROR(ENOSYS);
        av_dict_set(&input_options,"rtmp_read_origin","1",0);
    }
    int ret=avformat_open_input(&p->input,naki_job_config(p->job)->input,NULL,&input_options);
    if(ret>=0&&av_dict_count(input_options))ret=AVERROR_OPTION_NOT_FOUND;
    av_dict_free(&input_options);
    if (ret<0) return ret;
    naki_job_checkpoint(p->job,NAKI_CP_INPUT_OPEN_DONE,0);
    ret=naki_probe_video(p->input);
    atomic_store(&p->read_interrupt.deadline,0);
    if (ret<0) return ret;
    p->stream_index=ret;
    naki_job_checkpoint(p->job,NAKI_CP_INPUT_PROBE_DONE,0);
    p->stage="bounded live input initialization";
    if(avformat_version()<AV_VERSION_INT(63,3,104))return AVERROR(ENOSYS);
    ret=!strcmp(p->input->iformat->name,"mpegts") ?
        avformat_live_input_probe_poll(p->input,100000) : avformat_live_input_probe_limit(p->input,100000);
    if(ret<0)return ret;
    AVStream *stream=p->input->streams[p->stream_index];
    if (stream->codecpar->codec_id!=AV_CODEC_ID_H264 && stream->codecpar->codec_id!=AV_CODEC_ID_HEVC) return AVERROR(ENOSYS);
    p->input_tb=stream->time_base;
    p->sei_codec=stream->codecpar->codec_id;
    p->sei_length_size=naki_sei_length_size(p->sei_codec,stream->codecpar->extradata,stream->codecpar->extradata_size);
    p->sei_input=naki_job_config(p->job)->sei;
    if(naki_job_config(p->job)->output_count) {
        p->sei_input=(NakiSeiConfig){0};
        for(int i=0;i<naki_job_config(p->job)->output_count;i++)if(naki_sei_union(&p->sei_input,&naki_job_config(p->job)->outputs[i].sei))return AVERROR(ENOBUFS);
    }
    p->reader.audio_subscribed=naki_job_config(p->job)->audio_copy;p->reader.subscription_generation=1;
    if(p->sei_input.count&&p->sei_length_size<0)return AVERROR_INVALIDDATA;
    p->input_wrap_bits=stream->pts_wrap_bits;
    ret=naki_video_rate_resolve(av_guess_frame_rate(p->input,stream,NULL),naki_job_config(p->job)->fps,&p->video_rate);
    if(ret<0)return ret;
    p->frame_rate=p->video_rate.value;
    naki_job_video_rate(p->job,&p->video_rate);
    p->stage=naki_job_config(p->job)->decoder_async?"async decoder initialization":"synchronous decoder initialization";
    const AVCodec *codec=avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) return AVERROR_DECODER_NOT_FOUND;
    p->decoder=avcodec_alloc_context3(codec);
    if (!p->decoder) return AVERROR(ENOMEM);
    if ((ret=avcodec_parameters_to_context(p->decoder,stream->codecpar))<0) return ret;
    if(naki_job_config(p->job)->codec_trace) {
        p->stage="codec_trace requires native software HEVC input";
        if(stream->codecpar->codec_id!=AV_CODEC_ID_HEVC)return AVERROR(ENOSYS);
        p->decoder->export_side_data|=AV_CODEC_EXPORT_DATA_LIVE_TRACE;
    }
    p->decoder->pkt_timebase=p->input_tb;
    p->decoder->thread_count=naki_job_config(p->job)->max_threads;
    p->decoder->thread_type=FF_THREAD_FRAME; p->decoder->flags|=AV_CODEC_FLAG_COPY_OPAQUE;
    p->decoder->max_pixels=NAKI_INPUT_MAX_PIXELS;
    if((ret=naki_decode_output_policy(p->decoder,naki_job_config(p->job)->decoder_early_output))<0)return ret;
    if ((ret=avcodec_open2(p->decoder,codec,NULL))<0) return ret;
    p->initial_input_properties=(AVFrame){.width=p->decoder->width,.height=p->decoder->height,.format=p->decoder->pix_fmt,
        .color_range=p->decoder->color_range,.colorspace=p->decoder->colorspace,.color_primaries=p->decoder->color_primaries,
        .color_trc=p->decoder->color_trc,.chroma_location=p->decoder->chroma_sample_location,.sample_aspect_ratio=p->decoder->sample_aspect_ratio};
    ret=naki_decode_backend_init(&p->decoder,&p->async,naki_job_config(p->job)->decoder_async,
        naki_job_config(p->job)->decoder_auto_threads,naki_job_config(p->job)->queue_capacity,
        naki_job_config(p->job)->min_threads,naki_job_config(p->job)->budget_slots,pipeline_decode_notify,p);
    if (ret<0) return ret;
    pipeline_refresh_auto(p);
    naki_job_checkpoint(p->job,NAKI_CP_DECODER_READY,0);
    naki_job_running(p->job);
    return event_log(p,"decoder_ready");
}
static void branch_queue_stats(Branch *b) {
    pthread_mutex_lock(&b->queue.mutex);
    unsigned count=b->queue.count,high=b->queue.high_water;uint64_t bytes=b->queue.bytes,media=b->queue.media_us;
    unsigned video=b->queue.video_count,audio=b->queue.audio_count,video_high=b->queue.video_high,audio_high=b->queue.audio_high,reason=b->queue.rejection_reason;
    int64_t oldest=b->queue.count?b->queue.items[b->queue.head].ingress_us:0;
    uint64_t rejected[NAKI_BRANCH_REASONS];memcpy(rejected,b->queue.rejected,sizeof(rejected));
    pthread_mutex_unlock(&b->queue.mutex);Job *j=b->pipeline.job;
    NakiBranchQueueReport report={.bytes=bytes,.work_us=media,.oldest_us=oldest,.count=count,.high=high,
        .video=video,.audio=audio,.video_high=video_high,.audio_high=audio_high,.reason=reason};
    memcpy(report.rejected,rejected,sizeof(rejected));naki_job_branch_queue(j,&report);
}
static void fail_branch(Branch *b,int ret,const char *stage) {
    Job *j=b->pipeline.job;if(naki_job_done(j))return;
    naki_job_branch_fail(j,ret,stage);request_stop(j);naki_branch_queue_abort(&b->queue);branch_queue_stats(b);
}
static int check_branches(Pipeline *p,int finishing) {
    int active=0;int64_t now=now_us();
    for(int i=0;i<p->branch_count;i++) {
        Branch *b=&p->branches[i];Job *j=b->pipeline.job;
        if(!b->started||naki_job_done(j))continue;
        if(naki_job_cancelled(p->job)) {request_stop(j);naki_branch_queue_abort(&b->queue);continue;}
        NakiBranchAges ages=naki_job_branch_ages(j,now);
        int64_t age=ages.retained_us,write=ages.write_us;int failed=ages.failed;
        if(!failed) {
            if(age>(int64_t)b->spec->max_lag_ms*1000)fail_branch(b,AVERROR(ETIMEDOUT),"branch retained-frame ingress deadline");
            else if(write>(int64_t)b->spec->io_timeout_ms*1000)fail_branch(b,AVERROR(ETIMEDOUT),"branch output I/O deadline");
            else if(naki_branch_queue_check(&b->queue,now)==AVERROR(ETIMEDOUT)) {
                pthread_mutex_lock(&b->queue.mutex);int pressure=b->queue.admission_deadline_us&&now>=b->queue.admission_deadline_us;unsigned reason=b->queue.rejection_reason;pthread_mutex_unlock(&b->queue.mutex);
                char stage[96];snprintf(stage,sizeof(stage),pressure?"branch bounded admission window (%s)":"branch queued ingress deadline (%s)",naki_branch_reason_name(reason));
                fail_branch(b,AVERROR(ETIMEDOUT),stage);
            }
        }
        if(!naki_job_cancelled(j))active++;
        branch_queue_stats(b);
    }
    return !finishing&&!active?AVERROR(EPIPE):0;
}

static void *branch_main(void *opaque) {
    (void)crash_thread_init();Branch *b=opaque;Pipeline *p=&b->pipeline;Job *j=p->job;
    NakiMediaJobInfo info={.job_id=naki_job_id(j),.request_id=naki_job_request_id(j),.config=naki_job_config(j),.request_us=naki_job_started_us(j),.output_id=naki_job_output_id(j)};
    p->stage="branch media hook initialization";int ret=naki_media_hooks_open(&info,&p->hooks);
    if(ret>0||(ret>=0&&p->hooks.api_version!=NAKI_MEDIA_HOOK_API))ret=AVERROR(EINVAL);
    if(ret>=0) {naki_job_running(j);}
    while(ret>=0&&!naki_job_cancelled(j)) {
        media_phase(p,NAKI_MEDIA_PHASE_WAITING_INPUT);
        NakiBranchItem item={0};ret=naki_branch_queue_pop(&b->queue,&item,now_us()+20000);branch_queue_stats(b);
        if(ret==AVERROR_EOF) {media_phase(p,NAKI_MEDIA_PHASE_DRAINING);ret=pipeline_drain_output(p);break;}
        if(ret==AVERROR(ETIMEDOUT)||ret==AVERROR(EAGAIN)) {ret=pipeline_mux_live(p)?pipeline_drain_mux(p,0):0;continue;}
        if(ret<0)break;
        naki_job_input_received(j,0,1,now_us());
        if(item.frame)ret=pipeline_process_frame(p,item.frame);
        else if(naki_job_config(j)->audio_copy&&strcmp(naki_job_config(j)->output_format,"mp4")) {
            InputItem audio={.packet=item.packet,.params=item.parameters,.tb=item.time_base,.demuxed_us=item.ingress_us};
            ret=pipeline_handle_audio(p,&audio);
        }
        naki_branch_item_free(&item);
        if(ret>=0&&pipeline_mux_live(p)) {ret=pipeline_flush_deferred_audio(p);if(ret>=0)ret=pipeline_drain_mux(p,0);}
    }
    if(!naki_job_cancelled(naki_job_parent(j))&&naki_job_cancelled(j)&&!naki_job_failed(j))fail_branch(b,AVERROR(ETIMEDOUT),"branch overload cancellation");
    if(ret<0&&!naki_job_cancelled(naki_job_parent(j)))fail_branch(b,ret,p->stage);
    naki_branch_queue_abort(&b->queue);naki_watermark_owner_finish(naki_job_watermark(j),ret,naki_job_cancelled(naki_job_parent(j)));naki_watermark_stop(naki_job_watermark(j));
    ret=pipeline_finalize_output(p,ret,naki_job_cancelled(naki_job_parent(j)));
    if(ret<0&&!naki_job_cancelled(naki_job_parent(j)))fail_branch(b,ret,p->stage);
    pipeline_release_media_resources(p,NULL);
    pipeline_complete_job(p,ret,0);
    return NULL;
}
static int start_branches(Pipeline *p) {
    p->branch_count=naki_job_config(p->job)->output_count;p->branches=calloc(p->branch_count,sizeof(*p->branches));if(!p->branches)return AVERROR(ENOMEM);
    for(int i=0;i<p->branch_count;i++)p->branches[i].pipeline.job=naki_job_output(p->job,(unsigned)i);
    for(int i=0;i<p->branch_count;i++) {
        Branch *b=&p->branches[i];Job *j=naki_job_output(p->job,(unsigned)i);b->spec=&naki_job_config(p->job)->outputs[i];
        b->pipeline=(Pipeline){.job=j,.input_tb=p->input_tb,.frame_rate=p->frame_rate,.video_rate=p->video_rate,.input_wrap_bits=p->input_wrap_bits,
            .last_pts=AV_NOPTS_VALUE,.stage="branch initialization",.io_timeout_us=(int64_t)b->spec->io_timeout_ms*1000};
        naki_job_inherit_input(j,p->job,&p->video_rate);
        int ret=naki_branch_queue_init_limits(&b->queue,b->spec->queue_items,b->spec->audio_packets,b->spec->queue_bytes,(int64_t)b->spec->queue_media_ms*1000,(int64_t)b->spec->max_lag_ms*1000);
        if(ret<0)return ret;
        b->initialized=1;
        if(naki_job_done(j))continue;
        ret=pthread_create(&b->thread,NULL,branch_main,b);
        if(ret) {
            fail_branch(b,AVERROR(ret),"branch thread creation");naki_watermark_stop(naki_job_watermark(j));
            naki_job_branch_unstarted(j);continue;
        }
        b->started=1;
    }
    return check_branches(p,0);
}
static int admission_check(void *opaque) {
    Pipeline *p=opaque;
    return naki_job_cancelled(p->job)?AVERROR_EXIT:check_branches(p,0);
}
static int admit_branch_items(Pipeline *p,NakiBranchItem *items) {
    NakiBranchOffer offers[4]={0};
    for(int i=0;i<p->branch_count;i++)if(items[i].frame||items[i].packet)
        offers[i]=(NakiBranchOffer){.queue=&p->branches[i].queue,.item=&items[i],.result=AVERROR(EAGAIN)};
    naki_branch_admit_many(offers,(unsigned)p->branch_count,BRANCH_ADMISSION_GRACE_US,admission_check,p);
    for(int i=0;i<p->branch_count;i++) {
        Branch *b=&p->branches[i];Job *j=b->pipeline.job;
        if(items[i].frame) {
            Stamp *s=(Stamp *)items[i].frame->opaque_ref->data;
            naki_branch_release(j,(uint64_t)s->frame_id,s->branch_charge);
        }
        if(offers[i].queue&&offers[i].result<0&&!naki_job_cancelled(p->job)&&!naki_job_done(j)) {
            pthread_mutex_lock(&b->queue.mutex);unsigned reason=b->queue.rejection_reason;pthread_mutex_unlock(&b->queue.mutex);
            char stage[96];snprintf(stage,sizeof(stage),"branch bounded admission (%s)",naki_branch_reason_name(reason));fail_branch(b,offers[i].result,stage);
        }
        naki_branch_item_free(&items[i]);branch_queue_stats(b);
    }
    int result=check_branches(p,0);if(result<0)p->stage="all outputs failed after bounded admission";return result;
}
static int fanout_frame(Pipeline *p,AVFrame *frame) {
    NakiBranchItem items[4]={0};
    int ret=pipeline_prepare_decoded_frame(p,frame);if(ret<0)return ret;Stamp *source=(Stamp *)frame->opaque_ref->data;
    naki_job_shared_decoded(p->job,source,1);
    for(int i=0;i<p->branch_count;i++) {
        Branch *b=&p->branches[i];Job *j=b->pipeline.job;if(!b->started||naki_job_done(j)||naki_job_cancelled(j))continue;
        NakiBranchItem item={.frame=av_frame_clone(frame),.source_us=source->source_us,.ingress_us=source->ingress_us,.sequence=source->sequence,.time_base=p->input_tb};
        /* Queue work-duration budget never turns legal source PTS holes or
         * long display durations into an admission failure. No PTS is retimed. */
        int64_t duration=p->video_rate.nominal_frame_work_us;
        item.media_us=duration>0?(uint64_t)duration:1;
        ret=item.frame?av_buffer_make_writable(&item.frame->opaque_ref):AVERROR(ENOMEM);
        if(ret>=0)ret=naki_branch_item_charge(&item);
        NakiOutputSize size=naki_job_output_size_freeze(j,frame->width,frame->height);int width=size.width,height=size.height;
        int output_bytes=av_image_get_buffer_size(!strcmp(naki_job_config(j)->encoder,"libvvenc")?AV_PIX_FMT_YUV420P10LE:AV_PIX_FMT_YUV420P,width,height,32);
        uint64_t reservation=item.charged_bytes+(output_bytes>0?(uint64_t)output_bytes:0);
        int reserved=0;
        if(ret>=0&&output_bytes>0) {
            ret=naki_job_branch_admit(j,(uint64_t)source->frame_id,source->ingress_us,reservation,b->spec->inflight_bytes);
            reserved=ret>=0;
        } else if(ret>=0)ret=output_bytes<0?output_bytes:AVERROR(EINVAL);
        if(ret>=0) {
            Stamp *stamp=(Stamp *)item.frame->opaque_ref->data;stamp->branch_charge=reservation;stamp->branch_enqueued_us=now_us();
            items[i]=item;item=(NakiBranchItem){0};
        }
        if(ret<0) {if(reserved)naki_branch_release(j,(uint64_t)source->frame_id,reservation);fail_branch(b,ret,"branch frame admission budget");}
        naki_branch_item_free(&item);branch_queue_stats(b);
    }
    return admit_branch_items(p,items);
}
static int fanout_audio(Pipeline *p,const InputItem *audio) {
    NakiBranchItem items[4]={0};
    for(int i=0;i<p->branch_count;i++) {
        Branch *b=&p->branches[i];Job *j=b->pipeline.job;
        if(!b->started||naki_job_done(j)||naki_job_cancelled(j)||!naki_job_config(j)->audio_copy||!strcmp(naki_job_config(j)->output_format,"mp4"))continue;
        NakiBranchItem item={.packet=av_packet_clone(audio->packet),.parameters=avcodec_parameters_alloc(),.time_base=audio->tb,
            .source_us=audio->packet->pts==AV_NOPTS_VALUE?AV_NOPTS_VALUE:av_rescale_q(audio->packet->pts,audio->tb,AV_TIME_BASE_Q),.ingress_us=audio->demuxed_us};
        int ret=item.packet&&item.parameters?avcodec_parameters_copy(item.parameters,audio->params):AVERROR(ENOMEM);
        int64_t duration=audio->packet->duration>0?av_rescale_q(audio->packet->duration,audio->tb,AV_TIME_BASE_Q):0;
        if(duration<=0)duration=audio->params->sample_rate>0?av_rescale(1152,1000000,audio->params->sample_rate):40000;
        item.media_us=duration>0?(uint64_t)duration:1;
        if(ret>=0) {items[i]=item;item=(NakiBranchItem){0};}
        if(ret<0)fail_branch(b,ret,"branch audio admission budget");
        naki_branch_item_free(&item);branch_queue_stats(b);
    }
    return admit_branch_items(p,items);
}
static int finish_branches(Pipeline *p,int aborting) {
    for(int i=0;i<p->branch_count;i++)if(p->branches[i].initialized) {
        if(aborting) {request_stop(p->branches[i].pipeline.job);naki_branch_queue_abort(&p->branches[i].queue);}
        else naki_branch_queue_finish(&p->branches[i].queue);
    }
    for(;;) {
        int waiting=0;for(int i=0;i<p->branch_count;i++)if(p->branches[i].started&&!naki_job_done(p->branches[i].pipeline.job))waiting=1;
        if(!waiting)break;
        check_branches(p,1);struct pollfd fd={.fd=naki_job_wake_fd(p->job),.events=POLLIN};poll(&fd,1,20);
        uint64_t value;while(read(naki_job_wake_fd(p->job),&value,sizeof(value))>0) {}
    }
    int completed=0;
    for(int i=0;i<p->branch_count;i++) {
        Branch *b=&p->branches[i];if(b->started) {pthread_join(b->thread,NULL);b->started=0;}
        if((naki_job_state(b->pipeline.job)==NAKI_JOB_STATE_COMPLETED))completed++;
    }
    return completed?0:AVERROR(EPIPE);
}
void *pipeline_main(void *opaque) {
    (void)crash_thread_init();
    Job *job=opaque;
    Pipeline p={.job=job,.last_pts=AV_NOPTS_VALUE,.stage="initialization"};
    if(!naki_job_config(job)->output_count) {OutputConfig output;naki_logical_output(naki_job_config(job),0,&output);p.io_timeout_us=(int64_t)output.io_timeout_ms*1000;}
    p.read_interrupt.job=job;
    atomic_init(&p.read_interrupt.deadline,0);
    atomic_init(&p.read_interrupt.cleanup,0);
    pipeline_reader_init(&p);int decode_init=pipeline_decode_init(&p);
    InputItem pending={0}; AVFrame *frame=av_frame_alloc();int branches_joined=0,idle_finished=0;
    NakiMediaJobInfo info={.job_id=naki_job_id(job),.request_id=naki_job_request_id(job),.config=naki_job_config(job),.request_us=naki_job_started_us(job),
        .output_id=naki_job_config(job)->output_count?NULL:naki_job_output_id(job)};
    p.stage="media hook initialization";
    int ret=decode_init<0?decode_init:frame ? naki_media_hooks_open(&info,&p.hooks) : AVERROR(ENOMEM);
    if(ret>0 || (ret>=0 && p.hooks.api_version!=NAKI_MEDIA_HOOK_API))ret=AVERROR(EINVAL);
    if(ret>=0) {
        p.stage="required watermark startup gate";
        if(naki_job_config(job)->watermark.required)naki_job_waiting_assets(job);
        ret=naki_watermark_wait(naki_job_watermark(job));
        if(ret>=0&&naki_job_config(job)->output_count) {
            int ready=0;naki_job_waiting_assets(job);
            for(int i=0;i<naki_job_config(job)->output_count;i++) {
                Job *b=naki_job_output(job,(unsigned)i);if(!b||!*naki_job_output_id(b))continue;int gate=naki_watermark_wait(naki_job_watermark(b));
                if(gate<0) {
                    naki_job_startup_fail(b,gate);
                } else ready++;
            }
            if(!ready) {p.stage="all output startup gates failed";ret=AVERROR(EPIPE);}
        }
    }
    if(ret>=0)ret=initialize_decoder(&p);
    if (ret<0) goto cleanup;
    if(naki_job_config(job)->rendition_backend) {if((ret=naki_renditions_open(&p))<0)goto cleanup;}
    else if(naki_job_config(job)->output_count&&(ret=start_branches(&p))<0)goto cleanup;
    ret=pipeline_reader_start(&p);
    if (ret) {ret=AVERROR(ret); goto cleanup;}
    if(naki_job_config(job)->decoder_pump&&(ret=pipeline_decode_start(&p))<0)goto cleanup;
    int eof_sent=0, decoder_eof=0;
    while (!naki_job_cancelled(job) && !decoder_eof) {
        if(naki_job_renditions(job)){ret=naki_renditions_poll(&p,0);if(ret==AVERROR_EOF){ret=0;idle_finished=1;break;}if(ret<0)goto cleanup;if(!naki_renditions_capacity(&p)){poll(NULL,0,5);continue;}}
        if(p.branches&&(ret=check_branches(&p,0))<0) {p.stage="all outputs failed";goto cleanup;}
        uint64_t wake;
        while (read(naki_job_wake_fd(job),&wake,sizeof(wake))<0 && errno==EINTR) {}
        int progress=0;
        p.stage=naki_job_config(job)->decoder_async?"async decode receive":"synchronous decode receive";
        for (int batch=0;batch<1;batch++) {
            ret=naki_job_config(job)->decoder_pump?pipeline_decode_receive(&p,frame,&pending):naki_decode_receive(p.decoder,p.async,frame);
            if (ret==AVERROR_EOF) {decoder_eof=1; ret=0; break;}
            if (ret==AVERROR(EAGAIN)) {ret=0; break;}
            if (ret<0) goto cleanup;
            if(!naki_job_config(job)->decoder_pump)p.decoder_receive_us=now_us();
            ret=naki_job_renditions(job)?naki_renditions_frame(&p,frame):p.branches?fanout_frame(&p,frame):pipeline_process_frame(&p,frame); av_frame_unref(frame);
            if (ret<0) goto cleanup;
            progress=1;
        }
        if (decoder_eof) break;
        int input_done=0, input_error=0;
        if (!naki_job_config(job)->decoder_pump && !pending.packet && !eof_sent) pending=pipeline_reader_pop(&p,&input_done,&input_error);
        if(pending.params) {
            if(p.hooks.audio_packet&&!pending.hook_observed) {
                pending.hook_observed=1;
                NakiAudioPacketEvent event={.packet=pending.packet,.parameters=pending.params,.time_base=pending.tb,
                    .input_stream_index=pending.packet->stream_index,.demuxed_us=pending.demuxed_us,.dispatched_us=now_us()};
                p.stage="audio media hook";
                ret=p.hooks.audio_packet(p.hooks.opaque,&event);
                if(ret>0)ret=AVERROR(EINVAL);
            }
            if(ret>=0&&naki_job_renditions(job))ret=naki_renditions_audio(&p,&pending);
            else if(ret>=0&&p.branches)ret=fanout_audio(&p,&pending);
            else if(ret>=0 && naki_job_config(job)->audio_copy && strcmp(naki_job_config(job)->output_format,"mp4"))ret=pipeline_handle_audio(&p,&pending);
            if(ret==AVERROR(EAGAIN)&&naki_job_renditions(job))ret=0;
            else {pipeline_input_free(&pending);if(ret<0)goto cleanup;progress=1;}
        }
        if (input_done && input_error<0) {p.stage=p.sei_read_error?"input SEI parsing / complete-AU contract":"input read"; ret=input_error; goto cleanup;}
        if (!naki_job_config(job)->decoder_pump && !pending.params && (pending.packet || (input_done && !eof_sent))) {
            p.stage=naki_job_config(job)->decoder_async?"async decode submit":"synchronous decode submit";
            if (pending.packet) {
                Stamp *stamp=(Stamp *)pending.packet->opaque_ref->data;
                stamp->submitted_us=now_us();
                if(!stamp->submit_first_us)stamp->submit_first_us=stamp->submitted_us;
            }
            ret=naki_decode_send(p.decoder,p.async,pending.packet);
            if (!ret) {
                if (pending.packet) pipeline_input_free(&pending); else eof_sent=1;
                progress=1;
            } else if (ret==AVERROR(EAGAIN)) ret=0;
            else goto cleanup;
        }
        if(pipeline_mux_live(&p)) {
            ret=pipeline_flush_deferred_audio(&p);
            if(ret<0 || (ret=pipeline_drain_mux(&p,0))<0)goto cleanup;
        }
        if (!naki_job_config(job)->decoder_pump && now_us()>=p.next_stats_us) {
            pipeline_refresh_auto(&p);
            p.next_stats_us=now_us()+1000000;
        }
        if(!progress&&p.encoder_poll_pending) {
            p.stage="encoder pending packet";
            ret=pipeline_receive_encoded(&p);
            if(ret<0)goto cleanup;
            progress=p.encoder_polled_packet;
        }
        if (!progress) {
            media_phase(&p,NAKI_MEDIA_PHASE_WAITING_INPUT);
            struct pollfd fd={.fd=naki_job_wake_fd(job),.events=POLLIN};
            int n;
            do {n=poll(&fd,1,naki_job_renditions(job)?5:p.branches?20:pipeline_mux_live(&p) ? 10 : 1000);} while(n<0 && errno==EINTR && !naki_job_cancelled(job));
            if (n<0) {ret=AVERROR(errno); goto cleanup;}
        }
    }
    if(!naki_job_cancelled(job)&&!idle_finished) {
        if(pipeline_reader_pending_sei(&p))ret=AVERROR_INVALIDDATA;
        if(ret<0) {p.stage="selected SEI has no decoded picture at EOF";goto cleanup;}
    }
    if(naki_job_renditions(job)) {ret=naki_renditions_finish(&p,naki_job_cancelled(job));branches_joined=1;if(ret<0)p.stage="no rendition completed successfully";}
    else if(p.branches) {
        ret=finish_branches(&p,naki_job_cancelled(job));branches_joined=1;
        if(ret<0) {p.stage="all outputs failed";goto cleanup;}
    } else if (!naki_job_cancelled(job))ret=pipeline_drain_output(&p);
cleanup:
    media_phase(&p,NAKI_MEDIA_PHASE_DRAINING);
    {
        int stopped=naki_job_cancelled(job);
        if(ret<0||idle_finished)naki_job_cancel(job);
        if(stopped||ret<0||idle_finished) {
            /* Release a full shared reader ring before any private output
             * teardown/join can wait for its bounded I/O cleanup. */
            pipeline_reader_wake(&p);
        }
        if(naki_job_renditions(job)&&!branches_joined)naki_renditions_finish(&p,1);
        if(p.branches&&!branches_joined)finish_branches(&p,1);
        for(int i=0;i<naki_job_config(job)->output_count;i++) {
            Job *b=naki_job_output(job,(unsigned)i);
            if(!naki_job_done(b)&&!naki_job_remote(b))naki_watermark_owner_finish(naki_job_watermark(b),ret,stopped);
            naki_watermark_stop(naki_job_watermark(b));
            if(!naki_job_done(b)) {
                naki_job_shared_terminated(b,stopped,p.stage);
            }
        }
        naki_watermark_stop(naki_job_watermark(job));
        int policy_failed=naki_job_begin_shutdown(job);
        if(policy_failed) {stopped=0; ret=AVERROR(ETIMEDOUT); p.stage="continuous overload budget exhausted";}
        if(ret<0 && !stopped) {
            char message[AV_ERROR_MAX_STRING_SIZE]; av_strerror(ret,message,sizeof(message));
            yyjson_mut_doc *error_event=new_event("pipeline_error",job);
            json_str(error_event,"stage",p.stage); json_str(error_event,"error",message);
            logger_submit(naki_job_logger(job),NULL,error_event);
        }
        /* Wake a blocked producer and interrupt network reads before joining. */
        if (ret<0) naki_job_cancel(job);
        pipeline_reader_wake(&p);
        pipeline_decode_join(&p);
        pipeline_reader_join(&p);
        pipeline_refresh_auto(&p);
        avcodec_async_free(&p.async);
        naki_watermark_owner_finish(naki_job_watermark(job),ret,stopped);
        ret=pipeline_finalize_output(&p,ret,stopped);
        pipeline_input_free(&pending);
        pipeline_reader_clear(&p);
        pipeline_release_media_resources(&p,&frame);
        pipeline_decode_destroy(&p);pipeline_reader_destroy(&p);
        if(p.branches) {
            for(int i=0;i<p.branch_count;i++)if(p.branches[i].initialized)naki_branch_queue_destroy(&p.branches[i].queue);
            free(p.branches);
        }
        pipeline_complete_job(&p,ret,stopped);
    }
    return NULL;
}
