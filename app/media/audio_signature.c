#include "audio_signature.h"
#include <errno.h>
#include <string.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <libavutil/sha.h>

static const int rates[]={96000,88200,64000,48000,44100,32000,24000,22050,16000,12000,11025,8000,7350};
typedef struct Bits {const unsigned char *data;size_t size,position;} Bits;
static int bits(Bits *b,unsigned count) {
    if(count>24||b->position>b->size||count>b->size-b->position)return -1;
    unsigned result=0;for(unsigned i=0;i<count;i++,b->position++)result=(result<<1)|((b->data[b->position/8]>>(7-b->position%8))&1);
    return (int)result;
}
static int object_type(Bits *b) {
    int value=bits(b,5);if(value==31) {int extension=bits(b,6);return extension<0?-1:32+extension;}return value;
}
static int frequency(Bits *b) {
    int index=bits(b,4);return index==15?bits(b,24):index>=0&&index<13?rates[index]:-1;
}
static int asc(const unsigned char *data,size_t bytes,NakiAudioSignature *s) {
    if(bytes<2||bytes>65536)return AVERROR_INVALIDDATA;
    Bits b={data,bytes*8,0};int aot=object_type(&b),rate=frequency(&b),channels=bits(&b,4);
    if(aot==5||aot==29) {
        /* Explicit HE-AAC/PS carries core frequency first, output frequency
         * second. Preserve all extension bits in the configuration digest. */
        if(frequency(&b)<=0)return AVERROR_INVALIDDATA;
        aot=object_type(&b);
    }
    if(aot<1||aot>4||rate<=0||channels<1||channels>7)return AVERROR(ENOSYS);
    s->object_type=aot;s->core_rate=rate;s->channel_config=channels;return 0;
}
static int digest(NakiAudioSignature *s,const unsigned char *data,size_t bytes) {
    if(!data||bytes>65536)return AVERROR_INVALIDDATA;
    struct AVSHA *sha=av_sha_alloc();if(!sha)return AVERROR(ENOMEM);
    int ret=av_sha_init(sha,256);if(ret>=0) {av_sha_update(sha,data,bytes);av_sha_final(sha,s->config_hash);s->config_bytes=(int)bytes;}
    av_free(sha);return ret;
}
void naki_audio_signature_clear(NakiAudioSignature *s) {av_channel_layout_uninit(&s->layout);memset(s,0,sizeof(*s));}
int naki_audio_signature_read(NakiAudioSignature *s,const AVCodecParameters *p,const AVPacket *packet) {
    memset(s,0,sizeof(*s));s->codec=p->codec_id;
    if(p->extradata_size<0||p->extradata_size>65536||p->ch_layout.nb_channels>64||packet->size<0)return AVERROR_INVALIDDATA;
    size_t config_bytes=0;const unsigned char *config=av_packet_get_side_data(packet,AV_PKT_DATA_NEW_EXTRADATA,&config_bytes);
    if(config_bytes>65536)return AVERROR_INVALIDDATA;
    if(!config) {config=p->extradata;config_bytes=(size_t)p->extradata_size;}
    if((config_bytes&&!config)||(packet->size&&!packet->data))return AVERROR_INVALIDDATA;
    int channels=0,ret=0;unsigned char generated[2];
    if(s->codec==AV_CODEC_ID_AAC) {
        if(packet->size>=7&&packet->data[0]==0xff&&(packet->data[1]&0xf6)==0xf0) {
            s->framing=1;int index=(packet->data[2]>>2)&15;
            s->object_type=(packet->data[2]>>6)+1;s->channel_config=((packet->data[2]&1)<<2)|(packet->data[3]>>6);
            if(index>=13||!s->channel_config)return AVERROR(ENOSYS);
            int frame_bytes=((packet->data[3]&3)<<11)|(packet->data[4]<<3)|(packet->data[5]>>5);
            if(frame_bytes<(packet->data[1]&1?7:9)||frame_bytes>packet->size)return AVERROR_INVALIDDATA;
            s->core_rate=rates[index];
            if(!config_bytes) {
                generated[0]=(unsigned char)((s->object_type<<3)|(index>>1));generated[1]=(unsigned char)(((index&1)<<7)|(s->channel_config<<3));
                config=generated;config_bytes=sizeof(generated);
            } else {
                NakiAudioSignature declared={0};ret=asc(config,config_bytes,&declared);if(ret<0)return ret;
                if(declared.object_type!=s->object_type||declared.core_rate!=s->core_rate||declared.channel_config!=s->channel_config)return AVERROR_INVALIDDATA;
            }
        } else {
            s->framing=2;if(!config_bytes)return AVERROR(EAGAIN);
            ret=asc(config,config_bytes,s);if(ret<0)return ret;
        }
        channels=s->channel_config==7?8:s->channel_config;
        ret=digest(s,config,config_bytes);if(ret<0)return ret;
    } else if(s->codec==AV_CODEC_ID_MP3) {
        if(packet->size<4||packet->data[0]!=0xff||(packet->data[1]&0xe0)!=0xe0)return AVERROR_INVALIDDATA;
        s->framing=3;s->mpeg_version=(packet->data[1]>>3)&3;int layer=(packet->data[1]>>1)&3,index=(packet->data[2]>>2)&3;
        if(s->mpeg_version==1||layer!=1||index==3)return AVERROR(ENOSYS);
        static const int mpeg_rates[]={44100,48000,32000};s->core_rate=mpeg_rates[index]>>(s->mpeg_version==3?0:s->mpeg_version==2?1:2);
        channels=(packet->data[3]>>6)==3?1:2;
    } else return AVERROR(ENOSYS);
    /* These supported headers declare fixed coded layouts (PCE is rejected).
     * Use AAC's mapping, not default(N): config3 is surround, not 2.1.
     * SBR/PS decoder metadata may describe a different rendered rate/layout. */
    static const AVChannelLayout aac_layouts[]={AV_CHANNEL_LAYOUT_MONO,AV_CHANNEL_LAYOUT_STEREO,AV_CHANNEL_LAYOUT_SURROUND,
        AV_CHANNEL_LAYOUT_4POINT0,AV_CHANNEL_LAYOUT_5POINT0_BACK,AV_CHANNEL_LAYOUT_5POINT1_BACK,AV_CHANNEL_LAYOUT_7POINT1_WIDE_BACK};
    if(s->codec==AV_CODEC_ID_AAC)ret=av_channel_layout_copy(&s->layout,&aac_layouts[s->channel_config-1]);
    else av_channel_layout_default(&s->layout,channels);
    if(ret<0)naki_audio_signature_clear(s);
    return ret;
}
const char *naki_audio_signature_change(const NakiAudioSignature *a,const NakiAudioSignature *b) {
    if(a->codec!=b->codec)return "codec_changed";
    if(a->framing!=b->framing)return "packet_framing_changed";
    if(a->object_type!=b->object_type)return "audio_object_type_changed";
    if(a->core_rate!=b->core_rate)return "core_sample_rate_changed";
    if(a->channel_config!=b->channel_config||av_channel_layout_compare(&a->layout,&b->layout))return "channel_layout_changed";
    if(a->mpeg_version!=b->mpeg_version)return "mpeg_audio_version_changed";
    if(a->config_bytes!=b->config_bytes||memcmp(a->config_hash,b->config_hash,sizeof(a->config_hash)))return "audio_specific_config_changed";
    return NULL;
}
