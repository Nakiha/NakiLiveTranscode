#include "input_probe.h"
#include <libavutil/error.h>

#if !defined(AVFORMAT_LIVE_VIDEO_PROBE_VERSION) || AVFORMAT_LIVE_VIDEO_PROBE_VERSION != 1
#error "The matching FFmpeg buffered live video probe API 1 is required"
#endif

int naki_probe_video(AVFormatContext *input) {
    if (!input) return AVERROR(EINVAL);
    /* An older DSO with the same SONAME can otherwise silently ignore the
     * new flag when LD_LIBRARY_PATH overrides the verified SDK at runtime. */
    if (avformat_version()<AV_VERSION_INT(63,3,103)) return AVERROR(ENOSYS);
    input->flags |= AVFMT_FLAG_LIVE_VIDEO_PROBE;
    int ret=avformat_find_stream_info(input,NULL);
    if (ret<0) return ret;
    /* Match the fork's selection exactly. av_find_best_stream may choose a
     * different, not-yet-ready track, particularly with multi-video inputs. */
    for (unsigned i=0;i<input->nb_streams;i++) {
        const AVStream *stream=input->streams[i];
        const AVCodecParameters *par=stream->codecpar;
        if (par->codec_type!=AVMEDIA_TYPE_VIDEO ||
            (stream->disposition&AV_DISPOSITION_ATTACHED_PIC) ||
            (par->codec_id!=AV_CODEC_ID_H264 && par->codec_id!=AV_CODEC_ID_HEVC)) continue;
        if (par->width<=0 || par->height<=0 || par->format<0 ||
            stream->time_base.num<=0 || stream->time_base.den<=0) return AVERROR_INVALIDDATA;
        return (int)i;
    }
    return AVERROR_STREAM_NOT_FOUND;
}
