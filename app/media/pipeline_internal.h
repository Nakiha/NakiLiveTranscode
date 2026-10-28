#ifndef NAKI_PIPELINE_INTERNAL_H
#define NAKI_PIPELINE_INTERNAL_H
#include "worker/job.h"
#include "platform/runtime.h"
#include "platform/wakeup.h"
#include "worker/frame_policy_runtime.h"
#include "pipeline_reader.h"
#include "watermark.h"
#include "video_filter.h"
#include "live_rate.h"
#include "frame_trace.h"
#include "branch_queue.h"
#include "crash/crash.h"
#include "media_hooks.h"
#include "input_probe.h"
#include "control/resource_limits.h"
#include "audio_signature.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavformat/live_mux.h>
#include "destination_runtime.h"
#include <libavformat/read_origin.h>
#include <libavcodec/bsf.h>
#include <libavutil/buffer.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libavutil/mem.h>
#include <libavutil/time.h>
#include <libavutil/imgutils.h>

/* Private worker state. The shared pipeline owns input/async and the reader;
 * each output thread owns its encoder/filter/mux and delivery references.
 * Reader ring/SEI obligations use ring_mutex; never nest Job.mutex under it.
 * Job snapshots use Job.mutex; callbacks and logging run after unlocking. */
#define LEDGER_CAPACITY 256
#define IO_TIMEOUT_US 10000000
#define BRANCH_ADMISSION_GRACE_US 100000

#if !defined(AVFORMAT_LIVE_INPUT_POLL_VERSION) || AVFORMAT_LIVE_INPUT_POLL_VERSION != 1
#error "The matching FFmpeg resumable live input poll API 1 is required"
#endif

typedef struct Interrupt { Job *job; atomic_int_fast64_t deadline; atomic_int cleanup; } Interrupt;
typedef struct Ledger { int used; int64_t pts; Stamp stamp; AVBufferRef *owner; } Ledger;
typedef struct MuxTrace { Job *fanout_job; Stamp stamp; AVBufferRef *owner; int64_t encoded_us, pts_us, hook_us; int bytes, picture_type; } MuxTrace;
typedef struct DecodePump {
    pthread_mutex_t mutex;NakiWakeup wake;pthread_t thread;
    AVFrame *frame;InputItem audio;
    int occupied,prefer_video,done,error,started;
} DecodePump;
typedef struct Branch Branch;
typedef struct NakiMux NakiMux;
typedef struct Pipeline {
    Job *job;
    NakiMediaHooks hooks;
    NakiDestinations *destinations;
    void *destination_owner;
    int destination_copy;
    const char *stage;
    AVFormatContext *input;
    NakiMux *mux; /* Opaque, exclusively owned output state. */
    AVCodecContext *decoder, *encoder;
    AVCodecAsyncContext *async;
    AVFrame initial_input_properties; /* Immutable snapshot before async takes decoder ownership. */
    int64_t decoder_receive_us; /* receive return, written only by decoder frontend owner */
    AVFilterGraph *graph;
    AVFilterContext *source, *sink;
    NakiWatermarkCompositor *watermark_compositor;
    uint64_t watermark_revision;
    AVStream *video;
    int stream_index;
    int input_width, input_height, input_format, seen_input_i;
    AVRational input_tb, frame_rate;
    NakiVideoRate video_rate;
    NakiVideoRate output_rate;
    int rate_divisor;
    int input_wrap_bits;
    enum AVCodecID sei_codec;
    int sei_length_size,sei_read_error;
    NakiSeiConfig sei_input;
    int64_t next_frame_id;
    uint64_t rendition_frames;
    NakiTimeline timeline;
    NakiIntraState intra;
    Interrupt read_interrupt;
    PipelineReader reader;
    DecodePump pump;
    Ledger ledger[LEDGER_CAPACITY];
    NakiLiveRate rate;
    int64_t last_pts, synthetic_pts, next_stats_us;
    int have_output_display_pts;
    int64_t last_output_display_pts;
    int64_t encoder_api_us;
    int encoder_draining,encoder_poll_pending,encoder_polled_packet;
    int64_t first_encoder_pts, previous_key_pts;
    int seen_output_key;
    Branch *branches;
    int branch_count;
    int64_t io_timeout_us;
    enum AVColorSpace input_colorspace;
    enum AVColorRange input_range;
} Pipeline;
struct Branch {
    Pipeline pipeline;
    NakiBranchQueue queue;
    pthread_t thread;
    int initialized,started;
    const OutputConfig *spec;
};

/* MODULE INTERFACES */
void pipeline_decode_notify(void *opaque);
int pipeline_decode_init(Pipeline *p);
int pipeline_decode_start(Pipeline *p);
int pipeline_decode_receive(Pipeline *p,AVFrame *frame,InputItem *audio);
void pipeline_decode_join(Pipeline *p);
void pipeline_decode_destroy(Pipeline *p);
void pipeline_refresh_auto(Pipeline *p);
int pipeline_prepare_decoded_frame(Pipeline *p,AVFrame *frame);
/* pipeline.c */
int pipeline_process_frame(Pipeline *p,AVFrame *frame);
int pipeline_handle_audio(Pipeline *p,const InputItem *audio);
int pipeline_stamp_valid(const AVBufferRef *ref);
const NakiSeiBundle *pipeline_stamp_sei(const AVBufferRef *ref);
void pipeline_trace_pts(yyjson_mut_doc *doc, const char *key, int64_t pts);
NakiFrameIdentity pipeline_frame_identity(Pipeline *p,const Stamp *stamp,int64_t output_pts,AVRational output_tb);
void pipeline_checkpoint(Pipeline *p, int64_t *field);
int pipeline_interrupted(void *opaque);
int pipeline_flush_deferred_audio(Pipeline *p);
int pipeline_filter_output(Pipeline *p);

/* pipeline_encode.c */
void pipeline_ledger_clear(Pipeline *p);
int pipeline_encoder_send(Pipeline *p, const AVFrame *frame);
int pipeline_receive_encoded(Pipeline *p);
int pipeline_validate_output_timestamps(Pipeline *p,const AVPacket *packet,AVRational time_base);
int64_t pipeline_source_rate(Pipeline *p,int64_t now);
int pipeline_encode_frame(Pipeline *p, AVFrame *frame);
int pipeline_ledger_pending(Pipeline *p);

/* Opaque mux ownership: handoff only while both owners are quiescent.
 * The caller starts/joins its writer; move never copies callback storage. */
int pipeline_mux_prepare(Pipeline *p);
int pipeline_mux_move(Pipeline *from, Pipeline *to);
void pipeline_mux_release(Pipeline *p);
void pipeline_mux_retire_audio(Pipeline *p);
int pipeline_mux_live(const Pipeline *p);
uint64_t pipeline_mux_bytes(const Pipeline *p);
void pipeline_mux_cancel(Pipeline *p);

/* pipeline_mux.c */
int pipeline_drain_mux(Pipeline *p, int force);
int pipeline_submit_mux(Pipeline *p, AVPacket *packet);

/* pipeline_output.c */
int pipeline_finalize_output(Pipeline *p,int primary,int stopped);
int pipeline_open_output(Pipeline *p, AVFrame *frame);
int pipeline_open_mux(Pipeline *p);
int pipeline_drain_output(Pipeline *p);
void pipeline_release_media_resources(Pipeline *p,AVFrame **receive_frame);
void pipeline_complete_job(Pipeline *p,int ret,int stopped);

#endif
