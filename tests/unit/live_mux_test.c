#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <libavformat/live_mux.h>
#include <libavutil/mem.h>
#include <libavutil/time.h>

static void packet(AVPacket *p,int index,int size,int64_t dts) {
    assert(av_new_packet(p,size)==0);memset(p->data,0,size);p->stream_index=index;p->pts=p->dts=dts;
}
int main(void) {
    for(int format=0;format<2;format++) {
        AVFormatContext *s=NULL;AVPacket *in=av_packet_alloc(),*out=av_packet_alloc();
        assert(avformat_alloc_output_context2(&s,NULL,format ? "flv" : "mpegts",NULL)>=0);
        AVStream *v=avformat_new_stream(s,NULL);v->codecpar->codec_type=AVMEDIA_TYPE_VIDEO;
        v->codecpar->codec_id=AV_CODEC_ID_H264;v->codecpar->width=256;v->codecpar->height=144;
        v->time_base=(AVRational){1,90000};v->avg_frame_rate=(AVRational){25,1};
        assert(avio_open_dyn_buf(&s->pb)>=0);assert(avformat_write_header(s,NULL)>=0);
        assert(avformat_live_mux_init(s,50000,4,16)==0);
        packet(in,0,1,0);assert(av_interleaved_write_frame(s,in)==AVERROR(EINVAL) && !in->data);
        AVCodecParameters *a=avcodec_parameters_alloc();a->codec_type=AVMEDIA_TYPE_AUDIO;a->codec_id=AV_CODEC_ID_AAC;
        a->sample_rate=48000;av_channel_layout_default(&a->ch_layout,2);
        AVStream *added=NULL;
        assert(avformat_live_add_audio(s,v->codecpar,(AVRational){1,90000},&added)<0 && s->nb_streams==1);
        assert(avformat_live_add_audio(s,a,(AVRational){1,48000},&added)<0 && s->nb_streams==1 && !added);
        a->extradata=av_mallocz(2+AV_INPUT_BUFFER_PADDING_SIZE);a->extradata_size=2;a->extradata[0]=0x11;a->extradata[1]=0x90;
        int64_t before=avio_tell(s->pb);
        assert(avformat_live_add_audio(s,a,(AVRational){1,48000},&added)==0 && s->nb_streams==2);
        assert(avio_tell(s->pb)==before); /* No second file/connection header. */
        packet(in,0,4,0);assert(avformat_live_mux_send(s,in)==0 && !in->data);
        assert(avformat_live_mux_receive(s,out,0)==0);
        int64_t start=av_gettime_relative();av_usleep(60000);
        assert(avformat_live_mux_receive(s,out,0)==1 && out->stream_index==0);
        assert(av_gettime_relative()-start<250000);av_packet_unref(out);
        AVLiveMuxStats stats;assert(!avformat_live_mux_stats(s,&stats));assert(stats.timeout_releases==1);
        /* Constant DTS cannot defeat packet/byte bounds, even with silent audio. */
        for(int i=0;i<4;i++){packet(in,0,4,0);assert(!avformat_live_mux_send(s,in));}
        packet(in,0,1,0);assert(avformat_live_mux_send(s,in)==AVERROR(EAGAIN) && in->data);av_packet_unref(in);
        assert(avformat_live_mux_receive(s,out,0)==1);av_packet_unref(out);
        while(avformat_live_mux_receive(s,out,1)>0)av_packet_unref(out);
        packet(in,0,17,0);assert(avformat_live_mux_send(s,in)==AVERROR(EMSGSIZE) && in->data);av_packet_unref(in);
        packet(in,0,4,9000);assert(!avformat_live_mux_send(s,in));
        packet(in,added->index,4,av_rescale_q(4500,(AVRational){1,90000},added->time_base));assert(!avformat_live_mux_send(s,in));
        assert(avformat_live_mux_receive(s,out,0)==1 && out->stream_index==added->index);av_packet_unref(out);
        assert(avformat_live_mux_receive(s,out,0)==0);
        assert(avformat_live_mux_receive(s,out,1)==1 && out->stream_index==0);av_packet_unref(out);
        assert(!avformat_live_mux_stats(s,&stats));assert(stats.high_water_packets==4 && !stats.queued_packets && !stats.queued_bytes);
        avcodec_parameters_free(&a);av_packet_free(&in);av_packet_free(&out);
        av_write_trailer(s);uint8_t *buffer;avio_close_dyn_buf(s->pb,&buffer);s->pb=NULL;av_free(buffer);avformat_free_context(s);
        printf("PASS %s transactional audio registration, constant-DTS wall timeout and hard queue bounds\n",format ? "flv" : "mpegts");
    }
    return 0;
}
