#include "input_internal.h"

int pipeline_decode_receive(Pipeline *p,AVFrame *frame,InputItem *audio) {
    DecodePump *q=&p->in->pump;int ret=AVERROR(EAGAIN);
    pthread_mutex_lock(&q->mutex);
    /* Alternate when both lanes are ready: neither late audio nor video may
     * starve the other. All stream registration/mux work stays with this owner. */
    if(q->audio.packet&&(!q->occupied||!q->prefer_video)) {
        *audio=q->audio;q->audio=(InputItem){0};q->prefer_video=1;
        naki_wakeup_notify(&q->wake);
    } else if(q->occupied) {
        av_frame_move_ref(frame,q->frame);ret=0;q->occupied=0;q->prefer_video=0;
        naki_wakeup_notify(&q->wake);
    } else if(q->done)ret=q->error<0?q->error:AVERROR_EOF;
    pthread_mutex_unlock(&q->mutex);return ret;
}
