/* Link-time wrappers in the test worker only. Never preloaded into native SDK
 * internals: counters describe resources acquired by application call sites. */
#define _GNU_SOURCE
#include "live.h"
#include "pipeline_lifecycle_fixture.h"
#include "branch_queue.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libavfilter/avfilter.h>
#include <libavformat/avformat.h>

typedef struct Owner {
    char id[64];
    unsigned codecs,codec_frees,graphs,graph_frees,formats,format_frees,input,input_frees;
    unsigned avios,avio_frees,asyncs,async_frees,headers,trailers,closes,hooks,faults;
    unsigned frames,creates,queues,outputs,delegated,finalizing;
} Owner;
static Owner owners[5];
static atomic_uint count;
static _Thread_local Owner *owner;
static unsigned initial_threads,initial_fds;
static char media_paths[5][4096];
static int reported;
static unsigned entries(const char *path) {
    DIR *d=opendir(path);assert(d);unsigned n=0;struct dirent *e;
    while((e=readdir(d)))if(e->d_name[0]!='.')n++;
    assert(!closedir(d));return n;
}
static int fault(const char *mode) {
    const char *value=getenv("NAKI_TEST_LIFECYCLE_MODE"),*target=getenv("NAKI_TEST_LIFECYCLE_TARGET");
    return owner&&value&&!strcmp(value,mode)&&(!target||!strcmp(target,"all")||!strcmp(target,owner->id));
}
static int injected(const char *mode,int error) {if(!fault(mode))return 0;owner->faults++;return AVERROR(error);}
void naki_test_lifecycle_open(const NakiMediaJobInfo *info) {
    if(!getenv("NAKI_TEST_LIFECYCLE_DIR"))return;
    unsigned i=atomic_fetch_add(&count,1);assert(i<5);owner=&owners[i];
    snprintf(owner->id,sizeof(owner->id),"%s",info->output_id?info->output_id:"shared");
    owner->outputs=(unsigned)info->config->output_count;
    if(!i) {
        initial_threads=entries("/proc/self/task");initial_fds=entries("/proc/self/fd");
        snprintf(media_paths[0],sizeof(media_paths[0]),"%s",info->config->input);
        if(info->config->output_count)for(int k=0;k<info->config->output_count;k++)
            snprintf(media_paths[k+1],sizeof(media_paths[k+1]),"%s",info->config->outputs[k].output);
        else snprintf(media_paths[1],sizeof(media_paths[1]),"%s",info->config->output);
    }
}
void naki_test_lifecycle_close(void) {if(owner)assert(++owner->hooks==1);}
int naki_test_lifecycle_video(void) {
    if(fault("video")||fault("primary_trailer")) {owner->faults++;return AVERROR_EXTERNAL;}return 0;
}
AVCodecContext *__real_avcodec_alloc_context3(const AVCodec *);
AVCodecContext *__wrap_avcodec_alloc_context3(const AVCodec *codec) {
    AVCodecContext *p=__real_avcodec_alloc_context3(codec);if(owner&&p)owner->codecs++;return p;
}
void __real_avcodec_free_context(AVCodecContext **);
void __wrap_avcodec_free_context(AVCodecContext **p) {
    if(owner&&p&&*p)assert(++owner->codec_frees<=owner->codecs);
    __real_avcodec_free_context(p);
}
int __real_avcodec_open2(AVCodecContext *,const AVCodec *,AVDictionary **);
int __wrap_avcodec_open2(AVCodecContext *p,const AVCodec *codec,AVDictionary **options) {
    int fail=injected(av_codec_is_encoder(codec)?"encoder":"decoder",ENOMEM);
    return fail?fail:__real_avcodec_open2(p,codec,options);
}
int __real_avcodec_async_create(AVCodecAsyncContext **,AVCodecContext **,int,void (*)(void *),void *);
int __wrap_avcodec_async_create(AVCodecAsyncContext **p,AVCodecContext **codec,int cap,void (*notify)(void *),void *opaque) {
    int fail=injected("async",ENOMEM);if(fail)return fail;
    int ret=__real_avcodec_async_create(p,codec,cap,notify,opaque);
    if(owner&&ret>=0) {owner->asyncs++;owner->delegated++;assert(!*codec);}return ret;
}
void __real_avcodec_async_free(AVCodecAsyncContext **);
void __wrap_avcodec_async_free(AVCodecAsyncContext **p) {
    if(owner&&p&&*p) {owner->async_frees++;owner->codec_frees+=owner->delegated;owner->delegated=0;}
    __real_avcodec_async_free(p);
}
int __real_avcodec_async_receive_frame(AVCodecAsyncContext *,AVFrame *);
int __wrap_avcodec_async_receive_frame(AVCodecAsyncContext *p,AVFrame *f) {
    if(fault("empty")) {owner->faults++;return AVERROR_EOF;}
    if(owner&&owner->frames>=4) {int fail=injected("decode",EINVAL);if(fail)return fail;}
    int ret=__real_avcodec_async_receive_frame(p,f);if(owner&&ret>=0)owner->frames++;return ret;
}
AVFilterGraph *__real_avfilter_graph_alloc(void);
AVFilterGraph *__wrap_avfilter_graph_alloc(void) {AVFilterGraph *p=__real_avfilter_graph_alloc();if(owner&&p)owner->graphs++;return p;}
void __real_avfilter_graph_free(AVFilterGraph **);
void __wrap_avfilter_graph_free(AVFilterGraph **p) {
    if(owner&&p&&*p)assert(++owner->graph_frees<=owner->graphs);
    __real_avfilter_graph_free(p);
}
int __real_avfilter_graph_config(AVFilterGraph *,void *);
int __wrap_avfilter_graph_config(AVFilterGraph *p,void *opaque) {int fail=injected("filter",ENOMEM);return fail?fail:__real_avfilter_graph_config(p,opaque);}
AVFormatContext *__real_avformat_alloc_context(void);
AVFormatContext *__wrap_avformat_alloc_context(void) {AVFormatContext *p=__real_avformat_alloc_context();if(owner&&p)owner->input++;return p;}
int __real_avformat_open_input(AVFormatContext **,const char *,const AVInputFormat *,AVDictionary **);
int __wrap_avformat_open_input(AVFormatContext **p,const char *url,const AVInputFormat *fmt,AVDictionary **options) {
    int had=p&&*p;int ret=__real_avformat_open_input(p,url,fmt,options);
    if(owner&&had&&!*p)owner->input_frees++;
    return ret;
}
void __real_avformat_close_input(AVFormatContext **);
void __wrap_avformat_close_input(AVFormatContext **p) {
    if(owner&&p&&*p)owner->input_frees++;
    __real_avformat_close_input(p);
}
int __real_avformat_alloc_output_context2(AVFormatContext **,const AVOutputFormat *,const char *,const char *);
int __wrap_avformat_alloc_output_context2(AVFormatContext **p,const AVOutputFormat *fmt,const char *name,const char *url) {
    int fail=injected("output",ENOMEM);if(fail)return fail;
    int ret=__real_avformat_alloc_output_context2(p,fmt,name,url);if(owner&&*p)owner->formats++;return ret;
}
void __real_avformat_free_context(AVFormatContext *);
void __wrap_avformat_free_context(AVFormatContext *p) {
    if(owner&&p) {assert(owner->hooks);assert(++owner->format_frees<=owner->formats);}
    __real_avformat_free_context(p);
}
int __real_avio_open2(AVIOContext **,const char *,int,const AVIOInterruptCB *,AVDictionary **);
int __wrap_avio_open2(AVIOContext **p,const char *url,int flags,const AVIOInterruptCB *cb,AVDictionary **options) {
    int fail=injected("open",EACCES);if(fail)return fail;
    int ret=__real_avio_open2(p,url,flags,cb,options);if(owner&&*p)owner->avios++;return ret;
}
int __real_avio_closep(AVIOContext **);
int __wrap_avio_closep(AVIOContext **p) {
    if(owner&&p&&*p) {owner->avio_frees++;owner->closes++;}
    int ret=__real_avio_closep(p),fail=injected("close",ENOSPC);return fail?fail:ret;
}
AVIOContext *__real_avio_alloc_context(unsigned char *,int,int,void *,int (*)(void *,uint8_t *,int),int (*)(void *,const uint8_t *,int),int64_t (*)(void *,int64_t,int));
AVIOContext *__wrap_avio_alloc_context(unsigned char *buf,int size,int write,void *opaque,int (*read)(void *,uint8_t *,int),int (*out)(void *,const uint8_t *,int),int64_t (*seek)(void *,int64_t,int)) {
    if(injected("custom_io",ENOMEM))return NULL;
    AVIOContext *p=__real_avio_alloc_context(buf,size,write,opaque,read,out,seek);if(owner&&p)owner->avios++;return p;
}
void __real_avio_context_free(AVIOContext **);
void __wrap_avio_context_free(AVIOContext **p) {if(owner&&p&&*p)owner->avio_frees++;__real_avio_context_free(p);}
int __real_avformat_write_header(AVFormatContext *,AVDictionary **);
int __wrap_avformat_write_header(AVFormatContext *p,AVDictionary **options) {
    int fail=injected("header",EINVAL);if(fail)return fail;
    int ret=__real_avformat_write_header(p,options);if(owner&&ret>=0)owner->headers++;return ret;
}
int __real_av_write_trailer(AVFormatContext *p);
int __wrap_av_write_trailer(AVFormatContext *p) {
    if(owner) {assert(++owner->trailers==owner->headers);owner->finalizing=1;}
    int ret=__real_av_write_trailer(p),fail=injected("trailer",EIO);
    if(!fail)fail=injected("primary_trailer",EIO);
    return fail?fail:ret;
}
void __real_avio_flush(AVIOContext *);
void __wrap_avio_flush(AVIOContext *p) {
    __real_avio_flush(p);if(owner&&owner->finalizing&&injected("flush",ENOSPC))p->error=AVERROR(ENOSPC);
}
int __real_pthread_create(pthread_t *,const pthread_attr_t *,void *(*)(void *),void *);
int __wrap_pthread_create(pthread_t *t,const pthread_attr_t *a,void *(*fn)(void *),void *arg) {
    if(owner) {
        unsigned i=owner->creates++;
        if((fault("reader_thread")&&i==owner->outputs)||(fault("branch_thread")&&owner->outputs&&i==0)) {owner->faults++;return EAGAIN;}
    }
    return __real_pthread_create(t,a,fn,arg);
}
int __real_naki_branch_queue_init_limits(NakiBranchQueue *,unsigned,unsigned,uint64_t,int64_t,int64_t);
int __wrap_naki_branch_queue_init_limits(NakiBranchQueue *q,unsigned v,unsigned a,uint64_t b,int64_t media,int64_t lag) {
    if(owner&&owner->queues++==1) {int fail=injected("queue",ENOMEM);if(fail)return fail;}
    return __real_naki_branch_queue_init_limits(q,v,a,b,media,lag);
}
void naki_test_lifecycle_snapshot(yyjson_mut_doc *doc) {
    const char *directory=getenv("NAKI_TEST_LIFECYCLE_DIR");
    const char *kind=yyjson_mut_get_str(yyjson_mut_obj_get(yyjson_mut_doc_get_root(doc),"ipc_kind"));
    if(!directory||!kind||strcmp(kind,"finished")||reported)return;
    reported=1;unsigned threads=entries("/proc/self/task"),fds=entries("/proc/self/fd");assert(threads<initial_threads);
    /* The logger may still own the last trace segment until it processes finish. */
    assert(fds<=initial_fds+1);
    DIR *directory_fds=opendir("/proc/self/fd");assert(directory_fds);struct dirent *entry;
    while((entry=readdir(directory_fds)))if(entry->d_name[0]!='.') {
        char link[128],target[4096];assert(snprintf(link,sizeof(link),"/proc/self/fd/%s",entry->d_name)<(int)sizeof(link));
        ssize_t n=readlink(link,target,sizeof(target)-1);if(n<0)continue;target[n]=0;
        for(unsigned i=0;i<5;i++)assert(!*media_paths[i]||strcmp(target,media_paths[i]));
    }
    assert(!closedir(directory_fds));
    char path[4096];assert(snprintf(path,sizeof(path),"%s/lifecycle-audit.json",directory)<(int)sizeof(path));
    FILE *f=fopen(path,"wx");assert(f);fprintf(f,"{\"pid\":%ld,\"initial_threads\":%u,\"final_threads\":%u,\"initial_fds\":%u,\"final_fds\":%u,\"owners\":[",(long)getpid(),initial_threads,threads,initial_fds,fds);
    for(unsigned i=0;i<atomic_load(&count);i++) {
        Owner *o=&owners[i];assert(o->codecs==o->codec_frees&&o->graphs==o->graph_frees&&o->formats==o->format_frees);
        assert(o->input==o->input_frees&&o->avios==o->avio_frees&&o->asyncs==o->async_frees&&o->hooks==1&&o->headers==o->trailers);
        fprintf(f,"%s{\"id\":\"%s\",\"faults\":%u,\"hooks\":%u,\"headers\":%u,\"trailers\":%u,\"closes\":%u,\"codecs\":%u,\"graphs\":%u,\"formats\":%u,\"avios\":%u,\"inputs\":%u,\"asyncs\":%u,\"balanced\":true}",
            i?",":"",o->id,o->faults,o->hooks,o->headers,o->trailers,o->closes,o->codecs,o->graphs,o->formats,o->avios,o->input,o->asyncs);
    }
    assert(fprintf(f,"]}\n")>0&&!fclose(f));
}
