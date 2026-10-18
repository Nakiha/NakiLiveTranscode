#ifndef NAKI_VIDEO_RATE_H
#define NAKI_VIDEO_RATE_H
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/rational.h>
typedef struct NakiVideoRate {
    AVRational value;
    int from_fallback,nominal_gop_frames;
    int64_t nominal_frame_work_us;
} NakiVideoRate;
/* Resolve once at startup. This controls nominal hints/accounting; source PTS
 * still determine cadence and the live VVC two-second CRA grid. */
static inline int naki_video_rate_resolve(AVRational guessed,int fallback,NakiVideoRate *out) {
    int use_fallback=guessed.num<=0||guessed.den<=0;
    if(use_fallback) {if(fallback<1||fallback>240)return AVERROR(EINVAL);guessed=(AVRational){fallback,1};}
    AVRational rate;av_reduce(&rate.num,&rate.den,guessed.num,guessed.den,INT_MAX);
    int64_t gop=av_rescale_q(2,(AVRational){1,1},av_inv_q(rate));
    if(gop>INT_MAX)return AVERROR(ERANGE);
    int64_t work=av_rescale_q(1,av_inv_q(rate),(AVRational){1,1000000});
    *out=(NakiVideoRate){.value=rate,.from_fallback=use_fallback,.nominal_gop_frames=gop>0?(int)gop:1,.nominal_frame_work_us=work>0?work:1};
    return 0;
}
#endif
