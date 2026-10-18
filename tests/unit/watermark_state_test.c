/* Deterministic first-frame state matrix; gate only this executable's loader. */
#include "media/watermark.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
static pthread_mutex_t gate_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_changed=PTHREAD_COND_INITIALIZER;
static int gate_open;
typedef struct Start {void *(*fn)(void *);void *arg;} Start;
static void *start(void *opaque) {
    Start s=*(Start *)opaque;free(opaque);
    pthread_mutex_lock(&gate_mutex);
    while(!gate_open)pthread_cond_wait(&gate_changed,&gate_mutex);
    pthread_mutex_unlock(&gate_mutex);return s.fn(s.arg);
}
int __real_pthread_create(pthread_t *,const pthread_attr_t *,void *(*)(void *),void *);
int __wrap_pthread_create(pthread_t *t,const pthread_attr_t *attr,void *(*fn)(void *),void *arg) {
    Start *s=malloc(sizeof(*s));if(!s)return 12;*s=(Start){fn,arg};
    int ret=__real_pthread_create(t,attr,start,s);if(ret)free(s);return ret;
}
static void release_loader(void) {
    pthread_mutex_lock(&gate_mutex);gate_open=1;pthread_cond_broadcast(&gate_changed);pthread_mutex_unlock(&gate_mutex);
}
static uint64_t status(NakiWatermark *wm,const char *key) {
    yyjson_mut_doc *d=yyjson_mut_doc_new(NULL);yyjson_mut_doc_set_root(d,yyjson_mut_obj(d));naki_watermark_status(wm,d);
    uint64_t v=yyjson_mut_get_uint(yyjson_mut_obj_get(yyjson_mut_obj_get(yyjson_mut_doc_get_root(d),"watermark"),key));
    yyjson_mut_doc_free(d);return v;
}
static void wait_status(NakiWatermark *wm,const char *key,uint64_t value) {
    int i;for(i=0;i<500&&status(wm,key)!=value;i++)av_usleep(10000);assert(i<500);
}
int main(int argc,char **argv) {
    assert(argc==3);assert(curl_global_init(CURL_GLOBAL_DEFAULT)==CURLE_OK);
    FILE *out=fopen(argv[2],"w");assert(out);fputs("{\"cases\":[\n",out);int count=0,failures=0;
    const char *names[]={"none","enabled_prepared","enabled_failed","enabled_pending","disabled_prepared"};
    for(int required=0;required<2;required++)for(int initial=required;initial<2;initial++)
    for(int candidate=0;candidate<(required?1:5);candidate++)for(int video=0;video<3;video++) {
        gate_open=0;atomic_int cancel;atomic_init(&cancel,0);
        NakiWatermarkConfig cfg={.enabled=initial,.required=required,.load_timeout_ms=2000,.startup_timeout_ms=5000,
            .width=12,.height=8,.x=2,.y=2,.stretch=1,.opacity=1};
        assert(strlen(argv[1])<sizeof(cfg.uri));strcpy(cfg.uri,argv[1]);
        NakiWatermark *wm=naki_watermark_open(&cfg,av_gettime_relative(),&cancel);assert(wm);char error[256];
        if(candidate) {
            cfg.enabled=candidate!=4;if(candidate==2)strcpy(cfg.uri,"/nonexistent-watermark-state-test.png");
            assert(!naki_watermark_request(wm,&cfg,2,error,sizeof(error)));
        }
        if(candidate!=3) {
            release_loader();wait_status(wm,candidate==2?"failed_revision":"prepared_revision",candidate?2:1);
        } else assert(status(wm,"prepared_revision")!=2);
        if(required) {
            assert(!naki_watermark_wait(wm));cfg.enabled=0;
            assert(naki_watermark_request(wm,&cfg,2,error,sizeof(error))<0&&status(wm,"accepted_revision")==1);
        }
        AVFrame *f=av_frame_alloc();assert(f);f->format=AV_PIX_FMT_YUV420P;
        f->width=video==2?4096:32;f->height=video==2?2162:24;f->pts=0;
        f->color_range=AVCOL_RANGE_MPEG;f->colorspace=AVCOL_SPC_BT470BG;
        if(video==1)f->color_trc=AVCOL_TRC_SMPTE2084;
        assert(av_frame_get_buffer(f,32)>=0);ptrdiff_t lines[4];for(int i=0;i<4;i++)lines[i]=f->linesize[i];
        assert(!av_image_fill_black(f->data,lines,f->format,f->color_range,f->width,f->height));
        NakiWatermarkCompositor *compositor=NULL;uint64_t revision=99;
        int ret=naki_watermark_video(wm,f,f->width,f->height);
        if(ret>=0)ret=naki_watermark_frame(wm,&compositor,f,AV_TIME_BASE_Q,0,&revision);
        int fatal=video&&initial&&candidate!=4;
        int image=!video&&(candidate==1||(!candidate&&initial));
        uint64_t expected_revision=candidate==4?2:image?(candidate?2:1):initial?0:1;
        int pixel=f->data[0][6*f->linesize[0]+6];
        int match=fatal?ret<0:ret>=0&&revision==expected_revision&&abs(pixel-(image?81:16))<=1;
        fprintf(out,"%s{\"initial_enabled\":%d,\"required\":%d,\"candidate\":\"%s\",\"video\":%d,\"ret\":%d,\"revision\":%llu,\"expected_revision\":%llu,\"pixel\":%d,\"match\":%s}",
            count?",\n":"",initial,required,names[candidate],video,ret,(unsigned long long)revision,(unsigned long long)expected_revision,pixel,match?"true":"false");
        count++;failures+=!match;naki_watermark_compositor_free(&compositor);av_frame_free(&f);release_loader();naki_watermark_close(&wm);
    }
    fprintf(out,"\n],\"count\":%d,\"failures\":%d}\n",count,failures);fclose(out);curl_global_cleanup();
    printf("boundary matrix: %d cases, %d failures\n",count,failures);return failures?1:0;
}
