#include "video_filter.h"
#include <errno.h>
#include <stdio.h>
#include <libavutil/pixdesc.h>
static int prepare(AVFilterGraph **graph,AVFilterContext **source,AVFilterContext **sink,
    const AVFrame *input,AVRational tb,int width,int height,enum AVPixelFormat format,int fixed,
    enum AVColorSpace colorspace,enum AVColorRange range) {
    if(!graph||!source||!sink||*graph||*source||*sink||!input)return AVERROR(EINVAL);
    const char *pixel_format=av_get_pix_fmt_name(format);if(!pixel_format)return AVERROR(EINVAL);
    AVFilterGraph *g=avfilter_graph_alloc();if(!g)return AVERROR(ENOMEM);
    g->nb_threads=1;AVFilterContext *src=NULL,*dst=NULL;
    char args[512],chain[256];AVRational sar=input->sample_aspect_ratio;
    if(!sar.num||!sar.den)sar=(AVRational){1,1};
    snprintf(args,sizeof(args),"video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d:colorspace=%d:range=%d",
        input->width,input->height,input->format,tb.num,tb.den,sar.num,sar.den,
        input->colorspace==AVCOL_SPC_UNSPECIFIED?AVCOL_SPC_BT470BG:input->colorspace,
        input->color_range==AVCOL_RANGE_UNSPECIFIED?AVCOL_RANGE_MPEG:input->color_range);
    int ret=avfilter_graph_create_filter(&src,avfilter_get_by_name("buffer"),"in",args,NULL,g);
    if(ret<0)goto fail;
    ret=avfilter_graph_create_filter(&dst,avfilter_get_by_name("buffersink"),"out",NULL,NULL,g);
    if(ret<0)goto fail;
    if(fixed)snprintf(chain,sizeof(chain),"scale=%d:%d:flags=bilinear:out_color_matrix=%d:out_range=%d,format=pix_fmts=%s",
        width,height,colorspace==AVCOL_SPC_UNSPECIFIED?AVCOL_SPC_BT470BG:colorspace,range==AVCOL_RANGE_UNSPECIFIED?AVCOL_RANGE_MPEG:range,pixel_format);
    else if(width)snprintf(chain,sizeof(chain),"scale=%d:%d:flags=bilinear,format=pix_fmts=%s",width,height,pixel_format);
    else snprintf(chain,sizeof(chain),"null,format=pix_fmts=%s",pixel_format);
    AVFilterInOut *outputs=avfilter_inout_alloc(),*inputs=avfilter_inout_alloc();
    if(!outputs||!inputs) {avfilter_inout_free(&outputs);avfilter_inout_free(&inputs);ret=AVERROR(ENOMEM);goto fail;}
    outputs->name=av_strdup("in");outputs->filter_ctx=src;outputs->pad_idx=0;
    inputs->name=av_strdup("out");inputs->filter_ctx=dst;inputs->pad_idx=0;
    if(!outputs->name||!inputs->name)ret=AVERROR(ENOMEM);
    else ret=avfilter_graph_parse_ptr(g,chain,&inputs,&outputs,NULL);
    avfilter_inout_free(&inputs);avfilter_inout_free(&outputs);
    if(ret<0||(ret=avfilter_graph_config(g,NULL))<0)goto fail;
    *graph=g;*source=src;*sink=dst;return 0;
fail:
    avfilter_graph_free(&g);return ret;
}
int naki_video_filter_prepare(AVFilterGraph **g,AVFilterContext **s,AVFilterContext **d,
    const AVFrame *f,AVRational tb,int w,int h,enum AVPixelFormat fmt) {
    return prepare(g,s,d,f,tb,w,h,fmt,0,AVCOL_SPC_UNSPECIFIED,AVCOL_RANGE_UNSPECIFIED);
}
int naki_video_filter_prepare_fixed(AVFilterGraph **g,AVFilterContext **s,AVFilterContext **d,
    const AVFrame *f,AVRational tb,int w,int h,enum AVPixelFormat fmt,enum AVColorSpace c,enum AVColorRange r) {
    return prepare(g,s,d,f,tb,w,h,fmt,1,c,r);
}
