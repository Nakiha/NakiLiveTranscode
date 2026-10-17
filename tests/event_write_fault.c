#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static size_t (*real_fwrite)(const void *,size_t,size_t,FILE *);
__attribute__((constructor)) static void initialize(void) {
    void *symbol=dlsym(RTLD_NEXT,"fwrite");memcpy(&real_fwrite,&symbol,sizeof(real_fwrite));
}
size_t fwrite(const void *data,size_t size,size_t count,FILE *stream) {
    if(getenv("NAKI_TEST_EVENT_WRITE_FAILURE")) {
        char fd[64],path[4096];snprintf(fd,sizeof(fd),"/proc/self/fd/%d",fileno(stream));
        ssize_t length=readlink(fd,path,sizeof(path)-1);
        if(length>0) {
            path[length]=0;
            if(strstr(path,"/events/")&&strstr(path,".events.")&&strstr(path,".jsonl.active")) {errno=ENOSPC;return 0;}
        }
    }
    return real_fwrite(data,size,count,stream);
}
