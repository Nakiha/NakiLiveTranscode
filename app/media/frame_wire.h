#ifndef NAKI_FRAME_WIRE_H
#define NAKI_FRAME_WIRE_H
#include <stddef.h>
#include <stdint.h>
#include <libavutil/frame.h>
#define NAKI_FRAME_WIRE_VERSION 2
#define NAKI_FRAME_WIRE_META_MAX (256U*1024)
#define NAKI_FRAME_WIRE_DIM_MAX 8192
/* A sealed pool slot contains explicit versioned JSON metadata, alignment,
 * planar pixel bytes, and zero padding. Pixels are never in control JSON.
 * Stamp pointers/handles, AVFrame/AVBuffer internals and refcounts are omitted.
 * v2 is local to one matching application/FFmpeg build, not an external codec. */
size_t naki_frame_wire_capacity(uint64_t max_pixels);
int naki_frame_wire_pack(const AVFrame *frame,uint64_t max_pixels,uint8_t *data,size_t capacity,size_t *written);
/* owner retains the complete published slot. dst gets its own READONLY buffer
 * view, even if owner currently has multiple refs and later becomes unique.
 * The final owner callback returns the raw lease independently of mux/delivery.
 * dst must be empty. Partial construction is discarded on every failure. */
int naki_frame_wire_unpack(AVFrame *dst,AVBufferRef *owner,uint64_t max_pixels);
#endif
