#ifndef NAKI_WATERMARK_H
#define NAKI_WATERMARK_H
#include "watermark_config.h"
#include <stdatomic.h>
#include <libavutil/frame.h>
#include <libavutil/rational.h>
typedef struct NakiWatermark NakiWatermark;
typedef struct NakiWatermarkCompositor NakiWatermarkCompositor;
NakiWatermark *naki_watermark_open(const NakiWatermarkConfig *config,int64_t accepted_us,atomic_int *cancel);
int naki_watermark_request(NakiWatermark *wm,const NakiWatermarkConfig *config,uint64_t revision,char *error,size_t cap);
int naki_watermark_wait(NakiWatermark *wm);
void naki_watermark_stop(NakiWatermark *wm);
void naki_watermark_close(NakiWatermark **wm);
void naki_watermark_status(NakiWatermark *wm,yyjson_mut_doc *doc);
/* Call before output/header creation, also when initially disabled. */
int naki_watermark_video(NakiWatermark *wm,const AVFrame *input,int output_width,int output_height);
/* One synchronous frame in / frame out. Never performs resource I/O or decoding.
 * Source timestamp is for the GIF clock; frame->pts/time_base stay unchanged. */
int naki_watermark_frame(NakiWatermark *wm,NakiWatermarkCompositor **compositor,AVFrame *frame,
                         AVRational time_base,int64_t source_us,uint64_t *revision);
void naki_watermark_compositor_free(NakiWatermarkCompositor **compositor);
#endif
