#ifndef NAKI_ENCODED_DESCRIPTION_H
#define NAKI_ENCODED_DESCRIPTION_H
#include <libavcodec/avcodec.h>
/* Sealed before any writer starts. No borrowed codec context or stream: each
 * owner holds a reference to a deep parameter copy until after its join. */
typedef struct NakiEncodedDescription NakiEncodedDescription;
typedef struct NakiEncodedInfo {
    AVRational time_base;
    const uint8_t *extradata;
    int extradata_size;
} NakiEncodedInfo;
NakiEncodedDescription *naki_encoded_seal(const AVCodecContext *encoder);
NakiEncodedDescription *naki_encoded_ref(NakiEncodedDescription *description);
void naki_encoded_unref(NakiEncodedDescription **description);
NakiEncodedInfo naki_encoded_info(const NakiEncodedDescription *description);
int naki_encoded_parameters(const NakiEncodedDescription *description,AVCodecParameters *parameters);
#endif
