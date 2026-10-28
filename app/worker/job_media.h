#ifndef NAKI_JOB_MEDIA_H
#define NAKI_JOB_MEDIA_H
/* Media-facing worker contract. Job and its synchronization remain opaque.
 * Returned configuration/identity is borrowed and immutable for the active
 * incarnation. Handles retain their original owner and lifetime. */
#include "job_api.h"
#include "control/config.h"
#include "control/operation_store.h"
#include "observability/logger.h"
#include "observability/metrics.h"
#include "job_state.h"
#include "media/frame_trace.h"
#include "media/video_rate.h"
#include <libavcodec/async.h>
#include <libavformat/live_mux.h>
typedef struct NakiWatermark NakiWatermark;
typedef struct NakiRenditions NakiRenditions;
const Config *naki_job_config(const Job *job);
const char *naki_job_id(const Job *job);
const char *naki_job_request_id(const Job *job);
const char *naki_job_output_id(const Job *job);
const char *naki_job_log_path(const Job *job);
const char *naki_job_public_config(const Job *job);
Job *naki_job_parent(const Job *job);
Job *naki_job_output(const Job *job,unsigned index);
Logger *naki_job_logger(const Job *job);
TraceSink *naki_job_trace(const Job *job);
NakiWatermark *naki_job_watermark(const Job *job);
NakiOperations *naki_job_operations(const Job *job);
NakiRenditions *naki_job_renditions(const Job *job);
int naki_job_wake_fd(const Job *job);
int naki_job_remote(const Job *job);
int64_t naki_job_started_us(const Job *job);
int naki_job_cancelled(const Job *job);
int naki_job_done(const Job *job);
int naki_job_failed(const Job *job);
void naki_job_cancel(Job *job);
int64_t naki_job_source_rate(const Job *job);
void naki_job_source_rate_set(Job *job,int64_t rate);

typedef enum NakiJobCounter {
    NAKI_COUNT_AUDIO_UNSUPPORTED, NAKI_COUNT_AUDIO_DROPPED, NAKI_COUNT_AUDIO_TRACKS,
    NAKI_COUNT_AUDIO_WRITTEN, NAKI_COUNT_SEI_EXTRACTED, NAKI_COUNT_SEI_WRITTEN,
    NAKI_COUNT_INTRA_REQUESTED, NAKI_COUNT_INTRA_CONFIRMED, NAKI_COUNT_FILTERED
} NakiJobCounter;
void naki_job_count(Job *job,NakiJobCounter counter,uint64_t amount);
typedef enum NakiJobCheckpoint {
    NAKI_CP_INPUT_OPEN_STARTED, NAKI_CP_INPUT_OPEN_DONE, NAKI_CP_INPUT_PROBE_DONE,
    NAKI_CP_DECODER_READY, NAKI_CP_FIRST_PACKET, NAKI_CP_FIRST_DECODED,
    NAKI_CP_ENCODER_OPEN_STARTED, NAKI_CP_ENCODER_OPEN_DONE,
    NAKI_CP_OUTPUT_HEADER_DONE, NAKI_CP_FIRST_ENCODER_SUBMIT
} NakiJobCheckpoint;
void naki_job_checkpoint(Job *job,NakiJobCheckpoint point,int64_t when_us);
void naki_job_media_phase(Job *job,NakiMediaPhase phase);
void naki_job_input_received(Job *job,int video,int running,int64_t when_us);
void naki_job_decoded(Job *job,const Stamp *stamp,int branch);
void naki_job_encoded(Job *job,int64_t when_us);
void naki_job_video_rate(Job *job,const NakiVideoRate *rate);
void naki_job_decoder_observation(Job *job,const AVCodecAsyncAutoStats *stats,const AVCodecEarlyOutputStats *early,int available,int64_t when_us);
uint64_t naki_job_filter_generation(Job *job,int initialize);
typedef struct NakiOutputSize {int width,height;} NakiOutputSize;
void naki_job_output_size_set(Job *job,int width,int height);
NakiOutputSize naki_job_output_size_freeze(Job *job,int input_width,int input_height);
void naki_job_rate_dropped(Job *job,const Stamp *stamp);
int naki_job_frame_deciding(Job *job,const Stamp *stamp,unsigned sei_messages);
void naki_job_frame_selected(Job *job,int protect,int sei_protected,int keep,int rate_drop);
void naki_job_shared_decoded(Job *job,const Stamp *stamp,int record_wraps);
void naki_job_write_begin(Job *job,int64_t when_us);
void naki_job_write_flushing(Job *job);
void naki_job_write_end(Job *job);
void naki_job_bytes_written(Job *job,uint64_t bytes);
uint64_t naki_job_bytes(const Job *job);
void naki_job_finalize_begin(Job *job,int64_t began,int64_t deadline);
void naki_job_finalize_result(Job *job,int trailer,int flush,int closed,int primary,int64_t when_us);
void naki_job_mux_observation(Job *job,const AVLiveMuxStats *stats);
typedef struct NakiJobStartup {
    int prewarmed;
    int64_t started_us,assigned_us,worker_received_us,worker_spawned_us,worker_ready_us;
    int64_t input_open_started_us,input_open_done_us,input_probe_done_us,decoder_ready_us;
} NakiJobStartup;
NakiJobStartup naki_job_startup(Job *job);
void naki_job_first_frame(Job *job,const yyjson_mut_doc *record,int first_video);
typedef struct NakiWrittenFrame {
    const Stamp *stamp;
    int64_t encoded_us,mux_begin_us,interleaved_us,written_us;
    uint64_t bytes;
    int output_key;
} NakiWrittenFrame;
typedef struct NakiWrittenResult {int first_output,first_key;uint64_t ordinal,submitted;} NakiWrittenResult;
NakiWrittenResult naki_job_frame_written(Job *job,const NakiWrittenFrame *frame);
int naki_job_branch_admit(Job *job,uint64_t id,int64_t ingress,uint64_t bytes,uint64_t limit);
typedef struct NakiBranchAges {int failed;int64_t retained_us,write_us;} NakiBranchAges;
NakiBranchAges naki_job_branch_ages(Job *job,int64_t when_us);
typedef struct NakiBranchQueueReport {
    uint64_t bytes,work_us,rejected[6];int64_t oldest_us;
    unsigned count,high,video,audio,video_high,audio_high,reason;
} NakiBranchQueueReport;
void naki_job_branch_queue(Job *job,const NakiBranchQueueReport *report);
void naki_job_inherit_input(Job *job,Job *parent,const NakiVideoRate *rate);
void naki_job_running(Job *job);
void naki_job_waiting_assets(Job *job);
void naki_job_branch_fail(Job *job,int error,const char *stage);
void naki_job_branch_unstarted(Job *job);
void naki_job_startup_fail(Job *job,int error);
void naki_job_media_error(Job *job,int error,const char *stage);
void naki_job_shared_terminated(Job *job,int stopped,const char *stage);
int naki_job_begin_shutdown(Job *job);
void naki_job_complete(Job *job,int error,int stopped,const char *stage);
unsigned naki_job_output_failures(Job *job);
NakiJobState naki_job_state(Job *job);
#endif
