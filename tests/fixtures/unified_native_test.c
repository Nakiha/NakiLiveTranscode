#include "media/codec_progress.h"
#include "media/unified_rate.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/dict.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/time.h>

typedef struct Native {
    AVCodecContext *codec;
    uint64_t done;
    int admitted,accepted[32],returned[32],kind[32],null_sent,eagain,null_eagain;
    int max_active,max_future;
    NakiLiveRate model;
    NakiUnifiedRate budget;
} Native;
static int send_frame(void *opaque,const void *frame) {
    Native *n=opaque;int ret=avcodec_send_frame(n->codec,frame);
    if(ret==AVERROR(EAGAIN)) {n->eagain++;if(!frame)n->null_eagain++;}
    return ret;
}
static uint64_t progress(void *opaque) {return ((Native *)opaque)->done;}
static int receive_frames(void *opaque) {
    Native *n=opaque;AVPacket *packet=av_packet_alloc();assert(packet);int ret;
    while((ret=avcodec_receive_packet(n->codec,packet))>=0) {
        int seq=1+(int)((packet->pts-90000)/3000);
        assert(seq>0&&seq<=n->admitted&&n->accepted[seq]&&!n->returned[seq]);
        size_t size=0;const uint8_t *raw=av_packet_get_side_data(packet,AV_PKT_DATA_STRINGS_METADATA,&size);
        AVDictionary *info=NULL;assert(raw&&av_packet_unpack_dictionary(raw,size,&info)>=0);
        AVDictionaryEntry *entry=av_dict_get(info,"live_future_frames",NULL,0);assert(entry);
        int future=atoi(entry->value);assert(future>=0&&future<=4);
        if(future>n->max_future)n->max_future=future;
        assert(n->admitted-seq<=4);av_dict_free(&info);
        raw=av_packet_get_side_data(packet,AV_PKT_DATA_QUALITY_STATS,&size);assert(raw&&size>=5);
        n->kind[seq]=raw[4];n->returned[seq]=1;n->done++;
        int kind=raw[4]==AV_PICTURE_TYPE_I?0:raw[4]==AV_PICTURE_TYPE_P?1:2;
        int64_t dts=av_rescale_q(packet->dts,n->codec->time_base,AV_TIME_BASE_Q);
        int64_t bits=8*((int64_t)packet->size+(seq==1?n->codec->extradata_size:0));
        assert(!naki_unified_actual(&n->budget,(uint64_t)seq,bits,kind,dts));
        assert(!naki_unified_clock(&n->budget,av_gettime_relative()));
        assert(!naki_unified_can_write(&n->budget,(uint64_t)seq,dts));
        /* Native API fixture has no mux. This settlement is a ledger fixture,
         * not a claim about real flush timing or mux contract measurements. */
        assert(!naki_unified_written(&n->budget,(uint64_t)seq,dts,n->budget.clock));
        av_packet_unref(packet);
    }
    av_packet_free(&packet);
    return ret==AVERROR(EAGAIN)||ret==AVERROR_EOF?0:ret;
}
static void open_codec(Native *n) {
    const AVCodec *codec=avcodec_find_encoder_by_name("libvvenc");assert(codec);
    n->codec=avcodec_alloc_context3(codec);assert(n->codec);
    AVCodecContext *c=n->codec;c->width=320;c->height=180;c->pix_fmt=AV_PIX_FMT_YUV420P10LE;
    c->time_base=(AVRational){1,90000};c->framerate=(AVRational){30,1};
    c->thread_count=2;c->gop_size=60;c->max_b_frames=3;c->flags|=AV_CODEC_FLAG_GLOBAL_HEADER;
    AVDictionary *o=NULL;
    av_dict_set(&o,"preset","faster",0);av_dict_set(&o,"qp","32",0);av_dict_set(&o,"qpa","1",0);
    av_dict_set(&o,"live-pts-gop","1",0);av_dict_set(&o,"live-frame-intra","1",0);
    av_dict_set(&o,"live-qp-feedback","1",0);av_dict_set(&o,"live-max-future","4",0);
    av_dict_set(&o,"vvenc-params","GOPSize=4:IntraPeriod=60:RefreshSec=2:POC0IDR=1:STA=0:MaxParallelFrames=0:MCTF=1:BIM=0:MCTFFutureReference=0:InternalBitDepth=10:MinQTISlice=16:MinQTNonISlice=16:MinQTChromaISliceInChromaSamples=8:SearchRange=64:MinSearchWindow=32:FastIntraTools=2:RDOQ=2:SelectiveRDOQ=2:ALF=0:CCALF=0:Frame1=P 4 0 0 0 0 0 1.0 0 0 0 0 0 0 0 1 1 4 0 0:Frame2=B 2 1 0 0 0 0 1.0 0 0 0 0 0 0 1 1 1 2 1 1 -2:Frame3=B 1 2 0 0 0 0 1.0 0 0 0 0 0 0 2 1 1 1 1 1 -1:Frame4=B 3 2 0 0 0 0 1.0 0 0 0 0 0 0 2 1 1 1 1 1 -1",0);
    int ret=avcodec_open2(c,codec,&o);if(ret<0)fprintf(stderr,"open=%d\n",ret);
    assert(ret>=0&&!av_dict_count(o));av_dict_free(&o);
    naki_unified_init(&n->budget,&n->model,1100000,0,320*180,32,33333,av_gettime_relative());
    n->budget.config_bits=8*(int64_t)c->extradata_size;
}
static AVFrame *make_frame(Native *n,int seq,int intra) {
    AVFrame *f=av_frame_alloc();assert(f);f->width=320;f->height=180;f->format=AV_PIX_FMT_YUV420P10LE;
    f->pts=90000+(seq-1)*3000;f->duration=3000;f->pict_type=intra?AV_PICTURE_TYPE_I:AV_PICTURE_TYPE_NONE;
    assert(av_frame_get_buffer(f,32)>=0);
    for(int p=0;p<3;p++)for(int y=0;y<(p?90:180);y++)for(int x=0;x<(p?160:320);x++)
        ((uint16_t *)(f->data[p]+y*f->linesize[p]))[x]=(uint16_t)((x*3+y*5+seq*7+p*127)&1023);
    assert(av_dict_set(&f->metadata,"naki_qp_offset","0",0)>=0);
    assert(seq==++n->admitted);uint64_t id=0;
    assert(!naki_unified_admit(&n->budget,av_rescale_q(f->pts,n->codec->time_base,AV_TIME_BASE_Q),av_gettime_relative(),1,&id));
    assert(id==(uint64_t)seq);return f;
}
static void plan(Native *n,int begin,int inter,int close_i) {
    uint64_t ids[4];int kinds[4],count=inter+close_i;
    for(int i=0;i<count;i++) {ids[i]=(uint64_t)(begin+i);kinds[i]=i==inter?(begin==1?3:0):(i==inter-1?1:2);}
    assert(count>0&&count<=4);assert(!naki_unified_clock(&n->budget,av_gettime_relative()));
    assert(!naki_unified_plan(&n->budget,ids,kinds,count));
}
static void submit(Native *n,AVFrame *f) {
    NakiCodecProgress io={n,send_frame,receive_frames,progress};
    int seq=f?1+(int)((f->pts-90000)/3000):0;
    if(f) {NakiUnifiedItem *item=naki_unified_find(&n->budget,(uint64_t)seq);assert(item);
        assert(av_dict_set_int(&f->metadata,"naki_qp_offset",item->decision.qp-32,0)>=0);
        assert(!naki_unified_try(&n->budget,(uint64_t)seq));}
    int ret=naki_codec_submit(&io,f,AVERROR(EAGAIN),&n->null_sent);assert(ret>=0);
    if(f) {assert(!n->accepted[seq]);n->accepted[seq]=1;assert(!naki_unified_accept(&n->budget,(uint64_t)seq));}
}
static void run(const char *name,int non_i,int close_i,int eof,int cancel_partial) {
    Native n={0};open_codec(&n);
    AVFrame *f=make_frame(&n,1,1);plan(&n,1,0,1);submit(&n,f);av_frame_free(&f);assert(receive_frames(&n)>=0);
    if(n.done!=1) {fprintf(stderr,"incompatible: startup I needs future input\n");abort();}
    AVFrame *batch[4]={0};int count=non_i+close_i;assert(count<=4);
    for(int i=0;i<count;i++)batch[i]=make_frame(&n,i+2,close_i&&i==count-1);
    if(count)plan(&n,2,non_i,close_i);
    n.max_active=count>1?count:1;
    for(int i=0;i<count;i++) {
        submit(&n,batch[i]);av_frame_free(&batch[i]);
        if(cancel_partial&&i==1)break;
    }
    if(!cancel_partial) {
        if(eof)submit(&n,NULL);
        assert(receive_frames(&n)>=0);
        if(n.done!=(uint64_t)n.admitted) {
            fprintf(stderr,"incompatible %s: admitted=%d returned=%llu, fifth input forbidden\n",name,n.admitted,(unsigned long long)n.done);abort();
        }
        if(close_i)assert(n.kind[n.admitted]==AV_PICTURE_TYPE_I);
        for(int i=2;i<=1+non_i;i++)assert(n.kind[i]==(i==1+non_i?AV_PICTURE_TYPE_P:AV_PICTURE_TYPE_B));
    } else {assert(!n.null_sent&&n.done==1);int64_t D=n.budget.D;assert(D>0);
        assert(!naki_unified_cancel(&n.budget)&&!n.budget.C&&n.budget.D==D);}
    for(int i=0;i<4;i++)av_frame_free(&batch[i]);
    avcodec_free_context(&n.codec);
    printf("{\"case\":\"%s\",\"admitted\":%d,\"returned\":%llu,\"max_application_plus_encoder\":%d,\"native_future_max\":%d,\"null_sent\":%d,\"null_eagain\":%d,\"cancelled\":%d,\"product_ledger_used\":true,\"mux_settlement_simulated\":true}\n",name,n.admitted,(unsigned long long)n.done,n.budget.active_peak,n.max_future,n.null_sent,n.null_eagain,cancel_partial);
}
int main(void) {
    av_log_set_level(AV_LOG_ERROR);
    run("startup",0,0,1,0);run("full4_without_eof",4,0,0,0);
    for(int n=1;n<=3;n++) {char name[40];snprintf(name,sizeof(name),"short%d_then_I_without_eof",n);run(name,n,1,0,0);snprintf(name,sizeof(name),"eof_tail%d",n);run(name,n,0,1,0);}
    run("cancel_after_two_accepted",4,0,0,1);
    return 0;
}
