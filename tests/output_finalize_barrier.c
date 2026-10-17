#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libavformat/avformat.h>
#include <libavutil/time.h>

/* Pause after the real trailer returns, before the application's flush/close. */
int av_write_trailer(AVFormatContext *context) {
    static int (*real_trailer)(AVFormatContext *);
    if(!real_trailer)*(void **)(&real_trailer)=dlsym(RTLD_NEXT,"av_write_trailer");
    int result=real_trailer(context);
    const char *ready=getenv("NAKI_TEST_FINALIZE_READY"),*release=getenv("NAKI_TEST_FINALIZE_RELEASE");
    if(ready&&release&&context->url&&!strncmp(context->url,"http://",7)) {
        int fd=open(ready,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);if(fd>=0)close(fd);
        int64_t until=av_gettime_relative()+5000000;
        while(access(release,F_OK)&&av_gettime_relative()<until)usleep(1000);
    }
    return result;
}
