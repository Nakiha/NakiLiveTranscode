#ifndef NAKI_VIDEO_INTERNAL_H
#define NAKI_VIDEO_INTERNAL_H
#define LEDGER_CAPACITY 256
#include "pipeline_internal.h"
#include <libavutil/opt.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/avfilter.h>
#include "live_rate.h"
#include "video_filter.h"
typedef struct Ledger { int used; int64_t pts; Stamp stamp; AVBufferRef *owner; } Ledger;
void pipeline_ledger_clear(Pipeline *p);
int pipeline_encoder_send(Pipeline *p, const AVFrame *frame);
int pipeline_receive_encoded(Pipeline *p);
int pipeline_encode_frame(Pipeline *p, AVFrame *frame);
int pipeline_ledger_pending(Pipeline *p);
int pipeline_filter_output(Pipeline *p);
struct NakiVideo {
    AVCodecContext *encoder;
    AVFilterGraph *graph;
    AVFilterContext *source,*sink;
    NakiWatermarkCompositor *watermark_compositor;
    uint64_t watermark_revision,filter_generation,rendition_frames;
    int input_width,input_height,input_format;
    enum AVColorSpace input_colorspace;
    enum AVColorRange input_range;
    NakiIntraState intra;
    Ledger ledger[LEDGER_CAPACITY];
    NakiLiveRate rate;
    int64_t last_pts,last_output_display_pts,encoder_api_us;
    int have_output_display_pts,encoder_draining,encoder_poll_pending,encoder_polled_packet;
};
#endif
