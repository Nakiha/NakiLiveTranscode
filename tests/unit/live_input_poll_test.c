/* Packet identity and progress probe. Also builds against API-1 baseline SDK. */
#include "media/input_probe.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavformat/live_mux.h>
#include <libavutil/sha.h>
#include <libavutil/mem.h>
#include <libavutil/time.h>
static int64_t deadline;
static int interrupted(void *unused) {(void)unused;return av_gettime_relative()>=deadline;}
int main(int argc,char **argv) {
    if(argc!=5&&argc!=6)return 2;
    int poll=atoi(argv[2]),stop=atoi(argv[3]),cancel_ms=atoi(argv[4]),available=0,genpts=argc==6&&atoi(argv[5]);
    AVFormatContext *s=avformat_alloc_context();AVDictionary *options=NULL;
    if(!s)return 3;
    s->interrupt_callback=(AVIOInterruptCB){interrupted,NULL};s->probesize=1024*1024;s->max_analyze_duration=1000000;
    if(!strncmp(argv[1],"http:",5))av_dict_set(&options,"seekable","0",0);
    deadline=av_gettime_relative()+15000000;
    int ret=avformat_open_input(&s,argv[1],NULL,&options);av_dict_free(&options);
    if(ret<0)return 4;
    int video=naki_probe_video(s);if(video<0)return 5;
    if(genpts)s->flags|=AVFMT_FLAG_GENPTS;
#ifdef AVFORMAT_LIVE_INPUT_POLL_VERSION
    available=1;
    ret=poll?avformat_live_input_probe_poll(s,100000):avformat_live_input_probe_limit(s,100000);
#else
    ret=avformat_live_input_probe_limit(s,100000);
#endif
    if(ret<0)return 6;
    struct AVSHA *hashes[64]={0};uint64_t count[64]={0};
    AVPacket *p=av_packet_alloc();if(!p)return 7;
    int64_t began=av_gettime_relative(),max_read=0;int videos=0,early=0,retries=0;uint32_t versions=0;
    if(cancel_ms)deadline=began+(int64_t)cancel_ms*1000;
    for(;;) {
        int64_t before=av_gettime_relative();ret=av_read_frame(s,p);
        int64_t elapsed=av_gettime_relative()-before;if(elapsed>max_read)max_read=elapsed;
        for(unsigned n=0;n<s->nb_programs;n++)if(s->programs[n]->pmt_version>=0&&s->programs[n]->pmt_version<32)
            versions|=UINT32_C(1)<<s->programs[n]->pmt_version;
        if(ret==AVERROR(EAGAIN)){retries++;av_usleep(1000);continue;}
        if(ret<0)break;
        int n=p->stream_index;if(n<0||n>=64)return 8;
        AVStream *st=s->streams[n];
        if(st->codecpar->codec_type==AVMEDIA_TYPE_VIDEO||st->codecpar->codec_type==AVMEDIA_TYPE_AUDIO) {
            if(!hashes[n]) {hashes[n]=av_sha_alloc();if(!hashes[n])return 9;av_sha_init(hashes[n],256);}
            int64_t fields[]={p->pts,p->dts,p->duration,p->size};
            av_sha_update(hashes[n],(uint8_t *)fields,sizeof(fields));av_sha_update(hashes[n],p->data,p->size);count[n]++;
        }
        if(n==video) {videos++;if(av_gettime_relative()-began<500000)early++;}
        av_packet_unref(p);if(stop&&videos>=stop){ret=0;break;}
    }
    printf("{\"poll_available\":%d,\"poll_requested\":%d,\"genpts\":%d,\"result\":%d,\"eof\":%d,\"cancelled\":%d,"
           "\"video\":%d,\"video_before_500ms\":%d,\"max_read_ms\":%.3f,\"elapsed_ms\":%.3f,\"eagain\":%d,\"section_versions\":%u,\"streams\":[",
           available,poll,genpts,ret,ret==AVERROR_EOF,ret==AVERROR_EXIT,videos,early,max_read/1000.,(av_gettime_relative()-began)/1000.,retries,versions);
    int comma=0;
    for(int n=0;n<64;n++)if(hashes[n]) {
        uint8_t digest[32];av_sha_final(hashes[n],digest);av_free(hashes[n]);
        printf("%s{\"stream\":%d,\"packets\":%llu,\"sha256\":\"",comma++?",":"",n,(unsigned long long)count[n]);
        for(int k=0;k<32;k++)printf("%02x",digest[k]);
        printf("\"}");
    }
    printf("]}\n");av_packet_free(&p);avformat_close_input(&s);return 0;
}
