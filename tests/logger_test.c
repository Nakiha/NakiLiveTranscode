#define _POSIX_C_SOURCE 200809L
#include "logger.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static void *drain(void *opaque) {
    int fd=*(int *)opaque;
    char buffer[4096];
    while(read(fd,buffer,sizeof(buffer))>0) {}
    return NULL;
}
static yyjson_mut_doc *record(void) {
    yyjson_mut_doc *doc=yyjson_mut_doc_new(NULL);
    yyjson_mut_val *obj=yyjson_mut_obj(doc); yyjson_mut_doc_set_root(doc,obj);
    yyjson_mut_obj_add_str(doc,obj,"event","bounded_queue_test"); return doc;
}
int main(int argc,char **argv) {
    assert(argc==2);
    int descriptors[2]; assert(pipe(descriptors)==0);
    int saved=dup(STDOUT_FILENO); assert(saved>=0);
    assert(dup2(descriptors[1],STDOUT_FILENO)>=0); close(descriptors[1]);
    int flags=fcntl(STDOUT_FILENO,F_GETFL);
    assert(fcntl(STDOUT_FILENO,F_SETFL,flags|O_NONBLOCK)==0);
    char bytes[4096]={0};
    while(write(STDOUT_FILENO,bytes,sizeof(bytes))>0) {}
    assert(errno==EAGAIN);
    assert(fcntl(STDOUT_FILENO,F_SETFL,flags)==0);
    LoggerOptions options={.queue_capacity=32,.rotate_seconds=1,.rotate_bytes=1024};
    Logger *logger=logger_start(&options); assert(logger);
    TraceSink *sink=logger_trace_open(logger,argv[1]); assert(sink);
    logger_submit(logger,NULL,record()); /* Writer blocks on a deliberately full stdout pipe. */
    for(int i=0;i<1000;i++)logger_submit(logger,sink,record());
    logger_submit(logger,NULL,record());
    LoggerStats stats; logger_get_stats(logger,&stats);
    assert(stats.dropped_frames>0 && !stats.dropped_status && stats.high_water<=32 && stats.queued<=33);
    logger_trace_finish(logger,sink); logger_trace_release(logger,sink);
    pthread_t reader; assert(pthread_create(&reader,NULL,drain,&descriptors[0])==0);
    logger_stop(logger);
    fflush(stdout); assert(dup2(saved,STDOUT_FILENO)>=0); close(saved);
    pthread_join(reader,NULL); close(descriptors[0]);
    puts("PASS bounded logger queue, reserved status capacity, producer progress under blocked stdout, drain and seal");
    return 0;
}
