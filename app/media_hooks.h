#ifndef NAKI_MEDIA_HOOKS_H
#define NAKI_MEDIA_HOOKS_H

#include <stdint.h>
#include <libavcodec/codec_par.h>
#include <libavcodec/packet.h>
#include <libavutil/avutil.h>
#include <libavutil/rational.h>

#define NAKI_MEDIA_HOOK_API 1
#define NAKI_MEDIA_HOOK_MAX_ADDED_BYTES 65536
struct Config;

typedef struct NakiMediaJobInfo {
    const char *job_id, *request_id;
    const struct Config *config;
    int64_t request_us;
} NakiMediaJobInfo;

typedef struct NakiAudioPacketEvent {
    const AVPacket *packet;
    const AVCodecParameters *parameters;
    AVRational time_base;
    int input_stream_index;
    /* CLOCK_MONOTONIC-compatible service times, not network/wire timestamps. */
    int64_t demuxed_us, dispatched_us;
} NakiAudioPacketEvent;

typedef struct NakiVideoEncodedEvent {
    enum AVCodecID codec_id;
    AVRational time_base; /* encoder time base; packet has not been rescaled */
    int64_t encoded_us;   /* immediately after successful receive_packet */
    int64_t input_sequence;
    enum AVPictureType input_picture_type;
    /* NONE if encoder statistics do not report it. Key != all intra pictures. */
    enum AVPictureType output_picture_type;
    int output_key;
} NakiVideoEncodedEvent;

typedef struct NakiMediaEnd {
    int cancelled;
    int error_code; /* 0 on completion, negative AVERROR on failure */
    const char *stage;
} NakiMediaEnd;

/* Internal, compile-time extension API, not a stable binary/plugin ABI.
 * All callbacks run serially on the pipeline owner, without application locks.
 * Event pointers/strings/frames/parameters are borrowed until return. Retain
 * packets with av_packet_clone and parameters with avcodec_parameters_copy.
 * Packet callbacks must be bounded and nonblocking: no network/file I/O, joins
 * or waits for analysis. Any asynchronous work needs its own bounded queue.
 * Return 0 or a negative AVERROR; errors fail the job, never silently fallback.
 * close is called once on graceful cleanup, including open failure if set;
 * fatal signals/process termination do not guarantee callbacks.
 */
typedef struct NakiMediaHooks {
    unsigned api_version;
    void *opaque;
    /* Once per accepted input audio item, before copy preparation/deferral.
     * Also receives audio when delivery is disabled or output is video-only.
     * Demux may already have repaired timestamps; these are not raw wire PTS.
     */
    int (*audio_packet)(void *opaque, const NakiAudioPacketEvent *event);
    /* Once per encoded video AU, including encoder EOF drain, before mux.
     * May replace/extend compressed payload to add metadata, at most 64KiB.
     * Must preserve PTS/DTS/duration/stream_index/flags and keep a valid AU.
     * av_packet_make_writable is required before changing shared data. Do not
     * retain/mutate the packet header after return or use opaque_ref for state.
     */
    int (*video_packet)(void *opaque, const NakiVideoEncodedEvent *event, AVPacket *packet);
    void (*close)(void *opaque, const NakiMediaEnd *event);
} NakiMediaHooks;

/* Exactly one factory is linked into the worker. Default returns no callbacks.
 * Select a private implementation with CMake NAKI_MEDIA_HOOK_SOURCE. Its state
 * belongs to this job/worker; HTTP controller loss does not recreate it.
 */
int naki_media_hooks_open(const NakiMediaJobInfo *info, NakiMediaHooks *hooks);
#endif
