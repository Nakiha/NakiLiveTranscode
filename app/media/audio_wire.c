#include "audio_wire.h"
#include "control/intent.h"
#include "control/resource_limits.h"
#include <libavutil/intreadwrite.h>
#include <libavutil/mem.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#define PAR_FIELDS(X) X(codec_type) X(codec_id) X(format) X(bit_rate) X(bits_per_coded_sample) X(bits_per_raw_sample) \
    X(profile) X(level) X(sample_rate) X(block_align) X(frame_size) X(initial_padding) X(trailing_padding) X(seek_preroll)
#define PKT_FIELDS(X) X(pts) X(dts) X(duration) X(pos) X(flags) X(stream_index)
static char *hex(const uint8_t *p,size_t n){if(n>65536)return NULL;char *s=malloc(2*n+1);if(!s)return NULL;for(size_t i=0;i<n;i++){s[2*i]="0123456789abcdef"[p[i]>>4];s[2*i+1]="0123456789abcdef"[p[i]&15];}s[2*n]=0;return s;}
static int unhex(yyjson_val *v,uint8_t **data,size_t *bytes){
    if(!yyjson_is_str(v)||(yyjson_get_len(v)&1)||yyjson_get_len(v)>131072)return -EINVAL;
    *bytes=yyjson_get_len(v)/2;*data=av_mallocz(*bytes+AV_INPUT_BUFFER_PADDING_SIZE);if(!*data)return -ENOMEM;
    const char *s=yyjson_get_str(v);
    for(size_t i=0;i<2**bytes;i++){unsigned d=s[i]>='0'&&s[i]<='9'?s[i]-'0':s[i]>='a'&&s[i]<='f'?s[i]-'a'+10:16;if(d>15){av_freep(data);return -EINVAL;}if(!(i&1))(*data)[i/2]=(uint8_t)(d<<4);else (*data)[i/2]|=(uint8_t)d;}
    return 0;
}
static int side_ok(enum AVPacketSideDataType t){return t==AV_PKT_DATA_NEW_EXTRADATA||t==AV_PKT_DATA_PARAM_CHANGE||t==AV_PKT_DATA_SKIP_SAMPLES||t==AV_PKT_DATA_STRINGS_METADATA||t==AV_PKT_DATA_MPEGTS_STREAM_ID;}
int naki_audio_wire_pack(const InputItem *item,uint8_t *data,size_t capacity,size_t *written) {
    *written=0;const AVCodecParameters *p=item->params;const AVPacket *q=item->packet;
    if(!p||!q||q->size<0||(unsigned)q->size>NAKI_INPUT_PACKET_BYTES||p->extradata_size<0||p->extradata_size>65536||p->nb_coded_side_data||q->side_data_elems>16||item->tb.num<=0||item->tb.den<=0||item->demuxed_us<=0)return -EINVAL;
    yyjson_mut_doc *d=yyjson_mut_doc_new(NULL);if(!d)return -ENOMEM;yyjson_mut_val *root=yyjson_mut_obj(d),*par=yyjson_mut_obj(d),*pkt=yyjson_mut_obj(d),*sides=yyjson_mut_arr(d);yyjson_mut_doc_set_root(d,root);int ret=-EINVAL;
#define P(k) yyjson_mut_obj_add_sint(d,par,#k,p->k);
    PAR_FIELDS(P)
#undef P
    char layout[512];if(av_channel_layout_describe(&p->ch_layout,layout,sizeof(layout))<0)goto end;
    yyjson_mut_obj_add_strcpy(d,par,"layout",layout);yyjson_mut_obj_add_uint(d,par,"codec_tag",p->codec_tag);
    char *extra=hex(p->extradata,p->extradata_size);if(!extra){ret=-ENOMEM;goto end;}yyjson_mut_obj_add_strcpy(d,par,"extradata",extra);free(extra);
#define P(k) yyjson_mut_obj_add_sint(d,pkt,#k,q->k);
    PKT_FIELDS(P)
#undef P
    size_t total=0;
    for(int i=0;i<q->side_data_elems;i++){const AVPacketSideData *sd=&q->side_data[i];if(!side_ok(sd->type)||(sd->type==AV_PKT_DATA_MPEGTS_STREAM_ID&&sd->size!=1)||sd->size>65536-total){ret=-ENOTSUP;goto end;}total+=sd->size;char *b=hex(sd->data,sd->size);if(!b){ret=-ENOMEM;goto end;}yyjson_mut_val *v=yyjson_mut_obj(d);yyjson_mut_obj_add_int(d,v,"type",sd->type);yyjson_mut_obj_add_strcpy(d,v,"bytes",b);free(b);yyjson_mut_arr_add_val(sides,v);}
    yyjson_mut_obj_add_val(d,pkt,"side_data",sides);yyjson_mut_obj_add_val(d,root,"params",par);yyjson_mut_obj_add_val(d,root,"packet",pkt);
    yyjson_mut_obj_add_int(d,root,"tb_num",item->tb.num);yyjson_mut_obj_add_int(d,root,"tb_den",item->tb.den);yyjson_mut_obj_add_sint(d,root,"demuxed_us",item->demuxed_us);
    size_t n;char *json=yyjson_mut_write(d,0,&n);if(!json){ret=-ENOMEM;goto end;}
    size_t offset=(16+n+63)&~(size_t)63;
    if(n>256*1024||offset>capacity||(size_t)q->size+AV_INPUT_BUFFER_PADDING_SIZE>capacity-offset){free(json);ret=-ENOBUFS;goto end;}
    memcpy(data,"NKAW",4);AV_WL32(data+4,1);AV_WL32(data+8,n);AV_WL32(data+12,q->size);memcpy(data+16,json,n);free(json);
    memset(data+16+n,0,offset-16-n);memcpy(data+offset,q->data,q->size);memset(data+offset+q->size,0,AV_INPUT_BUFFER_PADDING_SIZE);
    *written=offset+q->size+AV_INPUT_BUFFER_PADDING_SIZE;ret=0;
end:yyjson_mut_doc_free(d);return ret;
}
static int integer(yyjson_val *root,const char *key,int64_t low,int64_t high,int64_t *out){yyjson_val *v=yyjson_obj_get(root,key);if(!yyjson_is_int(v)||(yyjson_is_uint(v)&&yyjson_get_uint(v)>INT64_MAX))return -EINVAL;*out=yyjson_get_sint(v);return *out<low||*out>high?-EINVAL:0;}
static void release_owner(void *opaque,uint8_t *data){(void)data;AVBufferRef *ref=opaque;av_buffer_unref(&ref);}
int naki_audio_wire_unpack(InputItem *item,AVBufferRef *owner) {
    if(!item||item->packet||item->params||!owner||owner->size<16)return -EINVAL;
    uint8_t *data=owner->data;unsigned n=AV_RL32(data+8),bytes=AV_RL32(data+12);size_t offset=(16+(size_t)n+63)&~(size_t)63;
    if(memcmp(data,"NKAW",4)||AV_RL32(data+4)!=1||n>256*1024||bytes>NAKI_INPUT_PACKET_BYTES||offset>owner->size||(size_t)bytes+AV_INPUT_BUFFER_PADDING_SIZE!=owner->size-offset)return -EINVAL;
    yyjson_doc *d=yyjson_read((char *)data+16,n,0);if(!d)return -EINVAL;
    yyjson_val *root=yyjson_doc_get_root(d),*par=yyjson_obj_get(root,"params"),*pkt=yyjson_obj_get(root,"packet");int ret=-EINVAL;int64_t value;
    /* This schema can contain bounded hex strings larger than control intents. */
    if(!yyjson_is_obj(root)||yyjson_obj_size(root)!=5||!yyjson_is_obj(par)||yyjson_obj_size(par)!=17||!yyjson_is_obj(pkt)||yyjson_obj_size(pkt)!=7)goto end;
    /* Validate duplicate structural keys separately; hex values are checked below. */
    yyjson_val *objects[]={root,par,pkt};for(unsigned j=0;j<3;j++){yyjson_obj_iter it=yyjson_obj_iter_with(objects[j]);yyjson_val *k;const char *seen[32];unsigned count=0;while((k=yyjson_obj_iter_next(&it))){const char *name=yyjson_get_str(k);if(strlen(name)!=yyjson_get_len(k))goto end;for(unsigned i=0;i<count;i++)if(!strcmp(name,seen[i]))goto end;seen[count++]=name;}}
    AVCodecParameters *p=avcodec_parameters_alloc();AVPacket *q=av_packet_alloc();item->params=p;item->packet=q;if(!p||!q){ret=-ENOMEM;goto end;}
#define P(k) do { if(integer(par,#k,INT_MIN,INT_MAX,&value))goto end; p->k=value; } while(0);
    /* bit_rate is the sole 64-bit parameter. */
    if(integer(par,"bit_rate",0,INT64_MAX,&value))goto end;
    p->bit_rate=value;
    P(codec_type) P(codec_id) P(format) P(bits_per_coded_sample) P(bits_per_raw_sample) P(profile) P(level) P(sample_rate) P(block_align) P(frame_size) P(initial_padding) P(trailing_padding) P(seek_preroll)
#undef P
    if(integer(par,"codec_tag",0,UINT32_MAX,&value))goto end;
    p->codec_tag=(uint32_t)value;
    yyjson_val *layout=yyjson_obj_get(par,"layout");if(!yyjson_is_str(layout)||yyjson_get_len(layout)>=512||strlen(yyjson_get_str(layout))!=yyjson_get_len(layout)||av_channel_layout_from_string(&p->ch_layout,yyjson_get_str(layout))<0)goto end;
    size_t extra=0;if(unhex(yyjson_obj_get(par,"extradata"),&p->extradata,&extra))goto end;
    p->extradata_size=(int)extra;
    if(integer(pkt,"pts",INT64_MIN,INT64_MAX,&value))goto end;
    q->pts=value;
    if(integer(pkt,"dts",INT64_MIN,INT64_MAX,&value))goto end;
    q->dts=value;
    if(integer(pkt,"duration",0,INT64_MAX,&value))goto end;
    q->duration=value;
    if(integer(pkt,"pos",INT64_MIN,INT64_MAX,&value))goto end;
    q->pos=value;
    if(integer(pkt,"flags",0,INT_MAX,&value))goto end;
    q->flags=(int)value;
    if(integer(pkt,"stream_index",0,63,&value))goto end;
    q->stream_index=(int)value;
    yyjson_val *sides=yyjson_obj_get(pkt,"side_data"),*sd;size_t i,m,total=0;if(!yyjson_is_arr(sides)||yyjson_arr_size(sides)>16)goto end;
    yyjson_arr_foreach(sides,i,m,sd){if(!yyjson_is_obj(sd)||yyjson_obj_size(sd)!=2||integer(sd,"type",0,INT_MAX,&value)||!side_ok(value))goto end;uint8_t *b=NULL;size_t length;if(unhex(yyjson_obj_get(sd,"bytes"),&b,&length))goto end;if((value==AV_PKT_DATA_MPEGTS_STREAM_ID&&length!=1)||length>65536-total){av_free(b);goto end;}total+=length;int r=av_packet_add_side_data(q,value,b,length);if(r<0){av_free(b);ret=r;goto end;}}
    if(integer(root,"tb_num",1,INT_MAX,&value))goto end;
    item->tb.num=value;
    if(integer(root,"tb_den",1,INT_MAX,&value))goto end;
    item->tb.den=value;
    if(integer(root,"demuxed_us",1,INT64_MAX,&value))goto end;
    item->demuxed_us=value;
    AVBufferRef *retained=av_buffer_ref(owner);if(!retained){ret=-ENOMEM;goto end;}q->buf=av_buffer_create(owner->data,owner->size,release_owner,retained,AV_BUFFER_FLAG_READONLY);if(!q->buf){av_buffer_unref(&retained);ret=-ENOMEM;goto end;}
    q->data=data+offset;q->size=(int)bytes;item->retained_bytes=owner->size;ret=0;
end:
    yyjson_doc_free(d);if(ret<0){av_packet_free(&item->packet);avcodec_parameters_free(&item->params);*item=(InputItem){0};}return ret;
}
