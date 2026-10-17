#include "video_filter.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libavutil/buffer.h>
#include <libavutil/imgutils.h>
static AVFrame *input(int w,int h,enum AVPixelFormat format,int64_t pts) {
    AVFrame *f=av_frame_alloc();assert(f);f->width=w;f->height=h;f->format=format;f->pts=pts;
    f->sample_aspect_ratio=(AVRational){1,1};f->colorspace=AVCOL_SPC_BT709;f->color_range=AVCOL_RANGE_MPEG;
    assert(av_frame_get_buffer(f,32)>=0);ptrdiff_t lines[4];for(int n=0;n<4;n++)lines[n]=f->linesize[n];
    assert(!av_image_fill_black(f->data,lines,format,f->color_range,w,h));
    f->opaque_ref=av_buffer_alloc(8);assert(f->opaque_ref);memset(f->opaque_ref->data,42,8);
    AVFrameSideData *sd=av_frame_new_side_data(f,AV_FRAME_DATA_SEI_UNREGISTERED,20);assert(sd);memset(sd->data,13,20);
    assert(!av_dict_set(&f->metadata,"test","preserved",0));return f;
}
static void pass(AVFilterContext *source,AVFilterContext *sink,AVFrame *in,int w,int h,int format) {
    AVFrame *out=av_frame_alloc();assert(out);
    assert(!av_buffersrc_add_frame_flags(source,in,AV_BUFFERSRC_FLAG_KEEP_REF));
    assert(!av_buffersink_get_frame(sink,out));
    assert(out->width==w&&out->height==h&&out->format==format&&out->pts==in->pts);
    assert(out->opaque_ref&&out->opaque_ref->size==8&&out->opaque_ref->data[0]==42);
    assert(out->nb_side_data==1&&out->side_data[0]->size==20&&out->side_data[0]->data[19]==13);
    assert(!strcmp(av_dict_get(out->metadata,"test",NULL,0)->value,"preserved"));
    assert(format==AV_PIX_FMT_YUV420P?out->data[0][0]==16:((uint16_t *)out->data[0])[0]==64);
    av_frame_free(&out);
}
int main(void) {
    /* Independent prepared graphs for each input generation; this does not
     * enable dynamic specs or fan-out in the production pipeline by itself. */
    for(int n=0;n<4;n++) {
        int ten=n&1;enum AVPixelFormat format=ten?AV_PIX_FMT_YUV420P10LE:AV_PIX_FMT_YUV420P;
        AVFrame *f=input(n<2?32:64,n<2?24:48,format,90000+n*3600);
        AVFilterGraph *graph=NULL;AVFilterContext *source=NULL,*sink=NULL;
        assert(!naki_video_filter_prepare(&graph,&source,&sink,f,(AVRational){1,90000},160,96,format));
        pass(source,sink,f,160,96,format);
        AVFilterGraph *failed=NULL;AVFilterContext *bad_source=NULL,*bad_sink=NULL;
        assert(naki_video_filter_prepare(&failed,&bad_source,&bad_sink,f,(AVRational){1,90000},160,96,AV_PIX_FMT_NONE)<0);
        assert(!failed&&!bad_source&&!bad_sink);f->pts+=3600;pass(source,sink,f,160,96,format);
        assert(!av_buffersrc_add_frame_flags(source,NULL,0));AVFrame *out=av_frame_alloc();assert(out);
        assert(av_buffersink_get_frame(sink,out)==AVERROR_EOF);av_frame_free(&out);av_frame_free(&f);avfilter_graph_free(&graph);
    }
    puts("private filter preparation: 8/10-bit, two input sizes, fixed canvas, PTS/opaque/side data, failed candidate isolation, EOF passed");return 0;
}
