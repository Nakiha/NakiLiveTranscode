/* Real FLV AAC sequence-header changes. Each phase uses independently encoded
 * payload; one continuous audio clock and the original reordered video PTS. */
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavformat/avformat.h>
#include <libavutil/mem.h>
typedef struct Item {AVPacket *packet;unsigned order;} Item;
static Item items[2048];static unsigned count;
static void keep(AVPacket *packet) {assert(count<2048&&packet->size<=1024*1024);items[count]=(Item){packet,count};count++;}
static AVFormatContext *open_input(const char *path,enum AVMediaType type,int *index) {
    AVFormatContext *input=NULL;assert(avformat_open_input(&input,path,NULL,NULL)>=0);assert(avformat_find_stream_info(input,NULL)>=0);
    *index=av_find_best_stream(input,type,-1,-1,NULL,0);assert(*index>=0);return input;
}
static void config(const AVPacket *packet,unsigned char bytes[2],int *rate,int *samples,int *header) {
    static const int rates[]={96000,88200,64000,48000,44100,32000,24000,22050,16000,12000,11025,8000,7350};
    assert(packet->size>=7&&packet->data[0]==255&&(packet->data[1]&0xf6)==0xf0);
    int aot=(packet->data[2]>>6)+1,index=(packet->data[2]>>2)&15,channels=((packet->data[2]&1)<<2)|(packet->data[3]>>6);
    assert(index<13&&channels);*rate=rates[index];*samples=1024*((packet->data[6]&3)+1);*header=packet->data[1]&1?7:9;
    assert((((packet->data[3]&3)<<11)|(packet->data[4]<<3)|(packet->data[5]>>5))==packet->size);
    bytes[0]=(unsigned char)((aot<<3)|(index>>1));bytes[1]=(unsigned char)(((index&1)<<7)|(channels<<3));
}
static int compare(const void *left,const void *right) {
    const Item *a=left,*b=right;if(a->packet->dts!=b->packet->dts)return a->packet->dts>b->packet->dts?1:-1;
    return a->order>b->order?1:a->order<b->order?-1:0;
}
int main(int argc,char **argv) {
    assert(argc==5);av_log_set_level(AV_LOG_ERROR);AVFormatContext *output=NULL;
    assert(avformat_alloc_output_context2(&output,NULL,"flv",argv[4])>=0);
    int video_index;AVFormatContext *video=open_input(argv[1],AVMEDIA_TYPE_VIDEO,&video_index);
    AVStream *out_video=avformat_new_stream(output,NULL),*out_audio=avformat_new_stream(output,NULL);assert(out_video&&out_audio);
    assert(avcodec_parameters_copy(out_video->codecpar,video->streams[video_index]->codecpar)>=0);out_video->time_base=(AVRational){1,1000};out_audio->time_base=out_video->time_base;
    out_video->codecpar->codec_tag=0;
    AVPacket *packet=av_packet_alloc();assert(packet);int64_t shift=INT64_MIN;
    while(av_read_frame(video,packet)>=0) {
        if(packet->stream_index==video_index) {
            assert(packet->pts!=AV_NOPTS_VALUE&&packet->dts!=AV_NOPTS_VALUE);
            av_packet_rescale_ts(packet,video->streams[video_index]->time_base,out_video->time_base);
            if(shift==INT64_MIN)shift=-packet->dts;
            packet->pts+=shift;packet->dts+=shift;packet->stream_index=0;
            AVPacket *copy=av_packet_clone(packet);assert(copy);keep(copy);
        }
        av_packet_unref(packet);
    }
    avformat_close_input(&video);
    int64_t clock=0;unsigned phase_count[2]={0};unsigned char configurations[2][2];
    for(int phase=0;phase<2;phase++) {
        int index;AVFormatContext *audio=open_input(argv[phase+2],AVMEDIA_TYPE_AUDIO,&index);
        assert(audio->streams[index]->codecpar->codec_id==AV_CODEC_ID_AAC);
        if(!phase)assert(avcodec_parameters_copy(out_audio->codecpar,audio->streams[index]->codecpar)>=0);
        out_audio->codecpar->codec_tag=0;
        while(av_read_frame(audio,packet)>=0) {
            if(packet->stream_index==index) {
                unsigned char asc[2];int rate,samples,header;config(packet,asc,&rate,&samples,&header);
                if(!phase_count[phase])memcpy(configurations[phase],asc,sizeof(asc));else assert(!memcmp(configurations[phase],asc,sizeof(asc)));
                AVPacket *raw=av_packet_alloc();assert(raw&&av_new_packet(raw,packet->size-header)>=0);memcpy(raw->data,packet->data+header,(size_t)raw->size);
                if(!phase&&!phase_count[phase]) {
                    av_freep(&out_audio->codecpar->extradata);out_audio->codecpar->extradata=av_mallocz(2+AV_INPUT_BUFFER_PADDING_SIZE);assert(out_audio->codecpar->extradata);
                    memcpy(out_audio->codecpar->extradata,asc,2);out_audio->codecpar->extradata_size=2;
                }
                if(phase&&!phase_count[phase]) {unsigned char *side=av_packet_new_side_data(raw,AV_PKT_DATA_NEW_EXTRADATA,2);assert(side);memcpy(side,asc,2);}
                int64_t next=av_add_stable((AVRational){1,90000},clock,(AVRational){1,rate},samples);
                raw->pts=raw->dts=av_rescale_q(clock,(AVRational){1,90000},out_audio->time_base);
                raw->duration=av_rescale_q(next,(AVRational){1,90000},out_audio->time_base)-raw->dts;raw->stream_index=1;raw->flags=AV_PKT_FLAG_KEY;
                clock=next;keep(raw);phase_count[phase]++;
            }
            av_packet_unref(packet);
        }
        avformat_close_input(&audio);
    }
    av_packet_free(&packet);assert(phase_count[0]>20&&phase_count[1]>20&&memcmp(configurations[0],configurations[1],2));
    assert(avio_open(&output->pb,argv[4],AVIO_FLAG_WRITE)>=0);assert(avformat_write_header(output,NULL)>=0);
    assert(av_cmp_q(out_video->time_base,(AVRational){1,1000})==0&&av_cmp_q(out_audio->time_base,(AVRational){1,1000})==0);
    qsort(items,count,sizeof(*items),compare);
    for(unsigned i=0;i<count;i++) {assert(av_write_frame(output,items[i].packet)>=0);av_packet_free(&items[i].packet);}
    assert(av_write_trailer(output)>=0);assert(avio_closep(&output->pb)>=0);avformat_free_context(output);
    printf("{\"first_phase_packets\":%u,\"second_phase_packets\":%u,\"first_asc\":\"%02x%02x\",\"second_asc\":\"%02x%02x\",\"continuous_audio_clock_90k\":%" PRId64 "}\n",
        phase_count[0],phase_count[1],configurations[0][0],configurations[0][1],configurations[1][0],configurations[1][1],clock);return 0;
}
