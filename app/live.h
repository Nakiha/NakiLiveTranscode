#ifndef NAKI_LIVE_H
#define NAKI_LIVE_H
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <yyjson.h>
#include <libavcodec/async.h>
#include "logger.h"
#include "netstats.h"
#include "frame_policy.h"
#include "watermark_config.h"
typedef struct NakiWatermark NakiWatermark;
#define NAKI_OUTPUT_MAX 4
typedef struct OutputConfig {
    char id[33],output[2048],output_format[16],encoder[16],encoder_preset[16],encoder_profile[24];
    int width,height,bitrate,encoder_threads,audio_copy,mux_wait_ms;
    int queue_items,queue_media_ms,max_lag_ms,io_timeout_ms;
    uint64_t queue_bytes,inflight_bytes;
    NakiWatermarkConfig watermark;
} OutputConfig;

typedef struct Config {
    char input[2048], output[2048], output_format[16];
    char encoder[16], encoder_preset[16];
    char encoder_profile[24];
    int width, height, bitrate, max_threads, min_threads, budget_slots;
    int queue_capacity, encoder_threads, fps, pace;
    int frame_log, frame_log_every, slow_frame_ms;
    int audio_copy, mux_wait_ms;
    NakiPolicyConfig policy;
    NakiWatermarkConfig watermark;
    int output_count;
    OutputConfig outputs[NAKI_OUTPUT_MAX];
} Config;
typedef struct Metric {
    uint64_t count;
    double sum_ms, max_ms;
    uint64_t bins[256], slow_count;
} Metric;
typedef struct Latencies {
    Metric demux_read, input_queue, decode, branch_queue, setup, filter, encode, encode_submit, encode_wait, mux;
    Metric ingress_to_mux, scheduled_to_mux, interleave_call, avio_flush;
} Latencies;
typedef struct FrameGroups {
    Metric i_frame, first_i, recurring_i, p_frame, b_frame, decoded_key, output_key;
} FrameGroups;
typedef struct Job {
    Config config;
    struct Job *parent,*outputs[NAKI_OUTPUT_MAX];
    char output_id[33];
    char id[64], request_id[129], log_path[4096], error[256], state[24];
    pthread_t thread;
    pthread_mutex_t mutex;
    atomic_int cancel, done;
    int wake_fd;
    Logger *logger;
    NetworkStats *network;
    TraceSink *trace;
    NakiWatermark *watermark;
    void (*watermark_snapshot)(NakiWatermark *,yyjson_mut_doc *);
    int64_t started_us, ended_us;
    int64_t assigned_us, worker_received_us, worker_spawned_us, worker_ready_us;
    int64_t first_video_output_us, first_key_output_us;
    int64_t input_open_started_us, input_open_done_us, input_probe_done_us, decoder_ready_us;
    int64_t first_packet_us, first_decoded_us, encoder_open_started_us, encoder_open_done_us;
    int64_t output_header_done_us, first_encoder_submit_us;
    int prewarmed;
    yyjson_mut_doc *first_chain[2]; /* Bounded checkpoints for replay after controller loss. */
    uint64_t packets_in, frames_decoded, frames_filtered, packets_out, bytes_out;
    uint64_t audio_tracks, audio_packets_out, audio_dropped, audio_unsupported;
    uint64_t mux_queued_packets, mux_queued_bytes, mux_high_water, mux_timeouts, mux_pressure, mux_forced;
    Latencies lifetime, window;
    Latencies i_lifetime, i_window;
    Latencies key_lifetime, key_window;
    FrameGroups groups, window_groups;
    int64_t window_start_us, last_output_us, write_started_us, last_i_output_us;
    int64_t auto_updated_us;
    char write_phase[24];
    uint64_t window_start_input, window_start_output, i_frames;
    AVCodecAsyncAutoStats auto_stats;
    NakiInFlight policy_inflight;
    NakiOverloadState overload;
    int64_t oldest_ingress_age_us;
    int policy_closing;
    int64_t overload_fail_write_us;
    char overload_fail_write_phase[24];
    uint64_t frame_id_last, frames_selected, frames_dropped_overload, frames_protected;
    uint64_t intra_requested, intra_confirmed, timestamp_wraps;
    uint64_t branch_queued,branch_queue_bytes,branch_queue_media_us,branch_queue_high;
    uint64_t branch_inflight_bytes,branch_inflight_frames,branch_inflight_high,filter_generations;
    NakiInFlight branch_pending;
    atomic_int branch_failed;
    int fixed_output_width,fixed_output_height;
} Job;

int64_t now_us(void);
void wake_job(void *opaque);
void request_stop(Job *job);
yyjson_mut_doc *new_event(const char *event, const Job *job);
void json_num(yyjson_mut_doc *doc, const char *name, double value);
void json_int(yyjson_mut_doc *doc, const char *name, int64_t value);
void json_str(yyjson_mut_doc *doc, const char *name, const char *value);
void metric_add(Metric *metric, double ms, double slow_ms);
yyjson_mut_doc *job_snapshot(Job *job);
yyjson_mut_doc *job_report(Job *job, int final);
void *pipeline_main(void *opaque);
int config_parse(const char *body, size_t size, Config *out, char *error, size_t cap);
int naki_outputs_parse(yyjson_val *root,Config *out,char *error,size_t cap);
void naki_output_config(Config *out,const Config *common,int index);
void naki_branch_release(Job *job,uint64_t id,uint64_t bytes);
int mkdir_tree(const char *path);
void naki_policy_tick(Job *job, int64_t current_us);
int naki_policy_track(Job *job, int kind, uint64_t id, int64_t ingress_us);
int naki_policy_untrack(Job *job, int kind, uint64_t id);
#endif
