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
/* Explicit integer decimation, never interpolation or timestamp retiming.
 * Unknown/fractional non-integral ratios fail rather than pretending to be CFR. */
static inline int naki_video_rate_output(const NakiVideoRate *input,int fps,NakiVideoRate *output,int *divisor) {
    if(!fps) {*output=*input;*divisor=1;return 0;}
    int64_t denominator=(int64_t)input->value.den*fps;
    if(fps<1||fps>240||input->from_fallback||denominator<=0||input->value.num<denominator||
       input->value.num%denominator)return AVERROR(EINVAL);
    int64_t ratio=input->value.num/denominator;
    if(ratio>256)return AVERROR(ERANGE);
    *divisor=(int)ratio;
    return naki_video_rate_resolve((AVRational){fps,1},fps,output);
}
static inline int naki_video_rate_keep(uint64_t frame_id,int divisor,int protected_picture) {
    return protected_picture || (frame_id-1)%(unsigned)divisor==0;
}
#endif
