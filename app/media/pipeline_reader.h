#ifndef NAKI_PIPELINE_READER_H
#define NAKI_PIPELINE_READER_H
#include <libavcodec/packet.h>
#include <libavcodec/codec_par.h>
typedef struct InputItem { AVPacket *packet; AVCodecParameters *params; AVRational tb; int64_t demuxed_us; uint64_t retained_bytes; int hook_observed; } InputItem;
void pipeline_input_free(InputItem *item);
#endif
