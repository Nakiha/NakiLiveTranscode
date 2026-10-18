#ifndef NAKI_PIPELINE_READER_H
#define NAKI_PIPELINE_READER_H
#include "control/frame_policy.h"
#include "sei.h"
#include <pthread.h>
#include <libavformat/avformat.h>
#define READER_CAPACITY 8
typedef struct InputItem { AVPacket *packet; AVCodecParameters *params; AVRational tb; int64_t demuxed_us; uint64_t retained_bytes; } InputItem;
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
    int64_t sei_pending[NAKI_INFLIGHT_CAPACITY];
} PipelineReader;
struct Pipeline;
void pipeline_input_free(InputItem *item);
int pipeline_reader_sei_obligation(struct Pipeline *p,int64_t sequence,int add);
InputItem pipeline_reader_pop(struct Pipeline *p, int *done, int *error);
void pipeline_reader_init(struct Pipeline *p);
int pipeline_reader_start(struct Pipeline *p);
void pipeline_reader_wake(struct Pipeline *p);
void pipeline_reader_join(struct Pipeline *p);
void pipeline_reader_clear(struct Pipeline *p);
void pipeline_reader_destroy(struct Pipeline *p);
int pipeline_reader_pending_sei(struct Pipeline *p);
#endif
