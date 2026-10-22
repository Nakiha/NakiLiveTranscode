#include "media/pipeline_internal.h"
#include <assert.h>
#include <sys/resource.h>
#include <sys/wait.h>

static void *notifier(void *opaque) {
    NakiWakeup *w=opaque;
    for(int i=0;i<10000;i++)naki_wakeup_notify(w);
    return NULL;
}
static void wakeups(void) {
    NakiWakeup w;assert(!naki_wakeup_init(&w,1));
    assert(fcntl(w.fd,F_GETFL)&O_NONBLOCK);
    assert(fcntl(w.fd,F_GETFD)&FD_CLOEXEC);
    struct pollfd p={.fd=w.fd,.events=POLLIN};
    assert(!naki_wakeup_clear(&w)&&poll(&p,1,0)==0);
    /* Force the exact notification-before-wait window that cond_signal lost. */
    naki_wakeup_notify(&w);
    assert(poll(&p,1,0)==1&&p.revents&POLLIN);
    assert(!naki_wakeup_wait(&w,1000)&&!naki_wakeup_clear(&w));
    assert(poll(&p,1,0)==0);
    pthread_t producers[4];
    for(int i=0;i<4;i++)assert(!pthread_create(&producers[i],NULL,notifier,&w));
    for(int i=0;i<4;i++)assert(!pthread_join(producers[i],NULL));
    uint64_t count=0;assert(read(w.fd,&count,sizeof(count))==sizeof(count)&&count==40000);
    /* Counter saturation is already-readable backpressure, never a wait. */
    count=UINT64_MAX-1;assert(write(w.fd,&count,sizeof(count))==sizeof(count));
    naki_wakeup_notify(&w);assert(!atomic_load(&w.error));
    assert(!naki_wakeup_wait(&w,0)&&!naki_wakeup_clear(&w));
    naki_wakeup_destroy(&w);assert(w.fd==-1);
    assert(!naki_wakeup_init(&w,0));naki_wakeup_notify(&w);naki_wakeup_destroy(&w);
    /* Failed creation stays safe for the pipeline's ordinary cleanup. */
    pid_t child=fork();assert(child>=0);
    if(!child) {
        struct rlimit limit={0,0};assert(!setrlimit(RLIMIT_NOFILE,&limit));
        assert(naki_wakeup_init(&w,1)==-EMFILE);naki_wakeup_destroy(&w);_exit(0);
    }
    int status;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));
}

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
    wakeups();
    Pipeline *p=calloc(1,sizeof(*p));assert(p);
    DecodePump *q=&p->pump;pthread_mutex_init(&q->mutex,NULL);assert(!naki_wakeup_init(&q->wake,1));
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
    naki_wakeup_destroy(&q->wake);pthread_mutex_destroy(&q->mutex);free(p);
    return 0;
}
