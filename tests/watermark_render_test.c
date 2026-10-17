/* Unencoded frame harness: exact timeline, alpha, pixel and opaque-ref checks. */
#include "watermark.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <libavutil/buffer.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
static atomic_uint large_rgba_allocations;
int __real_av_frame_get_buffer(AVFrame *frame,int align);
int __wrap_av_frame_get_buffer(AVFrame *frame,int align) {
    if(frame->format==AV_PIX_FMT_RGBA&&frame->width==2048&&frame->height==2048)
        atomic_fetch_add(&large_rgba_allocations,1);
    return __real_av_frame_get_buffer(frame,align);
}
int main(int argc,char **argv) {
    assert(argc==4||argc==5);assert(curl_global_init(CURL_GLOBAL_DEFAULT)==CURLE_OK);
    atomic_int cancel;atomic_init(&cancel,0);
    NakiWatermarkConfig cfg={.enabled=1,.required=1,.load_timeout_ms=3000,.startup_timeout_ms=5000,
        .width=12,.height=8,.x=2,.y=2,.stretch=1,.opacity=1};
    assert(strlen(argv[1])<sizeof(cfg.uri));strcpy(cfg.uri,argv[1]);
    if(argc==5)cfg.width=cfg.height=atoi(argv[4]);
    if(getenv("NAKI_WM_TEST_LOAD_TIMEOUT_MS"))cfg.load_timeout_ms=atoi(getenv("NAKI_WM_TEST_LOAD_TIMEOUT_MS"));
    int optional_setup=getenv("NAKI_WM_TEST_OPTIONAL_SETUP")!=NULL;
    if(optional_setup)cfg.required=0;
    if(getenv("NAKI_WM_TEST_DEADLINE"))cfg.startup_timeout_ms=100;
    NakiWatermark *wm=naki_watermark_open(&cfg,av_gettime_relative(),&cancel);assert(wm);
    if(getenv("NAKI_WM_TEST_DEADLINE")) {
        av_usleep(150000);cfg.startup_timeout_ms=5000;char error[256];
        assert(!naki_watermark_request(wm,&cfg,2,error,sizeof(error)));
    }
    int ret=naki_watermark_wait(wm);
    if(ret<0) {
        fprintf(stderr,"asset prepare failed: %d\n",ret);naki_watermark_close(&wm);
        if(argc==5) {
            unsigned allocations=atomic_load(&large_rgba_allocations);
            fprintf(stderr,"large_rgba_allocations_before_budget_rejection=%u\n",allocations);
            if(allocations!=4)return 7;
        }
        return 3;
    }
    if(getenv("NAKI_WM_TEST_GATE_UPDATE")) {
        char error[256];strcpy(cfg.uri,"/nonexistent-watermark-test.png");
        assert(!naki_watermark_request(wm,&cfg,2,error,sizeof(error)));
    }
    if(optional_setup) {
        int ready=0;
        for(int i=0;i<300&&!ready;i++) {
            yyjson_mut_doc *doc=yyjson_mut_doc_new(NULL);yyjson_mut_doc_set_root(doc,yyjson_mut_obj(doc));
            naki_watermark_status(wm,doc);
            yyjson_mut_val *status=yyjson_mut_obj_get(yyjson_mut_doc_get_root(doc),"watermark");
            ready=yyjson_mut_get_uint(yyjson_mut_obj_get(status,"prepared_revision"))==1;yyjson_mut_doc_free(doc);
            if(!ready)av_usleep(10000);
        }
        assert(ready);NakiWatermarkCompositor *compositor=NULL;
        AVFrame *frame=av_frame_alloc();assert(frame);frame->format=AV_PIX_FMT_GRAY8;frame->width=32;frame->height=24;frame->pts=0;
        assert(av_frame_get_buffer(frame,32)>=0);memset(frame->data[0],42,(size_t)frame->linesize[0]*frame->height);
        uint64_t revision=99;assert(!naki_watermark_frame(wm,&compositor,frame,AV_TIME_BASE_Q,0,&revision));
        assert(!compositor&&revision==0&&frame->data[0][0]==42);
        yyjson_mut_doc *doc=yyjson_mut_doc_new(NULL);yyjson_mut_doc_set_root(doc,yyjson_mut_obj(doc));naki_watermark_status(wm,doc);
        yyjson_mut_val *status=yyjson_mut_obj_get(yyjson_mut_doc_get_root(doc),"watermark");
        assert(yyjson_mut_get_uint(yyjson_mut_obj_get(status,"failed_revision"))==1);
        yyjson_mut_doc_free(doc);av_frame_free(&frame);naki_watermark_close(&wm);curl_global_cleanup();return 0;
    }
    int ten=atoi(argv[3]);NakiWatermarkCompositor *compositor=NULL;
    const char *color=getenv("NAKI_WM_TEST_COLOR");int bt709=color&&strstr(color,"709"),full=color&&strstr(color,"full");
    FILE *output=fopen(argv[2],"wb");assert(output);
    int64_t offsets[]={0,69000,70000,199000,200000,309000,310000,479000,480000,1010000};
    for(size_t i=0;i<sizeof(offsets)/sizeof(*offsets);i++) {
        AVFrame *frame=av_frame_alloc();assert(frame);
        frame->format=ten?AV_PIX_FMT_YUV420P10LE:AV_PIX_FMT_YUV420P;
        frame->width=32;frame->height=24;frame->pts=5000000+offsets[i];frame->time_base=AV_TIME_BASE_Q;frame->duration=1;
        assert(av_frame_get_buffer(frame,32)>=0);
        frame->opaque_ref=av_buffer_alloc(8);assert(frame->opaque_ref);memset(frame->opaque_ref->data,42,8);
        const enum AVFrameSideDataType side_types[]={AV_FRAME_DATA_A53_CC,AV_FRAME_DATA_SEI_UNREGISTERED,AV_FRAME_DATA_S12M_TIMECODE};
        const uint8_t payloads[3][20]={{0xfc,0x94,0x2c},{1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,42,43,44,45},{1,0,0,0,0x12,0x34,0x56,0}};
        const size_t sizes[]={3,20,16};
        for(int k=0;k<3;k++) {
            AVFrameSideData *sd=av_frame_new_side_data(frame,side_types[k],sizes[k]);assert(sd);
            memcpy(sd->data,payloads[k],sizes[k]);
        }
        assert(!av_dict_set(&frame->metadata,"watermark-test-main","unchanged",0));
        frame->color_range=full?AVCOL_RANGE_JPEG:AVCOL_RANGE_MPEG;frame->colorspace=bt709?AVCOL_SPC_BT709:AVCOL_SPC_BT470BG;
        for(int p=0;p<3;p++)for(int y=0;y<(p?12:24);y++)for(int x=0;x<(p?16:32);x++) {
            int value=p?128:full?0:16;
            if(ten)((uint16_t *)(frame->data[p]+y*frame->linesize[p]))[x]=value*4;
            else frame->data[p][y*frame->linesize[p]+x]=(uint8_t)value;
        }
        assert(naki_watermark_video(wm,frame,32,24)>=0);uint64_t revision=0;
        int hdr_change=getenv("NAKI_WM_TEST_HDR_CHANGE")&&i==1;
        if(hdr_change)frame->color_trc=AVCOL_TRC_SMPTE2084;
        int composed=naki_watermark_frame(wm,&compositor,frame,AV_TIME_BASE_Q,frame->pts,&revision);
        if(hdr_change) {
            assert(composed<0);char error[256];assert(naki_watermark_request(wm,&cfg,2,error,sizeof(error))<0);
            av_frame_free(&frame);fclose(output);naki_watermark_compositor_free(&compositor);naki_watermark_close(&wm);curl_global_cleanup();return 0;
        }
        assert(composed>=0);
        if(frame->nb_side_data!=3) {fprintf(stderr,"side_data_count=%d expected=3\n",frame->nb_side_data);return 6;}
        for(int k=0;k<3;k++) {
            int count=0;
            for(int j=0;j<frame->nb_side_data;j++)if(frame->side_data[j]->type==side_types[k]) {
                count++;assert(frame->side_data[j]->size==sizes[k]&&!memcmp(frame->side_data[j]->data,payloads[k],sizes[k]));
            }
            assert(count==1);
        }
        AVDictionaryEntry *entry=av_dict_get(frame->metadata,"watermark-test-main",NULL,0);
        assert(entry&&!strcmp(entry->value,"unchanged"));
        assert(revision==1&&frame->pts==5000000+offsets[i]&&frame->format==(ten?AV_PIX_FMT_YUV420P10LE:AV_PIX_FMT_YUV420P));
        assert(frame->opaque_ref&&frame->opaque_ref->size==8&&frame->opaque_ref->data[0]==42);
        int size=av_image_get_buffer_size(frame->format,32,24,1);uint8_t *data=av_malloc(size);assert(data);
        assert(av_image_copy_to_buffer(data,size,(const uint8_t *const *)frame->data,frame->linesize,frame->format,32,24,1)==size);
        assert(fwrite(data,1,size,output)==(size_t)size);av_free(data);av_frame_free(&frame);
    }
    fclose(output);naki_watermark_compositor_free(&compositor);naki_watermark_close(&wm);curl_global_cleanup();return 0;
}
