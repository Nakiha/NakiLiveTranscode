#include "input_internal.h"
#include "decode_output_policy.h"
#include "decode_backend.h"

int pipeline_input_init(Pipeline *p) {
    if(p->in)return AVERROR(EALREADY);
    p->in=calloc(1,sizeof(*p->in));if(!p->in)return AVERROR(ENOMEM);
    p->in->read_interrupt.job=p->job;
    atomic_init(&p->in->read_interrupt.deadline,0);atomic_init(&p->in->read_interrupt.cleanup,0);
    pipeline_reader_init(p);return pipeline_decode_init(p);
}
AVFrame pipeline_input_properties(const Pipeline *p) {
    return p->in?p->in->initial_input_properties:(AVFrame){.format=AV_PIX_FMT_NONE};
}
int pipeline_input_receive(Pipeline *p,AVFrame *frame,InputItem *audio) {
    if(naki_job_config(p->job)->decoder_pump)return pipeline_decode_receive(p,frame,audio);
    int ret=naki_decode_receive(p->in->decoder,p->in->async,frame);
    if(ret>=0)p->in->decoder_receive_us=now_us();
    return ret;
}
int pipeline_input_send(Pipeline *p,const AVPacket *packet) {
    if(packet) {
        Stamp *stamp=(Stamp *)packet->opaque_ref->data;stamp->submitted_us=now_us();
        if(!stamp->submit_first_us)stamp->submit_first_us=stamp->submitted_us;
    }
    return naki_decode_send(p->in->decoder,p->in->async,packet);
}
void pipeline_input_tick(Pipeline *p) {
    if(!naki_job_config(p->job)->decoder_pump&&now_us()>=p->in->next_stats_us) {
        pipeline_refresh_auto(p);p->in->next_stats_us=now_us()+1000000;
    }
}
int pipeline_input_sei_error(const Pipeline *p){return p->in&&p->in->sei_read_error;}
void pipeline_input_quiesce(Pipeline *p) {
    if(!p->in)return;
    pipeline_decode_join(p);pipeline_reader_join(p);pipeline_refresh_auto(p);
    /* async owns the codec after successful transfer; it joins all callbacks
     * before the private pump wake FD and reader synchronization are destroyed. */
    avcodec_async_free(&p->in->async);
}
void pipeline_input_clear(Pipeline *p){if(p->in)pipeline_reader_clear(p);}
void pipeline_input_release_decoder(Pipeline *p){if(p->in)avcodec_free_context(&p->in->decoder);}
void pipeline_input_close_demux(Pipeline *p){if(p->in)avformat_close_input(&p->in->input);}
void pipeline_input_destroy(Pipeline *p) {
    if(!p->in)return;
    if(p->in->async||p->in->decoder||p->in->input||p->in->reader.started||p->in->pump.started)abort();
    pipeline_decode_destroy(p);pipeline_reader_destroy(p);free(p->in);p->in=NULL;
}

void pipeline_refresh_auto(Pipeline *p) {
    AVCodecAsyncAutoStats stats={0};
    AVCodecEarlyOutputStats early={0};
    int early_ret=AVERROR(EINVAL);
    if (p->in->async) {
        avcodec_async_get_auto_stats(p->in->async,&stats);
        early_ret=avcodec_async_get_early_output_stats(p->in->async,&early);
    } else if (p->in->decoder)early_ret=avcodec_get_early_output_stats(p->in->decoder,&early);
    naki_job_decoder_observation(p->job,&stats,&early,early_ret>=0,now_us());
}
static int event_log(Pipeline *p, const char *event) {
    logger_submit(naki_job_logger(p->job),NULL,new_event(event,p->job)); return 0;
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
    stamp->decoder_receive_us=p->in->decoder_receive_us;
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
    stamp->first_i=frame->pict_type==AV_PICTURE_TYPE_I && !p->in->seen_input_i;
    if (frame->pict_type==AV_PICTURE_TYPE_I) p->in->seen_input_i=1;
    stamp->decoded_key=!!(frame->flags&AV_FRAME_FLAG_KEY);
    if(p->in->next_frame_id==INT64_MAX)return AVERROR(ERANGE);
    stamp->frame_id=++p->in->next_frame_id; stamp->raw_pts=frame->pts;
    if(naki_job_config(p->job)->policy.timestamps==NAKI_TS_SOURCE_ALL) {
        if(!naki_job_config(p->job)->decoder_pump)p->stage="strict source PTS";
        if((ret=naki_timeline_resolve(&p->in->timeline,frame->pts,p->input_wrap_bits,&frame->pts))<0)return ret;
        stamp->timestamp_source=1;
    } else {
        if(frame->best_effort_timestamp!=AV_NOPTS_VALUE)frame->pts=frame->best_effort_timestamp;
        if(frame->pts==AV_NOPTS_VALUE) {frame->pts=p->in->synthetic_pts; stamp->timestamp_source=3;}
        else stamp->timestamp_source=2;
        p->in->synthetic_pts=frame->pts+av_rescale_q(1,av_inv_q(p->frame_rate),p->input_tb);
    }
    stamp->source_pts=frame->pts;
    stamp->source_us=av_rescale_q(frame->pts,p->input_tb,AV_TIME_BASE_Q);
    stamp->timestamp_wraps=p->in->timeline.wraps;
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
int pipeline_input_open(Pipeline *p) {
    naki_job_media_phase(p->job,NAKI_MEDIA_PHASE_OPENING_INPUT);
    p->stage="input open/probe";
    p->in->input=avformat_alloc_context();
    if (!p->in->input) return AVERROR(ENOMEM);
    if(naki_job_config(p->job)->policy.timestamps==NAKI_TS_SOURCE_ALL)p->in->input->flags|=AVFMT_FLAG_NOFILLIN;
    p->in->input->interrupt_callback=(AVIOInterruptCB){pipeline_interrupted,&p->in->read_interrupt};
    p->in->input->probesize=1024*1024; p->in->input->max_analyze_duration=1000000;
    p->in->input->max_streams=64;
    atomic_store(&p->in->read_interrupt.deadline,now_us()+IO_TIMEOUT_US);
    naki_job_checkpoint(p->job,NAKI_CP_INPUT_OPEN_STARTED,0);
    AVDictionary *input_options=NULL;
    if(naki_job_config(p->job)->rtmp_tcp_nodelay&&!strncmp(naki_job_config(p->job)->input,"rtmp://",7))av_dict_set(&input_options,"tcp_nodelay","1",0);
    if(naki_job_config(p->job)->network_trace) {
        if(avformat_version()<AV_VERSION_INT(63,3,105))return AVERROR(ENOSYS);
        av_dict_set(&input_options,"rtmp_read_origin","1",0);
    }
    int ret=avformat_open_input(&p->in->input,naki_job_config(p->job)->input,NULL,&input_options);
    if(ret>=0&&av_dict_count(input_options))ret=AVERROR_OPTION_NOT_FOUND;
    av_dict_free(&input_options);
    if (ret<0) return ret;
    naki_job_checkpoint(p->job,NAKI_CP_INPUT_OPEN_DONE,0);
    ret=naki_probe_video(p->in->input);
    atomic_store(&p->in->read_interrupt.deadline,0);
    if (ret<0) return ret;
    p->in->stream_index=ret;
    naki_job_checkpoint(p->job,NAKI_CP_INPUT_PROBE_DONE,0);
    p->stage="bounded live input initialization";
    if(avformat_version()<AV_VERSION_INT(63,3,104))return AVERROR(ENOSYS);
    ret=!strcmp(p->in->input->iformat->name,"mpegts") ?
        avformat_live_input_probe_poll(p->in->input,100000) : avformat_live_input_probe_limit(p->in->input,100000);
    if(ret<0)return ret;
    AVStream *stream=p->in->input->streams[p->in->stream_index];
    if (stream->codecpar->codec_id!=AV_CODEC_ID_H264 && stream->codecpar->codec_id!=AV_CODEC_ID_HEVC) return AVERROR(ENOSYS);
    p->input_tb=stream->time_base;
    p->in->sei_codec=stream->codecpar->codec_id;
    p->in->sei_length_size=naki_sei_length_size(p->in->sei_codec,stream->codecpar->extradata,stream->codecpar->extradata_size);
    p->in->sei_input=naki_job_config(p->job)->sei;
    if(naki_job_config(p->job)->output_count) {
        p->in->sei_input=(NakiSeiConfig){0};
        for(int i=0;i<naki_job_config(p->job)->output_count;i++)if(naki_sei_union(&p->in->sei_input,&naki_job_config(p->job)->outputs[i].sei))return AVERROR(ENOBUFS);
    }
    p->in->reader.audio_subscribed=naki_job_config(p->job)->audio_copy;p->in->reader.subscription_generation=1;
    if(p->in->sei_input.count&&p->in->sei_length_size<0)return AVERROR_INVALIDDATA;
    p->input_wrap_bits=stream->pts_wrap_bits;
    ret=naki_video_rate_resolve(av_guess_frame_rate(p->in->input,stream,NULL),naki_job_config(p->job)->fps,&p->video_rate);
    if(ret<0)return ret;
    p->frame_rate=p->video_rate.value;
    naki_job_video_rate(p->job,&p->video_rate);
    p->stage=naki_job_config(p->job)->decoder_async?"async decoder initialization":"synchronous decoder initialization";
    const AVCodec *codec=avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) return AVERROR_DECODER_NOT_FOUND;
    p->in->decoder=avcodec_alloc_context3(codec);
    if (!p->in->decoder) return AVERROR(ENOMEM);
    if ((ret=avcodec_parameters_to_context(p->in->decoder,stream->codecpar))<0) return ret;
    if(naki_job_config(p->job)->codec_trace) {
        p->stage="codec_trace requires native software HEVC input";
        if(stream->codecpar->codec_id!=AV_CODEC_ID_HEVC)return AVERROR(ENOSYS);
        p->in->decoder->export_side_data|=AV_CODEC_EXPORT_DATA_LIVE_TRACE;
    }
    p->in->decoder->pkt_timebase=p->input_tb;
    p->in->decoder->thread_count=naki_job_config(p->job)->max_threads;
    p->in->decoder->thread_type=FF_THREAD_FRAME; p->in->decoder->flags|=AV_CODEC_FLAG_COPY_OPAQUE;
    p->in->decoder->max_pixels=NAKI_INPUT_MAX_PIXELS;
    if((ret=naki_decode_output_policy(p->in->decoder,naki_job_config(p->job)->decoder_early_output))<0)return ret;
    if ((ret=avcodec_open2(p->in->decoder,codec,NULL))<0) return ret;
    p->in->initial_input_properties=(AVFrame){.width=p->in->decoder->width,.height=p->in->decoder->height,.format=p->in->decoder->pix_fmt,
        .color_range=p->in->decoder->color_range,.colorspace=p->in->decoder->colorspace,.color_primaries=p->in->decoder->color_primaries,
        .color_trc=p->in->decoder->color_trc,.chroma_location=p->in->decoder->chroma_sample_location,.sample_aspect_ratio=p->in->decoder->sample_aspect_ratio};
    ret=naki_decode_backend_init(&p->in->decoder,&p->in->async,naki_job_config(p->job)->decoder_async,
        naki_job_config(p->job)->decoder_auto_threads,naki_job_config(p->job)->queue_capacity,
        naki_job_config(p->job)->min_threads,naki_job_config(p->job)->budget_slots,pipeline_decode_notify,p);
    if (ret<0) return ret;
    pipeline_refresh_auto(p);
    naki_job_checkpoint(p->job,NAKI_CP_DECODER_READY,0);
    naki_job_running(p->job);
    return event_log(p,"decoder_ready");
}
