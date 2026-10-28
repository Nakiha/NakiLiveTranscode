#include "input_internal.h"

/* Optional direct-output pump: this thread alone calls async send/receive and
 * stats while active. One video slot and one immutable audio/control slot,
 * existing bounded async queues and one retained input item. Audio must not
 * occupy the video slot and prevent receiving already completed pictures. */
void pipeline_decode_notify(void *opaque) {
    Pipeline *p=opaque;
    wake_job(p->job);
    naki_wakeup_notify(&p->in->pump.wake);
}
int pipeline_decode_init(Pipeline *p) {
    pthread_mutex_init(&p->in->pump.mutex,NULL);
    return naki_wakeup_init(&p->in->pump.wake,naki_job_config(p->job)->decoder_pump);
}
static void decoded_stamp_free(void *opaque,uint8_t *data) {
    AVBufferRef *origin=opaque;
    av_buffer_unref(&origin);av_free(data);
}
static void *decode_main(void *opaque) {
    (void)crash_thread_init();Pipeline *p=opaque;DecodePump *q=&p->in->pump;
    AVFrame *frame=av_frame_alloc();InputItem pending={0};
    int ret=frame?0:AVERROR(ENOMEM),eof_sent=0;int64_t next_stats=0;
    while(ret>=0&&!naki_job_cancelled(p->job)) {
        ret=naki_wakeup_clear(&q->wake);if(ret<0)break;
        int progress=0,available;
        pthread_mutex_lock(&q->mutex);available=!q->occupied;pthread_mutex_unlock(&q->mutex);
        if(available) {
            ret=avcodec_async_receive_frame(p->in->async,frame);
            if(ret>=0)p->in->decoder_receive_us=now_us();
            if(ret==AVERROR_EOF){ret=0;break;}
            if(ret>=0) {
                if(!pipeline_stamp_valid(frame->opaque_ref)){ret=AVERROR_INVALIDDATA;break;}
                /* Keep the original stamp's tracking destructor alive until
                 * the output owner takes over. A generic writable copy could
                 * otherwise untrack the packet while its decoded slot waits. */
                AVBufferRef *origin=frame->opaque_ref;
                uint8_t *data=av_memdup(origin->data,origin->size);
                AVBufferRef *copy=data?av_buffer_create(data,origin->size,decoded_stamp_free,origin,0):NULL;
                if(!copy){av_free(data);ret=AVERROR(ENOMEM);break;}
                frame->opaque_ref=copy;
                ((Stamp *)frame->opaque_ref->data)->async_ready_us=now_us();
                ret=pipeline_prepare_decoded_frame(p,frame);if(ret<0)break;
                Stamp *stamp=(Stamp *)frame->opaque_ref->data;
                int protect=stamp->picture_type==AV_PICTURE_TYPE_I||stamp->frame_id==1||
                    naki_sei_count(pipeline_stamp_sei(frame->opaque_ref),&naki_job_config(p->job)->sei)>0;
                if(!naki_job_config(p->job)->rendition_backend&&naki_job_config(p->job)->policy.intra==NAKI_INTRA_SOURCE&&!naki_job_config(p->job)->policy.overload_levels&&
                   !naki_video_rate_keep((uint64_t)stamp->frame_id,p->rate_divisor,protect)) {
                    naki_job_rate_dropped(p->job,stamp);
                    if(naki_job_config(p->job)->frame_log&&(stamp->frame_id-1)%naki_job_config(p->job)->frame_log_every==0) {
                        yyjson_mut_doc *event=new_event("frame_drop",p->job);
                        json_int(event,"frame_id",stamp->frame_id);json_int(event,"input_sequence",stamp->sequence);
                        json_int(event,"source_pts",stamp->source_pts);json_int(event,"overload_level",0);
                        json_str(event,"reason","output_rate");logger_submit(naki_job_logger(p->job),naki_job_trace(p->job),event);
                    }
                    av_frame_unref(frame);progress=1;
                    /* Continue feeding below; only retained pictures occupy
                     * the dispatch slot. No overload/SEI/I protection bypass. */
                } else {
                ret=naki_policy_track(p->job,NAKI_TRACK_FRAME,(uint64_t)stamp->frame_id,stamp->ingress_us);
                if(ret<0)break;
                stamp->decoder_tracked=1;
                pthread_mutex_lock(&q->mutex);av_frame_move_ref(q->frame,frame);q->occupied=1;pthread_mutex_unlock(&q->mutex);
                wake_job(p->job);progress=1;
                }
            } else if(ret==AVERROR(EAGAIN))ret=0;
            else break;
        }
        int done=0,error=0;
        if(!pending.packet&&!eof_sent)pending=pipeline_reader_pop(p,&done,&error);
        if(done&&error<0){ret=error;break;}
        if(pending.params) {
            pthread_mutex_lock(&q->mutex);
            if(!q->audio.packet) {q->audio=pending;pending=(InputItem){0};progress=1;}
            pthread_mutex_unlock(&q->mutex);
            if(progress)wake_job(p->job);
        } else if(pending.packet||(done&&!eof_sent)) {
            if(pending.packet) {
                Stamp *stamp=(Stamp *)pending.packet->opaque_ref->data;
                stamp->submitted_us=now_us();
                if(!stamp->submit_first_us)stamp->submit_first_us=stamp->submitted_us;
            }
            ret=avcodec_async_send_packet(p->in->async,pending.packet);
            if(!ret) {if(pending.packet)pipeline_input_free(&pending);else eof_sent=1;progress=1;}
            else if(ret==AVERROR(EAGAIN))ret=0;
            else break;
        }
        if(now_us()>=next_stats){pipeline_refresh_auto(p);next_stats=now_us()+1000000;}
        if(!progress) {
            /* A private eventfd preserves notifications racing this wait.
             * The timeout is only for the periodic stats snapshot; decoding,
             * slot release, reader progress and join all explicitly notify. */
            if(!naki_job_cancelled(p->job))ret=naki_wakeup_wait(&q->wake,1000);
        }
    }
    pipeline_input_free(&pending);av_frame_free(&frame);pipeline_refresh_auto(p);
    pthread_mutex_lock(&q->mutex);q->done=1;q->error=ret;pthread_mutex_unlock(&q->mutex);
    wake_job(p->job);return NULL;
}
int pipeline_decode_start(Pipeline *p) {
    int rate=naki_video_rate_output(&p->video_rate,naki_job_config(p->job)->output_fps,&p->output_rate,&p->rate_divisor);
    if(rate<0)return rate;
    p->in->pump.frame=av_frame_alloc();if(!p->in->pump.frame)return AVERROR(ENOMEM);
    int ret=pthread_create(&p->in->pump.thread,NULL,decode_main,p);
    if(ret)return AVERROR(ret);
    p->in->pump.started=1;return 0;
}
void pipeline_decode_join(Pipeline *p) {
    if(p->in->pump.started) {naki_wakeup_notify(&p->in->pump.wake);pthread_join(p->in->pump.thread,NULL);p->in->pump.started=0;}
}
void pipeline_decode_destroy(Pipeline *p) {
    if(p->in->pump.frame&&p->in->pump.frame->opaque_ref) {
        Stamp *s=(Stamp *)p->in->pump.frame->opaque_ref->data;
        if(s->decoder_tracked)naki_policy_untrack(p->job,NAKI_TRACK_FRAME,(uint64_t)s->frame_id);
    }
    av_frame_free(&p->in->pump.frame);pipeline_input_free(&p->in->pump.audio);
    naki_wakeup_destroy(&p->in->pump.wake);pthread_mutex_destroy(&p->in->pump.mutex);
}
