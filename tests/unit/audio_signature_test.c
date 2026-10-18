#include "media/audio_signature.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <libavutil/mem.h>
static AVPacket *adts(int aot,int frequency,int channels) {
    AVPacket *p=av_packet_alloc();assert(p&&!av_new_packet(p,8));
    unsigned char header[]={0xff,0xf1,0,0,1,0x1f,0xfc,0};memcpy(p->data,header,sizeof(header));
    p->data[2]=(unsigned char)(((aot-1)<<6)|(frequency<<2)|(channels>>2));p->data[3]=(unsigned char)((channels&3)<<6);return p;
}
static void extra(AVCodecParameters *p,const unsigned char *bytes,int count) {
    av_freep(&p->extradata);p->extradata=av_mallocz((size_t)count+AV_INPUT_BUFFER_PADDING_SIZE);assert(p->extradata);
    memcpy(p->extradata,bytes,(size_t)count);p->extradata_size=count;
}
static void same(const NakiAudioSignature *a,const NakiAudioSignature *b) {assert(!naki_audio_signature_change(a,b));}
static void different(const NakiAudioSignature *a,const NakiAudioSignature *b,const char *reason) {assert(!strcmp(naki_audio_signature_change(a,b),reason));}
static void write_bits(unsigned char *bytes,int *position,unsigned value,int count) {
    for(int i=count-1;i>=0;i--,(*position)++)bytes[*position/8]|=(unsigned char)(((value>>i)&1)<<(7-*position%8));
}
static void he_extra(AVCodecParameters *p,int aot,int channels) {
    unsigned char bytes[4]={0};int position=0;
    write_bits(bytes,&position,(unsigned)aot,5);write_bits(bytes,&position,6,4);write_bits(bytes,&position,(unsigned)channels,4);
    write_bits(bytes,&position,3,4);write_bits(bytes,&position,2,5);write_bits(bytes,&position,0,3);extra(p,bytes,sizeof(bytes));
}
int main(void) {
    AVCodecParameters *p=avcodec_parameters_alloc();assert(p);p->codec_id=AV_CODEC_ID_AAC;
    AVPacket *packet=adts(2,3,2);NakiAudioSignature accepted={0},observed={0};
    assert(!naki_audio_signature_read(&accepted,p,packet));assert(accepted.core_rate==48000&&accepted.object_type==2&&accepted.layout.nb_channels==2);
    /* Initialization's synthetic ASC matches an equivalent input ASC; the
     * signature did not retain the mutable input buffers or output codecpar. */
    unsigned char lc[]={0x11,0x90};extra(p,lc,2);assert(!naki_audio_signature_read(&observed,p,packet));same(&accepted,&observed);naki_audio_signature_clear(&observed);
    av_freep(&p->extradata);p->extradata_size=0;av_packet_free(&packet);packet=adts(2,4,2);
    assert(!naki_audio_signature_read(&observed,p,packet));different(&accepted,&observed,"core_sample_rate_changed");naki_audio_signature_clear(&observed);
    av_packet_free(&packet);packet=adts(2,3,1);assert(!naki_audio_signature_read(&observed,p,packet));different(&accepted,&observed,"channel_layout_changed");naki_audio_signature_clear(&observed);
    av_packet_free(&packet);packet=adts(1,3,2);assert(!naki_audio_signature_read(&observed,p,packet));different(&accepted,&observed,"audio_object_type_changed");naki_audio_signature_clear(&observed);
    av_packet_free(&packet);packet=adts(2,3,3);assert(!naki_audio_signature_read(&observed,p,packet));
    AVChannelLayout surround=AV_CHANNEL_LAYOUT_SURROUND;assert(!av_channel_layout_compare(&observed.layout,&surround));naki_audio_signature_clear(&observed);
    naki_audio_signature_clear(&accepted);av_packet_free(&packet);packet=av_packet_alloc();assert(packet&&!av_new_packet(packet,1));packet->data[0]=0;
    /* Real HE payload is exercised separately. These bounded ASC parser tests
     * distinguish coded core from SBR/PS rendered decoder metadata. */
    for(int ps=0;ps<2;ps++) {
        he_extra(p,ps?29:5,ps?1:2);p->sample_rate=48000;av_channel_layout_uninit(&p->ch_layout);av_channel_layout_default(&p->ch_layout,2);
        assert(!naki_audio_signature_read(&accepted,p,packet));assert(accepted.core_rate==24000&&accepted.object_type==2&&accepted.layout.nb_channels==(ps?1:2));
        p->sample_rate=24000;p->profile=-99;av_channel_layout_uninit(&p->ch_layout);av_channel_layout_default(&p->ch_layout,1);
        assert(!naki_audio_signature_read(&observed,p,packet));same(&accepted,&observed);naki_audio_signature_clear(&observed);naki_audio_signature_clear(&accepted);
    }
    extra(p,lc,2);assert(!naki_audio_signature_read(&accepted,p,packet));
    unsigned char *side=av_packet_new_side_data(packet,AV_PKT_DATA_NEW_EXTRADATA,2);assert(side);side[0]=0x12;side[1]=0x10;
    assert(!naki_audio_signature_read(&observed,p,packet));different(&accepted,&observed,"core_sample_rate_changed");naki_audio_signature_clear(&observed);
    side[0]=lc[0];side[1]=lc[1];assert(!naki_audio_signature_read(&observed,p,packet));same(&accepted,&observed);naki_audio_signature_clear(&observed);
    /* Same core but changed extension/configuration bytes must still reject. */
    av_packet_free_side_data(packet);unsigned char extended[]={0x11,0x90,0};extra(p,extended,3);
    assert(!naki_audio_signature_read(&observed,p,packet));different(&accepted,&observed,"audio_specific_config_changed");naki_audio_signature_clear(&observed);
    p->extradata_size=65537;assert(naki_audio_signature_read(&observed,p,packet)<0);p->extradata_size=1;assert(naki_audio_signature_read(&observed,p,packet)<0);
    naki_audio_signature_clear(&accepted);avcodec_parameters_free(&p);av_packet_free(&packet);
    p=avcodec_parameters_alloc();assert(p);p->codec_id=AV_CODEC_ID_MP3;packet=av_packet_alloc();assert(packet&&!av_new_packet(packet,4));
    unsigned char mp3[]={0xff,0xfb,0x94,0};memcpy(packet->data,mp3,4);assert(!naki_audio_signature_read(&accepted,p,packet));assert(accepted.core_rate==48000);
    packet->data[2]=0xb6;packet->data[3]=0x40;assert(!naki_audio_signature_read(&observed,p,packet));same(&accepted,&observed);naki_audio_signature_clear(&observed);
    packet->data[2]=0x90;assert(!naki_audio_signature_read(&observed,p,packet));different(&accepted,&observed,"core_sample_rate_changed");naki_audio_signature_clear(&observed);
    packet->data[2]=0x94;packet->data[3]=0xc0;assert(!naki_audio_signature_read(&observed,p,packet));different(&accepted,&observed,"channel_layout_changed");naki_audio_signature_clear(&observed);
    packet->data[1]=0xe9;assert(naki_audio_signature_read(&observed,p,packet)<0);
    naki_audio_signature_clear(&accepted);avcodec_parameters_free(&p);av_packet_free(&packet);
    puts("PASS immutable coded AAC/HE/PS/ADTS/ASC and MP3 input signatures, layout/rate/profile/config changes and bounded malformed input");return 0;
}
