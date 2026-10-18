#ifndef NAKI_VIDEO_FILTER_H
#define NAKI_VIDEO_FILTER_H
#include <libavfilter/avfilter.h>
#include <libavutil/frame.h>
/* Prepare a private owner graph. Publish output pointers only on success;
 * existing graphs/encoder/mux state are never touched by this operation. */
int naki_video_filter_prepare(AVFilterGraph **graph,AVFilterContext **source,AVFilterContext **sink,
    const AVFrame *input,AVRational time_base,int width,int height,enum AVPixelFormat format);
int naki_video_filter_prepare_fixed(AVFilterGraph **graph,AVFilterContext **source,AVFilterContext **sink,
    const AVFrame *input,AVRational time_base,int width,int height,enum AVPixelFormat format,
    enum AVColorSpace colorspace,enum AVColorRange range);
#endif
