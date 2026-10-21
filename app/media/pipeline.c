#include "pipeline_internal.h"

static int handle_audio(Pipeline *p, const InputItem *item);
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
    pthread_mutex_lock(&p->job->mutex);p->job->progress.media_phase=phase;pthread_mutex_unlock(&p->job->mutex);
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

void pipeline_checkpoint(Pipeline *p, int64_t *field) {
    pthread_mutex_lock(&p->job->mutex); if(!*field)*field=now_us(); pthread_mutex_unlock(&p->job->mutex);
}

int pipeline_interrupted(void *opaque) {
    Interrupt *io=opaque;
    int64_t deadline=atomic_load(&io->deadline);
    return (!atomic_load(&io->cleanup)&&atomic_load(&io->job->lifecycle.cancel)) || (deadline && now_us()>deadline);
}

void pipeline_refresh_auto(Pipeline *p) {
    AVCodecAsyncAutoStats stats={0};
    if (p->async) avcodec_async_get_auto_stats(p->async,&stats);
    pthread_mutex_lock(&p->job->mutex);
    p->job->progress.auto_stats=stats;
    p->job->progress.auto_updated_us=now_us();
    pthread_mutex_unlock(&p->job->mutex);
}
static int event_log(Pipeline *p, const char *event) {
    logger_submit(p->job->logger,NULL,new_event(event,p->job)); return 0;
}

static void audio_count(Pipeline *p, int unsupported) {
    pthread_mutex_lock(&p->job->mutex);
    if(unsupported)p->job->stats.audio_unsupported++; else p->job->stats.audio_dropped++;
    pthread_mutex_unlock(&p->job->mutex);
}
static int prepare_audio_params(AVCodecParameters *par, const AVPacket *pkt) {
    if(par->codec_id!=AV_CODEC_ID_AAC && par->codec_id!=AV_CODEC_ID_MP3)return AVERROR(ENOSYS);
    if(par->extradata_size>65536 || par->extradata_size<0)return AVERROR(EINVAL);
    size_t extra_bytes=0;const uint8_t *extra=av_packet_get_side_data(pkt,AV_PKT_DATA_NEW_EXTRADATA,&extra_bytes);
    if(extra) {
        if(extra_bytes<2||extra_bytes>65536)return AVERROR_INVALIDDATA;
        uint8_t *copy=av_mallocz(extra_bytes+AV_INPUT_BUFFER_PADDING_SIZE);if(!copy)return AVERROR(ENOMEM);
        memcpy(copy,extra,extra_bytes);av_freep(&par->extradata);par->extradata=copy;par->extradata_size=(int)extra_bytes;
    }
    if(par->codec_id==AV_CODEC_ID_AAC && pkt->size>=7 && pkt->data[0]==0xff && (pkt->data[1]&0xf6)==0xf0) {
        static const int rates[]={96000,88200,64000,48000,44100,32000,24000,22050,16000,12000,11025,8000,7350};
        int aot=(pkt->data[2]>>6)+1, frequency=(pkt->data[2]>>2)&15;
        int channels=((pkt->data[2]&1)<<2)|(pkt->data[3]>>6);
        if(frequency>=13 || !channels)return AVERROR(ENOSYS); /* PCE requires a parser, never guess it. */
        par->sample_rate=rates[frequency];
        av_channel_layout_uninit(&par->ch_layout); av_channel_layout_default(&par->ch_layout,channels==7 ? 8 : channels);
        if(!par->extradata_size) {
            par->extradata=av_mallocz(2+AV_INPUT_BUFFER_PADDING_SIZE);
            if(!par->extradata)return AVERROR(ENOMEM);
            par->extradata_size=2;
            par->extradata[0]=(aot<<3)|(frequency>>1);
            par->extradata[1]=((frequency&1)<<7)|(channels<<3);
        }
    }
    if((par->codec_id==AV_CODEC_ID_AAC && par->extradata_size>=2) ||
       (par->codec_id==AV_CODEC_ID_MP3 && (par->sample_rate<=0 || par->ch_layout.nb_channels<=0))) {
        /* Enhanced AAC has no legacy sound-rate/channel flags. Resolve its
           parameters with a bounded one-packet decode probe; the original
           compressed packet still goes through unchanged. */
        const AVCodec *codec=avcodec_find_decoder(par->codec_id);
        if(!codec)return AVERROR_DECODER_NOT_FOUND;
        AVCodecContext *probe=codec ? avcodec_alloc_context3(codec) : NULL;
        if(!probe)return AVERROR(ENOMEM);
        int ret=avcodec_parameters_to_context(probe,par);
        if(ret>=0)ret=avcodec_open2(probe,codec,NULL);
        AVFrame *decoded=av_frame_alloc();
        if(!decoded){avcodec_free_context(&probe);return AVERROR(ENOMEM);}
        if(ret>=0)ret=avcodec_send_packet(probe,pkt);
        if(ret>=0)ret=avcodec_receive_frame(probe,decoded);
        if(ret>=0) {
            probe->sample_rate=decoded->sample_rate;
            probe->frame_size=decoded->nb_samples;
            av_channel_layout_uninit(&probe->ch_layout);
            ret=av_channel_layout_copy(&probe->ch_layout,&decoded->ch_layout);
        }
        if(ret>=0 && probe->sample_rate>0 && probe->ch_layout.nb_channels>0) {
            par->sample_rate=probe->sample_rate;par->frame_size=probe->frame_size;
            av_channel_layout_uninit(&par->ch_layout);ret=av_channel_layout_copy(&par->ch_layout,&probe->ch_layout);
        }
        av_frame_free(&decoded);avcodec_free_context(&probe);
        if(ret<0)return ret;
    }
    if(par->sample_rate<=0 || par->ch_layout.nb_channels<=0 ||
       (par->codec_id==AV_CODEC_ID_AAC && par->extradata_size<2))return AVERROR(EAGAIN);
    return 0;
}
static int process_audio(Pipeline *p, const InputItem *item) {
    AudioTrack *track=NULL;
    for(int i=0;i<p->audio_count;i++)if(p->audio[i].input_index==item->packet->stream_index)track=&p->audio[i];
    if(!track) {
        if(p->audio_count==16) {audio_count(p,1);return 0;}
        track=&p->audio[p->audio_count++];
        *track=(AudioTrack){.input_index=item->packet->stream_index,.last_dts=AV_NOPTS_VALUE};
    }
    if(track->rejected){audio_count(p,1);return 0;}
    /* Read the immutable input snapshot before prepare_audio_params performs
     * any container/decoder normalization on this branch-owned copy. */
    NakiAudioSignature observed={0};int signature_ret=naki_audio_signature_read(&observed,item->params,item->packet);
    if(signature_ret==AVERROR(ENOMEM))return signature_ret;
    if(track->output) {
        const char *reason=signature_ret<0?"input_configuration_unreadable":naki_audio_signature_change(&track->accepted,&observed);
        if(reason) {
            track->rejected=1;audio_count(p,1);
            yyjson_mut_doc *doc=new_event("audio_track_changed_unsupported",p->job);
            json_int(doc,"input_stream",track->input_index);json_str(doc,"reason",reason);json_int(doc,"error_code",signature_ret);
            json_int(doc,"accepted_core_sample_rate",track->accepted.core_rate);json_int(doc,"observed_core_sample_rate",observed.core_rate);
            json_int(doc,"accepted_coded_channels",track->accepted.layout.nb_channels);json_int(doc,"observed_coded_channels",observed.layout.nb_channels);
            json_int(doc,"accepted_object_type",track->accepted.object_type);json_int(doc,"observed_object_type",observed.object_type);
            logger_submit(p->job->logger,NULL,doc);naki_audio_signature_clear(&observed);return 0;
        }
    } else if(signature_ret<0) {
        if(signature_ret!=AVERROR(EAGAIN)) {
            track->rejected=1;yyjson_mut_doc *doc=new_event("audio_track_rejected",p->job);
            json_int(doc,"input_stream",track->input_index);json_str(doc,"codec",avcodec_get_name(item->params->codec_id));
            json_str(doc,"reason","input_configuration_unreadable");json_int(doc,"error_code",signature_ret);logger_submit(p->job->logger,NULL,doc);
        }
        audio_count(p,signature_ret!=AVERROR(EAGAIN));naki_audio_signature_clear(&observed);return 0;
    }
    if(!track->output) {
        int ret=prepare_audio_params(item->params,item->packet);
        if(ret==AVERROR(EAGAIN)){audio_count(p,0);naki_audio_signature_clear(&observed);return 0;}
        if(ret==AVERROR(ENOMEM)) {naki_audio_signature_clear(&observed);return ret;}
        if(ret>=0 && item->params->codec_id==AV_CODEC_ID_AAC) {
            const AVBitStreamFilter *filter=av_bsf_get_by_name("aac_adtstoasc");
            ret=filter ? av_bsf_alloc(filter,&track->bsf) : AVERROR_BSF_NOT_FOUND;
            if(ret>=0)ret=avcodec_parameters_copy(track->bsf->par_in,item->params);
            if(ret>=0) {track->bsf->time_base_in=item->tb;ret=av_bsf_init(track->bsf);}
        }
        if(ret>=0)ret=avformat_live_add_audio(p->output,item->params,item->tb,&track->output);
        if(ret<0) {
            naki_audio_signature_clear(&observed);if(ret==AVERROR(ENOMEM))return ret;
            track->rejected=1;audio_count(p,1);
            yyjson_mut_doc *doc=new_event("audio_track_rejected",p->job);
            json_int(doc,"input_stream",track->input_index);json_str(doc,"codec",avcodec_get_name(item->params->codec_id));
            json_int(doc,"error_code",ret);logger_submit(p->job->logger,NULL,doc);
            return 0;
        }
        track->accepted=observed;memset(&observed,0,sizeof(observed));
        pthread_mutex_lock(&p->job->mutex);p->job->stats.audio_tracks++;pthread_mutex_unlock(&p->job->mutex);
        yyjson_mut_doc *doc=new_event("audio_track_added",p->job);
        json_int(doc,"input_stream",track->input_index);json_int(doc,"output_stream",track->output->index);
        json_str(doc,"codec",avcodec_get_name(item->params->codec_id));json_int(doc,"sample_rate",item->params->sample_rate);
        json_str(doc,"action","registered_without_reopening_output");logger_submit(p->job->logger,NULL,doc);
    }
    naki_audio_signature_clear(&observed);
    AVPacket *packet=av_packet_clone(item->packet);
    if(!packet)return AVERROR(ENOMEM);
    if(item->params->codec_id==AV_CODEC_ID_AAC && packet->size>=7 && packet->data[0]==0xff && (packet->data[1]&0xf6)==0xf0) {
        static const int rates[]={96000,88200,64000,48000,44100,32000,24000,22050,16000,12000,11025,8000,7350};
        int frequency=(packet->data[2]>>2)&15;
        int64_t raw=packet->dts!=AV_NOPTS_VALUE ? packet->dts : packet->pts;
        if(frequency<13) {
            /* Late ADTS is parsed without find_stream_info's decoded AAC
               parameters. Intermediate frames in one PES can lack timestamps.
               ADTS sample counts/rate give an exact duration in seconds even
               with implicit SBR; preserve each new PES anchor, expand only the
               missing/repeated timestamps between anchors. */
            int samples=1024*((packet->data[6]&3)+1);
            if(track->have_clock && (raw==AV_NOPTS_VALUE || raw==track->previous_input_dts)) {
                packet->pts=packet->dts=track->next_input_dts;
            } else if(raw!=AV_NOPTS_VALUE)packet->pts=packet->dts=raw;
            if(packet->dts!=AV_NOPTS_VALUE) {
                int64_t next=av_add_stable(item->tb,packet->dts,(AVRational){1,rates[frequency]},samples);
                packet->duration=next-packet->dts;track->next_input_dts=next;track->have_clock=1;
                if(raw!=AV_NOPTS_VALUE)track->previous_input_dts=raw;
            }
        }
    }
    int ret=0;
    AVRational tb=item->tb;
    if(track->bsf) {
        ret=av_bsf_send_packet(track->bsf,packet);
        if(ret>=0)ret=av_bsf_receive_packet(track->bsf,packet);
        tb=track->bsf->time_base_out;
    }
    if(ret>=0) {
        if(packet->dts==AV_NOPTS_VALUE)packet->dts=packet->pts;
        if(packet->pts==AV_NOPTS_VALUE)packet->pts=packet->dts;
        av_packet_rescale_ts(packet,tb,track->output->time_base);packet->stream_index=track->output->index;
        int64_t dts=packet->dts;
        if(dts==AV_NOPTS_VALUE || (track->last_dts!=AV_NOPTS_VALUE && dts<=track->last_dts) ||
           av_rescale_q(dts,track->output->time_base,AV_TIME_BASE_Q)<p->mux_max_dts_us) {
            audio_count(p,0);ret=0;
        } else {
            track->last_dts=dts;ret=pipeline_submit_mux(p,packet);
        }
    } else if(ret!=AVERROR(ENOMEM)) {audio_count(p,0);ret=0;}
    av_packet_free(&packet);
    return ret;
}
static int handle_audio(Pipeline *p, const InputItem *item) {
    if(p->video_started)return process_audio(p,item);
    NakiBranchItem accounting={.packet=item->packet,.parameters=item->params,.media_us=1,.ingress_us=0};
    int charged=naki_branch_item_charge(&accounting);if(charged<0)return charged;
    uint64_t cost=accounting.charged_bytes;
    if(cost>4*1024*1024){audio_count(p,0);return 0;}
    while(p->deferred_count && (p->deferred_count==128 || p->deferred_bytes+cost>4*1024*1024)) {
        InputItem *old=&p->deferred_audio[0];
        p->deferred_bytes-=old->retained_bytes;
        pipeline_input_free(old);p->deferred_count--;
        memmove(p->deferred_audio,p->deferred_audio+1,p->deferred_count*sizeof(*old));audio_count(p,0);
    }
    InputItem copy={.packet=av_packet_clone(item->packet),.params=avcodec_parameters_alloc(),.tb=item->tb,.retained_bytes=cost};
    if(!copy.packet || !copy.params) {pipeline_input_free(&copy);return AVERROR(ENOMEM);}
    int ret=avcodec_parameters_copy(copy.params,item->params);
    if(ret<0){pipeline_input_free(&copy);return ret;}
    p->deferred_audio[p->deferred_count++]=copy;p->deferred_bytes+=cost;
    return 0;
}
int pipeline_flush_deferred_audio(Pipeline *p) {
    if(!p->video_started)return 0;
    while(p->deferred_count) {
        InputItem item=p->deferred_audio[0];
        p->deferred_count--;p->deferred_bytes-=item.retained_bytes;
        memmove(p->deferred_audio,p->deferred_audio+1,p->deferred_count*sizeof(item));
        int ret=process_audio(p,&item);pipeline_input_free(&item);
        if(ret<0)return ret;
    }
    return 0;
}
int pipeline_filter_output(Pipeline *p) {
    AVFrame *frame=av_frame_alloc();
    if (!frame) return AVERROR(ENOMEM);
    int ret;
    while ((ret=av_buffersink_get_frame(p->sink,frame))>=0) {
        frame->sample_aspect_ratio=p->encoder->sample_aspect_ratio;
        if(p->job->stats.filter_generations>1) {frame->colorspace=p->encoder->colorspace;frame->color_range=p->encoder->color_range;}
        if(!pipeline_stamp_valid(frame->opaque_ref)) {ret=AVERROR_INVALIDDATA;break;}
        uint64_t watermark_revision=0;
        Stamp stamp=*(Stamp *)frame->opaque_ref->data;NakiOperationHandle lease=0;
        AVRational time_base=av_buffersink_get_time_base(p->sink);
        NakiFrameIdentity identity=pipeline_frame_identity(p,&stamp,frame->pts,time_base);
        p->stage="watermark composition";
        ret=naki_watermark_frame_tracked(p->job->watermark,&p->watermark_compositor,frame,
            time_base,&identity,&watermark_revision,&lease);
        if(ret>=0) {
            stamp.watermark_revision=watermark_revision;stamp.delivery_owner=p->job->watermark;stamp.delivery_lease=lease;
            size_t owned_size=frame->opaque_ref->size;
            Stamp *owned=av_memdup(frame->opaque_ref->data,owned_size);
            if(owned)*owned=stamp;
            AVBufferRef *reference=owned?av_buffer_create((uint8_t *)owned,owned_size,delivery_stamp_free,NULL,0):NULL;
            if(!reference) {
                av_free(owned);if(naki_watermark_release_frame(p->job->watermark,lease))abort();ret=AVERROR(ENOMEM);
            } else {av_buffer_unref(&frame->opaque_ref);frame->opaque_ref=reference;}
        }
        if(ret>=0) {
            if(p->watermark_revision!=watermark_revision) {
                yyjson_mut_doc *event=new_event("watermark_applied",p->job);
                naki_watermark_status(p->job->watermark,event);logger_submit(p->job->logger,NULL,event);
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
        if(!p->job->config.decoder_pump)p->stage="selected SEI picture association";
        if(frame->flags&AV_FRAME_FLAG_INTERLACED)return AVERROR(ENOSYS);
        int claim=pipeline_reader_sei_obligation(p,input_stamp->sequence,0);if(claim<0)return claim;
    }
    naki_policy_untrack(p->job,NAKI_TRACK_PACKET,(uint64_t)((Stamp *)frame->opaque_ref->data)->sequence);
    /* Stamp is a private packet property propagated by AV_CODEC_FLAG_COPY_OPAQUE. */
    int ret=av_buffer_make_writable(&frame->opaque_ref);
    if (ret<0) return ret;
    Stamp *stamp=(Stamp *)frame->opaque_ref->data;
    stamp->decoded_us=now_us();
    if(p->job->config.codec_trace) {
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
    pthread_mutex_lock(&p->job->mutex);p->job->progress.last_decoded_us=stamp->decoded_us;p->job->progress.media_phase=NAKI_MEDIA_PHASE_RUNNING;pthread_mutex_unlock(&p->job->mutex);
    pipeline_checkpoint(p,&p->job->progress.first_decoded_us);
    stamp->picture_type=frame->pict_type;
    stamp->first_i=frame->pict_type==AV_PICTURE_TYPE_I && !p->seen_input_i;
    if (frame->pict_type==AV_PICTURE_TYPE_I) p->seen_input_i=1;
    stamp->decoded_key=!!(frame->flags&AV_FRAME_FLAG_KEY);
    if(p->next_frame_id==INT64_MAX)return AVERROR(ERANGE);
    stamp->frame_id=++p->next_frame_id; stamp->raw_pts=frame->pts;
    if(p->job->config.policy.timestamps==NAKI_TS_SOURCE_ALL) {
        if(!p->job->config.decoder_pump)p->stage="strict source PTS";
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
    if(p->job->config.codec_trace) {
        yyjson_mut_doc *event=new_event("decoder_frame",p->job);
        json_int(event,"frame_id",stamp->frame_id);json_int(event,"input_sequence",stamp->sequence);
        json_int(event,"pts_us",stamp->source_us);json_int(event,"decoder_submit_monotonic_us",stamp->submitted_us);
        json_int(event,"decoded_monotonic_us",stamp->decoded_us);
        naki_decode_trace_json(event,&stamp->decode_trace);logger_submit(p->job->logger,p->job->trace,event);
    }
    if(stamp->source_us==INT64_MIN || stamp->source_us==INT64_MAX)return AVERROR(ERANGE);
    return 0;
}
static int rebuild_input_filter(Pipeline *p,const AVFrame *frame) {
    if(frame->width==p->input_width&&frame->height==p->input_height&&frame->format==p->input_format&&
       frame->colorspace==p->input_colorspace&&frame->color_range==p->input_range&&
       frame->color_primaries==p->encoder->color_primaries&&frame->color_trc==p->encoder->color_trc)return 0;
    p->stage="dynamic input validation";
    int ret=naki_watermark_video(p->job->watermark,frame,p->encoder->width,p->encoder->height);if(ret<0)return ret;
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
    pthread_mutex_lock(&p->job->mutex);uint64_t generation=++p->job->stats.filter_generations;pthread_mutex_unlock(&p->job->mutex);
    yyjson_mut_doc *event=new_event("input_filter_rebuilt",p->job);json_int(event,"generation",generation);
    json_int(event,"input_width",frame->width);json_int(event,"input_height",frame->height);json_int(event,"input_format",frame->format);
    json_int(event,"output_width",p->encoder->width);json_int(event,"output_height",p->encoder->height);
    json_str(event,"encoder_action","unchanged");logger_submit(p->job->logger,NULL,event);return 0;
}
static int process_frame(Pipeline *p,AVFrame *frame) {
    int ret=p->job->parent?av_buffer_make_writable(&frame->opaque_ref):p->job->config.decoder_pump?0:pipeline_prepare_decoded_frame(p,frame);
    if(ret<0)return ret;
    Stamp *stamp=(Stamp *)frame->opaque_ref->data;
    if(p->job->config.decoder_pump)stamp->decoder_owner_us=now_us();
    if(p->job->parent) {
        stamp->branch_started_us=now_us();
        pthread_mutex_lock(&p->job->mutex);
        if(!p->job->progress.first_decoded_us)p->job->progress.first_decoded_us=stamp->decoded_us;
        if(!p->job->progress.first_packet_us)p->job->progress.first_packet_us=stamp->ingress_us;
        p->job->progress.last_decoded_us=stamp->decoded_us;p->job->progress.media_phase=NAKI_MEDIA_PHASE_RUNNING;
        pthread_mutex_unlock(&p->job->mutex);
    }
    if(!p->rate_divisor) {
        p->stage="explicit output rate requires a known integer input/output ratio";
        ret=naki_video_rate_output(&p->video_rate,p->job->config.output_fps,&p->output_rate,&p->rate_divisor);
        if(ret<0)return ret;
    }
    if(!stamp->decoder_tracked&&(ret=naki_policy_track(p->job,NAKI_TRACK_FRAME,(uint64_t)stamp->frame_id,stamp->ingress_us))<0)return ret;
    if(p->graph&&(ret=rebuild_input_filter(p,frame))<0)return ret;
    stamp->intra_reason=naki_intra_decide(&p->job->config.policy,&p->intra,stamp->source_us,stamp->picture_type==AV_PICTURE_TYPE_I);
    int sei_messages=naki_sei_count(pipeline_stamp_sei(frame->opaque_ref),&p->job->config.sei);
    int sei_protected=sei_messages>0;
    int protect=stamp->intra_reason || stamp->picture_type==AV_PICTURE_TYPE_I || stamp->frame_id==1 || sei_protected;
    pthread_mutex_lock(&p->job->mutex);
    p->job->stats.frames_decoded++; if(p->job->stats.frame_id_last<(uint64_t)stamp->frame_id)p->job->stats.frame_id_last=(uint64_t)stamp->frame_id; p->job->stats.timestamp_wraps=stamp->timestamp_wraps;
    if(p->job->parent)p->job->stats.sei_extracted+=sei_messages;
    int level=p->job->flow.overload.level;
    pthread_mutex_unlock(&p->job->mutex);
    int keep=naki_frame_keep(&p->job->config.policy,level,(uint64_t)stamp->frame_id,protect);
    if(keep<0)return keep;
    int rate_drop=keep && !naki_video_rate_keep((uint64_t)stamp->frame_id,p->rate_divisor,protect);
    if(rate_drop)keep=0;
    pthread_mutex_lock(&p->job->mutex);
    if(protect)p->job->stats.frames_protected++;
    if(sei_protected)p->job->stats.sei_protected++;
    if(keep)p->job->stats.frames_selected++;
    else if(rate_drop)p->job->stats.frames_dropped_rate++;
    else p->job->stats.frames_dropped_overload++;
    pthread_mutex_unlock(&p->job->mutex);
    if(!keep) {
        if(!naki_policy_untrack(p->job,NAKI_TRACK_FRAME,(uint64_t)stamp->frame_id))return AVERROR_INVALIDDATA;
        naki_branch_release(p->job,(uint64_t)stamp->frame_id,stamp->branch_charge);
        if(p->job->config.frame_log && (stamp->frame_id-1)%p->job->config.frame_log_every==0) {
            yyjson_mut_doc *event=new_event("frame_drop",p->job);
            json_int(event,"frame_id",stamp->frame_id); json_int(event,"input_sequence",stamp->sequence);
            json_int(event,"source_pts",stamp->source_pts); json_int(event,"overload_level",level);
            json_str(event,"reason",rate_drop?"output_rate":"overload"); logger_submit(p->job->logger,p->job->trace,event);
        }
        return 0;
    }
    if(!p->graph) {
        p->stage="watermark video validation";
        if((ret=naki_watermark_video(p->job->watermark,frame,p->job->config.width?p->job->config.width:frame->width,
              p->job->config.height?p->job->config.height:frame->height))<0)return ret;
        if((ret=pipeline_open_output(p,frame))<0)return ret;
    }
    stamp->generation=(unsigned)p->job->stats.filter_generations;
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
    if(p->job->config.policy.timestamps==NAKI_TS_SOURCE_ALL)p->input->flags|=AVFMT_FLAG_NOFILLIN;
    p->input->interrupt_callback=(AVIOInterruptCB){pipeline_interrupted,&p->read_interrupt};
    p->input->probesize=1024*1024; p->input->max_analyze_duration=1000000;
    p->input->max_streams=64;
    atomic_store(&p->read_interrupt.deadline,now_us()+IO_TIMEOUT_US);
    pipeline_checkpoint(p,&p->job->progress.input_open_started_us);
    AVDictionary *input_options=NULL;
    if(p->job->config.rtmp_tcp_nodelay&&!strncmp(p->job->config.input,"rtmp://",7))av_dict_set(&input_options,"tcp_nodelay","1",0);
    if(p->job->config.network_trace) {
        if(avformat_version()<AV_VERSION_INT(63,3,105))return AVERROR(ENOSYS);
        av_dict_set(&input_options,"rtmp_read_origin","1",0);
    }
    int ret=avformat_open_input(&p->input,p->job->config.input,NULL,&input_options);
    if(ret>=0&&av_dict_count(input_options))ret=AVERROR_OPTION_NOT_FOUND;
    av_dict_free(&input_options);
    if (ret<0) return ret;
    pipeline_checkpoint(p,&p->job->progress.input_open_done_us);
    ret=naki_probe_video(p->input);
    atomic_store(&p->read_interrupt.deadline,0);
    if (ret<0) return ret;
    p->stream_index=ret;
    pipeline_checkpoint(p,&p->job->progress.input_probe_done_us);
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
    p->sei_input=p->job->config.sei;
    if(p->job->config.output_count) {
        p->sei_input=(NakiSeiConfig){0};
        for(int i=0;i<p->job->config.output_count;i++)if(naki_sei_union(&p->sei_input,&p->job->config.outputs[i].sei))return AVERROR(ENOBUFS);
    }
    if(p->sei_input.count&&p->sei_length_size<0)return AVERROR_INVALIDDATA;
    p->input_wrap_bits=stream->pts_wrap_bits;
    ret=naki_video_rate_resolve(av_guess_frame_rate(p->input,stream,NULL),p->job->config.fps,&p->video_rate);
    if(ret<0)return ret;
    p->frame_rate=p->video_rate.value;
    pthread_mutex_lock(&p->job->mutex);p->job->progress.video_rate=p->video_rate;pthread_mutex_unlock(&p->job->mutex);
    p->stage="async decoder initialization";
    const AVCodec *codec=avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) return AVERROR_DECODER_NOT_FOUND;
    p->decoder=avcodec_alloc_context3(codec);
    if (!p->decoder) return AVERROR(ENOMEM);
    if ((ret=avcodec_parameters_to_context(p->decoder,stream->codecpar))<0) return ret;
    if(p->job->config.codec_trace) {
        p->stage="codec_trace requires native software HEVC input";
        if(stream->codecpar->codec_id!=AV_CODEC_ID_HEVC)return AVERROR(ENOSYS);
        p->decoder->export_side_data|=AV_CODEC_EXPORT_DATA_LIVE_TRACE;
    }
    p->decoder->pkt_timebase=p->input_tb;
    p->decoder->thread_count=p->job->config.max_threads;
    p->decoder->thread_type=FF_THREAD_FRAME; p->decoder->flags|=AV_CODEC_FLAG_COPY_OPAQUE;
    p->decoder->max_pixels=NAKI_INPUT_MAX_PIXELS;
    if(stream->codecpar->codec_id==AV_CODEC_ID_HEVC &&
       (ret=av_opt_set_int(p->decoder->priv_data,"live_idr_early_output",1,0))<0) return ret;
    if(stream->codecpar->codec_id==AV_CODEC_ID_HEVC &&
       (ret=av_opt_set_int(p->decoder->priv_data,"live_poc_early_output",1,0))<0) return ret;
    if ((ret=avcodec_open2(p->decoder,codec,NULL))<0) return ret;
    if ((ret=avcodec_async_create(&p->async,&p->decoder,p->job->config.queue_capacity,pipeline_decode_notify,p))<0) return ret;
    AVCodecAsyncBudget *budget=NULL;
    ret=avcodec_async_budget_create(&budget,p->job->config.budget_slots,NULL);
    if (ret>=0) ret=avcodec_async_enable_auto(p->async,p->job->config.min_threads,budget);
    avcodec_async_budget_free(&budget);
    if (ret<0) return ret;
    pipeline_refresh_auto(p);
    pipeline_checkpoint(p,&p->job->progress.decoder_ready_us);
    pthread_mutex_lock(&p->job->mutex); p->job->lifecycle.state=NAKI_JOB_STATE_RUNNING; pthread_mutex_unlock(&p->job->mutex);
    return event_log(p,"decoder_ready");
}
static void branch_queue_stats(Branch *b) {
    pthread_mutex_lock(&b->queue.mutex);
    unsigned count=b->queue.count,high=b->queue.high_water;uint64_t bytes=b->queue.bytes,media=b->queue.media_us;
    unsigned video=b->queue.video_count,audio=b->queue.audio_count,video_high=b->queue.video_high,audio_high=b->queue.audio_high,reason=b->queue.rejection_reason;
    int64_t oldest=b->queue.count?b->queue.items[b->queue.head].ingress_us:0;
    uint64_t rejected[NAKI_BRANCH_REASONS];memcpy(rejected,b->queue.rejected,sizeof(rejected));
    pthread_mutex_unlock(&b->queue.mutex);Job *j=b->pipeline.job;
    pthread_mutex_lock(&j->mutex);j->stats.branch_queued=count;j->stats.branch_queue_bytes=bytes;j->stats.branch_queue_media_us=media;j->stats.branch_queue_high=high;
    j->stats.branch_video_queued=video;j->stats.branch_audio_queued=audio;j->stats.branch_video_high=video_high;j->stats.branch_audio_high=audio_high;j->stats.branch_rejection_reason=reason;
    j->progress.branch_oldest_ingress_us=oldest;
    memcpy(j->stats.branch_rejected,rejected,sizeof(rejected));pthread_mutex_unlock(&j->mutex);
}
static void fail_branch(Branch *b,int ret,const char *stage) {
    Job *j=b->pipeline.job;if(atomic_load(&j->lifecycle.done))return;
    pthread_mutex_lock(&j->mutex);
    if(!j->lifecycle.branch_failed) {
        char error[AV_ERROR_MAX_STRING_SIZE];av_strerror(ret,error,sizeof(error));
        j->lifecycle.branch_failed=1;snprintf(j->lifecycle.error,sizeof(j->lifecycle.error),"%s: %s",stage,error);j->lifecycle.state=NAKI_JOB_STATE_FAILING;
    }
    pthread_mutex_unlock(&j->mutex);request_stop(j);naki_branch_queue_abort(&b->queue);branch_queue_stats(b);
}
static int check_branches(Pipeline *p,int finishing) {
    int active=0;int64_t now=now_us();
    for(int i=0;i<p->branch_count;i++) {
        Branch *b=&p->branches[i];Job *j=b->pipeline.job;
        if(!b->started||atomic_load(&j->lifecycle.done))continue;
        if(atomic_load(&p->job->lifecycle.cancel)) {request_stop(j);naki_branch_queue_abort(&b->queue);continue;}
        pthread_mutex_lock(&j->mutex);
        int64_t age=naki_inflight_age(&j->flow.branch_pending,now),write=j->progress.write_started_us?now-j->progress.write_started_us:0;
        int failed=j->lifecycle.branch_failed;pthread_mutex_unlock(&j->mutex);
        if(!failed) {
            if(age>(int64_t)b->spec->max_lag_ms*1000)fail_branch(b,AVERROR(ETIMEDOUT),"branch retained-frame ingress deadline");
            else if(write>(int64_t)b->spec->io_timeout_ms*1000)fail_branch(b,AVERROR(ETIMEDOUT),"branch output I/O deadline");
            else if(naki_branch_queue_check(&b->queue,now)==AVERROR(ETIMEDOUT)) {
                pthread_mutex_lock(&b->queue.mutex);int pressure=b->queue.admission_deadline_us&&now>=b->queue.admission_deadline_us;unsigned reason=b->queue.rejection_reason;pthread_mutex_unlock(&b->queue.mutex);
                char stage[96];snprintf(stage,sizeof(stage),pressure?"branch bounded admission window (%s)":"branch queued ingress deadline (%s)",naki_branch_reason_name(reason));
                fail_branch(b,AVERROR(ETIMEDOUT),stage);
            }
        }
        if(!atomic_load(&j->lifecycle.cancel))active++;
        branch_queue_stats(b);
    }
    return !finishing&&!active?AVERROR(EPIPE):0;
}

static void *branch_main(void *opaque) {
    (void)crash_thread_init();Branch *b=opaque;Pipeline *p=&b->pipeline;Job *j=p->job;
    NakiMediaJobInfo info={.job_id=j->id,.request_id=j->request_id,.config=&j->config,.request_us=j->lifecycle.started_us,.output_id=j->output_id};
    p->stage="branch media hook initialization";int ret=naki_media_hooks_open(&info,&p->hooks);
    if(ret>0||(ret>=0&&p->hooks.api_version!=NAKI_MEDIA_HOOK_API))ret=AVERROR(EINVAL);
    if(ret>=0) {pthread_mutex_lock(&j->mutex);j->lifecycle.state=NAKI_JOB_STATE_RUNNING;pthread_mutex_unlock(&j->mutex);}
    while(ret>=0&&!atomic_load(&j->lifecycle.cancel)) {
        media_phase(p,NAKI_MEDIA_PHASE_WAITING_INPUT);
        NakiBranchItem item={0};ret=naki_branch_queue_pop(&b->queue,&item,now_us()+20000);branch_queue_stats(b);
        if(ret==AVERROR_EOF) {media_phase(p,NAKI_MEDIA_PHASE_DRAINING);ret=pipeline_drain_output(p);break;}
        if(ret==AVERROR(ETIMEDOUT)||ret==AVERROR(EAGAIN)) {ret=p->live_mux?pipeline_drain_mux(p,0):0;continue;}
        if(ret<0)break;
        pthread_mutex_lock(&j->mutex);j->progress.last_input_us=now_us();j->progress.media_phase=NAKI_MEDIA_PHASE_RUNNING;pthread_mutex_unlock(&j->mutex);
        if(item.frame)ret=process_frame(p,item.frame);
        else if(j->config.audio_copy&&strcmp(j->config.output_format,"mp4")) {
            InputItem audio={.packet=item.packet,.params=item.parameters,.tb=item.time_base,.demuxed_us=item.ingress_us};
            ret=handle_audio(p,&audio);
        }
        naki_branch_item_free(&item);
        if(ret>=0&&p->live_mux) {ret=pipeline_flush_deferred_audio(p);if(ret>=0)ret=pipeline_drain_mux(p,0);}
    }
    if(!atomic_load(&j->parent->lifecycle.cancel)&&atomic_load(&j->lifecycle.cancel)&&!j->lifecycle.branch_failed)fail_branch(b,AVERROR(ETIMEDOUT),"branch overload cancellation");
    if(ret<0&&!atomic_load(&j->parent->lifecycle.cancel))fail_branch(b,ret,p->stage);
    naki_branch_queue_abort(&b->queue);naki_watermark_owner_finish(j->watermark,ret,atomic_load(&j->parent->lifecycle.cancel));naki_watermark_stop(j->watermark);
    ret=pipeline_finalize_output(p,ret,atomic_load(&j->parent->lifecycle.cancel));
    if(ret<0&&!atomic_load(&j->parent->lifecycle.cancel))fail_branch(b,ret,p->stage);
    pipeline_release_media_resources(p,NULL);
    pipeline_complete_job(p,ret,0);
    return NULL;
}
static int start_branches(Pipeline *p) {
    p->branch_count=p->job->config.output_count;p->branches=calloc(p->branch_count,sizeof(*p->branches));if(!p->branches)return AVERROR(ENOMEM);
    for(int i=0;i<p->branch_count;i++)p->branches[i].pipeline.job=p->job->outputs[i];
    for(int i=0;i<p->branch_count;i++) {
        Branch *b=&p->branches[i];Job *j=p->job->outputs[i];b->spec=&p->job->config.outputs[i];
        b->pipeline=(Pipeline){.job=j,.input_tb=p->input_tb,.frame_rate=p->frame_rate,.video_rate=p->video_rate,.input_wrap_bits=p->input_wrap_bits,
            .custom_output_fd=-1,.last_pts=AV_NOPTS_VALUE,.mux_max_dts_us=INT64_MIN,.stage="branch initialization",.io_timeout_us=(int64_t)b->spec->io_timeout_ms*1000};
        b->pipeline.write_interrupt.job=j;atomic_init(&b->pipeline.write_interrupt.deadline,0);
        atomic_init(&b->pipeline.write_interrupt.cleanup,0);
        pthread_mutex_lock(&j->mutex);j->progress.input_open_started_us=p->job->progress.input_open_started_us;j->progress.input_open_done_us=p->job->progress.input_open_done_us;
        j->progress.input_probe_done_us=p->job->progress.input_probe_done_us;j->progress.decoder_ready_us=p->job->progress.decoder_ready_us;j->progress.video_rate=p->video_rate;pthread_mutex_unlock(&j->mutex);
        int ret=naki_branch_queue_init_limits(&b->queue,b->spec->queue_items,b->spec->audio_packets,b->spec->queue_bytes,(int64_t)b->spec->queue_media_ms*1000,(int64_t)b->spec->max_lag_ms*1000);
        if(ret<0)return ret;
        b->initialized=1;
        if(atomic_load(&j->lifecycle.done))continue;
        ret=pthread_create(&b->thread,NULL,branch_main,b);
        if(ret) {
            fail_branch(b,AVERROR(ret),"branch thread creation");naki_watermark_stop(j->watermark);
            pthread_mutex_lock(&j->mutex);j->lifecycle.state=NAKI_JOB_STATE_FAILED;j->flow.policy_closing=1;j->lifecycle.ended_us=now_us();pthread_mutex_unlock(&j->mutex);
            atomic_store(&j->lifecycle.done,1);continue;
        }
        b->started=1;
    }
    return check_branches(p,0);
}
static int admission_check(void *opaque) {
    Pipeline *p=opaque;
    return atomic_load(&p->job->lifecycle.cancel)?AVERROR_EXIT:check_branches(p,0);
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
        if(offers[i].queue&&offers[i].result<0&&!atomic_load(&p->job->lifecycle.cancel)&&!atomic_load(&j->lifecycle.done)) {
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
    pthread_mutex_lock(&p->job->mutex);p->job->stats.frames_decoded++;p->job->stats.frame_id_last=source->frame_id;p->job->stats.timestamp_wraps=p->timeline.wraps;
    metric_add(&p->job->stats.lifetime.decode,(source->decoded_us-source->submitted_us)/1000.0,p->job->config.slow_frame_ms);
    metric_add(&p->job->stats.window.decode,(source->decoded_us-source->submitted_us)/1000.0,p->job->config.slow_frame_ms);pthread_mutex_unlock(&p->job->mutex);
    for(int i=0;i<p->branch_count;i++) {
        Branch *b=&p->branches[i];Job *j=b->pipeline.job;if(!b->started||atomic_load(&j->lifecycle.done)||atomic_load(&j->lifecycle.cancel))continue;
        NakiBranchItem item={.frame=av_frame_clone(frame),.source_us=source->source_us,.ingress_us=source->ingress_us,.sequence=source->sequence,.time_base=p->input_tb};
        /* Queue work-duration budget never turns legal source PTS holes or
         * long display durations into an admission failure. No PTS is retimed. */
        int64_t duration=p->video_rate.nominal_frame_work_us;
        item.media_us=duration>0?(uint64_t)duration:1;
        ret=item.frame?av_buffer_make_writable(&item.frame->opaque_ref):AVERROR(ENOMEM);
        if(ret>=0)ret=naki_branch_item_charge(&item);
        pthread_mutex_lock(&j->mutex);
        if(!j->progress.fixed_output_width) {j->progress.fixed_output_width=j->config.width?j->config.width:frame->width;j->progress.fixed_output_height=j->config.height?j->config.height:frame->height;}
        int width=j->progress.fixed_output_width,height=j->progress.fixed_output_height;pthread_mutex_unlock(&j->mutex);
        int output_bytes=av_image_get_buffer_size(!strcmp(j->config.encoder,"libvvenc")?AV_PIX_FMT_YUV420P10LE:AV_PIX_FMT_YUV420P,width,height,32);
        uint64_t reservation=item.charged_bytes+(output_bytes>0?(uint64_t)output_bytes:0);
        int reserved=0;
        if(ret>=0&&output_bytes>0) {
            pthread_mutex_lock(&j->mutex);
            if(j->stats.branch_inflight_frames>=128||reservation>b->spec->inflight_bytes-j->stats.branch_inflight_bytes)ret=AVERROR(ENOBUFS);
            else if(!(ret=naki_inflight_add(&j->flow.branch_pending,NAKI_TRACK_FRAME,(uint64_t)source->frame_id,source->ingress_us))) {
                j->stats.branch_inflight_bytes+=reservation;j->stats.branch_inflight_frames++;reserved=1;
                if(j->stats.branch_inflight_frames>j->stats.branch_inflight_high)j->stats.branch_inflight_high=j->stats.branch_inflight_frames;
            }
            pthread_mutex_unlock(&j->mutex);
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
        if(!b->started||atomic_load(&j->lifecycle.done)||atomic_load(&j->lifecycle.cancel)||!j->config.audio_copy||!strcmp(j->config.output_format,"mp4"))continue;
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
        int waiting=0;for(int i=0;i<p->branch_count;i++)if(p->branches[i].started&&!atomic_load(&p->branches[i].pipeline.job->lifecycle.done))waiting=1;
        if(!waiting)break;
        check_branches(p,1);struct pollfd fd={.fd=p->job->wake_fd,.events=POLLIN};poll(&fd,1,20);
        uint64_t value;while(read(p->job->wake_fd,&value,sizeof(value))>0) {}
    }
    int completed=0;
    for(int i=0;i<p->branch_count;i++) {
        Branch *b=&p->branches[i];if(b->started) {pthread_join(b->thread,NULL);b->started=0;}
        if((b->pipeline.job->lifecycle.state==NAKI_JOB_STATE_COMPLETED))completed++;
    }
    return completed?0:AVERROR(EPIPE);
}
void *pipeline_main(void *opaque) {
    (void)crash_thread_init();
    Job *job=opaque;
    Pipeline p={.job=job,.custom_output_fd=-1,.last_pts=AV_NOPTS_VALUE,.mux_max_dts_us=INT64_MIN,.stage="initialization"};
    if(!job->config.output_count) {OutputConfig output;naki_logical_output(&job->config,0,&output);p.io_timeout_us=(int64_t)output.io_timeout_ms*1000;}
    p.read_interrupt.job=job; p.write_interrupt.job=job;
    atomic_init(&p.read_interrupt.deadline,0); atomic_init(&p.write_interrupt.deadline,0);
    atomic_init(&p.read_interrupt.cleanup,0);atomic_init(&p.write_interrupt.cleanup,0);
    pipeline_reader_init(&p);pipeline_decode_init(&p);
    InputItem pending={0}; AVFrame *frame=av_frame_alloc();int branches_joined=0;
    NakiMediaJobInfo info={.job_id=job->id,.request_id=job->request_id,.config=&job->config,.request_us=job->lifecycle.started_us,
        .output_id=job->config.output_count?NULL:job->output_id};
    p.stage="media hook initialization";
    int ret=frame ? naki_media_hooks_open(&info,&p.hooks) : AVERROR(ENOMEM);
    if(ret>0 || (ret>=0 && p.hooks.api_version!=NAKI_MEDIA_HOOK_API))ret=AVERROR(EINVAL);
    if(ret>=0) {
        p.stage="required watermark startup gate";
        pthread_mutex_lock(&job->mutex);
        if(job->config.watermark.required)job->lifecycle.state=NAKI_JOB_STATE_WAITING_ASSETS;
        pthread_mutex_unlock(&job->mutex);
        ret=naki_watermark_wait(job->watermark);
        if(ret>=0&&job->config.output_count) {
            int ready=0;pthread_mutex_lock(&job->mutex);job->lifecycle.state=NAKI_JOB_STATE_WAITING_ASSETS;pthread_mutex_unlock(&job->mutex);
            for(int i=0;i<job->config.output_count;i++) {
                Job *b=job->outputs[i];int gate=naki_watermark_wait(b->watermark);
                if(gate<0) {
                    pthread_mutex_lock(&b->mutex);b->lifecycle.branch_failed=1;b->lifecycle.state=NAKI_JOB_STATE_FAILED;snprintf(b->lifecycle.error,sizeof(b->lifecycle.error),"required watermark startup failed: %d",gate);b->lifecycle.ended_us=now_us();pthread_mutex_unlock(&b->mutex);
                    naki_watermark_stop(b->watermark);atomic_store(&b->lifecycle.done,1);
                } else ready++;
            }
            if(!ready) {p.stage="all output startup gates failed";ret=AVERROR(EPIPE);}
        }
    }
    if(ret>=0)ret=initialize_decoder(&p);
    if (ret<0) goto cleanup;
    if(job->config.output_count&&(ret=start_branches(&p))<0)goto cleanup;
    ret=pipeline_reader_start(&p);
    if (ret) {ret=AVERROR(ret); goto cleanup;}
    if(job->config.decoder_pump&&(ret=pipeline_decode_start(&p))<0)goto cleanup;
    int eof_sent=0, decoder_eof=0;
    while (!atomic_load(&job->lifecycle.cancel) && !decoder_eof) {
        if(p.branches&&(ret=check_branches(&p,0))<0) {p.stage="all outputs failed";goto cleanup;}
        uint64_t wake;
        while (read(job->wake_fd,&wake,sizeof(wake))<0 && errno==EINTR) {}
        int progress=0;
        p.stage="async decode receive";
        for (int batch=0;batch<1;batch++) {
            ret=job->config.decoder_pump?pipeline_decode_receive(&p,frame,&pending):avcodec_async_receive_frame(p.async,frame);
            if (ret==AVERROR_EOF) {decoder_eof=1; ret=0; break;}
            if (ret==AVERROR(EAGAIN)) {ret=0; break;}
            if (ret<0) goto cleanup;
            ret=p.branches?fanout_frame(&p,frame):process_frame(&p,frame); av_frame_unref(frame);
            if (ret<0) goto cleanup;
            progress=1;
        }
        if (decoder_eof) break;
        int input_done=0, input_error=0;
        if (!job->config.decoder_pump && !pending.packet && !eof_sent) pending=pipeline_reader_pop(&p,&input_done,&input_error);
        if(pending.params) {
            if(p.hooks.audio_packet) {
                NakiAudioPacketEvent event={.packet=pending.packet,.parameters=pending.params,.time_base=pending.tb,
                    .input_stream_index=pending.packet->stream_index,.demuxed_us=pending.demuxed_us,.dispatched_us=now_us()};
                p.stage="audio media hook";
                ret=p.hooks.audio_packet(p.hooks.opaque,&event);
                if(ret>0)ret=AVERROR(EINVAL);
            }
            if(ret>=0&&p.branches)ret=fanout_audio(&p,&pending);
            else if(ret>=0 && job->config.audio_copy && strcmp(job->config.output_format,"mp4"))ret=handle_audio(&p,&pending);
            pipeline_input_free(&pending);
            if(ret<0)goto cleanup;
            progress=1;
        }
        if (input_done && input_error<0) {p.stage=p.sei_read_error?"input SEI parsing / complete-AU contract":"input read"; ret=input_error; goto cleanup;}
        if (!job->config.decoder_pump && (pending.packet || (input_done && !eof_sent))) {
            p.stage="async decode submit";
            if (pending.packet) ((Stamp *)pending.packet->opaque_ref->data)->submitted_us=now_us();
            ret=avcodec_async_send_packet(p.async,pending.packet);
            if (!ret) {
                if (pending.packet) pipeline_input_free(&pending); else eof_sent=1;
                progress=1;
            } else if (ret==AVERROR(EAGAIN)) ret=0;
            else goto cleanup;
        }
        if(p.live_mux) {
            ret=pipeline_flush_deferred_audio(&p);
            if(ret<0 || (ret=pipeline_drain_mux(&p,0))<0)goto cleanup;
        }
        if (!job->config.decoder_pump && now_us()>=p.next_stats_us) {
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
            struct pollfd fd={.fd=job->wake_fd,.events=POLLIN};
            int n;
            do {n=poll(&fd,1,p.branches?20:p.live_mux ? 10 : 1000);} while(n<0 && errno==EINTR && !atomic_load(&job->lifecycle.cancel));
            if (n<0) {ret=AVERROR(errno); goto cleanup;}
        }
    }
    if(!atomic_load(&job->lifecycle.cancel)) {
        if(pipeline_reader_pending_sei(&p))ret=AVERROR_INVALIDDATA;
        if(ret<0) {p.stage="selected SEI has no decoded picture at EOF";goto cleanup;}
    }
    if(p.branches) {
        ret=finish_branches(&p,atomic_load(&job->lifecycle.cancel));branches_joined=1;
        if(ret<0) {p.stage="all outputs failed";goto cleanup;}
    } else if (!atomic_load(&job->lifecycle.cancel))ret=pipeline_drain_output(&p);
cleanup:
    media_phase(&p,NAKI_MEDIA_PHASE_DRAINING);
    {
        int stopped=atomic_load(&job->lifecycle.cancel);
        if(ret<0)atomic_store(&job->lifecycle.cancel,1);
        if(stopped||ret<0) {
            /* Release a full shared reader ring before any private output
             * teardown/join can wait for its bounded I/O cleanup. */
            pipeline_reader_wake(&p);
        }
        if(p.branches&&!branches_joined)finish_branches(&p,1);
        for(int i=0;i<job->config.output_count;i++) {
            Job *b=job->outputs[i];
            if(!atomic_load(&b->lifecycle.done))naki_watermark_owner_finish(b->watermark,ret,stopped);
            naki_watermark_stop(b->watermark);
            if(!atomic_load(&b->lifecycle.done)) {
                pthread_mutex_lock(&b->mutex);b->lifecycle.state=stopped?NAKI_JOB_STATE_STOPPED:NAKI_JOB_STATE_FAILED;b->lifecycle.branch_failed=!stopped;b->lifecycle.ended_us=now_us();
                snprintf(b->lifecycle.error,sizeof(b->lifecycle.error),"shared pipeline terminated at %s",p.stage);pthread_mutex_unlock(&b->mutex);atomic_store(&b->lifecycle.done,1);
            }
        }
        naki_watermark_stop(job->watermark);
        pthread_mutex_lock(&job->mutex);job->flow.policy_closing=1;int policy_failed=job->flow.overload.failed;pthread_mutex_unlock(&job->mutex);
        if(policy_failed) {stopped=0; ret=AVERROR(ETIMEDOUT); p.stage="continuous overload budget exhausted";}
        if(ret<0 && !stopped) {
            char message[AV_ERROR_MAX_STRING_SIZE]; av_strerror(ret,message,sizeof(message));
            yyjson_mut_doc *error_event=new_event("pipeline_error",job);
            json_str(error_event,"stage",p.stage); json_str(error_event,"error",message);
            logger_submit(job->logger,NULL,error_event);
        }
        /* Wake a blocked producer and interrupt network reads before joining. */
        if (ret<0) atomic_store(&job->lifecycle.cancel,1);
        pipeline_reader_wake(&p);
        pipeline_decode_join(&p);
        pipeline_reader_join(&p);
        pipeline_refresh_auto(&p);
        avcodec_async_free(&p.async);
        naki_watermark_owner_finish(job->watermark,ret,stopped);
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
