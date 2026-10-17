#define _GNU_SOURCE
#include "logger.h"
#include "crash.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/syscall.h>

#define MAX_RECORD_BYTES 65536
#define CONTROL_RESERVE 16
typedef struct Record { char *text; size_t size; TraceSink *sink; LoggerCategory category; } Record;
struct TraceSink {
    struct TraceSink *next;
    char prefix[4096], active[4176], sealed[4160];
    FILE *file;
    unsigned segment, pending;
    int finished, released, failed;
    uint64_t bytes;
    int64_t opened_at, retry_at;
    unsigned rotate_seconds;
    uint64_t rotate_bytes;
};
struct Logger {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t ready;
    LoggerOptions options;
    LoggerStats stats;
    Record *queue;
    unsigned head, count, writing;
    int stopping;
    TraceSink *sinks;
    TraceSink *events;
    int event_lock_fd,startup_complete,startup_error;
    int64_t last_emergency;
};
static int64_t monotonic_seconds(void) {struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return ts.tv_sec;}
static int64_t monotonic_us(void) {struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);return (int64_t)ts.tv_sec*1000000+ts.tv_nsec/1000;}
static void io_error(Logger *l) {pthread_mutex_lock(&l->mutex); l->stats.io_errors++; pthread_mutex_unlock(&l->mutex);}
static void seal(Logger *l, TraceSink *s) {
    if (!s->file) return;
    int failed=s->failed || ferror(s->file);
    if(fflush(s->file))failed=1;
    if (fclose(s->file)) failed=1;
    s->file=NULL;
    /* A failed file stays .active and is never advertised to the compressor. */
    if (failed || rename(s->active,s->sealed)) io_error(l);
    else {pthread_mutex_lock(&l->mutex); l->stats.rotations++; pthread_mutex_unlock(&l->mutex);}
    s->bytes=0;
}
static int open_segment(Logger *l, TraceSink *s) {
    int64_t now=monotonic_seconds();
    if (now<s->retry_at) return -1;
    s->segment++;
    snprintf(s->sealed,sizeof(s->sealed),"%s.%06u.jsonl",s->prefix,s->segment);
    snprintf(s->active,sizeof(s->active),"%s.active",s->sealed);
    s->file=fopen(s->active,"wx");
    if (!s->file) {s->retry_at=now+1; io_error(l); return -1;}
    s->opened_at=now; s->bytes=0; s->failed=0; return 0;
}
static void maintenance(Logger *l, int shutdown) {
    pthread_mutex_lock(&l->mutex); TraceSink *s=l->sinks; pthread_mutex_unlock(&l->mutex);
    while (s) {
        TraceSink *next=s->next;
        pthread_mutex_lock(&l->mutex);
        int finish=s->finished && !s->pending;
        int release=finish && s->released;
        pthread_mutex_unlock(&l->mutex);
        if (shutdown || finish || (s->file && monotonic_seconds()-s->opened_at>=(int64_t)s->rotate_seconds)) seal(l,s);
        else if (s->file && fflush(s->file)) io_error(l);
        if (release || shutdown) {
            pthread_mutex_lock(&l->mutex);
            TraceSink **cursor=&l->sinks;
            while (*cursor && *cursor!=s) cursor=&(*cursor)->next;
            if (*cursor) *cursor=s->next;
            pthread_mutex_unlock(&l->mutex);
            free(s);
        }
        s=next;
    }
}
static void *writer_main(void *opaque) {
    (void)crash_thread_init();
    Logger *l=opaque;
    int startup=0;
    if(l->events) {
        char lock[4176];snprintf(lock,sizeof(lock),"%s.lock",l->events->prefix);
        l->event_lock_fd=open(lock,O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);
        char marker[34];snprintf(marker,sizeof(marker),"%s\n",l->options.instance_id);
        if(l->event_lock_fd<0||flock(l->event_lock_fd,LOCK_EX|LOCK_NB)||ftruncate(l->event_lock_fd,0)||
           write(l->event_lock_fd,marker,33)!=33||open_segment(l,l->events))startup=-1;
    }
    pthread_mutex_lock(&l->mutex);l->startup_error=startup;l->startup_complete=1;pthread_cond_broadcast(&l->ready);pthread_mutex_unlock(&l->mutex);
    if(startup) {if(l->event_lock_fd>=0)close(l->event_lock_fd);l->event_lock_fd=-1;return NULL;}
    int64_t maintained=monotonic_us();
    for (;;) {
        pthread_mutex_lock(&l->mutex);
        if (!l->count && !l->stopping) {
            struct timespec deadline; clock_gettime(CLOCK_REALTIME,&deadline);
            deadline.tv_nsec+=100000000;
            if(deadline.tv_nsec>=1000000000) {deadline.tv_sec++; deadline.tv_nsec-=1000000000;}
            pthread_cond_timedwait(&l->ready,&l->mutex,&deadline);
        }
        if (!l->count) {
            int stop=l->stopping; pthread_mutex_unlock(&l->mutex);
            maintenance(l,stop);
            if(stop) {if(l->event_lock_fd>=0)close(l->event_lock_fd);l->event_lock_fd=-1;break;}
            continue;
        }
        Record record=l->queue[l->head];
        l->head=(l->head+1)%l->options.queue_capacity; l->count--; l->writing=1;
        pthread_mutex_unlock(&l->mutex);
        FILE *file=stdout;
        TraceSink *s=record.sink;
        if (s) {
            if(s->file&&s->failed)seal(l,s);
            if(s->file && s->bytes && (s->bytes+record.size+1>s->rotate_bytes ||
               monotonic_seconds()-s->opened_at>=(int64_t)s->rotate_seconds)) seal(l,s);
            if (!s->file && open_segment(l,s)) file=NULL;
            else file=s->file;
        }
        int custom=!s && l->options.stdout_write;
        int failed=custom ? l->options.stdout_write(l->options.stdout_opaque,record.text,record.size)!=0 :
            !file || fwrite(record.text,1,record.size,file)!=record.size || fputc('\n',file)==EOF;
        if (!s && !custom && file && fflush(file)) failed=1;
        if (failed) {
            if(s)s->failed=1;
            io_error(l);
            int64_t now=monotonic_seconds();
            if(l->events&&now>l->last_emergency) {
                l->last_emergency=now;
                fprintf(stderr,"logger_io_error role=%s instance_id=%s\n",l->options.role,l->options.instance_id);
            }
        }
        else if(s) s->bytes+=record.size+1;
        free(record.text);
        pthread_mutex_lock(&l->mutex);
        if (failed) {if(record.category==LOGGER_FRAME_TRACE)l->stats.dropped_frames++; else l->stats.dropped_status++;}
        else l->stats.written++;
        if(s) s->pending--;
        l->writing=0;
        int idle=!l->count;
        pthread_mutex_unlock(&l->mutex);
        int64_t now=monotonic_us();
        if(idle||now-maintained>=(int64_t)l->options.flush_interval_ms*1000) {maintenance(l,0);maintained=now;}
    }
    return NULL;
}
Logger *logger_start(const LoggerOptions *options) {
    if(options->queue_capacity<=CONTROL_RESERVE || options->queue_capacity>8192 || !options->rotate_bytes || !options->rotate_seconds) return NULL;
    Logger *l=calloc(1,sizeof(*l));
    if(!l) return NULL;
    l->options=*options; l->stats.capacity=options->queue_capacity;
    l->event_lock_fd=-1;
    if(!l->options.flush_interval_ms)l->options.flush_interval_ms=100;
    if(*options->event_prefix) {
        if(strlen(options->instance_id)!=32||!*options->role) {free(l);return NULL;}
        l->events=calloc(1,sizeof(*l->events));if(!l->events) {free(l);return NULL;}
        strcpy(l->events->prefix,options->event_prefix);
        l->events->rotate_bytes=options->event_rotate_bytes?options->event_rotate_bytes:16U*1024*1024;
        l->events->rotate_seconds=options->event_rotate_seconds?options->event_rotate_seconds:3600;
        l->sinks=l->events;
    }
    l->queue=calloc(options->queue_capacity,sizeof(*l->queue));
    if(!l->queue) {free(l->events);free(l); return NULL;}
    if(pthread_mutex_init(&l->mutex,NULL)) {free(l->events);free(l->queue); free(l); return NULL;}
    if(pthread_cond_init(&l->ready,NULL)) {pthread_mutex_destroy(&l->mutex);free(l->events); free(l->queue); free(l); return NULL;}
    if(pthread_create(&l->thread,NULL,writer_main,l)) {pthread_cond_destroy(&l->ready); pthread_mutex_destroy(&l->mutex);free(l->events); free(l->queue); free(l); return NULL;}
    pthread_mutex_lock(&l->mutex);
    while(!l->startup_complete)pthread_cond_wait(&l->ready,&l->mutex);
    int startup_error=l->startup_error;pthread_mutex_unlock(&l->mutex);
    if(startup_error) {
        pthread_join(l->thread,NULL);pthread_cond_destroy(&l->ready);pthread_mutex_destroy(&l->mutex);
        free(l->events);free(l->queue);free(l);return NULL;
    }
    return l;
}
static void envelope(Logger *l,yyjson_mut_doc *doc,uint64_t sequence) {
    yyjson_mut_val *root=yyjson_mut_doc_get_root(doc);struct timespec now;clock_gettime(CLOCK_REALTIME,&now);
    struct tm utc;gmtime_r(&now.tv_sec,&utc);char stamp[40],date[24];strftime(date,sizeof(date),"%Y-%m-%dT%H:%M:%S",&utc);
    unsigned micros=(unsigned)(now.tv_nsec/1000)%1000000;
    snprintf(stamp,sizeof(stamp),"%s.%06uZ",date,micros);
    const char *keys[]={"schema_version","category","role","instance_id","pid","tid","event_seq","ts_utc","mono_us"};
    for(unsigned i=0;i<sizeof(keys)/sizeof(*keys);i++)yyjson_mut_obj_remove_key(root,keys[i]);
    yyjson_mut_obj_add_int(doc,root,"schema_version",1);yyjson_mut_obj_add_str(doc,root,"category","event");
    yyjson_mut_obj_add_strcpy(doc,root,"role",*l->options.role?l->options.role:"unknown");
    if(*l->options.instance_id)yyjson_mut_obj_add_strcpy(doc,root,"instance_id",l->options.instance_id);
    yyjson_mut_obj_add_int(doc,root,"pid",getpid());yyjson_mut_obj_add_sint(doc,root,"tid",syscall(SYS_gettid));
    yyjson_mut_obj_add_uint(doc,root,"event_seq",sequence);yyjson_mut_obj_add_strcpy(doc,root,"ts_utc",stamp);
    yyjson_mut_obj_add_sint(doc,root,"mono_us",monotonic_us());
}
static int redact(yyjson_mut_doc *doc,yyjson_mut_val *value,unsigned depth) {
    if(depth>24)return -1;
    if(yyjson_mut_is_str(value)) {
        const char *text=yyjson_mut_get_str(value),*scheme=strstr(text,"://");
        if(scheme) {
            const char *authority=scheme+3,*end=authority+strcspn(authority,"/?#"),*host=authority;
            for(const char *p=authority;p<end;p++)if(*p=='@')host=p+1;
            size_t prefix=(size_t)(scheme-text)+3,host_bytes=(size_t)(end-host);
            if(prefix+host_bytes+12>4096)return -1;
            char clean[4096];memcpy(clean,text,prefix);memcpy(clean+prefix,host,host_bytes);
            memcpy(clean+prefix+host_bytes,"/[redacted]",12);
            yyjson_mut_val *copy=yyjson_mut_strcpy(doc,clean);if(!copy)return -1;
            yyjson_mut_set_str(value,yyjson_mut_get_str(copy));
        }
    } else if(yyjson_mut_is_arr(value)) {
        size_t index,max;yyjson_mut_val *child;
        yyjson_mut_arr_foreach(value,index,max,child)if(redact(doc,child,depth+1))return -1;
    } else if(yyjson_mut_is_obj(value)) {
        size_t index,max;yyjson_mut_val *key,*child;
        yyjson_mut_obj_foreach(value,index,max,key,child)if(redact(doc,child,depth+1))return -1;
    }
    return 0;
}
void logger_submit_record(Logger *l,LoggerCategory category,TraceSink *sink,yyjson_mut_doc *doc) {
    pthread_mutex_lock(&l->mutex);
    int valid=doc&&yyjson_mut_is_obj(yyjson_mut_doc_get_root(doc));
    if(category==LOGGER_EVENT) {
        sink=l->events;
        if(l->stats.event_sequence==UINT64_MAX)valid=0;
        else if(valid)envelope(l,doc,++l->stats.event_sequence);
        else l->stats.event_sequence++;
    } else if(category==LOGGER_FRAME_TRACE&&valid&&sink&&sink!=l->events) {
        yyjson_mut_val *root=yyjson_mut_doc_get_root(doc);
        yyjson_mut_obj_remove_key(root,"category");yyjson_mut_obj_remove_key(root,"event_seq");
        yyjson_mut_obj_add_str(doc,root,"category","frame_trace");
    } else valid=0;
    if(valid&&category==LOGGER_EVENT&&redact(doc,yyjson_mut_doc_get_root(doc),0))valid=0;
    size_t size=0;
    char *text=valid ? yyjson_mut_write(doc,0,&size) : NULL;
    if(doc) yyjson_mut_doc_free(doc);
    unsigned limit=l->options.queue_capacity-(category==LOGGER_FRAME_TRACE ? CONTROL_RESERVE : 0);
    if(!text || size>MAX_RECORD_BYTES || l->stopping || l->count>=limit || (sink && sink->finished)) {
        if(category==LOGGER_FRAME_TRACE) l->stats.dropped_frames++; else l->stats.dropped_status++;
        pthread_mutex_unlock(&l->mutex); free(text); return;
    }
    l->queue[(l->head+l->count)%l->options.queue_capacity]=(Record){text,size,sink,category};
    l->count++;
    if(sink) sink->pending++;
    if(l->count>l->stats.high_water) l->stats.high_water=l->count;
    pthread_cond_signal(&l->ready); pthread_mutex_unlock(&l->mutex);
}
void logger_submit(Logger *l,TraceSink *sink,yyjson_mut_doc *doc) {logger_submit_record(l,sink?LOGGER_FRAME_TRACE:LOGGER_EVENT,sink,doc);}
TraceSink *logger_trace_open(Logger *l, const char *prefix) {
    if(strlen(prefix)>=4096) return NULL;
    TraceSink *s=calloc(1,sizeof(*s));
    if(!s) return NULL;
    strcpy(s->prefix,prefix);
    s->rotate_bytes=l->options.rotate_bytes;s->rotate_seconds=l->options.rotate_seconds;
    pthread_mutex_lock(&l->mutex); s->next=l->sinks; l->sinks=s; pthread_mutex_unlock(&l->mutex);
    return s;
}
void logger_trace_finish(Logger *l, TraceSink *s) {
    if(!s)return;
    pthread_mutex_lock(&l->mutex); s->finished=1; pthread_cond_signal(&l->ready); pthread_mutex_unlock(&l->mutex);
}
void logger_trace_release(Logger *l, TraceSink *s) {
    if(!s)return;
    pthread_mutex_lock(&l->mutex); s->finished=1; s->released=1; pthread_cond_signal(&l->ready); pthread_mutex_unlock(&l->mutex);
}
void logger_get_stats(Logger *l, LoggerStats *out) {
    pthread_mutex_lock(&l->mutex); *out=l->stats; out->queued=l->count+l->writing; pthread_mutex_unlock(&l->mutex);
}
void logger_stop(Logger *l) {
    if(!l)return;
    pthread_mutex_lock(&l->mutex); l->stopping=1; pthread_cond_signal(&l->ready); pthread_mutex_unlock(&l->mutex);
    pthread_join(l->thread,NULL);
    pthread_cond_destroy(&l->ready); pthread_mutex_destroy(&l->mutex); free(l->queue); free(l);
}
