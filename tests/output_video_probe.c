/* Independent synchronous decode + sampled RGB proof, including native VVC. */
#include <stdio.h>
#include <stdlib.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
static int count;
static int drain(AVCodecContext *decoder,AVFrame *frame,AVRational tb) {
    int ret;
    while((ret=avcodec_receive_frame(decoder,frame))>=0) {
        uint8_t *rgb=av_malloc((size_t)frame->width*frame->height*3);if(!rgb)return AVERROR(ENOMEM);
        struct SwsContext *s=sws_getContext(frame->width,frame->height,frame->format,frame->width,frame->height,AV_PIX_FMT_RGB24,SWS_BILINEAR,NULL,NULL,NULL);
        if(!s) {av_free(rgb);return AVERROR(ENOMEM);}
        const int *coeff=sws_getCoefficients(frame->colorspace==AVCOL_SPC_BT709?SWS_CS_ITU709:SWS_CS_ITU601);
        if(sws_setColorspaceDetails(s,coeff,frame->color_range==AVCOL_RANGE_JPEG,coeff,1,0,1<<16,1<<16)<0) {sws_freeContext(s);av_free(rgb);return AVERROR(EINVAL);}
        uint8_t *dst[]={rgb};int lines[]={frame->width*3};
        int result=sws_scale(s,(const uint8_t *const *)frame->data,frame->linesize,0,frame->height,dst,lines);sws_freeContext(s);
        if(result!=frame->height) {av_free(rgb);return AVERROR_INVALIDDATA;}
        int x=frame->width>8?6:0,y=frame->height>8?6:0;uint8_t *mark=rgb+3*(y*frame->width+x),*center=rgb+3*((frame->height/2)*frame->width+frame->width/2);
        printf("%s{\"pts\":%lld,\"tb_num\":%d,\"tb_den\":%d,\"width\":%d,\"height\":%d,\"format\":\"%s\",\"picture\":%d,\"mark\":[%u,%u,%u],\"center\":[%u,%u,%u]}",
            count++?",\n":"",(long long)frame->pts,tb.num,tb.den,frame->width,frame->height,av_get_pix_fmt_name(frame->format),frame->pict_type,
            mark[0],mark[1],mark[2],center[0],center[1],center[2]);av_free(rgb);av_frame_unref(frame);
    }
    return ret==AVERROR(EAGAIN)||ret==AVERROR_EOF?0:ret;
}
int main(int argc,char **argv) {
    if(argc!=2)return 2;av_log_set_level(AV_LOG_ERROR);AVFormatContext *input=NULL;int ret=avformat_open_input(&input,argv[1],NULL,NULL);if(ret<0)return 3;
    ret=avformat_find_stream_info(input,NULL);if(ret<0)return 4;int video=av_find_best_stream(input,AVMEDIA_TYPE_VIDEO,-1,-1,NULL,0);if(video<0)return 5;
    const AVCodec *codec=avcodec_find_decoder(input->streams[video]->codecpar->codec_id);AVCodecContext *decoder=avcodec_alloc_context3(codec);if(!decoder)return 6;
    ret=avcodec_parameters_to_context(decoder,input->streams[video]->codecpar);decoder->thread_count=2;
    if(ret<0||avcodec_open2(decoder,codec,NULL)<0)return 7;
    AVPacket *packet=av_packet_alloc();AVFrame *frame=av_frame_alloc();if(!packet||!frame)return 8;puts("[");
    while((ret=av_read_frame(input,packet))>=0) {
        if(packet->stream_index==video) {
            ret=avcodec_send_packet(decoder,packet);if(ret>=0)ret=drain(decoder,frame,input->streams[video]->time_base);
        }
        av_packet_unref(packet);if(ret<0)break;
    }
    if(ret==AVERROR_EOF) {ret=avcodec_send_packet(decoder,NULL);if(ret>=0)ret=drain(decoder,frame,input->streams[video]->time_base);}
    puts("\n]");av_frame_free(&frame);av_packet_free(&packet);avcodec_free_context(&decoder);avformat_close_input(&input);return ret<0?9:0;
}
