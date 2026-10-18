#include "sei.h"
#include "control/resource_limits.h"
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>

#define OUTPUT_LIMIT (NAKI_INPUT_PACKET_BYTES + 262144U)
typedef struct Bytes { uint8_t *data; size_t size, capacity; } Bytes;
typedef struct Nals { const uint8_t *data; size_t size, pos; int length_size; } Nals;
typedef struct Nal { const uint8_t *data; size_t size, begin, end; } Nal;
static int append(Bytes *b,const void *data,size_t size) {
    if(size>OUTPUT_LIMIT-b->size)return AVERROR(ENOBUFS);
    if(b->size+size>b->capacity) {
        size_t cap=b->size+size+4096;if(cap>OUTPUT_LIMIT)cap=OUTPUT_LIMIT;
        uint8_t *p=av_realloc(b->data,cap);if(!p)return AVERROR(ENOMEM);
        b->data=p;b->capacity=cap;
    }
    if(size)memcpy(b->data+b->size,data,size);
    b->size+=size;return 0;
}
static int byte(Bytes *b,unsigned v) {uint8_t c=(uint8_t)v;return append(b,&c,1);}
static int prefix(const uint8_t *d,size_t n,size_t p) {
    if(n-p>=3&&!d[p]&&!d[p+1]&&d[p+2]==1)return 3;
    if(n-p>=4&&!d[p]&&!d[p+1]&&!d[p+2]&&d[p+3]==1)return 4;
    return 0;
}
static int next(Nals *s,Nal *n) {
    if(s->pos==s->size)return 0;
    size_t start=s->pos,begin,end;
    if(s->length_size) {
        if(s->size-start<(unsigned)s->length_size)return AVERROR_INVALIDDATA;
        uint32_t size=0;for(int i=0;i<s->length_size;i++)size=(size<<8)|s->data[start+i];
        begin=start+s->length_size;
        if(!size||size>s->size-begin)return AVERROR_INVALIDDATA;
        end=begin+size;
    } else {
        int p=prefix(s->data,s->size,start);
        /* Leading zero_byte is permitted, arbitrary leading junk is not. */
        while(!p&&start+3<s->size&&!s->data[start])p=prefix(s->data,s->size,++start);
        if(!p)return AVERROR_INVALIDDATA;
        begin=start+p;end=begin;
        while(end<s->size&&!prefix(s->data,s->size,end))end++;
        if(end==begin)return AVERROR_INVALIDDATA;
    }
    *n=(Nal){.data=s->data+begin,.size=end-begin,.begin=s->pos,.end=end};s->pos=end;return 1;
}
int naki_sei_length_size(enum AVCodecID codec,const uint8_t *e,int size) {
    if(size>0&&e[0]==1) {
        if(codec==AV_CODEC_ID_H264)return size>=5?(e[4]&3)+1:-1;
        if(codec==AV_CODEC_ID_HEVC)return size>=22?(e[21]&3)+1:-1;
    }
    return 0;
}
static int header(enum AVCodecID codec,const Nal *n,int *type,int *vcl,int *sei,int *suffix) {
    *vcl=*sei=*suffix=0;
    if(!n->size||(n->data[0]&128))return AVERROR_INVALIDDATA;
    if(codec==AV_CODEC_ID_H264) {
        *type=n->data[0]&31;*vcl=*type==1||*type==5;*sei=*type==6;
        if((*type>=2&&*type<=4)||*type==14||*type==15||*type==19||*type==20||*type==21)return AVERROR(ENOSYS);
        return 1;
    }
    if(n->size<2||!(n->data[1]&7))return AVERROR_INVALIDDATA;
    if(codec==AV_CODEC_ID_HEVC) {
        if((n->data[0]&1)||(n->data[1]&248))return AVERROR(ENOSYS);
        *type=(n->data[0]>>1)&63;*vcl=*type<=31;*sei=*type==39||*type==40;*suffix=*type==40;
        return 2;
    }
    if(codec==AV_CODEC_ID_VVC) {
        if(n->data[0]&63)return AVERROR(ENOSYS);
        *type=(n->data[1]>>3)&31;*vcl=*type<=11;*sei=*type==23||*type==24;*suffix=*type==24;
        return 2;
    }
    return AVERROR(ENOSYS);
}
static int unescape(const uint8_t *data,size_t size,Bytes *rbsp) {
    int zeros=0;
    for(size_t i=0;i<size;i++) {
        unsigned c=data[i];
        if(zeros>=2) {
            if(c==3) {
                if(i+1==size||data[i+1]>3)return AVERROR_INVALIDDATA;
                zeros=0;continue;
            }
            if(c<3)return AVERROR_INVALIDDATA;
        }
        int ret=byte(rbsp,c);if(ret<0)return ret;
        zeros=c==0?zeros+1:0;
    }
    return 0;
}
static int variable(const uint8_t *d,size_t end,size_t *pos,uint32_t *value) {
    uint32_t v=0;unsigned c;
    do {
        if(*pos==end)return AVERROR_INVALIDDATA;
        c=d[(*pos)++];if(v>UINT32_MAX-c)return AVERROR_INVALIDDATA;v+=c;
    } while(c==255);
    *value=v;return 0;
}
static int put_variable(Bytes *out,uint32_t value) {
    while(value>=255) {int ret=byte(out,255);if(ret<0)return ret;value-=255;}
    return byte(out,value);
}
/* Payload bytes are intentionally never interpreted, even type 5 < 16 bytes. */
static int message(Bytes *out,uint32_t type,const uint8_t *payload,uint32_t size) {
    int ret=put_variable(out,type);if(ret>=0)ret=put_variable(out,size);
    return ret<0?ret:append(out,payload,size);
}
static int write_nal(Bytes *out,enum AVCodecID codec,int suffix,int length_size,unsigned temporal_id_plus1,const Bytes *rbsp) {
    Bytes nal={0};int ret=0,zeros=0;
    if(codec==AV_CODEC_ID_H264)ret=byte(&nal,6);
    else if(codec==AV_CODEC_ID_HEVC) {ret=byte(&nal,(suffix?40:39)<<1);if(ret>=0)ret=byte(&nal,temporal_id_plus1);}
    else if(codec==AV_CODEC_ID_VVC) {ret=byte(&nal,0);if(ret>=0)ret=byte(&nal,((suffix?24:23)<<3)|temporal_id_plus1);}
    else ret=AVERROR(ENOSYS);
    for(size_t i=0;ret>=0&&i<rbsp->size;i++) {
        unsigned c=rbsp->data[i];
        if(zeros>=2&&c<=3) {ret=byte(&nal,3);zeros=0;}
        if(ret>=0)ret=byte(&nal,c);
        zeros=c==0?zeros+1:0;
    }
    if(ret>=0) {
        uint8_t pre[4]={0,0,0,1};
        if(length_size) {
            if(nal.size>=((uint64_t)1<<(length_size*8)))ret=AVERROR(ENOBUFS);
            else for(int i=0;i<length_size;i++)pre[length_size-1-i]=(uint8_t)(nal.size>>(i*8));
        }
        if(ret>=0)ret=append(out,pre,length_size?length_size:4);
        if(ret>=0)ret=append(out,nal.data,nal.size);
    }
    av_free(nal.data);return ret;
}
static int parse_sei(const Nal *n,int h,int suffix,const NakiSeiConfig *config,NakiSeiBundle *bundle,Bytes *remaining,int *changed) {
    Bytes rbsp={0};size_t size=n->size;
    while(size>(unsigned)h&&!n->data[size-1])size--; /* Annex B trailing_zero_8bits. */
    int ret=unescape(n->data+h,size-h,&rbsp);if(ret<0)goto end;
    if(!rbsp.size||rbsp.data[rbsp.size-1]!=128) {ret=AVERROR_INVALIDDATA;goto end;}
    size_t pos=0,end=rbsp.size-1;
    while(pos<end) {
        uint32_t type,len;
        if((ret=variable(rbsp.data,end,&pos,&type))<0||(ret=variable(rbsp.data,end,&pos,&len))<0)goto end;
        if(len>end-pos) {ret=AVERROR_INVALIDDATA;goto end;}
        if(naki_sei_selected(config,type)) {
            if(bundle->count==NAKI_SEI_MESSAGES||len>NAKI_SEI_PAYLOAD_BYTES-bundle->bytes) {ret=AVERROR(ENOBUFS);goto end;}
            bundle->messages[bundle->count++]=(NakiSeiMessage){type,bundle->bytes,len,(uint32_t)suffix};
            memcpy(bundle->data+bundle->bytes,rbsp.data+pos,len);bundle->bytes+=len;*changed=1;
        } else if((ret=message(remaining,type,rbsp.data+pos,len))<0)goto end;
        pos+=len;
    }
end:
    av_free(rbsp.data);return ret;
}
static int replace_packet(AVPacket *packet,const Bytes *data) {
    if(!data->size||data->size>INT_MAX)return AVERROR_INVALIDDATA;
    AVPacket *out=av_packet_alloc();if(!out)return AVERROR(ENOMEM);
    int ret=av_new_packet(out,(int)data->size);
    if(ret>=0)ret=av_packet_copy_props(out,packet);
    if(ret>=0) {memcpy(out->data,data->data,data->size);av_packet_unref(packet);av_packet_move_ref(packet,out);}
    av_packet_free(&out);return ret;
}
size_t naki_sei_bundle_size(const NakiSeiBundle *b) {return b?sizeof(*b)+b->bytes:0;}
int naki_sei_count(const NakiSeiBundle *b,const NakiSeiConfig *c) {
    int count=0;if(b)for(unsigned i=0;i<b->count;i++)count+=naki_sei_selected(c,b->messages[i].type);
    return count;
}
int naki_sei_extract(AVPacket *packet,enum AVCodecID codec,int length_size,const NakiSeiConfig *config,NakiSeiBundle **bundle) {
    *bundle=NULL;if(!config->count)return 0;
    if(packet->size<=0||(unsigned)packet->size>NAKI_INPUT_PACKET_BYTES||length_size<0||length_size>4)return AVERROR_INVALIDDATA;
    size_t side_size;
    if(av_packet_get_side_data(packet,AV_PKT_DATA_NEW_EXTRADATA,&side_size))return AVERROR(ENOSYS);
    NakiSeiBundle *b=av_mallocz(sizeof(*b)+NAKI_SEI_PAYLOAD_BYTES);if(!b)return AVERROR(ENOMEM);
    Nals s={packet->data,(size_t)packet->size,0,length_size};Nal n;Bytes rewritten={0};int ret,first_slices=0,vcls=0,changed=0;
    while((ret=next(&s,&n))>0) {
        int type,vcl,sei,suffix,h=header(codec,&n,&type,&vcl,&sei,&suffix);if(h<0) {ret=h;break;}
        if(vcl) {
            if(n.size<=(unsigned)h) {ret=AVERROR_INVALIDDATA;break;}
            /* H.264 first_mb_in_slice ue(v)==0 starts with 1. HEVC uses a flag. */
            first_slices+=!!(n.data[h]&128);vcls++;
            if(first_slices>1) {ret=AVERROR(ENOSYS);break;}
        }
        if(sei) {
            Bytes remaining={0};int removed=0;unsigned before=b->count;
            ret=parse_sei(&n,h,suffix,config,b,&remaining,&removed);
            /* H.264 SEI / HEVC prefix follows the next picture, not the last one. */
            if(ret>=0&&b->count>before&&((!suffix&&vcls)||(suffix&&!vcls)))ret=AVERROR(ENOSYS);
            if(ret>=0&&removed) {
                changed=1;
                if(remaining.size) {ret=byte(&remaining,128);if(ret>=0)ret=write_nal(&rewritten,codec,suffix,length_size,codec==AV_CODEC_ID_H264?1:n.data[1]&7,&remaining);}
            } else if(ret>=0)ret=append(&rewritten,packet->data+n.begin,n.end-n.begin);
            av_free(remaining.data);
        } else ret=append(&rewritten,packet->data+n.begin,n.end-n.begin);
        if(ret<0)break;
    }
    if(ret>=0&&(!vcls||first_slices!=1))ret=AVERROR(ENOSYS);
    if(ret>=0&&changed)ret=replace_packet(packet,&rewritten);
    if(ret>=0&&b->count) {*bundle=b;b=NULL;}
    av_free(b);av_free(rewritten.data);return ret<0?ret:0;
}
int naki_sei_inject(AVPacket *packet,enum AVCodecID codec,const NakiSeiBundle *b,const NakiSeiConfig *config) {
    if(!b)return 0;
    if(b->count>NAKI_SEI_MESSAGES||b->bytes>NAKI_SEI_PAYLOAD_BYTES)return AVERROR_INVALIDDATA;
    if(!naki_sei_count(b,config))return 0;
    int length_size=prefix(packet->data,(size_t)packet->size,0)?0:4;
    Bytes front={0},back={0},out={0};int ret=0;
    for(unsigned i=0;i<b->count;i++) {
        const NakiSeiMessage *m=&b->messages[i];if(!naki_sei_selected(config,m->type))continue;
        if(m->offset>b->bytes||m->size>b->bytes-m->offset) {ret=AVERROR_INVALIDDATA;goto end;}
        ret=message(codec!=AV_CODEC_ID_H264&&m->suffix?&back:&front,m->type,b->data+m->offset,m->size);if(ret<0)goto end;
    }
    if(front.size&&(ret=byte(&front,128))<0)goto end;
    if(back.size&&(ret=byte(&back,128))<0)goto end;
    Nals s={packet->data,(size_t)packet->size,0,length_size};Nal n;int inserted=0,vcls=0,suffix_written=0;unsigned tid=1;
    while((ret=next(&s,&n))>0) {
        int type,vcl,sei,suffix,h=header(codec,&n,&type,&vcl,&sei,&suffix);if(h<0) {ret=h;goto end;}
        if(vcl&&!inserted) {
            if(codec!=AV_CODEC_ID_H264)tid=n.data[1]&7;
            if(front.size&&(ret=write_nal(&out,codec,0,length_size,tid,&front))<0)goto end;
            inserted=1;
        }
        int end_nal=(codec==AV_CODEC_ID_HEVC&&(type==36||type==37))||(codec==AV_CODEC_ID_VVC&&(type==21||type==22));
        if(inserted&&end_nal&&back.size&&!suffix_written) {
            if((ret=write_nal(&out,codec,1,length_size,tid,&back))<0)goto end;
            suffix_written=1;
        }
        vcls+=vcl;if((ret=append(&out,packet->data+n.begin,n.end-n.begin))<0)goto end;
    }
    if(ret>=0&&!vcls)ret=AVERROR_INVALIDDATA;
    if(ret>=0&&back.size&&!suffix_written)ret=write_nal(&out,codec,1,length_size,tid,&back);
    if(ret>=0)ret=replace_packet(packet,&out);
end:
    av_free(front.data);av_free(back.data);av_free(out.data);return ret<0?ret:0;
}
