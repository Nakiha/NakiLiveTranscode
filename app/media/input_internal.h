#ifndef NAKI_INPUT_INTERNAL_H
#define NAKI_INPUT_INTERNAL_H
#define READER_CAPACITY 8
#include "pipeline_internal.h"
#include "platform/wakeup.h"
#include <pthread.h>
#include <libavformat/avformat.h>
#include <libavformat/read_origin.h>
#include "input_probe.h"
#if !defined(AVFORMAT_LIVE_INPUT_POLL_VERSION) || AVFORMAT_LIVE_INPUT_POLL_VERSION != 1
#error "The matching FFmpeg resumable live input poll API 1 is required"
#endif

/* One reader producer, one shared-pipeline consumer. All queued packet and
 * parameter references transfer together. Join before clearing/destroying. */
typedef struct PipelineReader {
    InputItem ring[READER_CAPACITY];
    int head, count, read_done, read_error;
    struct {int64_t us,bits;} rate[1024];
    int rate_head,rate_count;
    int64_t rate_bits;
    uint64_t reader_bytes;
    pthread_mutex_t ring_mutex;
    pthread_cond_t ring_space;
    pthread_t thread;
    int started;
    int audio_subscribed;
    int64_t subscription_generation;
    struct {int64_t sequence,generation;} epochs[NAKI_INFLIGHT_CAPACITY];
    int64_t sei_pending[NAKI_INFLIGHT_CAPACITY];
} PipelineReader;
typedef struct DecodePump {
    pthread_mutex_t mutex;NakiWakeup wake;pthread_t thread;
    AVFrame *frame;InputItem audio;
    int occupied,prefer_video,done,error,started;
} DecodePump;
int pipeline_decode_init(Pipeline *p);
void pipeline_decode_join(Pipeline *p);
void pipeline_decode_destroy(Pipeline *p);
void pipeline_refresh_auto(Pipeline *p);
void pipeline_reader_init(struct Pipeline *p);
void pipeline_reader_join(struct Pipeline *p);
void pipeline_reader_clear(struct Pipeline *p);
void pipeline_reader_destroy(struct Pipeline *p);
int pipeline_reader_sei_obligation(struct Pipeline *p,int64_t sequence,int add);
void pipeline_decode_notify(void *opaque);
int pipeline_decode_receive(Pipeline *p,AVFrame *frame,InputItem *audio);
struct NakiInput {
    AVFormatContext *input;
    AVCodecContext *decoder;
    AVCodecAsyncContext *async;
    AVFrame initial_input_properties;
    int64_t decoder_receive_us,next_frame_id,synthetic_pts,next_stats_us;
    int stream_index,seen_input_i;
    enum AVCodecID sei_codec;
    int sei_length_size,sei_read_error;
    NakiSeiConfig sei_input;
    NakiTimeline timeline;
    Interrupt read_interrupt;
    PipelineReader reader;
    DecodePump pump;
};
#endif
