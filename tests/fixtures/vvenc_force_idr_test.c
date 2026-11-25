/* Exercise the public libavcodec send/receive path and the live write ledger. */
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/vvenc_live.h>
#include <libavutil/dict.h>
#include <libavutil/time.h>

enum { FRAMES = 91, WIDTH = 128, HEIGHT = 192 };
typedef struct {
    int sent, written;
    int external;
    int64_t pts[FRAMES], last_dts;
    unsigned char seen[FRAMES];
} Run;

static int requested(int i) {
    return i == 3 || i == 7 || i == 8 || i == 80 || i == 81 || i == 90;
}

static void collect(AVCodecContext *ctx, AVPacket *packet, Run *run) {
    int i = 0, idr = 0, vcl = 0;
    while (i < run->sent && run->pts[i] != packet->pts) ++i;
    assert(i < run->sent && !run->seen[i]);
    run->seen[i] = 1;
    for (int n = 0; n + 4 < packet->size; ++n) {
        const uint8_t *p = packet->data + n;
        if (p[0] || p[1] || p[2] != 1) continue;
        int type = p[4] >> 3;
        if (type <= 11) ++vcl;
        if (type == 7 || type == 8) ++idr;
    }
    assert(vcl == 1 && idr == (i == 0 || requested(i) || (!run->external && i == 60)));
    assert(!!(packet->flags & AV_PKT_FLAG_KEY) == idr);
    assert(!run->written || packet->dts > run->last_dts);
    run->last_dts = packet->dts;
    int64_t dts_us = av_rescale_q(packet->dts, ctx->time_base, AV_TIME_BASE_Q);
    int64_t bits = 8LL * packet->size;
    assert(!av_vvenc_live_coded(ctx, packet->pts, dts_us, bits, NULL));
    assert(!av_vvenc_live_written(ctx, dts_us, av_gettime_relative(), bits));
    ++run->written;
    av_packet_unref(packet);
}

static int receive(AVCodecContext *ctx, AVPacket *packet, Run *run) {
    int ret;
    while ((ret = avcodec_receive_packet(ctx, packet)) == 0) collect(ctx, packet, run);
    return ret;
}

static void encode(int tier, int slots, int external) {
    Run run = {.external = external};
    const AVCodec *codec = avcodec_find_encoder_by_name("libvvenc");
    assert(codec);
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    assert(ctx && packet && frame);
    ctx->width = WIDTH;
    ctx->height = HEIGHT;
    ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    ctx->time_base = (AVRational){1, 90000};
    ctx->framerate = (AVRational){30, 1};
    ctx->thread_count = 8;
    ctx->thread_type = FF_THREAD_SLICE;
    ctx->rc_max_rate = 2000000;
    ctx->gop_size = 60;
    ctx->max_b_frames = 3;
    AVDictionary *options = NULL;
    assert(!av_dict_set(&options, "qp", "31", 0));
    assert(!av_dict_set_int(&options, "live-pts-gop", 1, 0));
    assert(!av_dict_set_int(&options, "live-max-future", 10, 0));
    assert(!av_dict_set_int(&options, "live-r28-tier", tier, 0));
    assert(!av_dict_set_int(&options, "live-r28-pictures", slots, 0));
    assert(!av_dict_set_int(&options, "live-frame-intra", external, 0));
    assert(!avcodec_open2(ctx, codec, &options) && !av_dict_count(options));
    av_dict_free(&options);

    frame->width = WIDTH;
    frame->height = HEIGHT;
    frame->format = ctx->pix_fmt;
    assert(!av_frame_get_buffer(frame, 32));
    for (int i = 0; i < FRAMES; ++i) {
        assert(!av_frame_make_writable(frame));
        for (int c = 0; c < 3; ++c) {
            int h = c ? HEIGHT / 2 : HEIGHT, w = c ? WIDTH / 2 : WIDTH;
            for (int y = 0; y < h; ++y)
                memset(frame->data[c] + y * frame->linesize[c], 32 + (i + 3*y + 32*c) % 184, w);
        }
        run.pts[i] = 90000 + 3000LL*i + (i % 5 == 1 ? 600 : 0);
        frame->pts = run.pts[i];
        frame->pict_type = requested(i) ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
        int ret = avcodec_send_frame(ctx, frame);
        if (ret == AVERROR(EAGAIN)) {
            assert(receive(ctx, packet, &run) == AVERROR(EAGAIN));
            ret = avcodec_send_frame(ctx, frame);
        }
        assert(ret == 0);
        ++run.sent;
        assert(receive(ctx, packet, &run) == AVERROR(EAGAIN));
    }
    assert(!avcodec_send_frame(ctx, NULL));
    assert(receive(ctx, packet, &run) == AVERROR_EOF);
    assert(run.written == FRAMES && !av_vvenc_live_pending(ctx));
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&ctx);
}

int main(void) {
    encode(1, 4, 0);
    encode(2, 8, 1);
    return 0;
}
