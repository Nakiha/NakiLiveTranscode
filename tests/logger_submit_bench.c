/* Manual microbenchmark; no media latency/capacity claim. See bench-logger.py. */
#define _GNU_SOURCE
#include "logger.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

static int64_t ns(void) {struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (int64_t)t.tv_sec*1000000000+t.tv_nsec;}
static _Thread_local int measured;
static _Thread_local int64_t wait_ns,hold_ns,locked_at;
static _Thread_local unsigned locks;
int __real_pthread_mutex_lock(pthread_mutex_t *);
int __real_pthread_mutex_unlock(pthread_mutex_t *);
int __wrap_pthread_mutex_lock(pthread_mutex_t *m) {
    int64_t began=measured?ns():0;int ret=__real_pthread_mutex_lock(m);
    if(measured) {locked_at=ns();wait_ns+=locked_at-began;locks++;}return ret;
}
int __wrap_pthread_mutex_unlock(pthread_mutex_t *m) {
    if(measured)hold_ns+=ns()-locked_at;
    return __real_pthread_mutex_unlock(m);
}
static atomic_int blocked,unblock;
static int block_writer;
static int output(void *opaque,const char *text,size_t size) {
    (void)opaque;(void)text;(void)size;
    if(block_writer) {atomic_store(&blocked,1);while(!atomic_load(&unblock))usleep(1000);}return 0;
}
typedef struct Sample {int64_t submit,wait,hold;unsigned locks;} Sample;
typedef struct Producer {Logger *l;Sample *samples;unsigned count;const char *payload;} Producer;
static pthread_barrier_t start;
static yyjson_mut_doc *record(const char *payload) {
    yyjson_mut_doc *doc=yyjson_mut_doc_new(NULL);assert(doc);yyjson_mut_val *root=yyjson_mut_obj(doc);yyjson_mut_doc_set_root(doc,root);
    assert(yyjson_mut_obj_add_str(doc,root,"event","bench"));assert(yyjson_mut_obj_add_str(doc,root,"payload",payload));
    assert(yyjson_mut_obj_add_str(doc,root,"uri","https://user:password@host/path?secret"));return doc;
}
static void *produce(void *opaque) {
    Producer *p=opaque;pthread_barrier_wait(&start);
    for(unsigned i=0;i<p->count;i++) {
        yyjson_mut_doc *doc=record(p->payload);wait_ns=hold_ns=0;locks=0;int64_t began=ns();measured=1;
        logger_submit(p->l,NULL,doc);measured=0;
        p->samples[i]=(Sample){ns()-began,wait_ns,hold_ns,locks};
    }
    return NULL;
}
static int compare(const void *a,const void *b) {int64_t x=*(const int64_t *)a,y=*(const int64_t *)b;return (x>y)-(x<y);}
static void distribution(const char *name,int64_t *values,unsigned count) {
    qsort(values,count,sizeof(*values),compare);double sum=0;for(unsigned i=0;i<count;i++)sum+=values[i];
    printf("\"%s_us\":{\"p50\":%.3f,\"p95\":%.3f,\"p99\":%.3f,\"max\":%.3f,\"mean\":%.3f}",
        name,values[count/2]/1000.,values[(count-1)*95/100]/1000.,values[(count-1)*99/100]/1000.,values[count-1]/1000.,sum/count/1000.);
}
int main(int argc,char **argv) {
    assert(argc==5);unsigned threads=(unsigned)atoi(argv[1]),count=(unsigned)atoi(argv[2]),bytes=(unsigned)atoi(argv[3]);block_writer=atoi(argv[4]);
    assert(threads&&threads<=32&&count&&count<=100000&&bytes<=60000);
    char *payload=malloc(bytes+1);assert(payload);memset(payload,'x',bytes);payload[bytes]=0;
    LoggerOptions o={.queue_capacity=block_writer?32:8192,.rotate_seconds=60,.rotate_bytes=1000000,.stdout_write=output};
    Logger *l=logger_start(&o);assert(l);
    if(block_writer) {
        logger_submit(l,NULL,record(payload));int64_t end=ns()+2000000000;
        while(!atomic_load(&blocked)&&ns()<end)usleep(1000);
        assert(atomic_load(&blocked));
    }
    unsigned total=threads*count;Sample *samples=calloc(total,sizeof(*samples));assert(samples);
    Producer p[32];pthread_t t[32];assert(!pthread_barrier_init(&start,NULL,threads+1));
    for(unsigned i=0;i<threads;i++) {p[i]=(Producer){l,samples+i*count,count,payload};assert(!pthread_create(&t[i],NULL,produce,&p[i]));}
    int64_t began=ns();pthread_barrier_wait(&start);
    for(unsigned i=0;i<threads;i++)pthread_join(t[i],NULL);
    int64_t produced=ns();atomic_store(&unblock,1);LoggerStats stats;
    int64_t end=ns()+10000000000;
    do {logger_get_stats(l,&stats);if(!stats.queued)break;usleep(100);}while(ns()<end);
    assert(!stats.queued&&stats.written+stats.dropped_status==total+(unsigned)block_writer);
    logger_stop(l);int64_t finished=ns();struct rusage usage;assert(!getrusage(RUSAGE_SELF,&usage));
    /* VmHWM resets at exec; ru_maxrss may also retain the launcher's RSS floor. */
    FILE *status=fopen("/proc/self/status","r");assert(status);char line[256];unsigned long hwm=0;
    while(fgets(line,sizeof(line),status))if(sscanf(line,"VmHWM: %lu kB",&hwm)==1)break;
    assert(hwm&&!fclose(status));
    printf("{\"threads\":%u,\"calls_per_thread\":%u,\"payload_bytes\":%u,\"blocked_writer\":%d,\"attempts\":%u,\"written\":%llu,\"dropped\":%llu,\"high_water\":%u,\"peak_rss_kib\":%ld,",
        threads,count,bytes,block_writer,total,(unsigned long long)stats.written,(unsigned long long)stats.dropped_status,stats.high_water,usage.ru_maxrss);
    printf("\"proc_peak_rss_kib\":%lu,\"producer_attempts_per_second\":%.3f,\"completed_records_per_second\":%.3f,",hwm,total*1e9/(produced-began),stats.written*1e9/(finished-began));
    int64_t *values=malloc(total*sizeof(*values));assert(values);uint64_t lock_count=0;
    for(unsigned i=0;i<total;i++) {values[i]=samples[i].submit;lock_count+=samples[i].locks;}distribution("submit",values,total);putchar(',');
    for(unsigned i=0;i<total;i++)values[i]=samples[i].wait;
    distribution("mutex_wait_sum",values,total);putchar(',');
    for(unsigned i=0;i<total;i++)values[i]=samples[i].hold;
    distribution("mutex_hold_sum",values,total);
    printf(",\"mutex_acquisitions\":%llu}\n",(unsigned long long)lock_count);
    free(values);free(samples);free(payload);pthread_barrier_destroy(&start);return 0;
}
