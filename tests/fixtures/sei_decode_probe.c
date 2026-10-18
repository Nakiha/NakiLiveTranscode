/* Independent decode check: remove opaque SEI NALs, retain packet PTS and
 * decode actual output. Standard decoders may reject private type-5 payloads. */
#include <stdio.h>
#include <string.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/intreadwrite.h>
static int prefix(const uint8_t *p,int n) {
    if(n>=3&&!p[0]&&!p[1]&&p[2]==1)return 3;
    return n>=4&&!p[0]&&!p[1]&&!p[2]&&p[3]==1?4:0;
}
static int receive(AVCodecContext *c,AVFrame *f,AVRational tb) {
    int ret;
    while((ret=avcodec_receive_frame(c,f))>=0) {printf("%lld\n",(long long)av_rescale_q(f->pts,tb,(AVRational){1,90000}));av_frame_unref(f);}
    return ret==AVERROR(EAGAIN)||ret==AVERROR_EOF?0:ret;
}
int main(int argc,char **argv) {
    if(argc!=2)return 2;
    AVFormatContext *in=NULL;AVPacket *p=av_packet_alloc();AVFrame *f=av_frame_alloc();
    if(!p||!f||avformat_open_input(&in,argv[1],NULL,NULL)<0||avformat_find_stream_info(in,NULL)<0)return 3;
    int idx=av_find_best_stream(in,AVMEDIA_TYPE_VIDEO,-1,-1,NULL,0);if(idx<0)return 4;
    AVStream *st=in->streams[idx];enum AVCodecID codec=st->codecpar->codec_id;
    AVCodecContext *c=avcodec_alloc_context3(avcodec_find_decoder(codec));
    if(!c||avcodec_parameters_to_context(c,st->codecpar)<0)return 5;
    c->pkt_timebase=st->time_base;c->thread_count=2;
    if(avcodec_open2(c,NULL,NULL)<0)return 6;
    int lp=strstr(in->iformat->name,"mov")||!strcmp(in->iformat->name,"flv");
    while(av_read_frame(in,p)>=0) {
        if(p->stream_index!=idx) {av_packet_unref(p);continue;}
        AVPacket *q=av_packet_alloc();if(!q||av_new_packet(q,p->size)<0||av_packet_copy_props(q,p)<0)return 7;
        int pos=0,written=0;
        while(pos<p->size) {
            int start=pos,h=lp?4:prefix(p->data+pos,p->size-pos);if(!h||h>p->size-pos)return 8;
            pos+=h;int end;
            if(lp) {unsigned size=AV_RB32(p->data+start);if(!size||size>(unsigned)(p->size-pos))return 9;end=pos+(int)size;}
            else {end=pos;while(end<p->size&&!prefix(p->data+end,p->size-end))end++;}
            int typ=codec==AV_CODEC_ID_H264?p->data[pos]&31:codec==AV_CODEC_ID_HEVC?(p->data[pos]>>1)&63:(p->data[pos+1]>>3)&31;
            int sei=codec==AV_CODEC_ID_H264?typ==6:codec==AV_CODEC_ID_HEVC?(typ==39||typ==40):(typ==23||typ==24);
            if(!sei) {memcpy(q->data+written,p->data+start,end-start);written+=end-start;}
            pos=end;
        }
        av_shrink_packet(q,written);
        if(avcodec_send_packet(c,q)<0||receive(c,f,st->time_base)<0)return 10;
        av_packet_free(&q);av_packet_unref(p);
    }
    if(avcodec_send_packet(c,NULL)<0||receive(c,f,st->time_base)<0)return 11;
    avcodec_free_context(&c);avformat_close_input(&in);av_frame_free(&f);av_packet_free(&p);return 0;
}
