#include "media/pipeline_internal.h"
#include <assert.h>

static void video(DecodePump *q,int64_t pts) {
    assert(!q->occupied&&!q->frame->buf[0]);
    q->frame->format=AV_PIX_FMT_GRAY8;q->frame->width=q->frame->height=16;
    q->frame->pts=pts;assert(av_frame_get_buffer(q->frame,0)>=0);q->occupied=1;
}
static void audio(DecodePump *q,int64_t pts) {
    assert(!q->audio.packet);q->audio.packet=av_packet_alloc();assert(q->audio.packet);
    assert(av_new_packet(q->audio.packet,1)>=0);q->audio.packet->pts=pts;
}
int main(void) {
    Pipeline *p=calloc(1,sizeof(*p));assert(p);
    DecodePump *q=&p->pump;pthread_mutex_init(&q->mutex,NULL);pthread_cond_init(&q->changed,NULL);
    q->frame=av_frame_alloc();AVFrame *out=av_frame_alloc();assert(q->frame&&out);
    InputItem item={0};
    /* Audio doesn't occupy the video slot. Replenishing audio cannot starve an
     * already queued picture; references move exactly once. */
    audio(q,10);assert(!q->occupied);video(q,20);
    assert(pipeline_decode_receive(p,out,&item)==AVERROR(EAGAIN));
    assert(item.packet&&item.packet->pts==10&&q->occupied&&!out->buf[0]);
    av_packet_free(&item.packet);audio(q,30);
    assert(pipeline_decode_receive(p,out,&item)==0);
    assert(out->pts==20&&out->buf[0]&&!q->frame->buf[0]&&!q->occupied&&q->audio.packet);
    av_frame_unref(out);q->done=1;
    assert(pipeline_decode_receive(p,out,&item)==AVERROR(EAGAIN));
    assert(item.packet&&item.packet->pts==30);av_packet_free(&item.packet);
    assert(pipeline_decode_receive(p,out,&item)==AVERROR_EOF);
    /* Error/EOF never jumps ahead of either bounded lane. */
    q->error=AVERROR(EIO);audio(q,40);video(q,50);q->prefer_video=0;
    assert(pipeline_decode_receive(p,out,&item)==AVERROR(EAGAIN));av_packet_free(&item.packet);
    assert(pipeline_decode_receive(p,out,&item)==0);assert(out->pts==50);av_frame_unref(out);
    assert(pipeline_decode_receive(p,out,&item)==AVERROR(EIO));
    q->done=0;q->error=0;
    assert(pipeline_decode_receive(p,out,&item)==AVERROR(EAGAIN));
    av_frame_free(&q->frame);av_frame_free(&out);
    pthread_cond_destroy(&q->changed);pthread_mutex_destroy(&q->mutex);free(p);
    return 0;
}
