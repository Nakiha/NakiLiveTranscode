#ifndef NAKI_METRICS_H
#define NAKI_METRICS_H
#include <stdint.h>
typedef struct Metric {
    uint64_t count;
    double sum_ms, max_ms;
    uint64_t bins[256], slow_count;
} Metric;
typedef struct Latencies {
    Metric demux_read, input_queue, decode, branch_queue, setup, filter, encode, encode_submit, encode_wait, mux;
    Metric ingress_to_mux, scheduled_to_mux, interleave_call, avio_flush;
    Metric network_to_mux;
} Latencies;
typedef struct FrameGroups {
    Metric i_frame, first_i, recurring_i, p_frame, b_frame, decoded_key, output_key;
} FrameGroups;
void metric_add(Metric *metric, double ms, double slow_ms);

#endif
