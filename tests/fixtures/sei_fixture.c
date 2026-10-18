/* Independent fixture serializer: no application SEI parser/writer is linked. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavformat/avformat.h>
#include <libavutil/intreadwrite.h>
static uint8_t raw[100000],nal[160000];static size_t used;
static void variable(unsigned value) {while(value>=255) {raw[used++]=255;value-=255;}raw[used++]=(uint8_t)value;}
static void msg(unsigned type,const uint8_t *data,unsigned size) {variable(type);variable(size);memcpy(raw+used,data,size);used+=size;}
static size_t sei(enum AVCodecID codec,int suffix,int64_t pts,int overflow) {
    uint8_t value[16];AV_WB64(value,(uint64_t)pts);memcpy(value+8,"\0\0\1\0\0\2\0\3",8);
    used=0;
    if(suffix)msg(101,(const uint8_t *)"end",3);
    else {
        msg(100,value,16);msg(100,value,16); /* Deliberate duplicate, not deduplicated. */
        if(codec==AV_CODEC_ID_H264)msg(101,(const uint8_t *)"end",3);
        msg(5,(const uint8_t *)"Q",1); /* Intentionally not a UUID. */
        uint8_t long_data[300];for(unsigned i=0;i<sizeof(long_data);i++)long_data[i]=(uint8_t)(i%7);
        msg(300,long_data,sizeof(long_data));msg(102,(const uint8_t *)"ignore",6);
        if(overflow)for(unsigned i=0;i<65;i++)msg(100,(const uint8_t *)"x",1);
    }
    raw[used++]=128;size_t n=0;int zeros=0;
    if(codec==AV_CODEC_ID_H264)nal[n++]=6;
    else {nal[n++]=(uint8_t)((suffix?40:39)<<1);nal[n++]=1;}
    for(size_t i=0;i<used;i++) {
        if(zeros>=2&&raw[i]<=3) {nal[n++]=3;zeros=0;}
        nal[n++]=raw[i];zeros=raw[i]==0?zeros+1:0;
    }
    return n;
}
int main(int argc,char **argv) {
    if(argc!=4)return 2;
    AVFormatContext *in=NULL,*out=NULL;AVPacket *p=av_packet_alloc();int ret=avformat_open_input(&in,argv[1],NULL,NULL);
    if(ret<0||avformat_find_stream_info(in,NULL)<0)return 3;
    int idx=av_find_best_stream(in,AVMEDIA_TYPE_VIDEO,-1,-1,NULL,0);if(idx<0)return 4;
    AVStream *src=in->streams[idx];enum AVCodecID codec=src->codecpar->codec_id;
    if(codec!=AV_CODEC_ID_H264&&codec!=AV_CODEC_ID_HEVC)return 5;
    int length=codec==AV_CODEC_ID_H264?(src->codecpar->extradata[4]&3)+1:(src->codecpar->extradata[21]&3)+1;
    if(length!=4)return 6;
    if(avformat_alloc_output_context2(&out,NULL,NULL,argv[2])<0)return 7;
    AVStream *dest=avformat_new_stream(out,NULL);if(!dest||avcodec_parameters_copy(dest->codecpar,src->codecpar)<0)return 8;
    dest->time_base=src->time_base;dest->avg_frame_rate=src->avg_frame_rate;
    if(avio_open(&out->pb,argv[2],AVIO_FLAG_WRITE)<0||avformat_write_header(out,NULL)<0)return 9;
    while(av_read_frame(in,p)>=0) {
        if(p->stream_index!=idx) {av_packet_unref(p);continue;}
        int64_t frame=av_rescale_q(p->pts,src->time_base,(AVRational){1,25});
        int chosen=!strcmp(argv[3],"all")||!strcmp(argv[3],"overflow")||frame%3==1;
        if(chosen) {
            size_t pos=0,at=SIZE_MAX;
            while(pos+4<=(size_t)p->size) {
                uint32_t size=AV_RB32(p->data+pos);if(!size||size>(size_t)p->size-pos-4)return 10;
                unsigned type=codec==AV_CODEC_ID_H264?p->data[pos+4]&31:(p->data[pos+4]>>1)&63;
                if(codec==AV_CODEC_ID_H264?(type==1||type==5):type<=31) {at=pos;break;}
                pos+=4+size;
            }
            if(at==SIZE_MAX)return 11;
            int64_t pts=av_rescale_q(p->pts,src->time_base,AV_TIME_BASE_Q);
            size_t front=sei(codec,0,pts,!strcmp(argv[3],"overflow"));uint8_t *copy=av_malloc(front);if(!copy)return 12;memcpy(copy,nal,front);
            size_t back=codec==AV_CODEC_ID_HEVC?sei(codec,1,pts,0):0;
            AVPacket *q=av_packet_alloc();if(!q||av_new_packet(q,p->size+(int)front+4+(back?(int)back+4:0))<0)return 13;
            if(av_packet_copy_props(q,p)<0)return 14;
            memcpy(q->data,p->data,at);AV_WB32(q->data+at,front);memcpy(q->data+at+4,copy,front);
            memcpy(q->data+at+4+front,p->data+at,p->size-at);
            if(back) {AV_WB32(q->data+p->size+4+front,back);memcpy(q->data+p->size+8+front,nal,back);}
            av_free(copy);av_packet_unref(p);av_packet_move_ref(p,q);av_packet_free(&q);
        }
        av_packet_rescale_ts(p,src->time_base,dest->time_base);p->stream_index=0;p->pos=-1;
        ret=av_interleaved_write_frame(out,p);av_packet_unref(p);if(ret<0)return 15;
    }
    ret=av_write_trailer(out);avio_closep(&out->pb);avformat_free_context(out);avformat_close_input(&in);av_packet_free(&p);
    return ret<0?16:0;
}
