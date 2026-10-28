#ifndef NAKI_PIPELINE_INTERNAL_H
#define NAKI_PIPELINE_INTERNAL_H
#include "worker/job_media.h"
#include "platform/runtime.h"
#include "platform/wakeup.h"
#include "worker/frame_policy_runtime.h"
#include "pipeline_reader.h"
#include "watermark.h"
#include "frame_trace.h"
#include "branch_queue.h"
#include "crash/crash.h"
#include "media_hooks.h"
#include "control/resource_limits.h"
#include "audio_signature.h"
#include "encoded_description.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <libavformat/avformat.h>
#include <libavformat/live_mux.h>
#include "destination_runtime.h"
#include <libavutil/buffer.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <libavutil/time.h>
#include <libavutil/imgutils.h>

/* Orchestration handles and immutable timing. Native input/decode, video,
 * mux and branch representations are private to their respective owners.
 * Owners release through explicit APIs; no component copies another's state. */
#define LEDGER_CAPACITY 256
#define IO_TIMEOUT_US 10000000
#define BRANCH_ADMISSION_GRACE_US 100000

#if !defined(AVFORMAT_LIVE_INPUT_POLL_VERSION) || AVFORMAT_LIVE_INPUT_POLL_VERSION != 1
#error "The matching FFmpeg resumable live input poll API 1 is required"
#endif

typedef struct Interrupt { Job *job; atomic_int_fast64_t deadline; atomic_int cleanup; } Interrupt;
typedef struct MuxTrace { Job *fanout_job; Stamp stamp; AVBufferRef *owner; int64_t encoded_us, pts_us, hook_us; int bytes, picture_type; } MuxTrace;
typedef struct NakiBranches NakiBranches;
typedef struct NakiMux NakiMux;
typedef struct NakiInput NakiInput;
typedef struct NakiVideo NakiVideo;
typedef struct Pipeline {
    Job *job;
    NakiMediaHooks hooks;
    NakiDestinations *destinations;
    void *destination_owner;
    int destination_copy;
    const char *stage;
    NakiInput *in;
    NakiVideo *v;
    NakiMux *mux;
    NakiEncodedDescription *encoded;
    struct {AVRational time_base,avg_frame_rate;int index,valid;} video;
    AVRational input_tb,frame_rate;
    NakiVideoRate video_rate,output_rate;
    int rate_divisor,input_wrap_bits;
    int64_t first_encoder_pts;
    NakiBranches *branches;
    int64_t io_timeout_us;
} Pipeline;
/* MODULE INTERFACES */
int pipeline_branches_start(Pipeline *p);
int pipeline_branches_check(Pipeline *p,int finishing);
int pipeline_branches_frame(Pipeline *p,AVFrame *frame);
int pipeline_branches_audio(Pipeline *p,const InputItem *audio);
int pipeline_branches_finish(Pipeline *p,int aborting);
void pipeline_branches_release(Pipeline *p);
int pipeline_input_init(Pipeline *p);
int pipeline_input_open(Pipeline *p);
AVFrame pipeline_input_properties(const Pipeline *p);
int pipeline_input_receive(Pipeline *p,AVFrame *frame,InputItem *audio);
int pipeline_input_send(Pipeline *p,const AVPacket *packet);
void pipeline_input_tick(Pipeline *p);
int pipeline_input_sei_error(const Pipeline *p);
void pipeline_input_quiesce(Pipeline *p);
void pipeline_input_clear(Pipeline *p);
void pipeline_input_release_decoder(Pipeline *p);
void pipeline_input_close_demux(Pipeline *p);
void pipeline_input_destroy(Pipeline *p);
int pipeline_video_poll(Pipeline *p,int *progress);
int pipeline_video_buffer_size(const Pipeline *p);
int pipeline_video_force_idr(Pipeline *p);
int pipeline_video_written(Pipeline *p,int64_t media_us,int64_t written_us,int64_t bits);
int64_t pipeline_video_api_us(const Pipeline *p);
void pipeline_video_close(Pipeline *p);
void pipeline_video_release(Pipeline *p);
int pipeline_mux_format(Pipeline *p,int *global_header);
int pipeline_decode_start(Pipeline *p);
int pipeline_prepare_decoded_frame(Pipeline *p,AVFrame *frame);
/* pipeline.c */
int pipeline_process_frame(Pipeline *p,AVFrame *frame);
int pipeline_handle_audio(Pipeline *p,const InputItem *audio);
int pipeline_stamp_valid(const AVBufferRef *ref);
const NakiSeiBundle *pipeline_stamp_sei(const AVBufferRef *ref);
void pipeline_trace_pts(yyjson_mut_doc *doc, const char *key, int64_t pts);
NakiFrameIdentity pipeline_frame_identity(Pipeline *p,const Stamp *stamp,int64_t output_pts,AVRational output_tb);
int pipeline_interrupted(void *opaque);
int pipeline_flush_deferred_audio(Pipeline *p);

/* pipeline_encode.c */
int pipeline_validate_output_timestamps(Pipeline *p,const AVPacket *packet,AVRational time_base);
int64_t pipeline_source_rate(Pipeline *p,int64_t now);

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
