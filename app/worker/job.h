#ifndef NAKI_JOB_H
#define NAKI_JOB_H
#include "control/config.h"
#include "observability/metrics.h"
#include "job_api.h"
#include "job_state.h"
#include "observability/logger.h"
#include "observability/netstats.h"
#include "media/video_rate.h"
#include <pthread.h>
#include <stdatomic.h>
#include <libavcodec/async.h>
#if !defined(AVCODEC_EARLY_OUTPUT_API_VERSION) || AVCODEC_EARLY_OUTPUT_API_VERSION != 1
#error "Matching native decoder early-output observation API 1 is required"
#endif
typedef struct NakiWatermark NakiWatermark;
typedef struct NakiOperations NakiOperations;
/* Worker-private mutable state. Controller code uses job_api.h only. */
/* Job.mutex protects all mutable groups except their explicit atomics. Owners
 * publish progress/statistics here; snapshots never take queue/codec locks.
 * Parent -> child snapshot locking and queue -> unlock -> Job publication keep
 * their existing order. Handles and shared-reference ownership stay on Job. */
typedef struct JobLifecycle {
    char error[256];
    NakiJobState state;
    atomic_int cancel,done;
    int64_t started_us, ended_us;
    int64_t stop_requested_us,stop_deadline_us;
    char stop_operation_id[129],stop_origin_request_id[129];
    int prewarmed;
    atomic_int branch_failed;
} JobLifecycle;

typedef struct JobProgress {
    int64_t assigned_us, worker_received_us, worker_spawned_us, worker_ready_us;
    int64_t first_video_output_us, first_key_output_us;
    int64_t input_open_started_us, input_open_done_us, input_probe_done_us, decoder_ready_us;
    int64_t first_packet_us, first_decoded_us, encoder_open_started_us, encoder_open_done_us;
    int64_t output_header_done_us, first_encoder_submit_us;
    yyjson_mut_doc *first_chain[2]; /* Bounded checkpoints for replay after controller loss. */
    NakiVideoRate video_rate;
    int64_t window_start_us, last_output_us, write_started_us, last_i_output_us;
    int64_t last_input_us,last_decoded_us,last_encoded_us,branch_oldest_ingress_us;
    NakiMediaPhase media_phase;
    uint64_t snapshot_generation;
    int64_t finalize_started_us,finalize_finished_us,finalize_deadline_us;
    int finalize_trailer_error,finalize_flush_error,finalize_close_error,finalize_primary_error;
    int64_t auto_updated_us;
    NakiWritePhase write_phase;
    AVCodecAsyncAutoStats auto_stats;
    AVCodecEarlyOutputStats early_stats;
    int early_stats_available;
    int fixed_output_width,fixed_output_height;
} JobProgress;

typedef struct JobStats {
    uint64_t packets_in, frames_decoded, frames_filtered, packets_out, bytes_out;
    uint64_t audio_tracks, audio_packets_out, audio_dropped, audio_unsupported;
    uint64_t mux_queued_packets, mux_queued_bytes, mux_high_water, mux_timeouts, mux_pressure, mux_forced;
    Latencies lifetime, window;
    Latencies i_lifetime, i_window;
    Latencies key_lifetime, key_window;
    FrameGroups groups, window_groups;
    FrameGroups network_groups,window_network_groups;
    uint64_t network_deadline_exceeded,network_window_exceeded,encoder_future_max;
    uint64_t window_start_input, window_start_output, i_frames;
    uint64_t frame_id_last, frames_selected, frames_dropped_overload, frames_protected;
    uint64_t frames_dropped_rate;
    uint64_t intra_requested, intra_confirmed, timestamp_wraps;
    uint64_t sei_extracted,sei_written,sei_protected;
    uint64_t branch_queued,branch_queue_bytes,branch_queue_media_us,branch_queue_high;
    unsigned branch_video_queued,branch_audio_queued,branch_video_high,branch_audio_high,branch_rejection_reason;
    uint64_t branch_rejected[6];
    uint64_t branch_inflight_bytes,branch_inflight_frames,branch_inflight_high,filter_generations;
} JobStats;

typedef struct JobFlow {
    NakiInFlight policy_inflight;
    NakiOverloadState overload;
    int64_t oldest_ingress_age_us;
    int policy_closing;
    int64_t overload_fail_write_us;
    NakiWritePhase overload_fail_write_phase;
    NakiInFlight branch_pending;
} JobFlow;

struct Job {
    Config config;
    struct Job *parent,*outputs[NAKI_OUTPUT_MAX];
    char output_id[33];
    char id[64], request_id[129], log_path[4096];
    pthread_t thread;
    pthread_mutex_t mutex;
    int wake_fd;
    Logger *logger;
    NetworkStats *network;
    TraceSink *trace;
    NakiWatermark *watermark;
    NakiOperations *operations; /* Root job owns the shared bounded control store. */
    void (*watermark_snapshot)(NakiWatermark *,yyjson_mut_doc *);
    JobLifecycle lifecycle;
    JobProgress progress;
    JobStats stats;
    JobFlow flow;
};


#endif
