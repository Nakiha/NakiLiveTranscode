#ifndef NAKI_CONFIG_H
#define NAKI_CONFIG_H
#include <stddef.h>
#include <stdint.h>
#include <yyjson.h>
#include "frame_policy.h"
#include "watermark_config.h"
#include "sei_config.h"
#define NAKI_OUTPUT_MAX 4
typedef struct OutputConfig {
    char id[33],output[2048],output_format[16],encoder[16],encoder_preset[16],encoder_profile[24];
    int width,height,bitrate,encoder_threads,audio_copy,mux_wait_ms;
    int output_fps,cqf,qp,maxrate,encoder_bit_depth,encoder_input_bit_depth,encoder_native8_reference;
    int queue_items,audio_packets,queue_media_ms,max_lag_ms,io_timeout_ms;
    uint64_t queue_bytes,inflight_bytes;
    NakiWatermarkConfig watermark;
    NakiSeiConfig sei;
} OutputConfig;

typedef struct Config {
    char input[2048], output[2048], output_format[16];
    char encoder[16], encoder_preset[16];
    char encoder_profile[24];
    int width, height, bitrate, max_threads, min_threads, budget_slots;
    int queue_capacity, encoder_threads, fps, pace;
    int frame_log, frame_log_every, slow_frame_ms;
    int network_trace,encoder_max_future_frames,rtmp_tcp_nodelay;
    int reader_queue_capacity,constrained_crf,source_rate_limit,decoder_pump,codec_trace;
    int audio_copy, mux_wait_ms;
    int output_fps,cqf,qp,maxrate,encoder_bit_depth,encoder_input_bit_depth,encoder_native8_reference;
    char single_output_id[33]; /* Logical identity; output_count still selects direct vs branch execution. */
    NakiPolicyConfig policy;
    NakiWatermarkConfig watermark;
    NakiSeiConfig sei;
    int output_count;
    OutputConfig outputs[NAKI_OUTPUT_MAX];
} Config;
int config_parse(const char *body, size_t size, Config *out, char *error, size_t cap);
int naki_config_fingerprint(const Config *config,char out[65],char *error,size_t cap);
int naki_logical_output_count(const Config *config);
void naki_logical_output(const Config *config,int index,OutputConfig *out);
int naki_outputs_parse(yyjson_val *root,Config *out,char *error,size_t cap);
void naki_output_config(Config *out,const Config *common,int index);

#endif
