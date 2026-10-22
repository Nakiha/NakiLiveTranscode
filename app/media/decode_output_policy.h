#ifndef NAKI_DECODE_OUTPUT_POLICY_H
#define NAKI_DECODE_OUTPUT_POLICY_H
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
/* Call before avcodec_open2. This policy never creates an async owner and never
 * asserts an unverified H264 POC contract. A missing H264 extension falls back
 * to the pinned decoder. There is no runtime mutation/heat-switch API. */
static inline int naki_decode_output_policy(AVCodecContext *ctx,int enabled) {
    int ret;
    if(enabled<0)enabled=ctx->codec_id==AV_CODEC_ID_HEVC;
    if(ctx->codec_id==AV_CODEC_ID_HEVC) {
        if((ret=av_opt_set_int(ctx->priv_data,"live_idr_early_output",enabled,0))<0)return ret;
        return av_opt_set_int(ctx->priv_data,"live_poc_early_output",enabled,0);
    }
    if(ctx->codec_id==AV_CODEC_ID_H264 &&
       av_opt_find(ctx->priv_data,"live_poc_early_output",NULL,0,0))
        return av_opt_set_int(ctx->priv_data,"live_poc_early_output",enabled,0);
    return 0;
}
#endif
