#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "media/input_probe.h"
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/sha.h>
#include <libavutil/time.h>

static int64_t deadline;
static int interrupted(void *unused) {(void)unused;return av_gettime_relative()>deadline;}
static void hex(const unsigned char *bytes,char *out) {
    for(int i=0;i<32;i++)sprintf(out+i*2,"%02x",bytes[i]);
}
static int receive(AVCodecContext *decoder,AVFrame *frame,struct AVSHA *hash,int *count) {
    int ret;
    while((ret=avcodec_receive_frame(decoder,frame))>=0) {
        int size=av_image_get_buffer_size(frame->format,frame->width,frame->height,1);
        uint8_t *pixels=size>0 ? av_malloc(size) : NULL;
        if(!pixels)return AVERROR(ENOMEM);
        ret=av_image_copy_to_buffer(pixels,size,(const uint8_t *const *)frame->data,frame->linesize,
                                   frame->format,frame->width,frame->height,1);
        if(ret>=0){av_sha_update(hash,pixels,ret);(*count)++;}
        av_free(pixels);av_frame_unref(frame);
        if(ret<0)return ret;
    }
    return ret==AVERROR(EAGAIN)||ret==AVERROR_EOF ? 0 : ret;
}
int main(int argc,char **argv) {
    if(argc!=4)return 2;
    AVFormatContext *input=avformat_alloc_context();
    AVCodecContext *decoder=NULL;
    AVPacket *packet=av_packet_alloc();AVFrame *frame=av_frame_alloc();
    struct AVSHA *pixels=av_sha_alloc(),*packets=av_sha_alloc(),*one=av_sha_alloc(),*all_hash=av_sha_alloc();
    int video=-1,count=0,frames=0,all=0,ret=0;
    if(!input||!packet||!frame||!pixels||!packets||!one||!all_hash)return 2;
    input->probesize=1048576;input->max_analyze_duration=1000000;
    if(!strcmp(argv[2],"live"))input->flags|=AVFMT_FLAG_LIVE_VIDEO_PROBE;
    else if(!strcmp(argv[2],"live-bytes")) {
        input->flags|=AVFMT_FLAG_LIVE_VIDEO_PROBE|AVFMT_FLAG_NOFILLIN;
        input->probesize=32768;input->max_analyze_duration=60000000;
    }
    else if(!strcmp(argv[2],"generic-no-clock"))input->flags|=AVFMT_FLAG_NOFILLIN;
    else if(!strcmp(argv[2],"tuned")){input->probesize=65536;input->max_analyze_duration=100000;input->fps_probe_size=0;}
    else if(!strcmp(argv[2],"invalid"))input->flags|=AVFMT_FLAG_LIVE_VIDEO_PROBE|AVFMT_FLAG_NOBUFFER;
    else if(!strcmp(argv[2],"invalid-parse"))input->flags|=AVFMT_FLAG_LIVE_VIDEO_PROBE|AVFMT_FLAG_NOPARSE;
    deadline=av_gettime_relative()+10000000;
    input->interrupt_callback=(AVIOInterruptCB){interrupted,NULL};
    int64_t start=av_gettime_relative();
    ret=avformat_open_input(&input,argv[1],NULL,NULL);
    int64_t opened=av_gettime_relative();
    if(ret>=0 && (!strcmp(argv[2],"live-bytes") || !strcmp(argv[2],"generic-no-clock")))
        for(unsigned i=0;i<input->nb_streams;i++)input->streams[i]->avg_frame_rate=(AVRational){0,1};
    if(ret>=0 && !strcmp(argv[2],"interrupt-probe"))deadline=opened-1;
    if(ret>=0) {
        if(!strncmp(argv[2],"live",4) || !strcmp(argv[2],"interrupt-probe")) {
            ret=naki_probe_video(input);
            if(ret>=0)video=ret;
        } else ret=avformat_find_stream_info(input,NULL);
    }
    int64_t ready=av_gettime_relative();
    int64_t probe_position=input && input->pb ? avio_tell(input->pb) : -1;
    if(!strcmp(argv[2],"invalid") || !strcmp(argv[2],"invalid-parse") || !strcmp(argv[2],"interrupt-probe")) {
        avformat_close_input(&input);av_packet_free(&packet);av_frame_free(&frame);
        av_free(pixels);av_free(packets);av_free(one);av_free(all_hash);
        return ret==(!strcmp(argv[2],"interrupt-probe") ? AVERROR_EXIT : AVERROR(EINVAL))?0:1;
    }
    if(ret<0)goto end;
    unsigned probed_streams=input->nb_streams;
    if(video<0)for(unsigned i=0;i<input->nb_streams;i++) {
        const AVStream *st=input->streams[i];
        if(st->codecpar->codec_type==AVMEDIA_TYPE_VIDEO && !(st->disposition&AV_DISPOSITION_ATTACHED_PIC) &&
           (st->codecpar->codec_id==AV_CODEC_ID_H264||st->codecpar->codec_id==AV_CODEC_ID_HEVC)){video=i;break;}
    }
    if(video<0){ret=AVERROR_STREAM_NOT_FOUND;goto end;}
    AVStream *stream=input->streams[video];AVRational fps=av_guess_frame_rate(input,stream,NULL);
    const AVCodec *codec=avcodec_find_decoder(stream->codecpar->codec_id);
    if(!codec){ret=AVERROR_DECODER_NOT_FOUND;goto end;}
    decoder=avcodec_alloc_context3(codec);
    if(!decoder){ret=AVERROR(ENOMEM);goto end;}
    if((ret=avcodec_parameters_to_context(decoder,stream->codecpar))<0)goto end;
    decoder->thread_count=1;
    if((ret=avcodec_open2(decoder,codec,NULL))<0)goto end;
    av_sha_init(pixels,256);av_sha_init(packets,256);av_sha_init(all_hash,256);
    FILE *ledger=fopen(argv[3],"w");
    if(!ledger){ret=AVERROR(errno);goto end;}
    char all_path[4096];
    int path_length=snprintf(all_path,sizeof(all_path),"%s.all",argv[3]);
    FILE *all_ledger=path_length>=0 && path_length<(int)sizeof(all_path) ? fopen(all_path,"w") : NULL;
    if(!all_ledger){ret=AVERROR(EIO);fclose(ledger);goto end;}
    while((ret=av_read_frame(input,packet))>=0) {
        all++;
        unsigned char digest[32];char text[65]={0};
        av_sha_init(one,256);av_sha_update(one,packet->data,packet->size);av_sha_final(one,digest);hex(digest,text);
        av_sha_update(all_hash,packet->data,packet->size);
        if(fprintf(all_ledger,"%d %lld %lld %lld %d %d %s\n",packet->stream_index,
                   (long long)packet->pts,(long long)packet->dts,(long long)packet->duration,
                   packet->flags,packet->size,text)<0){ret=AVERROR(EIO);av_packet_unref(packet);break;}
        if(packet->stream_index==video) {
            if(fprintf(ledger,"%lld %lld %d %d %s\n",(long long)packet->pts,(long long)packet->dts,
                       packet->size,packet->flags&AV_PKT_FLAG_KEY,text)<0){ret=AVERROR(EIO);av_packet_unref(packet);break;}
            av_sha_update(packets,packet->data,packet->size);count++;
            while((ret=avcodec_send_packet(decoder,packet))==AVERROR(EAGAIN))if((ret=receive(decoder,frame,pixels,&frames))<0)break;
            if(ret>=0)ret=receive(decoder,frame,pixels,&frames);
            if(ret<0){av_packet_unref(packet);break;}
        }
        av_packet_unref(packet);
    }
    if(ret==AVERROR_EOF) {
        ret=avcodec_send_packet(decoder,NULL);
        if(ret>=0)ret=receive(decoder,frame,pixels,&frames);
    }
    if(fclose(ledger) && ret>=0)ret=AVERROR(EIO);
    if(fclose(all_ledger) && ret>=0)ret=AVERROR(EIO);
    if(ret>=0){
        unsigned char digest[32];char a[65]={0},b[65]={0},c[65]={0};
        av_sha_final(pixels,digest);hex(digest,a);av_sha_final(packets,digest);hex(digest,b);
        av_sha_final(all_hash,digest);hex(digest,c);
        printf("{\"open_ms\":%.3f,\"probe_ms\":%.3f,\"probe_position\":%lld,\"initial_streams\":%u,\"final_streams\":%u,\"packets\":%d,\"all_packets\":%d,\"frames\":%d,\"fps_num\":%d,\"fps_den\":%d,\"pixel_sha256\":\"%s\",\"packet_sha256\":\"%s\",\"all_packet_sha256\":\"%s\"}\n",
               (opened-start)/1000.,(ready-opened)/1000.,(long long)probe_position,probed_streams,input->nb_streams,count,all,frames,fps.num,fps.den,a,b,c);
    }
end:
    if(ret<0){char error[128];av_strerror(ret,error,sizeof(error));fprintf(stderr,"probe test: %s\n",error);}
    avcodec_free_context(&decoder);avformat_close_input(&input);av_packet_free(&packet);av_frame_free(&frame);
    av_free(pixels);av_free(packets);av_free(one);av_free(all_hash);return ret<0;
}
