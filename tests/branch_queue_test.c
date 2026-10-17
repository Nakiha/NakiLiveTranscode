#include "branch_queue.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>
#include <libavutil/time.h>
static atomic_int released,created;
static void released_buffer(void *opaque,uint8_t *data) {(void)opaque;av_free(data);atomic_fetch_add(&released,1);}
static NakiBranchItem frame(int64_t pts) {
    NakiBranchItem i={.source_us=pts,.ingress_us=100,.sequence=pts,.time_base={1,1000000},.media_us=40};
    i.frame=av_frame_alloc();assert(i.frame);i.frame->format=AV_PIX_FMT_YUV420P;i.frame->width=32;i.frame->height=24;
    assert(av_frame_get_buffer(i.frame,32)>=0);memset(i.frame->data[0],16,i.frame->linesize[0]*24);
    i.frame->pts=pts;i.frame->opaque_ref=av_buffer_create(av_malloc(8),8,released_buffer,NULL,0);assert(i.frame->opaque_ref);
    atomic_fetch_add(&created,1);assert(!naki_branch_item_charge(&i));return i;
}
static void *consumer(void *opaque) {
    NakiBranchItem i={0};int ret=naki_branch_queue_pop(opaque,&i,av_gettime_relative()+2000000);
    assert(ret==AVERROR(ECANCELED));return NULL;
}
int main(void) {
    NakiBranchQueue q;assert(!naki_branch_queue_init(&q,2,1000000,100,1000));
    NakiBranchItem a=frame(0),b=frame(40),c=frame(80),item={0};
    AVFrame *sibling=av_frame_clone(a.frame);assert(sibling);
    uint64_t bytes=a.charged_bytes;
    assert(!naki_branch_queue_push(&q,&a,100)&&!a.frame);
    assert(!naki_branch_queue_push(&q,&b,100));
    assert(naki_branch_queue_push(&q,&c,100)==AVERROR(EAGAIN)&&c.frame);
    assert(q.count==2&&q.bytes==2*bytes&&q.high_water==2);
    assert(!naki_branch_queue_pop(&q,&item,0)&&item.source_us==0&&item.frame->pts==0);
    assert(!av_frame_make_writable(item.frame));item.frame->data[0][0]=99;
    assert(sibling->data[0][0]==16);naki_branch_item_free(&item);assert(!atomic_load(&released));
    av_frame_free(&sibling);assert(atomic_load(&released)==1);
    assert(!naki_branch_queue_push(&q,&c,100));
    assert(naki_branch_queue_check(&q,1101)==AVERROR(ETIMEDOUT));
    naki_branch_queue_finish(&q);NakiBranchItem d=frame(90);
    assert(naki_branch_queue_push(&q,&d,100)==AVERROR_EOF&&d.frame);naki_branch_item_free(&d);
    assert(!naki_branch_queue_pop(&q,&item,0)&&item.source_us==40);naki_branch_item_free(&item);
    assert(!naki_branch_queue_pop(&q,&item,0)&&item.source_us==80);naki_branch_item_free(&item);
    assert(naki_branch_queue_pop(&q,&item,0)==AVERROR_EOF);naki_branch_queue_destroy(&q);
    assert(!naki_branch_queue_init(&q,2,bytes-1,100,1000));a=frame(0);
    assert(naki_branch_queue_push(&q,&a,100)==AVERROR(EFBIG)&&a.frame);naki_branch_item_free(&a);naki_branch_queue_destroy(&q);
    assert(!naki_branch_queue_init(&q,2,1000000,100,1000));a=frame(INT64_MIN+1);b=frame(INT64_MAX);
    assert(!naki_branch_queue_push(&q,&a,100));
    assert(!naki_branch_queue_push(&q,&b,100)); /* Legal PTS gap is not queued media duration. */
    naki_branch_queue_abort(&q);assert(!q.count&&!q.bytes);naki_branch_queue_destroy(&q);
    assert(!naki_branch_queue_init(&q,2,1000000,100,1000));a=frame(0);b=frame(40);q.byte_limit=a.charged_bytes+b.charged_bytes-1;
    assert(!naki_branch_queue_push(&q,&a,100));assert(naki_branch_queue_push(&q,&b,100)==AVERROR(EAGAIN));
    naki_branch_item_free(&b);naki_branch_queue_abort(&q);naki_branch_queue_destroy(&q);
    assert(!naki_branch_queue_init(&q,2,1000000,60,1000));a=frame(0);b=frame(40);
    assert(!naki_branch_queue_push(&q,&a,100));assert(naki_branch_queue_push(&q,&b,100)==AVERROR(EAGAIN)&&b.frame);
    b.media_us=61;assert(naki_branch_queue_push(&q,&b,100)==AVERROR(EFBIG));
    naki_branch_item_free(&b);naki_branch_queue_destroy(&q);
    assert(!naki_branch_queue_init(&q,2,1000000,100,1000));
    assert(naki_branch_queue_pop(&q,&item,av_gettime_relative()+1000)==AVERROR(ETIMEDOUT));
    pthread_t thread;assert(!pthread_create(&thread,NULL,consumer,&q));naki_branch_queue_abort(&q);assert(!pthread_join(thread,NULL));naki_branch_queue_destroy(&q);
    /* Immutable audio parameters are retained independently of the demuxer. */
    assert(!naki_branch_queue_init(&q,2,1000000,100,1000));
    AVCodecParameters *original=avcodec_parameters_alloc();assert(original);original->codec_id=AV_CODEC_ID_AAC;
    a=(NakiBranchItem){.packet=av_packet_alloc(),.parameters=avcodec_parameters_alloc(),.source_us=AV_NOPTS_VALUE,.ingress_us=100,.media_us=40};
    assert(a.packet&&a.parameters&&!av_new_packet(a.packet,100)&&!avcodec_parameters_copy(a.parameters,original));
    original->codec_id=AV_CODEC_ID_MP3;assert(!naki_branch_queue_push(&q,&a,100));avcodec_parameters_free(&original);
    assert(!naki_branch_queue_pop(&q,&item,0)&&item.parameters->codec_id==AV_CODEC_ID_AAC);naki_branch_item_free(&item);naki_branch_queue_destroy(&q);
    /* A default TS audio PES can yield 16 consecutive audio packets. One
     * retained video plus that burst must still obey the combined 16-item
     * bound, independently of how quickly a consumer happens to be scheduled. */
    assert(!naki_branch_queue_init(&q,NAKI_BRANCH_QUEUE_MAX,1000000,1000000,1000));a=frame(0);
    assert(!naki_branch_queue_push(&q,&a,100));
    for(int n=0;n<NAKI_BRANCH_QUEUE_MAX;n++) {
        a=(NakiBranchItem){.packet=av_packet_alloc(),.parameters=avcodec_parameters_alloc(),.ingress_us=100,.media_us=40};
        assert(a.packet&&a.parameters&&!av_new_packet(a.packet,100));a.packet->pts=n;a.parameters->codec_id=AV_CODEC_ID_AAC;
        int pushed=naki_branch_queue_push(&q,&a,100);
        if(n<NAKI_BRANCH_QUEUE_MAX-1)assert(!pushed&&!a.packet&&!a.parameters);
        else {assert(pushed==AVERROR(EAGAIN)&&a.packet&&a.parameters);naki_branch_item_free(&a);}
    }
    assert(q.count==NAKI_BRANCH_QUEUE_MAX&&q.high_water==NAKI_BRANCH_QUEUE_MAX);
    assert(!naki_branch_queue_pop(&q,&item,0)&&item.frame);naki_branch_item_free(&item);
    for(int n=0;n<NAKI_BRANCH_QUEUE_MAX-1;n++) {
        assert(!naki_branch_queue_pop(&q,&item,0)&&item.packet&&item.packet->pts==n);naki_branch_item_free(&item);
    }
    assert(!q.count&&!q.bytes&&!q.media_us);naki_branch_queue_destroy(&q);
    NakiBranchItem custom[2];uint64_t baseline;
    for(int k=0;k<2;k++) {
        custom[k]=(NakiBranchItem){.packet=av_packet_alloc(),.parameters=avcodec_parameters_alloc(),.ingress_us=100,.media_us=40};
        assert(custom[k].packet&&custom[k].parameters&&!av_new_packet(custom[k].packet,100));
        assert(!naki_branch_item_charge(&custom[k]));baseline=custom[k].charged_bytes;
        original=avcodec_parameters_alloc();assert(original);original->ch_layout.order=AV_CHANNEL_ORDER_CUSTOM;original->ch_layout.nb_channels=8;
        original->ch_layout.u.map=av_calloc(8,sizeof(AVChannelCustom));assert(original->ch_layout.u.map);
        for(int ch=0;ch<8;ch++) {original->ch_layout.u.map[ch].id=AV_CHAN_UNKNOWN;snprintf(original->ch_layout.u.map[ch].name,sizeof(original->ch_layout.u.map[ch].name),"test%d",ch);}
        assert(!avcodec_parameters_copy(custom[k].parameters,original));
        assert(custom[k].parameters->ch_layout.u.map!=original->ch_layout.u.map);avcodec_parameters_free(&original);
        assert(!strcmp(custom[k].parameters->ch_layout.u.map[7].name,"test7"));
        assert(!naki_branch_item_charge(&custom[k])&&custom[k].charged_bytes==baseline+8*sizeof(AVChannelCustom));
    }
    assert(!naki_branch_queue_init(&q,2,custom[0].charged_bytes-1,100,1000));
    assert(naki_branch_queue_push(&q,&custom[0],100)==AVERROR(EFBIG)&&custom[0].parameters->ch_layout.u.map);naki_branch_queue_destroy(&q);
    assert(!naki_branch_queue_init(&q,2,2*custom[0].charged_bytes-1,100,1000));
    assert(!naki_branch_queue_push(&q,&custom[0],100));assert(naki_branch_queue_push(&q,&custom[1],100)==AVERROR(EAGAIN)&&custom[1].packet);
    naki_branch_item_free(&custom[1]);naki_branch_queue_destroy(&q);
    assert(atomic_load(&released)==atomic_load(&created));puts("branch queue: ownership, COW, count/byte/media/lag bounds, legal PTS gaps, EOF, timed wait and cancellation passed");return 0;
}
