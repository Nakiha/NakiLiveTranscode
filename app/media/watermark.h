#ifndef NAKI_WATERMARK_H
#define NAKI_WATERMARK_H
#include "control/watermark_config.h"
#include "control/component_update.h"
#include "control/operation_store.h"
#include "observability/logger.h"
#include <stdatomic.h>
#include <libavutil/frame.h>
#include <libavutil/rational.h>
typedef struct NakiWatermark NakiWatermark;
typedef struct NakiWatermarkCompositor NakiWatermarkCompositor;
NakiWatermark *naki_watermark_open(const NakiWatermarkConfig *config,int64_t accepted_us,atomic_int *cancel);
int naki_watermark_bind(NakiWatermark *wm,NakiOperations *operations,const char *job_id,const char *output_id,const char *create_request_id,Logger *logger);
int naki_watermark_submit(NakiWatermark *wm,const NakiUpdateRequest *request,const char *origin_request_id,
                          yyjson_mut_doc **result,char *error,size_t cap);
int naki_watermark_wait(NakiWatermark *wm);
void naki_watermark_stop(NakiWatermark *wm);
void naki_watermark_close(NakiWatermark **wm);
void naki_watermark_status(NakiWatermark *wm,yyjson_mut_doc *doc);
/* Call before output/header creation, also when initially disabled. */
int naki_watermark_video(NakiWatermark *wm,const AVFrame *input,int output_width,int output_height);
/* One synchronous frame in / frame out. Never performs resource I/O or decoding.
 * Source timestamp is for the GIF clock; frame->pts/time_base stay unchanged. */
int naki_watermark_frame_tracked(NakiWatermark *wm,NakiWatermarkCompositor **compositor,AVFrame *frame,
                         AVRational time_base,const NakiFrameIdentity *identity,uint64_t *revision,NakiOperationHandle *lease);
int naki_watermark_release_frame(NakiWatermark *wm,NakiOperationHandle lease);
int naki_watermark_written(NakiWatermark *wm,NakiOperationHandle lease,const NakiFrameIdentity *frame,int64_t written_us);
/* Owner-only completion for a claim made before media open/compose failed. */
void naki_watermark_owner_finish(NakiWatermark *wm,int error_code,int cancelled);
void naki_watermark_compositor_free(NakiWatermarkCompositor **compositor);
#endif
