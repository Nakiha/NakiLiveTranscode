#ifndef NAKI_SEI_H
#define NAKI_SEI_H
#include <stddef.h>
#include <stdint.h>
#include <yyjson.h>
#include <libavcodec/packet.h>
#include <libavcodec/codec_id.h>
#define NAKI_SEI_TYPES 64
#define NAKI_SEI_UNION_TYPES (4 * NAKI_SEI_TYPES)
#define NAKI_SEI_MESSAGES 64
#define NAKI_SEI_PAYLOAD_BYTES 65536U
typedef struct NakiSeiConfig { unsigned count; uint32_t types[NAKI_SEI_UNION_TYPES]; } NakiSeiConfig;
typedef struct NakiSeiMessage { uint32_t type, offset, size, suffix; } NakiSeiMessage;
/* Self-contained, safe to copy with opaque_ref COW; no embedded pointers. */
typedef struct NakiSeiBundle {
    uint32_t count, bytes;
    NakiSeiMessage messages[NAKI_SEI_MESSAGES];
    uint8_t data[];
} NakiSeiBundle;
int naki_sei_config(yyjson_val *value,NakiSeiConfig *out,char *error,size_t cap);
int naki_sei_union(NakiSeiConfig *out,const NakiSeiConfig *add);
int naki_sei_selected(const NakiSeiConfig *config,uint32_t type);
int naki_sei_length_size(enum AVCodecID codec,const uint8_t *extra,int size);
/* Complete, single-layer, single-picture demux AUs only. Selected input SEI
 * are removed before decoding to prevent decoder validation/auto-forwarding
 * of opaque customer data. The returned bundle must be av_free'd. */
int naki_sei_extract(AVPacket *packet,enum AVCodecID codec,int length_size,
                     const NakiSeiConfig *config,NakiSeiBundle **bundle);
size_t naki_sei_bundle_size(const NakiSeiBundle *bundle);
int naki_sei_count(const NakiSeiBundle *bundle,const NakiSeiConfig *config);
int naki_sei_inject(AVPacket *packet,enum AVCodecID codec,
                    const NakiSeiBundle *bundle,const NakiSeiConfig *config);
#endif
