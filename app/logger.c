#include "logger.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAX_RECORD_BYTES 65536
#define CONTROL_RESERVE 16
typedef struct Record { char *text; size_t size; TraceSink *sink; } Record;
struct TraceSink {
    struct TraceSink *next;
    char prefix[4096], active[4176], sealed[4160];
    FILE *file;
    unsigned segment, pending;
    int finished, released, failed;
    uint64_t bytes;
    int64_t opened_at, retry_at;
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
};
static int64_t monotonic_seconds(void) {struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return ts.tv_sec;}
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
        if (shutdown || finish || (s->file && monotonic_seconds()-s->opened_at>=(int64_t)l->options.rotate_seconds)) seal(l,s);
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
    Logger *l=opaque;
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
            if(stop) break;
            continue;
        }
        Record record=l->queue[l->head];
        l->head=(l->head+1)%l->options.queue_capacity; l->count--; l->writing=1;
        pthread_mutex_unlock(&l->mutex);
        FILE *file=stdout;
        TraceSink *s=record.sink;
        if (s) {
            if(s->file && s->bytes && (s->bytes+record.size+1>l->options.rotate_bytes ||
               monotonic_seconds()-s->opened_at>=(int64_t)l->options.rotate_seconds)) seal(l,s);
            if (!s->file && open_segment(l,s)) file=NULL;
            else file=s->file;
        }
        int custom=!s && l->options.stdout_write;
        int failed=custom ? l->options.stdout_write(l->options.stdout_opaque,record.text,record.size)!=0 :
            !file || fwrite(record.text,1,record.size,file)!=record.size || fputc('\n',file)==EOF;
        if (!s && !custom && file && fflush(file)) failed=1;
        if (failed) {if(s)s->failed=1; io_error(l);}
        else if(s) s->bytes+=record.size+1;
        free(record.text);
        pthread_mutex_lock(&l->mutex);
        if (failed) {if(s)l->stats.dropped_frames++; else l->stats.dropped_status++;}
        else l->stats.written++;
        if(s) s->pending--;
        l->writing=0;
        int idle=!l->count;
        pthread_mutex_unlock(&l->mutex);
        if(idle) maintenance(l,0);
    }
    return NULL;
}
Logger *logger_start(const LoggerOptions *options) {
    if(options->queue_capacity<=CONTROL_RESERVE || options->queue_capacity>8192 || !options->rotate_bytes || !options->rotate_seconds) return NULL;
    Logger *l=calloc(1,sizeof(*l));
    if(!l) return NULL;
    l->options=*options; l->stats.capacity=options->queue_capacity;
    l->queue=calloc(options->queue_capacity,sizeof(*l->queue));
    if(!l->queue) {free(l); return NULL;}
    if(pthread_mutex_init(&l->mutex,NULL)) {free(l->queue); free(l); return NULL;}
    if(pthread_cond_init(&l->ready,NULL)) {pthread_mutex_destroy(&l->mutex); free(l->queue); free(l); return NULL;}
    if(pthread_create(&l->thread,NULL,writer_main,l)) {pthread_cond_destroy(&l->ready); pthread_mutex_destroy(&l->mutex); free(l->queue); free(l); return NULL;}
    return l;
}
void logger_submit(Logger *l, TraceSink *sink, yyjson_mut_doc *doc) {
    size_t size=0;
    char *text=doc ? yyjson_mut_write(doc,0,&size) : NULL;
    if(doc) yyjson_mut_doc_free(doc);
    pthread_mutex_lock(&l->mutex);
    unsigned limit=l->options.queue_capacity-(sink ? CONTROL_RESERVE : 0);
    if(!text || size>MAX_RECORD_BYTES || l->stopping || l->count>=limit || (sink && sink->finished)) {
        if(sink) l->stats.dropped_frames++; else l->stats.dropped_status++;
        pthread_mutex_unlock(&l->mutex); free(text); return;
    }
    l->queue[(l->head+l->count)%l->options.queue_capacity]=(Record){text,size,sink};
    l->count++;
    if(sink) sink->pending++;
    if(l->count>l->stats.high_water) l->stats.high_water=l->count;
    pthread_cond_signal(&l->ready); pthread_mutex_unlock(&l->mutex);
}
TraceSink *logger_trace_open(Logger *l, const char *prefix) {
    if(strlen(prefix)>=4096) return NULL;
    TraceSink *s=calloc(1,sizeof(*s));
    if(!s) return NULL;
    strcpy(s->prefix,prefix);
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
