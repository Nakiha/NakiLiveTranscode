#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <libavformat/avformat.h>
#include <libavformat/network_observer.h>
#include <libavutil/error.h>
static int calls, failures, valid=1;
static void observer(void *opaque,const AVNetworkObservation *e) {
    (void)opaque;
    if(e->version!=1 || e->completed_us<e->started_us ||
       (e->syscall_start_us && (e->syscall_start_us<e->started_us || e->syscall_start_us>e->completed_us)))valid=0;
    calls++;
    if(e->result<0)failures++;
    errno=EDOM; /* The instrumentation must preserve errno around user callbacks. */
}
int main(int argc,char **argv) {
    if(argc!=2)return 2;
    avformat_network_init(); avformat_set_network_observer(observer,NULL);
    AVIOContext *io=NULL; AVDictionary *options=NULL;
    av_dict_set(&options,"rw_timeout","200000",0);
    int ret=avio_open2(&io,argv[1],AVIO_FLAG_READ,NULL,&options);
    av_dict_free(&options);
    if(ret>=0) {unsigned char byte; ret=avio_read(io,&byte,1);}
    avio_closep(&io); avformat_set_network_observer(NULL,NULL); avformat_network_deinit();
    printf("{\"calls\":%d,\"failures\":%d,\"valid\":%d,\"result\":%d}\n",calls,failures,valid,ret);
    return valid && calls>0 && failures>0 && ret==AVERROR(ETIMEDOUT) ? 0 : 1;
}
