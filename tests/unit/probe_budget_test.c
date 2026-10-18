/* Controlled nonseekable AVIO: measure budgets and inject errors after progress. */
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavformat/avformat.h>
#include <libavutil/mem.h>
#include <libavutil/time.h>

typedef struct Input {
    uint8_t *data;
    int size, pos, armed, reads, eio, cancel, slow;
    int64_t deadline;
} Input;

static int interrupted(void *opaque) {
    Input *in=opaque;
    return av_gettime_relative()>=in->deadline || (in->armed && in->cancel && in->reads>=8);
}

static int read_input(void *opaque,uint8_t *buffer,int capacity) {
    Input *in=opaque;
    if(interrupted(in))return AVERROR_EXIT;
    if(in->armed && in->eio && in->reads>=8)return AVERROR(EIO);
    if(in->pos==in->size)return AVERROR_EOF;
    if(in->armed){in->reads++;if(in->slow)av_usleep(2000);}
    int count=FFMIN(FFMIN(capacity,188),in->size-in->pos);
    memcpy(buffer,in->data+in->pos,count);in->pos+=count;
    return count;
}

int main(int argc,char **argv) {
    if(argc!=4)return 2;
    FILE *file=fopen(argv[1],"rb");
    if(!file || fseek(file,0,SEEK_END))return 2;
    long length=ftell(file);
    if(length<=0 || length>32*1024*1024 || fseek(file,0,SEEK_SET))return 2;
    Input in={.size=(int)length,.deadline=av_gettime_relative()+10000000};
    in.data=av_malloc(length);
    if(!in.data || fread(in.data,1,length,file)!=(size_t)length)return 2;
    fclose(file);
    AVFormatContext *context=avformat_alloc_context();
    uint8_t *buffer=av_malloc(4096);
    AVIOContext *io=buffer ? avio_alloc_context(buffer,4096,0,&in,read_input,NULL,NULL) : NULL;
    if(!context || !io)return 2;
    context->pb=io;
    context->flags|=AVFMT_FLAG_CUSTOM_IO|AVFMT_FLAG_LIVE_VIDEO_PROBE;
    context->interrupt_callback=(AVIOInterruptCB){interrupted,&in};
    context->max_analyze_duration=60000000;
    context->probesize=1024*1024;
    int ret=avformat_open_input(&context,NULL,av_find_input_format(argv[2]),NULL);
    if(ret>=0) {
        if(!strcmp(argv[3],"media"))context->max_analyze_duration=100000;
        if(!strcmp(argv[3],"wall")){context->max_analyze_duration=40000;in.slow=1;}
        if(!strcmp(argv[3],"bytes"))context->probesize=4096;
        in.eio=!strcmp(argv[3],"eio");in.cancel=!strcmp(argv[3],"cancel");
        in.armed=1;in.reads=0;
        int start=in.pos;
        int64_t began=av_gettime_relative();
        ret=avformat_find_stream_info(context,NULL);
        printf("{\"ret\":%d,\"eio\":%d,\"exit\":%d,\"timeout\":%d,\"bytes_error\":%d,"
               "\"probe_reads\":%d,\"probe_bytes\":%d,\"total_bytes\":%d,\"file_bytes\":%d,\"probe_ms\":%.3f}\n",
               ret,AVERROR(EIO),AVERROR_EXIT,AVERROR(ETIMEDOUT),AVERROR(ENOBUFS),
               in.reads,in.pos-start,in.pos,in.size,(av_gettime_relative()-began)/1000.0);
    }
    avformat_close_input(&context);
    av_freep(&io->buffer);avio_context_free(&io);av_free(in.data);
    return ret<0 && !in.armed ? 1 : 0;
}
