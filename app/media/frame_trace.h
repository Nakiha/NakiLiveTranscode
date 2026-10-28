#ifndef NAKI_FRAME_TRACE_H
#define NAKI_FRAME_TRACE_H
/* Local frame lineage only; no Pipeline/Job mutable state. Wire codecs serialize
 * selected scalar fields, never this struct or its local delivery handles. */
#include <stdint.h>
#include "control/operation_store.h"
#include "codec_trace.h"
#include "live_rate.h"
typedef struct NakiWatermark NakiWatermark;
typedef struct Stamp {
    AVLiveDecodeTrace decode_trace;
    NakiEncoderTrace encode_trace;
    int64_t network_first_read_us,network_complete_us,demux_finished_us,network_tag_position;
    int encoder_future_frames,actual_slice_qp,decoder_tracked;
    NakiRateDecision rate;
    int64_t async_ready_us,decoder_owner_us,decoder_receive_us,submit_first_us;
    int64_t sequence, read_us, ingress_us, submitted_us, decoded_us, filtered_us;
    int64_t encoder_submit_us, encoder_accepted_us, scheduled_us, filter_started_us;
    int picture_type, decoded_key, input_key, first_i;
    int intra_reason, timestamp_source, tracked_packet;
    int64_t frame_id, packet_pts, packet_dts, raw_pts, source_pts, source_us;
    int64_t subscription_generation,source_rate_bps;
    uint64_t watermark_revision;
    NakiWatermark *delivery_owner;
    NakiOperationHandle delivery_lease;
    uint64_t branch_charge;
    unsigned generation;
    uint64_t timestamp_wraps;
    int64_t branch_enqueued_us,branch_started_us;
    uint32_t sei_size;
} Stamp;
#endif
