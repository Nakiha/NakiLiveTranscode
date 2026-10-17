#ifndef NAKI_AUDIO_SIGNATURE_H
#define NAKI_AUDIO_SIGNATURE_H
#include <libavcodec/codec_par.h>
#include <libavcodec/packet.h>
/* Immutable accepted INPUT configuration; never derived from mux codecpar.
 * AAC rates/channels below describe the coded core, not SBR/PS rendering. */
typedef struct NakiAudioSignature {
    enum AVCodecID codec;
    int framing,object_type,core_rate,channel_config,mpeg_version;
    AVChannelLayout layout;
    unsigned char config_hash[32];
    int config_bytes;
} NakiAudioSignature;
int naki_audio_signature_read(NakiAudioSignature *signature,const AVCodecParameters *parameters,const AVPacket *packet);
const char *naki_audio_signature_change(const NakiAudioSignature *accepted,const NakiAudioSignature *observed);
void naki_audio_signature_clear(NakiAudioSignature *signature);
#endif
