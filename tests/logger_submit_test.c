#define _GNU_SOURCE
#include "logger.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

/* Pause preparation, not the queue mutex or writer, at a reproducible boundary. */
static atomic_int entered[33],resume[33],freed,serialized;
static _Thread_local int gate,fail_write;
static _Thread_local Logger *stopping_logger;
static atomic_int stop_entered;
static LoggerStats stop_stats;
int __real_pthread_join(pthread_t,void **);
int __wrap_pthread_join(pthread_t thread,void **result) {
    if(stopping_logger)atomic_store(&stop_entered,1);
    int ret=__real_pthread_join(thread,result);
    if(stopping_logger)logger_get_stats(stopping_logger,&stop_stats);
    return ret;
}
char *__real_yyjson_mut_val_write_opts(const yyjson_mut_val *,yyjson_write_flag,const yyjson_alc *,size_t *,yyjson_write_err *);
char *__wrap_yyjson_mut_val_write_opts(const yyjson_mut_val *v,yyjson_write_flag f,const yyjson_alc *a,size_t *n,yyjson_write_err *e) {
    atomic_fetch_add(&serialized,1);
    if(gate) {
        atomic_store(&entered[gate],1);
        while(!atomic_load(&resume[gate]))usleep(1000);
    }
    if(fail_write)return NULL;
    return __real_yyjson_mut_val_write_opts(v,f,a,n,e);
}
void __real_yyjson_mut_doc_free(yyjson_mut_doc *);
void __wrap_yyjson_mut_doc_free(yyjson_mut_doc *doc) {
    atomic_fetch_add(&freed,1);__real_yyjson_mut_doc_free(doc);
}
static int64_t now_us(void) {struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (int64_t)t.tv_sec*1000000+t.tv_nsec/1000;}
static void await(atomic_int *value,int expected) {
    int64_t end=now_us()+2000000;
    while(atomic_load(value)!=expected&&now_us()<end)usleep(1000);
    assert(atomic_load(value)==expected);
}
typedef struct Capture {
    atomic_int count,block,blocked;
    uint64_t seq[256];int64_t mono[256],tid[256];
} Capture;
static int capture(void *opaque,const char *text,size_t size) {
    Capture *c=opaque;
    if(atomic_load(&c->block)) {atomic_store(&c->blocked,1);while(atomic_load(&c->block))usleep(1000);}
    int i=atomic_load(&c->count);assert(i<256);
    yyjson_doc *doc=yyjson_read(text,size,0);assert(doc);yyjson_val *root=yyjson_doc_get_root(doc);
    c->seq[i]=yyjson_get_uint(yyjson_obj_get(root,"event_seq"));
    c->mono[i]=yyjson_get_sint(yyjson_obj_get(root,"mono_us"));
    c->tid[i]=yyjson_get_sint(yyjson_obj_get(root,"tid"));
    assert(yyjson_get_sint(yyjson_obj_get(root,"pid"))==getpid());
    assert(!strstr(text,"password")&&!strstr(text,"secret"));
    yyjson_doc_free(doc);atomic_store(&c->count,i+1);return 0;
}
static yyjson_mut_doc *record(void) {
    yyjson_mut_doc *doc=yyjson_mut_doc_new(NULL);assert(doc);
    yyjson_mut_val *root=yyjson_mut_obj(doc);yyjson_mut_doc_set_root(doc,root);
    assert(yyjson_mut_obj_add_str(doc,root,"event","submit_test"));
    assert(yyjson_mut_obj_add_str(doc,root,"uri","https://user:password@host/path?secret"));return doc;
}
static Logger *start(Capture *c,unsigned capacity) {
    LoggerOptions o={.queue_capacity=capacity,.rotate_seconds=60,.rotate_bytes=1000000,.stdout_write=capture,.stdout_opaque=c};
    Logger *l=logger_start(&o);assert(l);return l;
}
typedef struct Producer {Logger *l;TraceSink *sink;int gate;int64_t tid;} Producer;
static void *produce(void *opaque) {
    Producer *p=opaque;gate=p->gate;p->tid=syscall(SYS_gettid);
    logger_submit(p->l,p->sink,record());return NULL;
}
static pthread_t launch(Producer *p,Logger *l,TraceSink *sink,int id) {
    *p=(Producer){l,sink,id,0};atomic_store(&entered[id],0);atomic_store(&resume[id],0);
    pthread_t t;assert(!pthread_create(&t,NULL,produce,p));await(&entered[id],1);return t;
}
static void drain(Logger *l) {
    LoggerStats s;int64_t end=now_us()+2000000;
    do {logger_get_stats(l,&s);if(!s.queued)return;usleep(1000);}while(now_us()<end);
    assert(!s.queued);
}
static void ordered(void) {
    Capture c={0};Logger *l=start(&c,64);Producer p;
    int before=atomic_load(&freed);pthread_t t=launch(&p,l,NULL,1);
    int64_t begin=now_us();LoggerStats s;logger_get_stats(l,&s);assert(s.queued==1&&s.event_sequence==1);
    logger_submit(l,NULL,record());assert(now_us()-begin<500000);usleep(20000);assert(!atomic_load(&c.count));
    atomic_store(&resume[1],1);pthread_join(t,NULL);drain(l);logger_get_stats(l,&s);
    assert(s.written==2&&!s.dropped_status&&atomic_load(&freed)==before+2);logger_stop(l);
    assert(c.seq[0]==1&&c.seq[1]==2&&c.mono[0]<=c.mono[1]);
    assert(c.tid[0]==p.tid&&c.tid[1]==syscall(SYS_gettid));
}
static void full_queue(void) {
    Capture c={0};atomic_store(&c.block,1);Logger *l=start(&c,32);LoggerStats s;
    int before=atomic_load(&freed);logger_submit(l,NULL,record());await(&c.blocked,1);
    for(int i=0;i<32;i++)logger_submit(l,NULL,record());
    int writes=atomic_load(&serialized);int64_t began=now_us();
    for(int i=0;i<100;i++)logger_submit(l,NULL,record());
    assert(now_us()-began<500000&&atomic_load(&serialized)==writes);
    logger_get_stats(l,&s);assert(s.queued==33&&s.high_water==32&&s.event_sequence==133&&s.dropped_status==100);
    atomic_store(&c.block,0);drain(l);logger_submit(l,NULL,record());drain(l);logger_get_stats(l,&s);
    assert(s.written==34&&s.dropped_status==100&&atomic_load(&freed)==before+134);logger_stop(l);
    assert(c.seq[32]==33&&c.seq[33]==134);
}
static void prepare_limit(const char *directory) {
    Capture c={0};Logger *l=start(&c,128);Producer p[32];pthread_t t[32];char prefix[8192];
    snprintf(prefix,sizeof(prefix),"%s/prepare.trace",directory);TraceSink *sink=logger_trace_open(l,prefix);assert(sink);
    for(int i=0;i<16;i++)t[i]=launch(&p[i],l,sink,i+1);
    int writes=atomic_load(&serialized);logger_submit(l,sink,record());assert(atomic_load(&serialized)==writes);
    for(int i=16;i<32;i++)t[i]=launch(&p[i],l,NULL,i+1);
    writes=atomic_load(&serialized);logger_submit(l,NULL,record());assert(atomic_load(&serialized)==writes);
    LoggerStats s;logger_get_stats(l,&s);assert(s.queued==32&&s.dropped_frames==1&&s.dropped_status==1&&s.event_sequence==17);
    /* Release while preparing: pending reservations must keep the sink alive. */
    logger_trace_finish(l,sink);logger_trace_release(l,sink);usleep(150000);
    for(int i=31;i>=0;i--)atomic_store(&resume[i+1],1);
    for(int i=0;i<32;i++)pthread_join(t[i],NULL);
    drain(l);logger_get_stats(l,&s);assert(s.dropped_frames==17&&s.dropped_status==1&&s.written==16);logger_stop(l);
    for(int i=0;i<16;i++)assert(c.seq[i]==(unsigned)i+1);
    char file[8300];snprintf(file,sizeof(file),"%s.000001.jsonl",prefix);assert(access(file,F_OK));
}
static atomic_int stop_done;
static void *stop(void *opaque) {stopping_logger=opaque;logger_stop(opaque);atomic_store(&stop_done,1);return NULL;}
static void stopping(void) {
    Capture c={0};Logger *l=start(&c,64);Producer p;pthread_t producer=launch(&p,l,NULL,1),stopper;
    assert(!pthread_create(&stopper,NULL,stop,l));await(&stop_entered,1);usleep(20000);assert(!atomic_load(&stop_done));
    atomic_store(&resume[1],1);pthread_join(producer,NULL);pthread_join(stopper,NULL);
    assert(atomic_load(&stop_done)&&!atomic_load(&c.count));
    assert(stop_stats.dropped_status==1&&stop_stats.event_sequence==1&&!stop_stats.written&&!stop_stats.queued);
}
static yyjson_mut_doc *payload(size_t bytes,char fill) {
    yyjson_mut_doc *doc=yyjson_mut_doc_new(NULL);assert(doc);yyjson_mut_val *root=yyjson_mut_obj(doc);yyjson_mut_doc_set_root(doc,root);
    char *text=malloc(bytes+1);assert(text);memset(text,fill,bytes);text[bytes]=0;
    assert(yyjson_mut_obj_add_strncpy(doc,root,"payload",text,bytes));free(text);return doc;
}
static void budgets(const char *directory) {
    Capture c={0};Logger *l=start(&c,64);char prefix[8192];snprintf(prefix,sizeof(prefix),"%s/boundary.trace",directory);
    TraceSink *sink=logger_trace_open(l,prefix);assert(sink);int before=atomic_load(&freed);
    size_t overhead=strlen("{\"payload\":\"\",\"category\":\"frame_trace\"}");
    logger_submit(l,sink,payload(65536-overhead,'a'));drain(l);
    logger_submit(l,sink,payload(65537-overhead,'a'));
    logger_submit(l,sink,payload(65536,'\001')); /* Expanded JSON exceeds the record limit. */
    logger_submit(l,sink,payload(250000,'\001')); /* Writer growth exceeds the scratch cap. */
    logger_submit(l,sink,payload(256*1024+1,'a'));
    yyjson_mut_doc *doc=record();yyjson_mut_val *v=yyjson_mut_doc_get_root(doc);
    for(int i=0;i<25;i++) {yyjson_mut_val *child=yyjson_mut_obj(doc);assert(yyjson_mut_obj_add_val(doc,v,"deep",child));v=child;}
    logger_submit(l,sink,doc);
    doc=record();yyjson_mut_val *array=yyjson_mut_arr(doc);assert(yyjson_mut_obj_add_val(doc,yyjson_mut_doc_get_root(doc),"nodes",array));
    for(int i=0;i<65536;i++)assert(yyjson_mut_arr_add_null(doc,array));
    logger_submit(l,sink,doc);
    doc=record();assert(yyjson_mut_obj_add_str(doc,yyjson_mut_doc_get_root(doc),"invalid_utf8","\xff"));logger_submit(l,NULL,doc);
    fail_write=1;logger_submit(l,NULL,record());fail_write=0;
    logger_submit_record(l,(LoggerCategory)99,NULL,record());
    logger_submit(l,NULL,NULL);drain(l);
    LoggerStats s;logger_get_stats(l,&s);assert(s.written==1&&s.dropped_frames==6&&s.dropped_status==4&&s.event_sequence==3);
    /* NULL has no document to free; every other consumed input is freed once. */
    assert(atomic_load(&freed)==before+10);
    logger_trace_release(l,sink);logger_stop(l);
    char file[8300];snprintf(file,sizeof(file),"%s.000001.jsonl",prefix);struct stat st;assert(!stat(file,&st)&&st.st_size==65537);
}
static void preparing_maintenance(const char *directory) {
    LoggerOptions o={.queue_capacity=64,.rotate_seconds=60,.rotate_bytes=1000000,.event_rotate_seconds=1,.flush_interval_ms=10};
    strcpy(o.role,"worker");strcpy(o.instance_id,"12345678901234567890123456789012");
    snprintf(o.event_prefix,sizeof(o.event_prefix),"%s/maintenance.events",directory);Logger *l=logger_start(&o);assert(l);
    logger_submit(l,NULL,record());drain(l);Producer p;pthread_t t=launch(&p,l,NULL,1);
    char file[8300];snprintf(file,sizeof(file),"%s.000001.jsonl",o.event_prefix);
    int64_t end=now_us()+2000000;while(access(file,F_OK)&&now_us()<end)usleep(1000);assert(!access(file,F_OK));
    atomic_store(&resume[1],1);pthread_join(t,NULL);logger_stop(l);
}
int main(int argc,char **argv) {
    assert(argc==2);ordered();full_queue();prepare_limit(argv[1]);stopping();budgets(argv[1]);preparing_maintenance(argv[1]);
    puts("PASS ordered reservations, unlocked preparation, bounded preparation/queue/scratch, exact loss/ownership, sink release/stop races and maintenance");return 0;
}
