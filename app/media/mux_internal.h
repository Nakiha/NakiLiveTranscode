#ifndef NAKI_MUX_INTERNAL_H
#define NAKI_MUX_INTERNAL_H
/* Only the mux implementation may include this header. The object has a stable
 * address from allocation through close, including an owner handoff. Protocol
 * interrupts and custom AVIO callbacks never point into a Pipeline. */
#include "pipeline_internal.h"
#include <libavcodec/bsf.h>

typedef struct AudioTrack {
    int input_index, rejected, have_clock, pending_reported;
    AVStream *output;
    AVBSFContext *bsf;
    NakiAudioSignature accepted;
    int64_t last_dts, next_input_dts, previous_input_dts;
} AudioTrack;

struct NakiMux {
    AVFormatContext *output;
    int seen_output_key;
    int64_t previous_key_pts;
    Interrupt interrupt;
    int header_written, custom_output_fd, live_mux, video_started;
    int64_t mux_max_dts_us;
    AudioTrack audio[16];
    int audio_count;
    InputItem deferred_audio[128];
    int deferred_count;
    uint64_t deferred_bytes;
};
#endif
