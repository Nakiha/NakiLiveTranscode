/* Correctness helper, not a performance benchmark. Uses the service's actual
 * backend and early-output initialization helpers. Expected count is explicit. */
#include "media/decode_backend.h"
#include "media/decode_output_policy.h"
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/sha.h>
#include <libavutil/time.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Record {
    int64_t pts,best,dts,duration,source;
    int width,height,format,sar_num,sar_den,colorspace,range,primaries,trc,chroma,flags,errors,type,repeat;
    uint8_t pixels[32];
} Record;
static Record record(AVFrame *f) {
    Record r={.pts=f->pts,.best=f->best_effort_timestamp,.dts=f->pkt_dts,.duration=f->duration,
        .width=f->width,.height=f->height,.format=f->format,.sar_num=f->sample_aspect_ratio.num,
        .sar_den=f->sample_aspect_ratio.den,.colorspace=f->colorspace,.range=f->color_range,
        .primaries=f->color_primaries,.trc=f->color_trc,.chroma=f->chroma_location,
        .flags=f->flags,.errors=f->decode_error_flags,.type=f->pict_type,.repeat=f->repeat_pict};
    assert(f->opaque_ref&&f->opaque_ref->size==sizeof(int64_t));
    memcpy(&r.source,f->opaque_ref->data,sizeof(r.source));
    int n=av_image_get_buffer_size(f->format,f->width,f->height,1);assert(n>0);
    uint8_t *image=av_malloc(n);struct AVSHA *sha=av_sha_alloc();assert(image&&sha);
    assert(av_image_copy_to_buffer(image,n,(const uint8_t *const *)f->data,f->linesize,f->format,f->width,f->height,1)==n);
    av_sha_init(sha,256);av_sha_update(sha,image,n);av_sha_final(sha,r.pixels);av_free(sha);av_free(image);
    return r;
}
int main(int argc,char **argv) {
    assert(argc==3);int expected=atoi(argv[2]);assert(expected>0&&expected<=4096);
    AVFormatContext *input=NULL;assert(avformat_open_input(&input,argv[1],NULL,NULL)>=0);
    assert(avformat_find_stream_info(input,NULL)>=0);
    const AVCodec *codec=NULL;int stream=av_find_best_stream(input,AVMEDIA_TYPE_VIDEO,-1,-1,&codec,0);assert(stream>=0);
    AVPacket **packets=NULL,*read=av_packet_alloc();AVFrame *frame=av_frame_alloc();assert(read&&frame);int count=0,read_ret;
    while((read_ret=av_read_frame(input,read))>=0) {
        if(read->stream_index==stream) {
            packets=av_realloc_array(packets,count+1,sizeof(*packets));assert(packets);
            packets[count]=av_packet_clone(read);assert(packets[count]);
            packets[count]->opaque_ref=av_buffer_alloc(sizeof(int64_t));assert(packets[count]->opaque_ref);
            int64_t id=count;memcpy(packets[count]->opaque_ref->data,&id,sizeof(id));count++;
        }
        av_packet_unref(read);
    }
    assert(read_ret==AVERROR_EOF&&count>0);
    Record *baseline=calloc(expected,sizeof(*baseline));assert(baseline);
    for(int backend=0;backend<3;backend++)for(int early=0;early<2;early++) {
        AVCodecContext *decoder=avcodec_alloc_context3(codec);AVCodecAsyncContext *async=NULL;assert(decoder);
        assert(avcodec_parameters_to_context(decoder,input->streams[stream]->codecpar)>=0);
        decoder->pkt_timebase=input->streams[stream]->time_base;decoder->thread_type=FF_THREAD_FRAME;decoder->thread_count=2;
        decoder->flags|=AV_CODEC_FLAG_COPY_OPAQUE;
        assert(naki_decode_output_policy(decoder,early)>=0);
        assert(avcodec_open2(decoder,codec,NULL)>=0);
        assert(naki_decode_backend_init(&decoder,&async,backend!=0,backend==2,1,2,2,NULL,NULL)>=0);
        assert((backend!=0)==(async!=NULL));assert((backend==0)==(decoder!=NULL));
        for(int cycle=0;cycle<3;cycle++) {
            int next=0,closed=0,produced=0,eof=0;int64_t deadline=av_gettime_relative()+10000000;
            while(!eof) {
                assert(av_gettime_relative()<deadline);
                if(!closed) {
                    int ret=naki_decode_send(decoder,async,next<count?packets[next]:NULL);
                    if(ret>=0) {if(next<count)next++;else closed=1;}
                    else assert(ret==AVERROR(EAGAIN));
                }
                for(;;) {
                    int ret=naki_decode_receive(decoder,async,frame);
                    if(ret==AVERROR(EAGAIN))break;
                    if(ret==AVERROR_EOF){eof=1;break;}
                    assert(ret>=0&&produced<expected);
                    Record r=record(frame);
                    if(!backend&&!early&&!cycle)baseline[produced]=r;
                    else {
                        Record want=baseline[produced];
                        if(early)r.dts=want.dts;
                        /* Compare fields without structure padding. */
                        assert(r.pts==want.pts&&r.best==want.best&&r.dts==want.dts&&r.duration==want.duration&&r.source==want.source);
                        assert(r.width==want.width&&r.height==want.height&&r.format==want.format&&r.sar_num==want.sar_num&&r.sar_den==want.sar_den);
                        assert(r.colorspace==want.colorspace&&r.range==want.range&&r.primaries==want.primaries&&r.trc==want.trc&&r.chroma==want.chroma);
                        assert(r.flags==want.flags&&r.errors==want.errors&&r.type==want.type&&r.repeat==want.repeat&&!memcmp(r.pixels,want.pixels,32));
                    }
                    produced++;av_frame_unref(frame);
                }
                if(!eof)av_usleep(100);
            }
            assert(closed&&next==count&&produced==expected);
            if(async)assert(avcodec_async_flush(async)>=0);else avcodec_flush_buffers(decoder);
        }
        avcodec_async_free(&async);avcodec_free_context(&decoder);
        printf("PASS backend=%s early=%d cycles=3 expected=%d\n",backend==0?"sync":backend==1?"async-fixed":"async-auto",early,expected);
    }
    free(baseline);for(int i=0;i<count;i++)av_packet_free(&packets[i]);av_free(packets);
    av_packet_free(&read);av_frame_free(&frame);avformat_close_input(&input);return 0;
}
