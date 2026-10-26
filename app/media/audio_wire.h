#ifndef NAKI_AUDIO_WIRE_H
#define NAKI_AUDIO_WIRE_H
#include "pipeline_reader.h"
#include <libavutil/buffer.h>
#define NAKI_AUDIO_WIRE_CAPACITY (8U*1024*1024+256U*1024+128U)
int naki_audio_wire_pack(const InputItem *item,uint8_t *data,size_t capacity,size_t *written);
int naki_audio_wire_unpack(InputItem *item,AVBufferRef *owner);
#endif
