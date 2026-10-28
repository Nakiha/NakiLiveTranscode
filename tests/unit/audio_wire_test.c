#include "media/audio_wire.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mem.h>
static void free_item(InputItem *item){av_packet_free(&item->packet);avcodec_parameters_free(&item->params);}
static void returned(void *opaque,uint8_t *data){(void)data;(*(int *)opaque)++;}
static int decode_probe(const InputItem *item,int manual){
    AVCodecContext *c=avcodec_alloc_context3(avcodec_find_decoder(item->params->codec_id));assert(c);
    int ret=avcodec_parameters_to_context(c,item->params);c->thread_count=1;if(manual)c->flags2|=AV_CODEC_FLAG2_SKIP_MANUAL;
    if(ret>=0)ret=avcodec_open2(c,avcodec_find_decoder(item->params->codec_id),NULL);
    int sent=ret>=0?avcodec_send_packet(c,item->packet):ret;AVFrame *f=av_frame_alloc();int received=sent>=0?avcodec_receive_frame(c,f):sent;
    fprintf(stderr,"manual=%d bytes=%d extra=%d flags=%d padding=%d side=%d send=%d receive=%d samples=%d\n",manual,item->packet->size,item->params->extradata_size,item->packet->flags,item->params->initial_padding,item->packet->side_data_elems,sent,received,f->nb_samples);
    if(manual)assert(received==0&&f->nb_samples>0);
    av_frame_free(&f);avcodec_free_context(&c);return received;
}
static int actual;
static void compare(const InputItem *source){
    uint8_t *data=av_malloc(NAKI_AUDIO_WIRE_CAPACITY);assert(data);size_t bytes;
    assert(!naki_audio_wire_pack(source,data,NAKI_AUDIO_WIRE_CAPACITY,&bytes));int released=0;
    AVBufferRef *owner=av_buffer_create(data,bytes,returned,&released,AV_BUFFER_FLAG_READONLY);assert(owner);
    InputItem copy={0};assert(!naki_audio_wire_unpack(&copy,owner));av_buffer_unref(&owner);assert(!released);
    assert(copy.packet->size==source->packet->size&&!memcmp(copy.packet->data,source->packet->data,copy.packet->size));
    assert(copy.packet->pts==source->packet->pts&&copy.packet->dts==source->packet->dts&&copy.packet->duration==source->packet->duration&&copy.packet->stream_index==source->packet->stream_index);
    assert(copy.params->codec_id==source->params->codec_id&&copy.params->sample_rate==source->params->sample_rate&&copy.params->bit_rate==source->params->bit_rate);
    assert(copy.params->extradata_size==source->params->extradata_size);if(copy.params->extradata_size)assert(!memcmp(copy.params->extradata,source->params->extradata,copy.params->extradata_size));
    assert(copy.packet->side_data_elems==source->packet->side_data_elems);
    for(int i=0;i<source->packet->side_data_elems;i++){const AVPacketSideData *a=&source->packet->side_data[i],*b=&copy.packet->side_data[i];assert(a->type==b->type&&a->size==b->size&&!memcmp(a->data,b->data,a->size));}
    assert(!av_channel_layout_compare(&copy.params->ch_layout,&source->params->ch_layout));
    if(actual){assert(decode_probe(source,0)==decode_probe(&copy,0));decode_probe(&copy,1);}
    free_item(&copy);assert(released==1);av_free(data);
}
int main(int argc,char **argv){
    if(argc==2){AVFormatContext *input=NULL;assert(!avformat_open_input(&input,argv[1],NULL,NULL));assert(avformat_find_stream_info(input,NULL)>=0);
        InputItem item={.packet=av_packet_alloc(),.tb={1,48000},.demuxed_us=123};assert(item.packet);
        while(av_read_frame(input,item.packet)>=0){AVStream *stream=input->streams[item.packet->stream_index];if(stream->codecpar->codec_type==AVMEDIA_TYPE_AUDIO&&av_rescale_q(item.packet->pts,stream->time_base,AV_TIME_BASE_Q)>1000000){item.params=avcodec_parameters_alloc();assert(item.params&&!avcodec_parameters_copy(item.params,stream->codecpar));item.tb=stream->time_base;actual=1;compare(&item);break;}av_packet_unref(item.packet);}
        free_item(&item);avformat_close_input(&input);return 0;}

    InputItem source={.params=avcodec_parameters_alloc(),.packet=av_packet_alloc(),.tb={1,48000},.demuxed_us=123};assert(source.params&&source.packet);
    AVCodecParameters *p=source.params;p->codec_type=AVMEDIA_TYPE_AUDIO;p->codec_id=AV_CODEC_ID_AAC;p->sample_rate=48000;p->bit_rate=128000;
    av_channel_layout_default(&p->ch_layout,1);p->extradata=av_mallocz(2+AV_INPUT_BUFFER_PADDING_SIZE);assert(p->extradata);p->extradata_size=2;p->extradata[0]=0x11;p->extradata[1]=0x88;
    assert(!av_new_packet(source.packet,64));for(int i=0;i<64;i++)source.packet->data[i]=(uint8_t)i;
    source.packet->pts=48000;source.packet->dts=48000;source.packet->duration=1024;source.packet->stream_index=1;
    uint8_t *id=av_packet_new_side_data(source.packet,AV_PKT_DATA_MPEGTS_STREAM_ID,1);assert(id);*id=192;
    compare(&source);
    p->codec_id=AV_CODEC_ID_MP3;p->sample_rate=44100;p->frame_size=1152;av_freep(&p->extradata);p->extradata_size=0;
    compare(&source);
    id=av_packet_new_side_data(source.packet,AV_PKT_DATA_MPEGTS_STREAM_ID,2);assert(id);id[0]=192;id[1]=0;
    uint8_t *invalid=av_malloc(NAKI_AUDIO_WIRE_CAPACITY);assert(invalid);size_t bytes=0;assert(naki_audio_wire_pack(&source,invalid,NAKI_AUDIO_WIRE_CAPACITY,&bytes)==-ENOTSUP&&!bytes);av_free(invalid);
    free_item(&source);puts("PASS AAC/MP3 parameters, compressed bytes, bounded MPEGTS stream ID, clock, final raw release");return 0;
}
