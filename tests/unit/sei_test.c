#include "media/sei.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
static AVPacket *packet(const uint8_t *data,int size) {
    AVPacket *p=av_packet_alloc();assert(p&&!av_new_packet(p,size));memcpy(p->data,data,size);
    p->pts=12345;p->dts=12000;p->duration=3600;p->flags=AV_PKT_FLAG_KEY;p->stream_index=2;return p;
}
static void config(void) {
    char e[200];NakiSeiConfig c;
    const char *good="{\"types\":[101,\"uuid\",100]}";
    yyjson_doc *d=yyjson_read(good,strlen(good),0);assert(d&&!naki_sei_config(yyjson_doc_get_root(d),&c,e,sizeof(e)));
    assert(c.count==3&&c.types[0]==5&&c.types[1]==100&&c.types[2]==101);yyjson_doc_free(d);
    const char *bad[]={"null","[]","{\"types\":[true]}","{\"types\":[-1]}","{\"types\":[65536]}","{\"types\":[\"UUID\"]}","{\"types\":[5,\"uuid\"]}","{\"types\":[],\"x\":1}"};
    for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++) {
        d=yyjson_read(bad[i],strlen(bad[i]),0);assert(d&&naki_sei_config(yyjson_doc_get_root(d),&c,e,sizeof(e))<0);yyjson_doc_free(d);
    }
}
int main(void) {
    config();
    NakiSeiConfig c={.count=3,.types={5,100,101}};
    /* Independent encoded vector: malformed UUID (3 raw bytes), escaped 000001,
     * an opaque type 100 and an empty type 101, all preceding the same picture. */
    const uint8_t source[]={0,0,0,1,6,5,3,0,0,3,1,100,1,0xaa,101,0,128,0,0,1,0x65,128};
    AVPacket *p=packet(source,sizeof(source));NakiSeiBundle *b=NULL;
    assert(!naki_sei_extract(p,AV_CODEC_ID_H264,0,&c,&b)&&b&&b->count==3&&b->bytes==4);
    assert(b->messages[0].type==5&&b->messages[0].size==3&&!memcmp(b->data,"\0\0\1",3));
    assert(b->messages[1].type==100&&b->data[3]==0xaa&&b->messages[2].size==0);
    assert(p->size==5&&p->pts==12345&&p->dts==12000&&p->duration==3600&&p->flags==AV_PKT_FLAG_KEY&&p->stream_index==2);
    assert(!naki_sei_inject(p,AV_CODEC_ID_H264,b,&c));
    NakiSeiBundle *again=NULL;assert(!naki_sei_extract(p,AV_CODEC_ID_H264,0,&c,&again));
    assert(naki_sei_bundle_size(b)==naki_sei_bundle_size(again)&&!memcmp(b,again,naki_sei_bundle_size(b)));av_free(again);av_packet_free(&p);
    /* Independent expected VVC prefix header and message serialization. */
    const uint8_t vvc[]={0,0,0,1,0,0x39,128};p=packet(vvc,sizeof(vvc));
    assert(!naki_sei_inject(p,AV_CODEC_ID_VVC,b,&c));
    const uint8_t expected[]={0,0,0,1,0,0xb9,5,3,0,0,3,1,100,1,0xaa,101,0,128};
    assert(p->size==(int)(sizeof(expected)+sizeof(vvc))&&!memcmp(p->data,expected,sizeof(expected)));av_packet_free(&p);
    const uint8_t temporal_vvc[]={0,0,0,1,0,0x3b,128};p=packet(temporal_vvc,sizeof(temporal_vvc));
    assert(!naki_sei_inject(p,AV_CODEC_ID_VVC,b,&c)&&p->data[5]==0xbb);av_packet_free(&p);
    /* HEVC suffix remains after VCL; H.264 maps suffix into its sole SEI NAL. */
    const uint8_t hevc[]={0,0,0,1,2,1,128};p=packet(hevc,sizeof(hevc));
    for(unsigned i=0;i<b->count;i++)b->messages[i].suffix=1;
    assert(!naki_sei_inject(p,AV_CODEC_ID_HEVC,b,&c)&&p->data[sizeof(hevc)+4]==80);
    assert(!naki_sei_extract(p,AV_CODEC_ID_HEVC,0,&c,&again)&&again->count==3&&again->messages[0].suffix);av_free(again);av_packet_free(&p);
    /* Length-prefixed input, partial selection, repeated messages and long type. */
    const uint8_t lp[]={0,0,0,11,6,5,1,42,100,1,99,255,45,0,128,0,0,0,2,0x65,128};
    NakiSeiConfig subset={.count=2,.types={100,300}};p=packet(lp,sizeof(lp));
    assert(!naki_sei_extract(p,AV_CODEC_ID_H264,4,&subset,&again)&&again->count==2&&again->messages[1].type==300);
    assert(p->size==15&&p->data[3]==5&&p->data[5]==5&&p->data[7]==42);av_free(again);av_packet_free(&p);
    const uint8_t duplicate[]={0,0,1,6,100,1,7,100,1,7,128,0,0,1,0x65,128};
    p=packet(duplicate,sizeof(duplicate));assert(!naki_sei_extract(p,AV_CODEC_ID_H264,0,&c,&again)&&again->count==2);av_free(again);av_packet_free(&p);
    const uint8_t invalid[][16]={
        {0,0,1,6,5,8,1,128,0,0,1,0x65,128}, /* size exceeds RBSP */
        {0,0,1,6,5,1,1,0,0,0,1,0x65,128}, /* missing trailing bits */
        {0,0,1,0x65,128,0,0,1,0x65,128}, /* two pictures */
        {0,0,1,6,5,1,7,128}, /* SEI without a picture */
        {0,0,1,0x65,128,0,0,1,6,5,1,7,128} /* next-picture prefix */
    };
    const int sizes[]={13,13,10,8,13};
    for(unsigned i=0;i<sizeof(sizes)/sizeof(*sizes);i++) {
        p=packet(invalid[i],sizes[i]);assert(naki_sei_extract(p,AV_CODEC_ID_H264,0,&c,&again)<0&&!again);av_packet_free(&p);
    }
    uint8_t excessive[512];int pos=0;excessive[pos++]=0;excessive[pos++]=0;excessive[pos++]=1;excessive[pos++]=6;
    for(unsigned i=0;i<=NAKI_SEI_MESSAGES;i++) {excessive[pos++]=100;excessive[pos++]=1;excessive[pos++]=9;}
    excessive[pos++]=128;excessive[pos++]=0;excessive[pos++]=0;excessive[pos++]=1;excessive[pos++]=0x65;excessive[pos++]=128;
    p=packet(excessive,pos);assert(naki_sei_extract(p,AV_CODEC_ID_H264,0,&c,&again)==AVERROR(ENOBUFS));av_packet_free(&p);
    for(unsigned length=65536;length<=65537;length++) {
        uint8_t *large=av_malloc(66000);assert(large);pos=0;
        large[pos++]=0;large[pos++]=0;large[pos++]=1;large[pos++]=6;large[pos++]=100;
        unsigned remaining=length;while(remaining>=255) {large[pos++]=255;remaining-=255;}large[pos++]=(uint8_t)remaining;
        memset(large+pos,0x55,length);pos+=(int)length;large[pos++]=128;
        large[pos++]=0;large[pos++]=0;large[pos++]=1;large[pos++]=0x65;large[pos++]=128;
        p=packet(large,pos);av_free(large);
        int ret=naki_sei_extract(p,AV_CODEC_ID_H264,0,&c,&again);
        if(length==65536) {assert(!ret&&again->bytes==65536);av_free(again);}
        else assert(ret==AVERROR(ENOBUFS)&&!again);
        av_packet_free(&p);
    }
    av_free(b);puts("SEI: opaque short UUID, byte-exact codec framing, type/size escapes, subsets, order, suffix, malformed/AU/budget rejection passed");return 0;
}
