/* Offline quality evidence: synchronous native decode, service reference filter,
 * tightly packed 10-bit planes. Never used by media workers or timed runs. */
#include "media/video_filter.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/imgutils.h>

#define LIMIT 250000
typedef struct Export {
    int indices[LIMIT], requested, next, decoded, width, height, reference;
    AVRational tb;
    FILE *raw, *manifest;
    AVFilterGraph *graph;
    AVFilterContext *source, *sink;
    AVFrame *filtered;
    uint8_t *packed;
    int bytes;
} Export;

static int emit(Export *e, AVFrame *f) {
    int index = e->decoded++;
    if (e->next == e->requested || index != e->indices[e->next]) return 0;
    if (f->pts == AV_NOPTS_VALUE) return AVERROR_INVALIDDATA;
    int64_t pts = f->pts;
    if (!e->reference && (f->width != e->width || f->height != e->height ||
        (f->format != AV_PIX_FMT_YUV420P && f->format != AV_PIX_FMT_YUV420P10LE)))
        return AVERROR_INVALIDDATA;
    /* Lift decoded 8-bit samples into the common 10-bit metric domain. Keep
       the same unrounded 10-bit reference for both encoder precisions. */
    if (e->reference || f->format == AV_PIX_FMT_YUV420P) {
        if (!e->graph) {
            int ret = naki_video_filter_prepare(&e->graph, &e->source, &e->sink,
                f, e->tb, e->width, e->height, AV_PIX_FMT_YUV420P10LE);
            if (ret < 0) return ret;
        }
        int ret = av_buffersrc_add_frame_flags(e->source, f, AV_BUFFERSRC_FLAG_KEEP_REF);
        if (ret < 0) return ret;
        ret = av_buffersink_get_frame(e->sink, e->filtered);
        if (ret < 0) return ret;
        f = e->filtered;
    }
    if (f->width != e->width || f->height != e->height || f->format != AV_PIX_FMT_YUV420P10LE)
        return AVERROR_INVALIDDATA;
    int ret = av_image_copy_to_buffer(e->packed, e->bytes, (const uint8_t *const *)f->data,
        f->linesize, f->format, f->width, f->height, 1);
    if (ret != e->bytes || fwrite(e->packed, 1, e->bytes, e->raw) != (size_t)e->bytes)
        return AVERROR(EIO);
    if (fprintf(e->manifest, "%s{\"index\":%d,\"pts\":%" PRId64 ",\"tb_num\":%d,\"tb_den\":%d,"
        "\"colorspace\":%d,\"range\":%d}", e->next ? ",\n" : "", index, pts,
        e->tb.num, e->tb.den, f->colorspace, f->color_range) < 0) return AVERROR(EIO);
    e->next++;
    av_frame_unref(e->filtered);
    return 0;
}

static int receive(AVCodecContext *decoder, AVFrame *frame, Export *e) {
    int ret;
    while (e->next < e->requested && (ret = avcodec_receive_frame(decoder, frame)) >= 0) {
        ret = emit(e, frame);
        av_frame_unref(frame);
        if (ret < 0) return ret;
    }
    if (e->next == e->requested) return 0;
    return ret == AVERROR(EAGAIN) || ret == AVERROR_EOF ? 0 : ret;
}

int main(int argc, char **argv) {
    if (argc != 8 || (strcmp(argv[7], "reference") && strcmp(argv[7], "decoded"))) {
        fprintf(stderr, "usage: %s INPUT RAW INDICES MANIFEST WIDTH HEIGHT reference|decoded\n", argv[0]);
        return 2;
    }
    av_log_set_level(AV_LOG_ERROR);
    Export *e = calloc(1, sizeof(*e));
    if (!e) return 3;
    AVFormatContext *input = NULL;
    AVCodecContext *decoder = NULL;
    AVFrame *frame = NULL;
    AVPacket *packet = NULL;
    FILE *indices = NULL;
    int ret = AVERROR(EINVAL);
    char *end = NULL;
    long dimension = strtol(argv[5], &end, 10);
    if (*end || dimension < 2 || dimension > 8192 || dimension % 2) goto done;
    e->width = (int)dimension;
    dimension = strtol(argv[6], &end, 10);
    if (*end || dimension < 2 || dimension > 8192 || dimension % 2) goto done;
    e->height = (int)dimension;
    e->reference = !strcmp(argv[7], "reference");
    indices = fopen(argv[3], "r");
    if (!indices) goto done;
    int value, scanned;
    while ((scanned = fscanf(indices, "%d", &value)) == 1) {
        if (e->requested == LIMIT || value < 0 || value >= LIMIT ||
            (e->requested && value <= e->indices[e->requested - 1])) goto done;
        e->indices[e->requested++] = value;
    }
    if (scanned != EOF || !e->requested) goto done;
    ret = avformat_open_input(&input, argv[1], NULL, NULL);
    if (ret < 0 || (ret = avformat_find_stream_info(input, NULL)) < 0) goto done;
    int video = av_find_best_stream(input, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (video < 0) { ret = video; goto done; }
    e->tb = input->streams[video]->time_base;
    const AVCodec *codec = avcodec_find_decoder(input->streams[video]->codecpar->codec_id);
    decoder = avcodec_alloc_context3(codec);
    frame = av_frame_alloc(); e->filtered = av_frame_alloc(); packet = av_packet_alloc();
    e->bytes = av_image_get_buffer_size(AV_PIX_FMT_YUV420P10LE, e->width, e->height, 1);
    e->packed = av_malloc(e->bytes);
    if (!decoder || !frame || !e->filtered || !packet || !e->packed) { ret = AVERROR(ENOMEM); goto done; }
    ret = avcodec_parameters_to_context(decoder, input->streams[video]->codecpar);
    decoder->thread_count = 2;
    if (ret < 0 || (ret = avcodec_open2(decoder, codec, NULL)) < 0) goto done;
    e->raw = fopen(argv[2], "wbx"); e->manifest = fopen(argv[4], "wx");
    if (!e->raw || !e->manifest) { ret = AVERROR(EIO); goto done; }
    if (fputs("[\n", e->manifest) == EOF) { ret = AVERROR(EIO); goto done; }
    while (e->next < e->requested && (ret = av_read_frame(input, packet)) >= 0) {
        if (packet->stream_index == video) {
            ret = avcodec_send_packet(decoder, packet);
            if (ret >= 0) ret = receive(decoder, frame, e);
        }
        av_packet_unref(packet);
        if (ret < 0) break;
    }
    if (ret == AVERROR_EOF) {
        ret = avcodec_send_packet(decoder, NULL);
        if (ret >= 0) ret = receive(decoder, frame, e);
    }
    if (ret >= 0 && e->next != e->requested) ret = AVERROR_INVALIDDATA;
    if (ret >= 0 && fputs("\n]\n", e->manifest) == EOF) ret = AVERROR(EIO);
done:
    if (indices) fclose(indices);
    if (e->raw && fclose(e->raw)) ret = AVERROR(EIO);
    if (e->manifest && fclose(e->manifest)) ret = AVERROR(EIO);
    av_frame_free(&frame); av_frame_free(&e->filtered); av_packet_free(&packet);
    avcodec_free_context(&decoder); avformat_close_input(&input); avfilter_graph_free(&e->graph);
    av_free(e->packed);
    if (ret < 0) {
        char error[AV_ERROR_MAX_STRING_SIZE]; av_strerror(ret, error, sizeof(error));
        fprintf(stderr, "quality export failed: %s (%d/%d frames)\n", error, e->next, e->requested);
    }
    free(e);
    return ret < 0 ? 1 : 0;
}
