#include "pipeline_internal.h"

/* One output owner sends/receives encoder data. Ledger entries retain one
 * delivery reference until the corresponding PTS is transferred to a mux
 * packet; missing mappings and exhausted slots remain explicit errors. */
static void mux_trace_free(void *opaque,uint8_t *data) {
    (void)opaque;MuxTrace *meta=(MuxTrace *)data;av_buffer_unref(&meta->owner);av_free(data);
}

void pipeline_ledger_clear(Pipeline *p) {
    for(int i=0;i<LEDGER_CAPACITY;i++) {av_buffer_unref(&p->ledger[i].owner);p->ledger[i].used=0;}
}

int pipeline_encoder_send(Pipeline *p, const AVFrame *frame) {
    int64_t began=now_us(); int ret=avcodec_send_frame(p->encoder,frame);
    p->encoder_api_us+=now_us()-began; return ret;
}

static int encoder_receive(Pipeline *p, AVPacket *packet) {
    int64_t began=now_us(); int ret=avcodec_receive_packet(p->encoder,packet);
    p->encoder_api_us+=now_us()-began; return ret;
}

int pipeline_receive_encoded(Pipeline *p) {
    AVPacket *packet=av_packet_alloc();
    if(!packet)return AVERROR(ENOMEM);
    int ret;
    int cooperative=p->job->config.decoder_pump&&!p->job->parent&&!p->encoder_draining;
    p->encoder_poll_pending=0;p->encoder_polled_packet=0;
    while((ret=encoder_receive(p,packet))>=0) {
        int64_t encoded_us=now_us(), hook_us=0;
        pthread_mutex_lock(&p->job->mutex);p->job->progress.last_encoded_us=encoded_us;pthread_mutex_unlock(&p->job->mutex);
        if(p->job->parent) {pthread_mutex_lock(&p->job->parent->mutex);p->job->parent->progress.last_encoded_us=encoded_us;pthread_mutex_unlock(&p->job->parent->mutex);}
        int slot=-1;
        for(int i=0;i<LEDGER_CAPACITY;i++)if(p->ledger[i].used && p->ledger[i].pts==packet->pts){slot=i;break;}
        if(slot<0){ret=AVERROR_INVALIDDATA;break;}
        if(p->job->config.policy.timestamps==NAKI_TS_SOURCE_ALL && !strcmp(p->job->config.output_format,"flv")) {
            int64_t pts=av_rescale_q(packet->pts,p->encoder->time_base,(AVRational){1,1000});
            int64_t dts=av_rescale_q(packet->dts,p->encoder->time_base,(AVRational){1,1000});
            if(packet->pts==AV_NOPTS_VALUE || packet->dts==AV_NOPTS_VALUE || pts<0 || dts<0 ||
               pts>INT32_MAX || dts>INT32_MAX || pts-dts>8388607 || pts-dts< -8388608) {
                yyjson_mut_doc *event=new_event("strict_timestamp_rejected",p->job);
                json_int(event,"encoder_pts",packet->pts);json_int(event,"encoder_dts",packet->dts);
                json_int(event,"encoder_time_base_num",p->encoder->time_base.num);
                json_int(event,"encoder_time_base_den",p->encoder->time_base.den);
                json_int(event,"pts_ms",pts);json_int(event,"dts_ms",dts);
                json_int(event,"first_encoder_pts",p->first_encoder_pts);
                logger_submit(p->job->logger,NULL,event);
                p->stage="strict FLV timestamp range requires explicit retiming";ret=AVERROR(ERANGE);break;
            }
        }
        Stamp *mapped=&p->ledger[slot].stamp;
        if(p->job->config.encoder_max_future_frames) {
            size_t size=0;const uint8_t *data=av_packet_get_side_data(packet,AV_PKT_DATA_STRINGS_METADATA,&size);
            AVDictionary *info=NULL;
            if(!data||av_packet_unpack_dictionary(data,size,&info)<0) {ret=AVERROR_INVALIDDATA;break;}
            AVDictionaryEntry *entry=av_dict_get(info,"live_future_frames",NULL,0);
            char *end=NULL;long depth=entry?strtol(entry->value,&end,10):-1;
            int valid=entry&&end&&!*end&&depth>=0&&depth<=p->job->config.encoder_max_future_frames;
            if(p->job->config.constrained_crf) {
                AVDictionaryEntry *qp=av_dict_get(info,"live_slice_qp",NULL,0);
                char *qp_end=NULL;long actual=qp?strtol(qp->value,&qp_end,10):100;
                if(!qp||!qp_end||*qp_end||actual < -12||actual>63)valid=0;
                mapped->actual_slice_qp=(int)actual;
            }
            if(p->job->config.codec_trace) {
                NakiEncoderTrace *t=&mapped->encode_trace;
#define PARSE_TRACE(key,dest) do { \
    AVDictionaryEntry *e=av_dict_get(info,key,NULL,0);char *tail=NULL;errno=0; \
    int64_t value=e?strtoll(e->value,&tail,10):-1; \
    if(!e||!tail||*tail||errno||value<0) {valid=0;} dest=value; \
} while(0)
                PARSE_TRACE("vvenc_trace_version",t->version);PARSE_TRACE("vvenc_trace_cts",t->cts);PARSE_TRACE("vvenc_trace_poc",t->poc);
#define GET_TRACE(n) PARSE_TRACE("vvenc_"#n,t->n);
                NAKI_ENCODER_TRACE_FIELDS(GET_TRACE)
#undef GET_TRACE
#undef PARSE_TRACE
                if(t->version!=4||t->cts!=packet->pts||!t->input_us||!t->au_us)valid=0;
            }
            av_dict_free(&info);
            if(!valid){ret=AVERROR_INVALIDDATA;break;}
            mapped->encoder_future_frames=(int)depth;
        }
        size_t picture_size=0; const uint8_t *picture_stats=av_packet_get_side_data(packet,AV_PKT_DATA_QUALITY_STATS,&picture_size);
        int output_type=picture_size>=5 ? picture_stats[4] : AV_PICTURE_TYPE_NONE;
        if(mapped->intra_reason) {
            size_t size=0; const uint8_t *stats=av_packet_get_side_data(packet,AV_PKT_DATA_QUALITY_STATS,&size);
            if(size<5 || stats[4]!=AV_PICTURE_TYPE_I) {p->stage="encoder did not confirm requested I picture"; ret=AVERROR_INVALIDDATA; break;}
            pthread_mutex_lock(&p->job->mutex);p->job->stats.intra_confirmed++;pthread_mutex_unlock(&p->job->mutex);
        }
        if(p->hooks.video_packet) {
            size_t stats_size=0;
            const uint8_t *stats=av_packet_get_side_data(packet,AV_PKT_DATA_QUALITY_STATS,&stats_size);
            Stamp *source=&p->ledger[slot].stamp;
            NakiVideoEncodedEvent event={.codec_id=p->encoder->codec_id,.time_base=p->encoder->time_base,
                .encoded_us=encoded_us,.input_sequence=source->sequence,.input_picture_type=source->picture_type,
                .frame_id=source->frame_id,.source_pts=source->source_pts,.source_time_base=p->input_tb,.generation=source->generation,
                .output_picture_type=stats_size>=5 ? stats[4] : AV_PICTURE_TYPE_NONE,
                .output_key=!!(packet->flags&AV_PKT_FLAG_KEY)};
            int64_t pts=packet->pts,dts=packet->dts,duration=packet->duration;
            int flags=packet->flags,index=packet->stream_index,size=packet->size;
            p->stage="encoded video media hook";
            int64_t began=now_us();
            ret=p->hooks.video_packet(p->hooks.opaque,&event,packet);
            hook_us=now_us()-began;
            if(ret>0)ret=AVERROR(EINVAL);
            if(ret>=0 && (packet->pts!=pts || packet->dts!=dts || packet->duration!=duration ||
                packet->flags!=flags || packet->stream_index!=index || packet->size<=0 || !packet->data ||
                (int64_t)packet->size-size>NAKI_MEDIA_HOOK_MAX_ADDED_BYTES))ret=AVERROR(EINVAL);
            if(ret<0)break;
        }
        p->stage="selected SEI output repackaging";
        ret=naki_sei_inject(packet,p->encoder->codec_id,pipeline_stamp_sei(p->ledger[slot].owner),&p->job->config.sei);
        if(ret<0)break;
        if(p->job->config.constrained_crf) {
            int64_t bits=8*((int64_t)packet->size+(packet->pts==p->first_encoder_pts?p->encoder->extradata_size:0));
            int kind=output_type==AV_PICTURE_TYPE_I?0:output_type==AV_PICTURE_TYPE_P?1:output_type==AV_PICTURE_TYPE_B?2:-1;
            /* Match the actual mux time-base rounding, not a display-PTS
             * forecast. The pending reservation survives until real flush. */
            int64_t output_dts=av_rescale_q(packet->dts,p->encoder->time_base,p->video->time_base);
            int64_t media_us=av_rescale_q(output_dts,p->video->time_base,AV_TIME_BASE_Q);
            if(naki_rate_feedback(&p->rate,&mapped->rate,p->encoder->width*p->encoder->height,bits,kind,media_us)<0) {ret=AVERROR_INVALIDDATA;break;}
        }
        MuxTrace *meta=av_malloc(sizeof(*meta));
        AVBufferRef *reference=meta?av_buffer_create((uint8_t *)meta,sizeof(*meta),mux_trace_free,NULL,0):NULL;
        if(!reference){av_free(meta);ret=AVERROR(ENOMEM);break;}
        *meta=(MuxTrace){.stamp=p->ledger[slot].stamp,.owner=p->ledger[slot].owner,.encoded_us=encoded_us,.hook_us=hook_us,
            .pts_us=av_rescale_q(packet->pts,p->encoder->time_base,AV_TIME_BASE_Q),.bytes=packet->size,.picture_type=output_type};
        p->ledger[slot].owner=NULL;p->ledger[slot].used=0;
        av_buffer_unref(&packet->opaque_ref);packet->opaque_ref=reference;
        av_packet_rescale_ts(packet,p->encoder->time_base,p->video->time_base);packet->stream_index=p->video->index;
        if(!packet->duration)packet->duration=av_rescale_q(1,av_inv_q(p->encoder->framerate),p->video->time_base);
        ret=pipeline_submit_mux(p,packet);av_packet_unref(packet);
        if(ret<0)break;
        /* Pending receive advances admitted encoding work and may spend tens
         * of ms per AU. Give input/audio a turn between coding-order packets. */
        p->encoder_polled_packet=1;
        if(cooperative) {p->encoder_poll_pending=1;break;}
    }
    av_packet_free(&packet);
    return ret==AVERROR(EAGAIN) || ret==AVERROR_EOF ? 0 : ret;
}

int pipeline_encode_frame(Pipeline *p, AVFrame *frame) {
    if (!pipeline_stamp_valid(frame->opaque_ref)) return AVERROR_INVALIDDATA;
    Stamp stamp=*(Stamp *)frame->opaque_ref->data;
    stamp.filtered_us=now_us();
    AVRational filter_tb=av_buffersink_get_time_base(p->sink);
    frame->pts=av_rescale_q(frame->pts,filter_tb,p->encoder->time_base);
    frame->duration=av_rescale_q(frame->duration,filter_tb,p->encoder->time_base);
    if(p->job->config.output_fps)frame->duration=av_rescale_q(1,av_inv_q(p->encoder->framerate),p->encoder->time_base);
    frame->time_base=p->encoder->time_base;
    frame->pict_type=stamp.intra_reason ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    if (p->last_pts!=AV_NOPTS_VALUE && frame->pts<=p->last_pts) {
        p->stage="non-monotonic encoder PTS"; return AVERROR_INVALIDDATA;
    }
    int strict=p->job->config.policy.timestamps==NAKI_TS_SOURCE_ALL;
    int64_t output_display_pts=0;
    if(strict) {
        /* Check the retained display sequence, before encoder B-picture reorder.
         * Packet PTS in encode/write order must not be required to increase. */
        output_display_pts=av_rescale_q(frame->pts,p->encoder->time_base,p->video->time_base);
        if(output_display_pts==AV_NOPTS_VALUE || output_display_pts==INT64_MAX ||
           (p->have_output_display_pts && output_display_pts<=p->last_output_display_pts)) {
            p->stage="strict output PTS precision collision"; return AVERROR_INVALIDDATA;
        }
    }
    if (p->last_pts==AV_NOPTS_VALUE) p->first_encoder_pts=frame->pts;
    int slot=-1;
    for (int i=0;i<LEDGER_CAPACITY;i++) if(!p->ledger[i].used) {slot=i; break;}
    if (slot<0) return AVERROR(ENOBUFS);
    p->stage="encode";
    int ret;
    AVBufferRef *owner=av_buffer_ref(frame->opaque_ref);
    if(!owner)return AVERROR(ENOMEM);
    if(p->job->config.constrained_crf) {
        int64_t began=now_us();
        int byte_input=frame->format==AV_PIX_FMT_YUV420P;
        if(!byte_input&&frame->format!=AV_PIX_FMT_YUV420P10LE) {av_buffer_unref(&owner);return AVERROR_INVALIDDATA;}
        double complexity=naki_rate_complexity_samples(&p->rate,frame->data[0],frame->linesize[0]/(byte_input?1:2),frame->width,frame->height,byte_input);
        int64_t source=p->job->config.source_rate_limit?pipeline_source_rate(p,began):0;
        int64_t pts_us=av_rescale_q(frame->pts,p->encoder->time_base,AV_TIME_BASE_Q);
        int64_t period=av_rescale_q(1,av_inv_q(p->encoder->framerate),AV_TIME_BASE_Q);
        if(naki_rate_select(&p->rate,p->job->config.qp,p->job->config.maxrate,p->job->config.source_rate_limit,
            source,began,pts_us,period,frame->width*frame->height,!!stamp.intra_reason,complexity,&stamp.rate)<0) {
            av_buffer_unref(&owner);return AVERROR_INVALIDDATA;
        }
        if(av_dict_set_int(&frame->metadata,"naki_qp_offset",stamp.rate.qp-p->job->config.qp,0)<0) {av_buffer_unref(&owner);return AVERROR(ENOMEM);}
    }
    stamp.encoder_submit_us=now_us();
    pipeline_checkpoint(p,&p->job->progress.first_encoder_submit_us);
    while ((ret=pipeline_encoder_send(p,frame))==AVERROR(EAGAIN)) {
        if ((ret=pipeline_receive_encoded(p))<0) {av_buffer_unref(&owner);return ret;}
    }
    if (ret<0) {av_buffer_unref(&owner);return ret;}
    stamp.encoder_accepted_us=now_us();
    if(strict) {p->last_output_display_pts=output_display_pts;p->have_output_display_pts=1;}
    naki_intra_accept(&p->job->config.policy,&p->intra,stamp.source_us,stamp.intra_reason);
    if(stamp.intra_reason) {pthread_mutex_lock(&p->job->mutex);p->job->stats.intra_requested++;pthread_mutex_unlock(&p->job->mutex);}
    p->ledger[slot]=(Ledger){.used=1,.pts=frame->pts,.stamp=stamp,.owner=owner}; p->last_pts=frame->pts;
    pthread_mutex_lock(&p->job->mutex); p->job->stats.frames_filtered++; pthread_mutex_unlock(&p->job->mutex);
    return pipeline_receive_encoded(p);
}

int pipeline_ledger_pending(Pipeline *p) {
    for(int i=0;i<LEDGER_CAPACITY;i++)if(p->ledger[i].used)return 1;
    return 0;
}
