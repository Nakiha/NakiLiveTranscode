#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/time.h>

static int64_t deadline;
static int interrupted(void *unused) {(void)unused;return av_gettime_relative()>deadline;}
static int read_exact(FILE *f,uint8_t *b,size_t n) {return fread(b,1,n,f)==n;}
static void pace(int64_t due) {
    for(;;) {int64_t left=due-av_gettime_relative();if(left<=0)return;av_usleep((unsigned)FFMIN(10000,left));}
}
static int decode(AVCodecContext *codec,AVPacket *pkt,AVFrame *frame,uint64_t *count) {
    int ret=avcodec_send_packet(codec,pkt);
    if(ret<0)return ret;
    while((ret=avcodec_receive_frame(codec,frame))>=0){(*count)++;av_frame_unref(frame);}
    return ret==AVERROR(EAGAIN) || ret==AVERROR_EOF ? 0 : ret;
}
static int inspect(const char *path) {
    AVFormatContext *input=NULL;AVCodecContext *codecs[64]={0};
    uint64_t frames[64]={0},packets[64]={0};
    AVPacket *packet=av_packet_alloc();AVFrame *frame=av_frame_alloc();
    if(!packet || !frame || avformat_open_input(&input,path,NULL,NULL)<0)return 10;
    int ret=0;
    /* Deliberately no find_stream_info: discover and open every decoder as its
       packets arrive, including tracks that do not exist at initial probing. */
    while((ret=av_read_frame(input,packet))>=0) {
        int i=packet->stream_index;
        if(i>=64){ret=AVERROR(ENOSPC);break;}
        AVStream *stream=input->streams[i];
        if(stream->codecpar->codec_type!=AVMEDIA_TYPE_AUDIO && stream->codecpar->codec_type!=AVMEDIA_TYPE_VIDEO) {
            av_packet_unref(packet);continue;
        }
        if(!codecs[i]) {
            const AVCodec *codec=avcodec_find_decoder(stream->codecpar->codec_id);
            codecs[i]=codec ? avcodec_alloc_context3(codec) : NULL;
            if(!codecs[i]){ret=AVERROR_DECODER_NOT_FOUND;break;}
            ret=avcodec_parameters_to_context(codecs[i],stream->codecpar);
            codecs[i]->thread_count=2;codecs[i]->pkt_timebase=stream->time_base;
            if(ret>=0)ret=avcodec_open2(codecs[i],codec,NULL);
            if(ret<0)break;
        }
        packets[i]++;ret=decode(codecs[i],packet,frame,&frames[i]);av_packet_unref(packet);
        if(ret<0)break;
    }
    if(ret==AVERROR_EOF)ret=0;
    for(unsigned i=0;i<input->nb_streams && i<64;i++)if(codecs[i] && ret>=0)ret=decode(codecs[i],NULL,frame,&frames[i]);
    printf("{\"streams\":[");int comma=0;
    for(unsigned i=0;i<input->nb_streams && i<64;i++)if(codecs[i]) {
        printf("%s{\"index\":%u,\"codec_name\":\"%s\",\"sample_rate\":%d,\"channels\":%d,\"nb_read_frames\":\"%llu\",\"nb_read_packets\":\"%llu\"}",
               comma++ ? "," : "",i,avcodec_get_name(input->streams[i]->codecpar->codec_id),codecs[i]->sample_rate,
               codecs[i]->ch_layout.nb_channels,(unsigned long long)frames[i],(unsigned long long)packets[i]);
    }
    printf("],\"error\":%d}\n",ret);
    for(int i=0;i<64;i++)avcodec_free_context(&codecs[i]);
    av_frame_free(&frame);av_packet_free(&packet);avformat_close_input(&input);
    return ret<0 ? 11 : 0;
}
int main(int argc,char **argv) {
    if(argc==3 && !strcmp(argv[1],"inspect"))return inspect(argv[2]);
    if(argc!=5&&argc!=6&&argc!=7)return 2;
    long timeout_seconds=25;
    if(argc>=6) {
        char *end=NULL;timeout_seconds=strtol(argv[5],&end,10);
        if(!*argv[5]||*end||timeout_seconds<1||timeout_seconds>86400)return 2;
    }
    if(argc==7 && strcmp(argv[6],"0") && strcmp(argv[6],"1"))return 2;
    int pause_source=!strcmp(argv[1],"serve-pause");
    int hold_source=!strcmp(argv[1],"serve-hold");
    int serve=!strcmp(argv[1],"serve")||pause_source||hold_source;
    FILE *file=fopen(argv[3],serve ? "rb" : "wb");
    if(!file)return 3;
    AVIOContext *io=NULL;AVDictionary *options=NULL;
    avformat_network_init();deadline=av_gettime_relative()+timeout_seconds*1000000;
    if(!strncmp(argv[2],"rtmp://",7))av_dict_set(&options,"listen","1",0);
    if(argc==7 && !strcmp(argv[6],"1") && !strncmp(argv[2],"rtmp://",7))av_dict_set(&options,"tcp_nodelay","1",0);
    av_dict_set(&options,"rw_timeout","3000000",0);
    AVIOInterruptCB cb={interrupted,NULL};
    int ret=avio_open2(&io,argv[2],serve ? AVIO_FLAG_WRITE : AVIO_FLAG_READ,&cb,&options);
    av_dict_free(&options);
    if(ret<0){fprintf(stderr,"open error %d\n",ret);fclose(file);return 4;}
    puts("CONNECTED_ONCE");fflush(stdout);
    uint8_t buffer[65536];int64_t epoch=av_gettime_relative();uint64_t total=0;
    if(serve && (!strcmp(argv[4],"flv")||!strcmp(argv[4],"flv-fast"))) {
        uint8_t header[13];
        int have_media_clock=0;uint32_t timestamp_origin=0;
        if(!read_exact(file,header,13))return 5;
        avio_write(io,header,13);avio_flush(io);
        while(read_exact(file,header,11)) {
            unsigned size=(header[1]<<16)|(header[2]<<8)|header[3];
            uint32_t ts=(header[7]<<24)|(header[4]<<16)|(header[5]<<8)|header[6];
            if(size>4*1024*1024)return 6;
            uint8_t *body=av_malloc(size+4);
            if(!body || !read_exact(file,body,size+4))return 7;
            /* The benchmark preserves a nonzero source timeline for strict
             * DTS. Start pacing at the first coded media tag, not that PTS
             * origin; sequence headers/metadata at timestamp zero aren't media. */
            int media=0;
            if(header[0]==9 && size>=2)media=(body[0]&0x80)?((body[0]&15)==1||(body[0]&15)==3):body[1]==1;
            if(header[0]==8 && size)media=(body[0]>>4)!=10||(size>=2&&body[1]==1);
            if(hold_source&&!have_media_clock&&media) {have_media_clock=1;timestamp_origin=ts;epoch=av_gettime_relative();}
            if(!strcmp(argv[4],"flv"))pace(epoch+((int64_t)ts-timestamp_origin)*1000);
            avio_write(io,header,11);avio_write(io,body,size+4);avio_flush(io);av_free(body);
            if(io->error<0){ret=io->error;break;}
        }
    } else if(serve) {
        int rate=atoi(argv[4]);if(rate<=0)return 8;
        size_t n;int paused=0;
        while((n=fread(buffer,1,1316,file))) {
            if(!pause_source)pace(epoch+(int64_t)(total*1000000/rate));
            avio_write(io,buffer,n);avio_flush(io);total+=n;
            if(io->error<0){ret=io->error;break;}
            if(pause_source&&!paused&&total>=1000000) {
                paused=1;puts("SOURCE_PAUSED");fflush(stdout);av_usleep(1200000);
                puts("SOURCE_RESUMED");fflush(stdout);
            }
        }
    } else {
        while((ret=avio_read_partial(io,buffer,sizeof(buffer)))>0) {
            if(fwrite(buffer,1,ret,file)!=(size_t)ret)return 9;
            total+=ret;fflush(file);
        }
    }
    if(serve) {
        printf("SOURCE_STOPPED bytes=%llu result=%d elapsed_ms=%.3f\n",
               (unsigned long long)total,ret,(av_gettime_relative()-epoch)/1000.);
        fflush(stdout);
        if(hold_source)while(av_gettime_relative()<deadline)av_usleep(10000);
        else av_usleep(3000000); /* Connection liveness alone does not imply continued media delivery. */
    }
    avio_closep(&io);fclose(file);
    printf("CLOSED bytes=%llu result=%d\n",(unsigned long long)total,ret);
    return 0;
}
