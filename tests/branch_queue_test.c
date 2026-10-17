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
static NakiBranchItem audio(int64_t pts,int track) {
    NakiBranchItem i={.packet=av_packet_alloc(),.parameters=avcodec_parameters_alloc(),.source_us=pts,.ingress_us=100,.media_us=40};
    assert(i.packet&&i.parameters&&!av_new_packet(i.packet,100));i.packet->pts=pts;i.packet->stream_index=track;
    i.parameters->codec_id=AV_CODEC_ID_AAC;i.parameters->sample_rate=48000;av_channel_layout_default(&i.parameters->ch_layout,2);
    i.packet->opaque_ref=av_buffer_create(av_malloc(8),8,released_buffer,NULL,0);assert(i.packet->opaque_ref);atomic_fetch_add(&created,1);
    assert(!naki_branch_item_charge(&i));return i;
}
typedef struct RecoveryConsumer {NakiBranchQueue *queue;unsigned before_drain;} RecoveryConsumer;
static void *recover_consumer(void *opaque) {
    RecoveryConsumer *consumer=opaque;av_usleep(10000);
    pthread_mutex_lock(&consumer->queue->mutex);consumer->before_drain=consumer->queue->count;pthread_mutex_unlock(&consumer->queue->mutex);
    for(int n=0;n<2;n++) {NakiBranchItem item={0};assert(!naki_branch_queue_pop(consumer->queue,&item,0));naki_branch_item_free(&item);}
    return NULL;
}
static void scheduler_recovery_test(void) {
    NakiBranchQueue queue;assert(!naki_branch_queue_init_limits(&queue,4,4,1000000,1000000,10000000));
    int64_t now=av_gettime_relative();
    for(int n=0;n<4;n++) {NakiBranchItem item=frame(n);item.ingress_us=now;assert(!naki_branch_queue_push(&queue,&item,now));}
    NakiBranchItem pending=frame(9);pending.ingress_us=now;assert(naki_branch_queue_admit(&queue,&pending,now,1000000)==AVERROR(EAGAIN));
    NakiBranchItem item={0};assert(!naki_branch_queue_pop(&queue,&item,0));naki_branch_item_free(&item);
    assert(queue.count==3&&queue.admission_deadline_us); /* One slot, not recovery. */
    RecoveryConsumer consumer={&queue,0};pthread_t thread;assert(!pthread_create(&thread,NULL,recover_consumer,&consumer));
    NakiBranchOffer offer={.queue=&queue,.item=&pending,.result=AVERROR(EAGAIN)};
    assert(!naki_branch_admit_many(&offer,1,1000000,NULL,NULL));assert(!pthread_join(thread,NULL));
    assert(consumer.before_drain==3&&!offer.result&&!pending.frame&&!queue.admission_deadline_us&&queue.count==2);
    naki_branch_queue_destroy(&queue);
}
static void mixed_quota_tests(void) {
    /* Owners are deliberately not scheduled until the full burst is retained.
     * All outputs accept one video plus16 audio, in one common ordered FIFO. */
    for(int outputs=2;outputs<=4;outputs+=2) {
        NakiBranchQueue queues[4];NakiBranchItem item={0};
        for(int b=0;b<outputs;b++) {
            assert(!naki_branch_queue_init_limits(&queues[b],1,16,1000000,1000000,10000000));
            item=frame(0);assert(!naki_branch_queue_push(&queues[b],&item,100));
        }
        for(int n=0;n<16;n++)for(int b=0;b<outputs;b++) {item=audio(n,n%4);assert(!naki_branch_queue_push(&queues[b],&item,100));}
        for(int b=0;b<outputs;b++) {
            NakiBranchQueue *q=&queues[b];assert(q->count==17&&q->video_count==1&&q->audio_count==16);
            item=frame(1);assert(naki_branch_queue_push(q,&item,100)==AVERROR(EAGAIN)&&q->rejection_reason==NAKI_BRANCH_VIDEO_COUNT);naki_branch_item_free(&item);
            item=audio(16,0);assert(naki_branch_queue_push(q,&item,100)==AVERROR(EAGAIN)&&q->rejection_reason==NAKI_BRANCH_AUDIO_COUNT);naki_branch_item_free(&item);
            assert(!naki_branch_queue_pop(q,&item,0)&&item.frame);naki_branch_item_free(&item);
            for(int n=0;n<16;n++) {assert(!naki_branch_queue_pop(q,&item,0)&&item.packet&&item.packet->pts==n&&item.packet->stream_index==n%4);naki_branch_item_free(&item);}
            assert(!q->count&&!q->bytes&&!q->media_us&&!q->video_count&&!q->audio_count&&q->high_water==17);naki_branch_queue_destroy(q);
        }
    }
    NakiBranchQueue q;assert(!naki_branch_queue_init_limits(&q,16,256,1000000,1000000,10000000));
    for(int n=0;n<16;n++) {NakiBranchItem i=frame(n);assert(!naki_branch_queue_push(&q,&i,100));}
    for(int n=0;n<256;n++) {NakiBranchItem i=audio(n,n%16);assert(!naki_branch_queue_push(&q,&i,100));}
    assert(q.count==272&&q.video_high==16&&q.audio_high==256);naki_branch_queue_abort(&q);
    assert(!q.count&&!q.video_count&&!q.audio_count&&!q.bytes&&!q.media_us);naki_branch_queue_destroy(&q);
    NakiBranchItem i=frame(0);uint64_t bytes=i.charged_bytes;
    assert(!naki_branch_queue_init_limits(&q,2,16,bytes-1,1000000,10000000));
    assert(naki_branch_queue_push(&q,&i,100)==AVERROR(EFBIG)&&q.rejection_reason==NAKI_BRANCH_BYTES);naki_branch_item_free(&i);naki_branch_queue_destroy(&q);
    assert(!naki_branch_queue_init_limits(&q,2,16,1000000,39,10000000));i=audio(0,0);
    assert(naki_branch_queue_push(&q,&i,100)==AVERROR(EFBIG)&&q.rejection_reason==NAKI_BRANCH_WORK);naki_branch_item_free(&i);naki_branch_queue_destroy(&q);
    assert(!naki_branch_queue_init_limits(&q,2,16,1000000,1000000,1000));i=audio(0,0);
    assert(naki_branch_queue_push(&q,&i,1101)==AVERROR(ETIMEDOUT)&&q.rejection_reason==NAKI_BRANCH_LAG);naki_branch_item_free(&i);naki_branch_queue_destroy(&q);
    /* Pressure recovery must drain both types below their own half marks. */
    assert(!naki_branch_queue_init_limits(&q,2,4,1000000,1000000,10000000));int64_t now=av_gettime_relative();
    for(int n=0;n<4;n++) {i=n<2?frame(n):audio(n,0);i.ingress_us=now;assert(!naki_branch_queue_push(&q,&i,now));}
    i=frame(5);i.ingress_us=now;assert(naki_branch_queue_admit(&q,&i,now,100000)==AVERROR(EAGAIN));int64_t deadline=q.admission_deadline_us;
    NakiBranchItem popped={0};for(int n=0;n<2;n++) {assert(!naki_branch_queue_pop(&q,&popped,0));naki_branch_item_free(&popped);assert(q.admission_deadline_us==deadline);}
    assert(!naki_branch_queue_pop(&q,&popped,0));naki_branch_item_free(&popped);assert(!q.admission_deadline_us);
    naki_branch_item_free(&i);naki_branch_queue_destroy(&q);
    assert(naki_branch_queue_init_limits(&q,17,16,1000000,1000000,10000000)<0);
    assert(naki_branch_queue_init_limits(&q,16,257,1000000,1000000,10000000)<0);
}
static void *consumer(void *opaque) {
    NakiBranchItem i={0};int ret=naki_branch_queue_pop(opaque,&i,av_gettime_relative()+2000000);
    assert(ret==AVERROR(ECANCELED));return NULL;
}
typedef struct AdmissionTest {
    NakiBranchQueue *blocked,*healthy;
    int calls,drain,cancel;
} AdmissionTest;
static int admission_schedule(void *opaque) {
    AdmissionTest *test=opaque;
    if(++test->calls==2) {
        /* Every branch was offered its item before the producer waited. */
        assert(test->healthy->count==1&&test->blocked->count==16);
        if(test->cancel)return AVERROR_EXIT;
        if(test->drain) {
            NakiBranchItem item={0};
            for(int n=0;n<9;n++) {
                assert(!naki_branch_queue_pop(test->blocked,&item,0)&&item.sequence==n);
                naki_branch_item_free(&item);
            }
        }
    }
    return 0;
}
static void admission_tests(void) {
    for(int mode=0;mode<3;mode++) {
        NakiBranchQueue queues[2];NakiBranchItem pending[2];NakiBranchOffer offers[2];
        for(int b=0;b<2;b++) {
            assert(!naki_branch_queue_init(&queues[b],16,1000000,1000000,10000000));
            pending[b]=frame(16+b);pending[b].ingress_us=av_gettime_relative();
            offers[b]=(NakiBranchOffer){&queues[b],&pending[b],AVERROR(EAGAIN)};
        }
        for(int n=0;n<16;n++) {
            NakiBranchItem item=frame(n);item.ingress_us=av_gettime_relative();
            assert(!naki_branch_queue_push(&queues[0],&item,av_gettime_relative()));
        }
        AdmissionTest test={.blocked=&queues[0],.healthy=&queues[1],.drain=mode==0,.cancel=mode==2};
        int64_t began=av_gettime_relative();
        int ret=naki_branch_admit_many(offers,2,20000,admission_schedule,&test);
        assert(offers[1].result==0&&!pending[1].frame&&queues[1].count==1);
        if(mode==0) {
            assert(!ret&&!offers[0].result&&!pending[0].frame&&!queues[0].admission_deadline_us);
            NakiBranchItem item={0};
            for(int n=9;n<=16;n++) {
                assert(!naki_branch_queue_pop(&queues[0],&item,0)&&item.sequence==n);
                naki_branch_item_free(&item);
            }
        } else {
            assert(pending[0].frame&&queues[0].count==16);
            if(mode==1)assert(!ret&&offers[0].result==AVERROR(ETIMEDOUT)&&av_gettime_relative()-began>=20000);
            else assert(ret==AVERROR_EXIT&&offers[0].result==AVERROR_EXIT&&test.calls==2);
            assert(pending[0].charged_bytes<=queues[0].byte_limit&&pending[0].media_us<=(uint64_t)queues[0].media_limit_us);
            naki_branch_item_free(&pending[0]);
            if(mode==1) {
                /* The slow branch has exhausted its one pressure window.
                 * Retire it and keep delivering subsequent fast-branch items,
                 * rather than demonstrating fairness for only its first item. */
                naki_branch_queue_abort(&queues[0]);NakiBranchItem received={0};
                assert(!naki_branch_queue_pop(&queues[1],&received,0)&&received.sequence==17);
                naki_branch_item_free(&received);
                for(int n=18;n<82;n++) {
                    NakiBranchItem next=frame(n);next.ingress_us=av_gettime_relative();
                    NakiBranchOffer subsequent[2]={{0},{&queues[1],&next,AVERROR(EAGAIN)}};
                    assert(!naki_branch_admit_many(subsequent,2,20000,NULL,NULL)&&!subsequent[1].result&&!next.frame);
                    assert(!naki_branch_queue_pop(&queues[1],&received,0)&&received.sequence==n);
                    naki_branch_item_free(&received);
                }
                assert(!queues[1].count&&!queues[1].admission_deadline_us);
            }
        }
        assert(queues[0].high_water==16&&queues[1].high_water==1);
        for(int b=0;b<2;b++)naki_branch_queue_destroy(&queues[b]);
    }
    /* A single free slot is not recovery: sustained pressure keeps the same
     * deadline even when the producer can transfer one item in between. */
    for(int capacity=2;capacity<=16;capacity+=14) {
        NakiBranchQueue q;assert(!naki_branch_queue_init(&q,capacity,1000000,1000000,10000000));
        int64_t now=av_gettime_relative();
        for(int n=0;n<capacity;n++) {NakiBranchItem item=frame(n);item.ingress_us=now;assert(!naki_branch_queue_push(&q,&item,now));}
        NakiBranchItem next=frame(capacity),item={0};next.ingress_us=now;
        assert(naki_branch_queue_admit(&q,&next,now,100000)==AVERROR(EAGAIN));
        int64_t deadline=q.admission_deadline_us;
        for(int n=0;n<4;n++) {
            assert(!naki_branch_queue_pop(&q,&item,0));naki_branch_item_free(&item);
            assert(!naki_branch_queue_admit(&q,&next,now+n+1,100000)&&q.admission_deadline_us==deadline);
            next=frame(capacity+n+1);next.ingress_us=now;
            assert(naki_branch_queue_admit(&q,&next,now+n+2,100000)==AVERROR(EAGAIN));
        }
        assert(naki_branch_queue_admit(&q,&next,deadline,100000)==AVERROR(ETIMEDOUT)&&next.frame);
        naki_branch_item_free(&next);naki_branch_queue_destroy(&q);
    }
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
    /* Historical shared-cap initializer still obeys its old combined bound.
     * Production uses the independently tested mixed-quota initializer. */
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
    admission_tests();mixed_quota_tests();scheduler_recovery_test();
    assert(atomic_load(&released)==atomic_load(&created));puts("branch queue: ownership, COW, separate video/audio quotas, legacy bounds, FIFO admission recovery, branch fairness, sustained pressure, EOF and cancellation passed");return 0;
}
