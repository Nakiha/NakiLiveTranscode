#include "pipeline_internal.h"
#include <time.h>

/* Optional direct-output pump: this thread alone calls async send/receive and
 * stats while active. One dispatch slot, existing bounded async queues and one
 * retained input item. Audio parameters are immutable reader snapshots. */
void pipeline_decode_notify(void *opaque) {
    Pipeline *p=opaque;
    wake_job(p->job);
    pthread_cond_signal(&p->pump.changed);
}
void pipeline_decode_init(Pipeline *p) {
    pthread_mutex_init(&p->pump.mutex,NULL);
    pthread_condattr_t attr;pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr,CLOCK_MONOTONIC);
    pthread_cond_init(&p->pump.changed,&attr);pthread_condattr_destroy(&attr);
}
static void decoded_stamp_free(void *opaque,uint8_t *data) {
    AVBufferRef *origin=opaque;
    av_buffer_unref(&origin);av_free(data);
}
static void *decode_main(void *opaque) {
    (void)crash_thread_init();Pipeline *p=opaque;DecodePump *q=&p->pump;
    AVFrame *frame=av_frame_alloc();InputItem pending={0};
    int ret=frame?0:AVERROR(ENOMEM),eof_sent=0;int64_t next_stats=0;
    while(ret>=0&&!atomic_load(&p->job->lifecycle.cancel)) {
        int progress=0,available;
        pthread_mutex_lock(&q->mutex);available=!q->occupied;pthread_mutex_unlock(&q->mutex);
        if(available) {
            ret=avcodec_async_receive_frame(p->async,frame);
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
                    naki_sei_count(pipeline_stamp_sei(frame->opaque_ref),&p->job->config.sei)>0;
                if(p->job->config.policy.intra==NAKI_INTRA_SOURCE&&!p->job->config.policy.overload_levels&&
                   !naki_video_rate_keep((uint64_t)stamp->frame_id,p->rate_divisor,protect)) {
                    pthread_mutex_lock(&p->job->mutex);
                    p->job->stats.frames_decoded++;p->job->stats.frames_dropped_rate++;
                    if(p->job->stats.frame_id_last<(uint64_t)stamp->frame_id)p->job->stats.frame_id_last=(uint64_t)stamp->frame_id;
                    p->job->stats.timestamp_wraps=stamp->timestamp_wraps;
                    pthread_mutex_unlock(&p->job->mutex);
                    if(p->job->config.frame_log&&(stamp->frame_id-1)%p->job->config.frame_log_every==0) {
                        yyjson_mut_doc *event=new_event("frame_drop",p->job);
                        json_int(event,"frame_id",stamp->frame_id);json_int(event,"input_sequence",stamp->sequence);
                        json_int(event,"source_pts",stamp->source_pts);json_int(event,"overload_level",0);
                        json_str(event,"reason","output_rate");logger_submit(p->job->logger,p->job->trace,event);
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
            if(!q->occupied) {q->audio=pending;pending=(InputItem){0};q->occupied=1;progress=1;}
            pthread_mutex_unlock(&q->mutex);
            if(progress)wake_job(p->job);
        } else if(pending.packet||(done&&!eof_sent)) {
            if(pending.packet)((Stamp *)pending.packet->opaque_ref->data)->submitted_us=now_us();
            ret=avcodec_async_send_packet(p->async,pending.packet);
            if(!ret) {if(pending.packet)pipeline_input_free(&pending);else eof_sent=1;progress=1;}
            else if(ret==AVERROR(EAGAIN))ret=0;
            else break;
        }
        if(now_us()>=next_stats){pipeline_refresh_auto(p);next_stats=now_us()+1000000;}
        if(!progress) {
            struct timespec until;clock_gettime(CLOCK_MONOTONIC,&until);
            until.tv_nsec+=2000000;if(until.tv_nsec>=1000000000){until.tv_nsec-=1000000000;until.tv_sec++;}
            pthread_mutex_lock(&q->mutex);
            /* Callback notifications can race the wait; a 2ms monotonic poll
             * bounds a missed notification without sharing the owner's fd. */
            if(!atomic_load(&p->job->lifecycle.cancel))pthread_cond_timedwait(&q->changed,&q->mutex,&until);
            pthread_mutex_unlock(&q->mutex);
        }
    }
    pipeline_input_free(&pending);av_frame_free(&frame);pipeline_refresh_auto(p);
    pthread_mutex_lock(&q->mutex);q->done=1;q->error=ret;pthread_mutex_unlock(&q->mutex);
    wake_job(p->job);return NULL;
}
int pipeline_decode_start(Pipeline *p) {
    int rate=naki_video_rate_output(&p->video_rate,p->job->config.output_fps,&p->output_rate,&p->rate_divisor);
    if(rate<0)return rate;
    p->pump.frame=av_frame_alloc();if(!p->pump.frame)return AVERROR(ENOMEM);
    int ret=pthread_create(&p->pump.thread,NULL,decode_main,p);
    if(ret)return AVERROR(ret);
    p->pump.started=1;return 0;
}
int pipeline_decode_receive(Pipeline *p,AVFrame *frame,InputItem *audio) {
    DecodePump *q=&p->pump;int ret=AVERROR(EAGAIN);
    pthread_mutex_lock(&q->mutex);
    if(q->occupied) {
        if(q->audio.packet) {*audio=q->audio;q->audio=(InputItem){0};}
        else {av_frame_move_ref(frame,q->frame);ret=0;}
        q->occupied=0;pthread_cond_signal(&q->changed);
    } else if(q->done)ret=q->error<0?q->error:AVERROR_EOF;
    pthread_mutex_unlock(&q->mutex);return ret;
}
void pipeline_decode_join(Pipeline *p) {
    if(p->pump.started) {pthread_cond_signal(&p->pump.changed);pthread_join(p->pump.thread,NULL);p->pump.started=0;}
}
void pipeline_decode_destroy(Pipeline *p) {
    if(p->pump.frame&&p->pump.frame->opaque_ref) {
        Stamp *s=(Stamp *)p->pump.frame->opaque_ref->data;
        if(s->decoder_tracked)naki_policy_untrack(p->job,NAKI_TRACK_FRAME,(uint64_t)s->frame_id);
    }
    av_frame_free(&p->pump.frame);pipeline_input_free(&p->pump.audio);
    pthread_cond_destroy(&p->pump.changed);pthread_mutex_destroy(&p->pump.mutex);
}
