#define _GNU_SOURCE
#include "watermark.h"
#include <assert.h>
#include <curl/curl.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavfilter/buffersink.h>
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
static atomic_int mode,entered,release_gate;
static atomic_int reserve_on_unpin;
static NakiOperations *shared_operations;
static NakiOperationHandle concurrent_reservation;
int __real_naki_operations_pin(NakiOperations *,NakiOperationHandle,unsigned,unsigned);
int __wrap_naki_operations_pin(NakiOperations *store,NakiOperationHandle handle,unsigned set,unsigned clear) {
    int ret=__real_naki_operations_pin(store,handle,set,clear);
    if((clear&NAKI_OP_PIN_EFFECTIVE)&&atomic_exchange(&reserve_on_unpin,0)) {
        assert(store==shared_operations);char fingerprint[65];memset(fingerprint,'c',64);fingerprint[64]=0;
        NakiOperationIntent intent={.kind=NAKI_OP_UPDATE,.job_id="job-A",.operation_id="other-output",.output_id="other",
            .fingerprint=fingerprint,.origin_request_id="concurrent-request",.expected_revision=1,.revision=2};
        assert(naki_operations_reserve(store,&intent,av_gettime_relative()+1800000001,&concurrent_reservation)==1);
        assert(concurrent_reservation!=handle); /* Simulate other output reusing the now-unpinned expired slot. */
    }
    return ret;
}
int __real_avcodec_open2(AVCodecContext *,const AVCodec *,AVDictionary **);
int __wrap_avcodec_open2(AVCodecContext *context,const AVCodec *codec,AVDictionary **options) {
    if(atomic_load(&mode)==4&&codec&&codec->id==AV_CODEC_ID_PNG) {
        atomic_store(&entered,1);while(!atomic_load(&release_gate))av_usleep(1000);
        return AVERROR(ECANCELED);
    }
    return __real_avcodec_open2(context,codec,options);
}
int __real_av_frame_get_buffer(AVFrame *,int);
int __wrap_av_frame_get_buffer(AVFrame *frame,int align) {
    if(frame->format==AV_PIX_FMT_RGBA&&frame->width==32&&frame->height==24) {
        if(atomic_load(&mode)==1) {atomic_store(&entered,1);while(!atomic_load(&release_gate))av_usleep(1000);}
        if(atomic_load(&mode)==2&&atomic_exchange(&mode,0)==2)return AVERROR(ENOMEM);
    }
    return __real_av_frame_get_buffer(frame,align);
}
int __real_av_buffersink_get_frame(AVFilterContext *,AVFrame *);
int __wrap_av_buffersink_get_frame(AVFilterContext *context,AVFrame *frame) {
    if(atomic_load(&mode)==3&&atomic_exchange(&mode,0)==3)return AVERROR_EXTERNAL;
    return __real_av_buffersink_get_frame(context,frame);
}
static AVFrame *frame(void) {
    AVFrame *f=av_frame_alloc();assert(f);f->format=AV_PIX_FMT_YUV420P;f->width=32;f->height=24;
    f->color_range=AVCOL_RANGE_MPEG;f->colorspace=AVCOL_SPC_BT470BG;f->pts=1000000;
    assert(av_frame_get_buffer(f,32)>=0);ptrdiff_t lines[4];for(int i=0;i<4;i++)lines[i]=f->linesize[i];
    assert(!av_image_fill_black(f->data,lines,f->format,f->color_range,f->width,f->height));return f;
}
static NakiFrameIdentity identity(uint64_t id) {
    return (NakiFrameIdentity){.frame_id=id,.source_pts=(int64_t)id*40000,.source_us=(int64_t)id*40000,
        .source_tb_num=1,.source_tb_den=1000000,.output_tb_num=1,.output_tb_den=90000,.output_pts=(int64_t)id*3600};
}
static NakiWatermark *open_wm(NakiOperations **operations,atomic_int *cancel,unsigned capacity) {
    *operations=naki_operations_open(capacity,2*1024*1024,1800000000);assert(*operations);atomic_init(cancel,0);
    NakiWatermarkConfig c={.load_timeout_ms=2000,.startup_timeout_ms=3000,.width=12,.height=8,.x=2,.y=2,.stretch=1,.opacity=1};
    NakiWatermark *wm=naki_watermark_open(&c,av_gettime_relative(),cancel);assert(wm);
    assert(!naki_watermark_bind(wm,*operations,"job-A","main","create-test",NULL));return wm;
}
static int submit(NakiWatermark *wm,const char *op,uint64_t expected,const char *patch,yyjson_mut_doc **answer) {
    char text[8192],error[256];snprintf(text,sizeof(text),"{\"operation_id\":\"%s\",\"target\":{\"output_id\":\"main\",\"component\":\"watermark\"},\"expected_revision\":%llu,\"patch\":%s}",op,(unsigned long long)expected,patch);
    yyjson_doc *doc=yyjson_read(text,strlen(text),0);assert(doc);NakiUpdateRequest request;
    assert(!naki_update_parse(yyjson_doc_get_root(doc),"job-A",&request,error,sizeof(error)));yyjson_doc_free(doc);
    return naki_watermark_submit(wm,&request,"origin-first",answer,error,sizeof(error));
}
static uint64_t status(NakiWatermark *wm,const char *key) {
    yyjson_mut_doc *d=yyjson_mut_doc_new(NULL);yyjson_mut_doc_set_root(d,yyjson_mut_obj(d));naki_watermark_status(wm,d);
    uint64_t value=yyjson_mut_get_uint(yyjson_mut_obj_get(yyjson_mut_obj_get(yyjson_mut_doc_get_root(d),"watermark"),key));yyjson_mut_doc_free(d);return value;
}
static void prepared(NakiWatermark *wm,uint64_t revision) {
    int64_t until=av_gettime_relative()+4000000;while(status(wm,"prepared_revision")!=revision&&av_gettime_relative()<until)av_usleep(1000);
    assert(status(wm,"prepared_revision")==revision);
}
static yyjson_mut_doc *operation(NakiOperations *ops,const char *op,const char *state,const char *delivery) {
    NakiOperationHandle handle;assert(!naki_operations_lookup(ops,"job-A",op,&handle));yyjson_mut_doc *doc=naki_operations_get(ops,handle);assert(doc);
    yyjson_mut_val *root=yyjson_mut_doc_get_root(doc);assert(!strcmp(yyjson_mut_get_str(yyjson_mut_obj_get(root,"state")),state));
    if(delivery)assert(!strcmp(yyjson_mut_get_str(yyjson_mut_obj_get(yyjson_mut_obj_get(root,"delivery"),"state")),delivery));
    return doc;
}
typedef struct Owner {NakiWatermark *wm;NakiWatermarkCompositor *compositor;AVFrame *frame;NakiFrameIdentity identity;NakiOperationHandle lease;uint64_t revision;int result;} Owner;
static void *compose(void *opaque) {
    Owner *o=opaque;o->result=naki_watermark_frame_tracked(o->wm,&o->compositor,o->frame,AV_TIME_BASE_Q,&o->identity,&o->revision,&o->lease);return NULL;
}
static void await_claim(void) {
    int64_t until=av_gettime_relative()+3000000;while(!atomic_load(&entered)&&av_gettime_relative()<until)av_usleep(1000);assert(atomic_load(&entered));
}
static void check(NakiOperations *ops,const char *op,const char *state,const char *delivery) {
    yyjson_mut_doc *doc=operation(ops,op,state,delivery);yyjson_mut_doc_free(doc);
}
static void cleanup(Owner *owner,NakiOperations **operations) {
    assert(!naki_watermark_release_frame(owner->wm,owner->lease));owner->lease=0;
    naki_watermark_compositor_free(&owner->compositor);av_frame_free(&owner->frame);
    naki_watermark_close(&owner->wm);naki_operations_close(operations);
}
static void written(Owner *owner) {
    assert(!naki_watermark_written(owner->wm,owner->lease,&owner->identity,av_gettime_relative()));
}
int main(int argc,char **argv) {
    assert(argc==2);assert(curl_global_init(CURL_GLOBAL_DEFAULT)==CURLE_OK);
    char enable[4096];snprintf(enable,sizeof(enable),"{\"enabled\":true,\"resource\":{\"uri\":\"%s\"}}",argv[1]);
    NakiOperations *ops=NULL;atomic_int cancel;yyjson_mut_doc *answer=NULL;pthread_t thread;
    Owner a={.wm=open_wm(&ops,&cancel,8),.frame=frame(),.identity=identity(1)};
    assert(!submit(a.wm,"A",1,enable,&answer));yyjson_mut_doc_free(answer);prepared(a.wm,2);
    atomic_store(&mode,1);atomic_store(&entered,0);atomic_store(&release_gate,0);
    assert(!pthread_create(&thread,NULL,compose,&a));await_claim();check(ops,"A","applying","not_ready");
    /* Retry is looked up before stale CAS and cannot restart resource loading. */
    assert(!submit(a.wm,"A",1,enable,&answer));yyjson_mut_doc_free(answer);
    assert(submit(a.wm,"A",2,"{\"placement\":{\"opacity\":0.5}}",&answer)==-EEXIST);
    assert(!submit(a.wm,"B",2,"{\"placement\":{\"opacity\":0.5}}",&answer));yyjson_mut_doc_free(answer);
    check(ops,"A","applying","not_ready");check(ops,"B","accepted","not_ready");assert(status(a.wm,"accepted_revision")==3);
    atomic_store(&release_gate,1);assert(!pthread_join(thread,NULL));atomic_store(&mode,0);
    assert(!a.result&&a.revision==2&&a.lease);check(ops,"A","applied","pending");prepared(a.wm,3);
    Owner b={.wm=a.wm,.compositor=a.compositor,.frame=frame(),.identity=identity(2)};a.compositor=NULL;compose(&b);
    assert(!b.result&&b.revision==3&&b.lease);written(&b);written(&a);
    yyjson_mut_doc *doc=operation(ops,"A","applied","first_written");
    yyjson_mut_val *delivery=yyjson_mut_obj_get(yyjson_mut_doc_get_root(doc),"delivery");
    assert(yyjson_mut_get_uint(yyjson_mut_obj_get(yyjson_mut_obj_get(delivery,"first_written"),"frame_id"))==1);yyjson_mut_doc_free(doc);
    check(ops,"B","applied","first_written");
    assert(!naki_watermark_release_frame(a.wm,a.lease));a.lease=0;av_frame_free(&a.frame);cleanup(&b,&ops);

    /* Stop cannot cancel an owner claim or close delivery before its lease. */
    a=(Owner){.wm=open_wm(&ops,&cancel,8),.frame=frame(),.identity=identity(3)};
    assert(!submit(a.wm,"stop-race",1,enable,&answer));yyjson_mut_doc_free(answer);prepared(a.wm,2);
    atomic_store(&mode,1);atomic_store(&entered,0);atomic_store(&release_gate,0);
    assert(!pthread_create(&thread,NULL,compose,&a));await_claim();naki_watermark_stop(a.wm);
    check(ops,"stop-race","applying","not_ready");atomic_store(&release_gate,1);
    assert(!pthread_join(thread,NULL));atomic_store(&mode,0);assert(!a.result&&a.lease);
    check(ops,"stop-race","applied","pending");
    assert(!submit(a.wm,"stop-race",1,enable,&answer));yyjson_mut_doc_free(answer);
    assert(submit(a.wm,"after-stop",2,"{\"enabled\":false}",&answer)<0);
    assert(!naki_watermark_release_frame(a.wm,a.lease));a.lease=0;check(ops,"stop-race","applied","not_written");cleanup(&a,&ops);

    /* Optional compositor setup failure retains bare effective configuration;
     * the next sparse patch merges from failed desired, including its URI. */
    a=(Owner){.wm=open_wm(&ops,&cancel,8),.frame=frame(),.identity=identity(4)};
    assert(!submit(a.wm,"setup-fail",1,enable,&answer));yyjson_mut_doc_free(answer);prepared(a.wm,2);
    atomic_store(&mode,2);compose(&a);assert(!a.result&&a.revision==1&&!a.lease);check(ops,"setup-fail","failed","not_ready");
    assert(!submit(a.wm,"recover",2,"{\"placement\":{\"opacity\":0.75}}",&answer));yyjson_mut_doc_free(answer);prepared(a.wm,3);
    compose(&a);assert(!a.result&&a.revision==3&&a.lease);check(ops,"recover","applied","pending");cleanup(&a,&ops);

    /* Actual native composition failure belongs to the owner, not controller. */
    a=(Owner){.wm=open_wm(&ops,&cancel,8),.frame=frame(),.identity=identity(5)};
    assert(!submit(a.wm,"compose-fail",1,enable,&answer));yyjson_mut_doc_free(answer);prepared(a.wm,2);
    atomic_store(&mode,3);compose(&a);assert(a.result==AVERROR_EXTERNAL&&!a.lease);check(ops,"compose-fail","failed","not_ready");cleanup(&a,&ops);

    /* Capacity is reserved before changing desired; replay remains available. */
    a=(Owner){.wm=open_wm(&ops,&cancel,1),.frame=frame(),.identity=identity(6)};
    assert(!submit(a.wm,"full",1,enable,&answer));yyjson_mut_doc_free(answer);prepared(a.wm,2);
    assert(submit(a.wm,"overflow",2,"{\"enabled\":false}",&answer)==-ENOSPC);assert(status(a.wm,"accepted_revision")==2);
    assert(!submit(a.wm,"full",1,enable,&answer));yyjson_mut_doc_free(answer);cleanup(&a,&ops);
    /* External job cancellation is visible to loader before internal stop. */
    a=(Owner){.wm=open_wm(&ops,&cancel,8),.frame=frame(),.identity=identity(7)};
    atomic_store(&mode,4);atomic_store(&entered,0);atomic_store(&release_gate,0);
    assert(!submit(a.wm,"external-cancel",1,enable,&answer));yyjson_mut_doc_free(answer);await_claim();
    atomic_store(&cancel,1);atomic_store(&release_gate,1);
    int64_t until=av_gettime_relative()+3000000;
    for(;;) {
        NakiOperationHandle handle;assert(!naki_operations_lookup(ops,"job-A","external-cancel",&handle));
        doc=naki_operations_get(ops,handle);assert(doc);
        int cancelled=!strcmp(yyjson_mut_get_str(yyjson_mut_obj_get(yyjson_mut_doc_get_root(doc),"state")),"cancelled");yyjson_mut_doc_free(doc);
        if(cancelled)break;
        assert(av_gettime_relative()<until);av_usleep(1000);
    }
    check(ops,"external-cancel","cancelled","not_ready");assert(status(a.wm,"failed_revision")!=2);
    naki_watermark_stop(a.wm);check(ops,"external-cancel","cancelled","not_ready");atomic_store(&mode,0);cleanup(&a,&ops);

    /* A written old effective is retired before clearing its final pin. The
     * other output may reserve its expired slot immediately after unpin. */
    a=(Owner){.wm=open_wm(&ops,&cancel,2),.frame=frame(),.identity=identity(8)};
    assert(!submit(a.wm,"expired-A",1,enable,&answer));yyjson_mut_doc_free(answer);prepared(a.wm,2);compose(&a);assert(!a.result);written(&a);
    assert(!naki_watermark_release_frame(a.wm,a.lease));a.lease=0;
    assert(!submit(a.wm,"replacement-B",2,"{\"placement\":{\"opacity\":0.8}}",&answer));yyjson_mut_doc_free(answer);prepared(a.wm,3);
    shared_operations=ops;atomic_store(&reserve_on_unpin,1);a.identity=identity(9);compose(&a);
    assert(!a.result&&a.revision==3&&a.lease&&!atomic_load(&reserve_on_unpin));
    NakiOperationHandle gone;assert(naki_operations_lookup(ops,"job-A","expired-A",&gone)==-ENOENT);
    check(ops,"replacement-B","applied","pending");cleanup(&a,&ops);
    curl_global_cleanup();puts("PASS owner claim/replacement, stale retry, stop race, setup/native failure, delayed revision writes and bounded admission");return 0;
}
