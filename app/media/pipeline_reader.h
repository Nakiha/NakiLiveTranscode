#ifndef NAKI_PIPELINE_READER_H
#define NAKI_PIPELINE_READER_H
#include "control/frame_policy.h"
#include "sei.h"
#include <libavformat/avformat.h>
#define READER_CAPACITY 8
typedef struct InputItem { AVPacket *packet; AVCodecParameters *params; AVRational tb; int64_t demuxed_us; uint64_t retained_bytes; int hook_observed; } InputItem;
struct Pipeline;
void pipeline_input_free(InputItem *item);
InputItem pipeline_reader_pop(struct Pipeline *p, int *done, int *error);
int pipeline_reader_start(struct Pipeline *p);
void pipeline_reader_wake(struct Pipeline *p);
int pipeline_reader_pending_sei(struct Pipeline *p);
int pipeline_reader_subscribe(struct Pipeline *p,const NakiSeiConfig *sei,int audio,int64_t *generation);
int pipeline_reader_epoch_consumed(struct Pipeline *p,int64_t sequence);
int pipeline_reader_epoch_ready(struct Pipeline *p,int64_t generation);
#endif
