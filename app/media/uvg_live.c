#include "uvg_live.h"
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>
#include <libavutil/intreadwrite.h>
#include <uvg266.h>

/* Upstream owns process-global strategy and RC tables. Each normal service
 * worker is a process. Protect the full lifetime against a second instance. */
static atomic_flag in_use = ATOMIC_FLAG_INIT;
struct NakiUvg {
    const uvg_api *api;
    uvg_encoder *encoder;
    AVPacket *pending;
    int width, height;
    bool eof;
};

static int copy_chunks(uint8_t *dst, size_t size, const uvg_data_chunk *chunk)
{
    for (; chunk; chunk=chunk->next) {
        if (chunk->len > size) return AVERROR_INVALIDDATA;
        memcpy(dst, chunk->data, chunk->len);
        dst += chunk->len;
        size -= chunk->len;
    }
    return size ? AVERROR_INVALIDDATA : 0;
}

void naki_uvg_close(NakiUvg **owner)
{
    NakiUvg *ctx=*owner;
    if (!ctx) return;
    if (ctx->encoder) ctx->api->encoder_close(ctx->encoder);
    av_packet_free(&ctx->pending);
    av_freep(owner);
    atomic_flag_clear(&in_use);
}

int naki_uvg_open(NakiUvg **owner, AVCodecContext *c, int quality,
                  int follow_policy, int extra_tools)
{
    if (c->pix_fmt!=AV_PIX_FMT_YUV420P || c->width<=0 || c->height<=0 ||
        ((c->width|c->height)&1) || c->framerate.num<=0 || c->framerate.den<=0)
        return AVERROR(EINVAL);
    if (atomic_flag_test_and_set(&in_use)) return AVERROR(EBUSY);
    NakiUvg *ctx=av_mallocz(sizeof(*ctx));
    if (!ctx) { atomic_flag_clear(&in_use); return AVERROR(ENOMEM); }
    *owner=ctx;
    ctx->api=uvg_api_get(8);
    ctx->pending=av_packet_alloc();
    ctx->width=c->width;
    ctx->height=c->height;
    uvg_config *cfg=ctx->api ? ctx->api->config_alloc() : NULL;
    int ret=AVERROR(ENOMEM);
    if (!cfg || !ctx->pending || !ctx->api->config_init(cfg)) goto done;
    ret=AVERROR(EINVAL);
    if (!ctx->api->config_parse(cfg,"preset","ultrafast") ||
        !ctx->api->config_parse(cfg,"gop","lp-g4d3t1")) goto done;
    cfg->width=c->width; cfg->height=c->height;
    cfg->framerate_num=c->framerate.num; cfg->framerate_denom=c->framerate.den;
    cfg->qp=quality; cfg->owf=0; cfg->open_gop=0; cfg->wpp=1;
    cfg->threads=c->thread_count; cfg->calc_psnr=0;
    cfg->intra_period=follow_policy ? 0 : c->gop_size;
    cfg->vps_period=(c->flags&AV_CODEC_FLAG_GLOBAL_HEADER) ? -1 : 1;
    cfg->vui.colorprim=c->color_primaries; cfg->vui.transfer=c->color_trc;
    cfg->vui.colormatrix=c->colorspace; cfg->vui.fullrange=c->color_range==AVCOL_RANGE_JPEG;
    cfg->vui.sar_width=c->sample_aspect_ratio.num; cfg->vui.sar_height=c->sample_aspect_ratio.den;
    if (c->bit_rate>0) { cfg->target_bitrate=c->bit_rate; cfg->rc_algorithm=UVG_LAMBDA; }
    if (extra_tools) {
        cfg->amvr=1; cfg->dep_quant=1;
        cfg->max_btt_depth[0]=1; cfg->max_btt_depth[1]=1;
    }
    ctx->encoder=ctx->api->encoder_open(cfg);
    if (!ctx->encoder) goto done;
    ret=0;
    if (c->flags&AV_CODEC_FLAG_GLOBAL_HEADER) {
        uint32_t len=0; uvg_data_chunk *chunks=NULL;
        if (!ctx->api->encoder_headers(ctx->encoder,&chunks,&len) || !len || len>INT_MAX-AV_INPUT_BUFFER_PADDING_SIZE)
            ret=AVERROR_INVALIDDATA;
        else if (!(c->extradata=av_mallocz(len+AV_INPUT_BUFFER_PADDING_SIZE)))
            ret=AVERROR(ENOMEM);
        else {
            ret=copy_chunks(c->extradata,len,chunks);
            c->extradata_size=len;
        }
        ctx->api->chunk_free(chunks);
    }
done:
    if (cfg) ctx->api->config_destroy(cfg);
    if (ret<0) naki_uvg_close(owner);
    return ret;
}

static uvg_picture *copy_picture(NakiUvg *ctx, const AVFrame *f)
{
    int pw=(f->width+7)&~7, ph=(f->height+7)&~7;
    uvg_picture *pic=ctx->api->picture_alloc(pw,ph);
    if (!pic) return NULL;
    for (int p=0;p<3;p++) {
        int shift=p!=0, w=f->width>>shift, h=f->height>>shift;
        int stride=pic->stride>>shift;
        for (int y=0;y<(ph>>shift);y++) {
            uint8_t *dst=pic->data[p]+y*stride;
            memcpy(dst,f->data[p]+(ptrdiff_t)(y<h?y:h-1)*f->linesize[p],w);
            memset(dst+w,dst[w-1],(pw>>shift)-w);
        }
    }
    pic->pts=f->pts;
    pic->live_force_intra=f->pict_type==AV_PICTURE_TYPE_I;
    return pic;
}

int naki_uvg_send(NakiUvg *ctx, const AVFrame *f, int qp)
{
    if (ctx->eof) return AVERROR_EOF;
    if (ctx->pending->size) return AVERROR(EAGAIN);
    if (f && (f->width!=ctx->width || f->height!=ctx->height ||
              f->format!=AV_PIX_FMT_YUV420P || qp < -1 || qp>51)) return AVERROR(EINVAL);
    uvg_picture *pic=f ? copy_picture(ctx,f) : NULL;
    if (f && !pic) return AVERROR(ENOMEM);
    if (pic) pic->live_qp_plus1=qp+1;
    uvg_picture *source=NULL;
    uvg_data_chunk *chunks=NULL;
    uint32_t size=0;
    uvg_frame_info info={0};
    int ret=AVERROR_INVALIDDATA;
    if (!ctx->api->encoder_encode(ctx->encoder,pic,&chunks,&size,NULL,&source,&info)) goto done;
    if (!f) { ctx->eof=true; ret=!size&&!source&&!chunks ? 0 : AVERROR_INVALIDDATA; goto done; }
    if (!source || source->pts!=f->pts || !chunks || !size || size>INT_MAX ||
        (qp>=0 && info.qp!=qp)) goto done;
    AVPacket *pkt=ctx->pending;
    if ((ret=av_new_packet(pkt,size))<0 || (ret=copy_chunks(pkt->data,size,chunks))<0) goto done;
    pkt->pts=pkt->dts=source->pts;
    pkt->duration=f->duration;
    if (info.nal_unit_type==UVG_NAL_IDR_N_LP || info.nal_unit_type==UVG_NAL_IDR_W_RADL)
        pkt->flags|=AV_PKT_FLAG_KEY;
    uint8_t *stats=av_packet_new_side_data(pkt,AV_PKT_DATA_QUALITY_STATS,8);
    if (!stats) { ret=AVERROR(ENOMEM); goto done; }
    memset(stats,0,8);
    AV_WL32(stats,info.qp*FF_QP2LAMBDA);
    stats[4]=info.slice_type==UVG_SLICE_I ? AV_PICTURE_TYPE_I :
             info.slice_type==UVG_SLICE_P ? AV_PICTURE_TYPE_P : AV_PICTURE_TYPE_B;
done:
    if (ret<0) av_packet_unref(ctx->pending);
    ctx->api->picture_free(pic);
    ctx->api->picture_free(source);
    ctx->api->chunk_free(chunks);
    return ret;
}

int naki_uvg_receive(NakiUvg *ctx, AVPacket *pkt)
{
    if (!ctx->pending->size) return ctx->eof ? AVERROR_EOF : AVERROR(EAGAIN);
    av_packet_move_ref(pkt,ctx->pending);
    return 0;
}
