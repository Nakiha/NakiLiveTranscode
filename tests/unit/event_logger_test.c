#define _GNU_SOURCE
#include "observability/logger.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static atomic_int mode,blocked;
size_t __real_fwrite(const void *,size_t,size_t,FILE *);
static int kind(FILE *file,const char *tag) {
    char link[64],path[8192];snprintf(link,sizeof(link),"/proc/self/fd/%d",fileno(file));
    ssize_t bytes=readlink(link,path,sizeof(path)-1);if(bytes<0)return 0;path[bytes]=0;return strstr(path,tag)!=NULL;
}
size_t __wrap_fwrite(const void *data,size_t unit,size_t count,FILE *file) {
    int current=atomic_load(&mode),event=kind(file,".events."),trace=kind(file,".trace.");
    if((current==1&&event)||(current==4&&trace)) {
        atomic_store(&blocked,1);while(atomic_load(&mode)==current)usleep(1000);
    }
    if(current==2&&trace)usleep(2000);
    if(current==3&&event&&atomic_exchange(&mode,0)==3&&unit==1) {
        size_t part=count/2;__real_fwrite(data,unit,part,file);errno=ENOSPC;return part;
    }
    return __real_fwrite(data,unit,count,file);
}
static int64_t now_us(void) {struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (int64_t)t.tv_sec*1000000+t.tv_nsec/1000;}
static yyjson_mut_doc *record(const char *event) {
    yyjson_mut_doc *doc=yyjson_mut_doc_new(NULL);assert(doc);yyjson_mut_val *root=yyjson_mut_obj(doc);yyjson_mut_doc_set_root(doc,root);
    yyjson_mut_obj_add_str(doc,root,"event",event);
    yyjson_mut_obj_add_str(doc,root,"resource_uri","https://user:password@host.example/path?token=secret");return doc;
}
static LoggerOptions options(const char *directory,unsigned phase,unsigned capacity) {
    LoggerOptions o={.queue_capacity=capacity,.rotate_bytes=100U*1024*1024,.rotate_seconds=60,.event_rotate_seconds=60};
    snprintf(o.instance_id,sizeof(o.instance_id),"%032x",phase);strcpy(o.role,"worker");
    assert(snprintf(o.event_prefix,sizeof(o.event_prefix),"%s/worker-%s.events",directory,o.instance_id)<(int)sizeof(o.event_prefix));return o;
}
static void await_blocked(void) {int64_t end=now_us()+2000000;while(!atomic_load(&blocked)&&now_us()<end)usleep(1000);assert(atomic_load(&blocked));}
static void *producer(void *opaque) {Logger *l=opaque;for(int i=0;i<200;i++)logger_submit(l,NULL,record("concurrent"));return NULL;}
static unsigned validate(const char *file,const char *instance) {
    FILE *reader=fopen(file,"r");assert(reader);char *line=NULL;size_t capacity=0;uint64_t previous=0;unsigned count=0;
    while(getline(&line,&capacity,reader)>0) {
        yyjson_doc *doc=yyjson_read(line,strlen(line),0);assert(doc);yyjson_val *root=yyjson_doc_get_root(doc);
        uint64_t seq=yyjson_get_uint(yyjson_obj_get(root,"event_seq"));assert(seq>previous);previous=seq;
        assert(!strcmp(yyjson_get_str(yyjson_obj_get(root,"instance_id")),instance));
        assert(!strcmp(yyjson_get_str(yyjson_obj_get(root,"category")),"event"));
        assert(!strstr(line,"password")&&!strstr(line,"token=secret")&&!strstr(line,"/path"));
        count++;yyjson_doc_free(doc);
    }
    free(line);assert(!fclose(reader));return count;
}
int main(int argc,char **argv) {
    assert(argc==2);atomic_init(&mode,0);atomic_init(&blocked,0);
    LoggerOptions o=options(argv[1],1,8192);Logger *l=logger_start(&o);assert(l);
    pthread_t producers[4];for(int i=0;i<4;i++)assert(!pthread_create(&producers[i],NULL,producer,l));
    for(int i=0;i<4;i++)pthread_join(producers[i],NULL);
    LoggerStats stats;logger_get_stats(l,&stats);assert(stats.event_sequence==800&&!stats.dropped_status);logger_stop(l);
    char sealed[8192];snprintf(sealed,sizeof(sealed),"%s.000001.jsonl",o.event_prefix);assert(validate(sealed,o.instance_id)==800);

    o=options(argv[1],2,32);l=logger_start(&o);assert(l);char prefix[8192];snprintf(prefix,sizeof(prefix),"%s/job-2.trace",argv[1]);
    TraceSink *trace=logger_trace_open(l,prefix);assert(trace);atomic_store(&mode,4);atomic_store(&blocked,0);
    logger_submit(l,trace,record("trace"));await_blocked();
    for(int i=0;i<1000;i++)logger_submit(l,trace,record("trace"));
    logger_submit(l,NULL,record("reserved_event"));logger_get_stats(l,&stats);
    assert(stats.dropped_frames>0&&!stats.dropped_status&&stats.queued<=33);
    atomic_store(&mode,0);logger_trace_release(l,trace);logger_stop(l);

    o=options(argv[1],3,1024);o.event_rotate_seconds=1;l=logger_start(&o);assert(l);snprintf(prefix,sizeof(prefix),"%s/job-3.trace",argv[1]);trace=logger_trace_open(l,prefix);assert(trace);
    atomic_store(&mode,2);logger_submit(l,NULL,record("visible_while_trace_busy"));
    for(int i=0;i<900;i++)logger_submit(l,trace,record("trace"));
    char active[8192];snprintf(active,sizeof(active),"%s.000001.jsonl.active",o.event_prefix);
    snprintf(sealed,sizeof(sealed),"%s.000001.jsonl",o.event_prefix);
    int64_t began=now_us(),end=began+750000;struct stat st;int visible=0;
    /* The one-second rotation uses whole monotonic seconds. The event may
     * already be sealed when the observer runs across a second boundary. */
    while(now_us()<end) {if((!stat(active,&st)&&st.st_size>0)||(!stat(sealed,&st)&&st.st_size>0)) {visible=1;break;}usleep(1000);}
    assert(visible);logger_get_stats(l,&stats);assert(stats.queued>100);
    snprintf(sealed,sizeof(sealed),"%s.000001.jsonl",o.event_prefix);end=began+2000000;
    while(access(sealed,F_OK)&&now_us()<end)usleep(1000);
    assert(!access(sealed,F_OK));atomic_store(&mode,0);logger_trace_release(l,trace);logger_stop(l);

    o=options(argv[1],4,32);l=logger_start(&o);assert(l);atomic_store(&mode,1);atomic_store(&blocked,0);
    logger_submit(l,NULL,record("blocked_event"));await_blocked();began=now_us();
    for(int i=0;i<1000;i++)logger_submit(l,NULL,record("overflow_event"));
    assert(now_us()-began<1000000);logger_get_stats(l,&stats);assert(stats.dropped_status>0&&!stats.dropped_frames&&stats.event_sequence==1001);
    atomic_store(&mode,0);logger_stop(l);

    o=options(argv[1],5,32);l=logger_start(&o);assert(l);atomic_store(&mode,3);logger_submit(l,NULL,record("short_write"));
    end=now_us()+2000000;do {logger_get_stats(l,&stats);if(stats.io_errors)break;usleep(1000);}while(now_us()<end);
    assert(stats.io_errors&&stats.dropped_status);logger_submit(l,NULL,record("after_failure"));logger_stop(l);
    snprintf(active,sizeof(active),"%s.000001.jsonl.active",o.event_prefix);assert(!access(active,F_OK));
    snprintf(sealed,sizeof(sealed),"%s.000002.jsonl",o.event_prefix);assert(validate(sealed,o.instance_id)==1);

    o=options("/nonexistent/naki-events",6,32);assert(!logger_start(&o));
    puts("PASS event sequence/order, separate category reserve, busy trace flush/rotation, bounded producer progress, short-write loss/retry, startup failure and URI redaction");return 0;
}
