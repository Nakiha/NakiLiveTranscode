#include "frame_wire.h"
#include "frame_trace.h"
#include "sei.h"
#include "control/resource_limits.h"
#include <errno.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include "libavutil/intreadwrite.h"
#include "libavutil/imgutils.h"
#include <limits.h>
#define PREFIX 16U
#define SIDE_BYTES_MAX (32U*1024)
#define FRAME_I32(X) X(width) X(height) X(format) X(pict_type) X(flags) X(decode_error_flags) X(repeat_pict) \
    X(color_range) X(color_primaries) X(color_trc) X(colorspace) X(chroma_location)
#define FRAME_I64(X) X(pts) X(pkt_dts) X(duration) X(best_effort_timestamp)
#define FRAME_CROP(X) X(crop_top) X(crop_bottom) X(crop_left) X(crop_right)
#define SOURCE_I64(X) X(network_first_read_us) X(network_complete_us) X(demux_finished_us) X(network_tag_position) \
    X(async_ready_us) X(decoder_owner_us) X(decoder_receive_us) X(submit_first_us) X(sequence) X(read_us) X(ingress_us) \
    X(submitted_us) X(decoded_us) X(filtered_us) X(encoder_submit_us) X(encoder_accepted_us) X(scheduled_us) X(filter_started_us) \
    X(frame_id) X(packet_pts) X(packet_dts) X(raw_pts) X(source_pts) X(source_us) X(subscription_generation) X(source_rate_bps)
#define SOURCE_I32(X) X(picture_type) X(decoded_key) X(input_key) X(first_i) X(intra_reason) X(timestamp_source) X(tracked_packet)
#define COUNT_FIELD(n) +1
enum {FRAME_FIELDS=0 FRAME_I32(COUNT_FIELD) FRAME_I64(COUNT_FIELD) FRAME_CROP(COUNT_FIELD) +10,
      STAMP_FIELDS=0 SOURCE_I64(COUNT_FIELD) SOURCE_I32(COUNT_FIELD) +1,
      TRACE_FIELDS=1 AV_LIVE_DECODE_TRACE_FIELDS(COUNT_FIELD)};
#undef COUNT_FIELD
size_t naki_frame_wire_capacity(uint64_t pixels) {
    if(!pixels||pixels>NAKI_INPUT_MAX_PIXELS)return 0;
    return (size_t)(3*pixels+256U*NAKI_FRAME_WIRE_DIM_MAX+NAKI_FRAME_WIRE_META_MAX+256);
}
static int shape(const AVFrame *f,uint64_t limit) {
    if(!limit||limit>NAKI_INPUT_MAX_PIXELS||f->width<1||f->height<1||f->width>NAKI_FRAME_WIRE_DIM_MAX||f->height>NAKI_FRAME_WIRE_DIM_MAX||
       (uint64_t)f->width*f->height>limit||(f->format!=AV_PIX_FMT_YUV420P&&f->format!=AV_PIX_FMT_YUV420P10LE)||
       f->crop_left>=(size_t)f->width||f->crop_right>=(size_t)f->width||f->crop_top>=(size_t)f->height||f->crop_bottom>=(size_t)f->height||
       f->crop_left+f->crop_right>=(size_t)f->width||f->crop_top+f->crop_bottom>=(size_t)f->height||
       f->sample_aspect_ratio.num<0||f->sample_aspect_ratio.den<0||(f->sample_aspect_ratio.num&&!f->sample_aspect_ratio.den)||
       f->time_base.num<0||f->time_base.den<0||(f->time_base.num&&!f->time_base.den))return -EINVAL;
    return av_image_get_buffer_size(f->format,f->width,f->height,64);
}
static int int64_field(yyjson_val *root,const char *key,int64_t *out) {
    yyjson_val *v=yyjson_obj_get(root,key);
    if(!yyjson_is_int(v)||(yyjson_is_uint(v)&&yyjson_get_uint(v)>INT64_MAX))return -EINVAL;
    *out=yyjson_get_sint(v);return 0;
}
static int bounded(yyjson_val *root,const char *key,int64_t low,int64_t high,int64_t *out) {
    return int64_field(root,key,out)||*out<low||*out>high?-EINVAL:0;
}
static int safe_side(enum AVFrameSideDataType type) {
    switch(type) {
    case AV_FRAME_DATA_A53_CC:case AV_FRAME_DATA_STEREO3D:case AV_FRAME_DATA_DISPLAYMATRIX:
    case AV_FRAME_DATA_MASTERING_DISPLAY_METADATA:case AV_FRAME_DATA_CONTENT_LIGHT_LEVEL:
    case AV_FRAME_DATA_SEI_UNREGISTERED:case AV_FRAME_DATA_ICC_PROFILE:return 1;
    default:return 0; /* Unsupported opaque layouts fail explicitly, never drop. */
    }
}
static char *hex(const uint8_t *data,size_t n) {
    if(n>NAKI_SEI_PAYLOAD_BYTES)return NULL;
    static const char alphabet[]="0123456789abcdef";char *out=malloc(2*n+1);if(!out)return NULL;
    for(size_t i=0;i<n;i++){out[2*i]=alphabet[data[i]>>4];out[2*i+1]=alphabet[data[i]&15];}out[2*n]=0;return out;
}
static int unhex(yyjson_val *v,uint8_t *data,size_t n) {
    if(!yyjson_is_str(v)||yyjson_get_len(v)!=2*n)return -EINVAL;
    const unsigned char *s=(const unsigned char *)yyjson_get_str(v);
    for(size_t i=0;i<2*n;i++) {
        unsigned digit=s[i]>='0'&&s[i]<='9'?s[i]-'0':s[i]>='a'&&s[i]<='f'?s[i]-'a'+10:16;
        if(digit>15)return -EINVAL;
        if(!(i&1))data[i/2]=(uint8_t)(digit<<4);else data[i/2]|=(uint8_t)digit;
    }
    return 0;
}
static yyjson_mut_val *dictionary(yyjson_mut_doc *d,const AVDictionary *dict,int *error) {
    yyjson_mut_val *items=yyjson_mut_arr(d);const AVDictionaryEntry *entry=NULL;unsigned count=0;size_t bytes=0;
    while((entry=av_dict_iterate(dict,entry))) {
        size_t key=strlen(entry->key),value=strlen(entry->value);bytes+=key+value;
        if(++count>64||key>256||value>4096||bytes>16384){*error=-ENOBUFS;break;}
        yyjson_mut_val *pair=yyjson_mut_obj(d);
        if(!yyjson_mut_obj_add_strcpy(d,pair,"key",entry->key)||!yyjson_mut_obj_add_strcpy(d,pair,"value",entry->value)||!yyjson_mut_arr_add_val(items,pair)){*error=-ENOMEM;break;}
    }
    return items;
}
static int read_dictionary(yyjson_val *v,AVDictionary **out) {
    if(!yyjson_is_arr(v)||yyjson_arr_size(v)>64)return -EINVAL;
    size_t i,max,bytes=0;yyjson_val *pair;
    yyjson_arr_foreach(v,i,max,pair) {
        yyjson_val *k=yyjson_obj_get(pair,"key"),*x=yyjson_obj_get(pair,"value");
        if(!yyjson_is_obj(pair)||yyjson_obj_size(pair)!=2||!yyjson_is_str(k)||!yyjson_is_str(x)||yyjson_get_len(k)>256||yyjson_get_len(x)>4096||strlen(yyjson_get_str(k))!=yyjson_get_len(k)||strlen(yyjson_get_str(x))!=yyjson_get_len(x))return -EINVAL;
        bytes+=yyjson_get_len(k)+yyjson_get_len(x);if(bytes>16384)return -ENOBUFS;
        int ret=av_dict_set(out,yyjson_get_str(k),yyjson_get_str(x),AV_DICT_MULTIKEY);if(ret<0)return ret;
    }
    return 0;
}
int naki_frame_wire_pack(const AVFrame *f,uint64_t limit,uint8_t *data,size_t capacity,size_t *written) {
    if(!f||!data||!written||!f->opaque_ref||f->opaque_ref->size<sizeof(Stamp))return -EINVAL;
    *written=0;const Stamp *s=(const Stamp *)f->opaque_ref->data;
    if(f->opaque_ref->size!=sizeof(*s)+s->sei_size||s->delivery_owner||s->delivery_lease||s->branch_charge||f->opaque||s->frame_id<=0||s->source_us==INT64_MIN||s->source_us==INT64_MAX||s->source_pts!=f->pts||s->timestamp_wraps>INT64_MAX)return -EINVAL;
    int pixels=shape(f,limit);if(pixels<0)return pixels;
    yyjson_mut_doc *d=yyjson_mut_doc_new(NULL);if(!d)return -ENOMEM;
    yyjson_mut_val *root=yyjson_mut_obj(d),*stamp=yyjson_mut_obj(d);yyjson_mut_doc_set_root(d,root);int ret=0;
#define F(n) yyjson_mut_obj_add_sint(d,root,#n,f->n);
    FRAME_I32(F) FRAME_I64(F) FRAME_CROP(F)
#undef F
    yyjson_mut_obj_add_int(d,root,"sar_num",f->sample_aspect_ratio.num);yyjson_mut_obj_add_int(d,root,"sar_den",f->sample_aspect_ratio.den);
    yyjson_mut_obj_add_int(d,root,"tb_num",f->time_base.num);yyjson_mut_obj_add_int(d,root,"tb_den",f->time_base.den);
#define S(n) yyjson_mut_obj_add_sint(d,stamp,#n,s->n);
    SOURCE_I64(S) SOURCE_I32(S) S(timestamp_wraps)
#undef S
    yyjson_mut_obj_add_val(d,root,"stamp",stamp);
    yyjson_mut_val *trace=yyjson_mut_obj(d);yyjson_mut_obj_add_sint(d,trace,"version",s->decode_trace.version);
#define T(n) yyjson_mut_obj_add_sint(d,trace,#n,s->decode_trace.n);
    AV_LIVE_DECODE_TRACE_FIELDS(T)
#undef T
    yyjson_mut_obj_add_val(d,root,"decode_trace",trace);
    yyjson_mut_val *sides=yyjson_mut_arr(d);size_t side_bytes=0;int live_trace_present=0;
    if(f->nb_side_data>16){ret=-ENOBUFS;goto end;}
    for(int i=0;i<f->nb_side_data;i++) {
        const AVFrameSideData *sd=f->side_data[i];
        if(sd->type==AV_FRAME_DATA_LIVE_DECODE_TRACE) {
            if(s->decode_trace.version!=AV_LIVE_DECODE_TRACE_VERSION||av_dict_count(sd->metadata)){ret=-EINVAL;goto end;}
            live_trace_present=1;continue; /* Rebuild from the atomic source snapshot, not mutable bytes. */
        }
        if(!safe_side(sd->type)){ret=-ENOTSUP;goto end;}
        if(sd->size>SIDE_BYTES_MAX-side_bytes){ret=-ENOBUFS;goto end;}side_bytes+=sd->size;
        yyjson_mut_val *v=yyjson_mut_obj(d);char *bytes=hex(sd->data,sd->size);if(!bytes){ret=-ENOMEM;goto end;}
        yyjson_mut_obj_add_int(d,v,"type",sd->type);yyjson_mut_obj_add_strcpy(d,v,"bytes",bytes);free(bytes);
        yyjson_mut_obj_add_val(d,v,"metadata",dictionary(d,sd->metadata,&ret));yyjson_mut_arr_add_val(sides,v);if(ret<0)goto end;
    }
    if(live_trace_present!=(s->decode_trace.version!=0)){ret=-EINVAL;goto end;}
    yyjson_mut_obj_add_val(d,root,"side_data",sides);yyjson_mut_obj_add_val(d,root,"metadata",dictionary(d,f->metadata,&ret));if(ret<0)goto end;
    yyjson_mut_val *sei=yyjson_mut_arr(d);const NakiSeiBundle *bundle=NULL;
    if(s->sei_size) {
        if(s->sei_size<sizeof(NakiSeiBundle)){ret=-EINVAL;goto end;}
        bundle=(const NakiSeiBundle *)(f->opaque_ref->data+sizeof(*s));
        if(!bundle->count||bundle->count>NAKI_SEI_MESSAGES||bundle->bytes>NAKI_SEI_PAYLOAD_BYTES||sizeof(*bundle)+bundle->bytes!=s->sei_size){ret=-EINVAL;goto end;}
        size_t sei_bytes=0;
        for(unsigned i=0;i<bundle->count;i++) {
            const NakiSeiMessage *m=&bundle->messages[i];
            if(m->offset>bundle->bytes||m->size>bundle->bytes-m->offset||m->suffix>1){ret=-EINVAL;goto end;}
            if(m->size>NAKI_SEI_PAYLOAD_BYTES-sei_bytes){ret=-ENOBUFS;goto end;}sei_bytes+=m->size;
            char *bytes=hex(bundle->data+m->offset,m->size);if(!bytes){ret=-ENOMEM;goto end;}
            yyjson_mut_val *v=yyjson_mut_obj(d);yyjson_mut_obj_add_uint(d,v,"type",m->type);yyjson_mut_obj_add_uint(d,v,"suffix",m->suffix);
            yyjson_mut_obj_add_strcpy(d,v,"bytes",bytes);free(bytes);yyjson_mut_arr_add_val(sei,v);
        }
    }
    yyjson_mut_obj_add_val(d,root,"sei",sei);
    uint8_t *layout[4];int strides[4];
    if(av_image_fill_arrays(layout,strides,data,f->format,f->width,f->height,64)!=pixels){ret=-EINVAL;goto end;}
    yyjson_mut_val *planes=yyjson_mut_arr(d);
    for(unsigned i=0;i<3;i++){
        yyjson_mut_val *plane=yyjson_mut_obj(d);uint64_t rows=i?(f->height+1)/2:f->height;
        yyjson_mut_obj_add_uint(d,plane,"offset",(uint64_t)(layout[i]-data));
        yyjson_mut_obj_add_uint(d,plane,"stride",strides[i]);yyjson_mut_obj_add_uint(d,plane,"length",rows*strides[i]);
        yyjson_mut_arr_add_val(planes,plane);
    }
    yyjson_mut_obj_add_val(d,root,"planes",planes);
    size_t length;char *json=yyjson_mut_write(d,0,&length);if(!json){ret=-ENOMEM;goto end;}
    size_t offset=(PREFIX+length+63)&~(size_t)63;
    if(length>NAKI_FRAME_WIRE_META_MAX||offset>capacity||capacity-offset<(size_t)pixels+64){free(json);ret=-ENOBUFS;goto end;}
    memcpy(data,"NKFW",4);AV_WL32(data+4,NAKI_FRAME_WIRE_VERSION);AV_WL32(data+8,length);AV_WL32(data+12,pixels);
    memcpy(data+PREFIX,json,length);free(json);memset(data+PREFIX+length,0,offset-PREFIX-length);
    memset(data+offset,0,(size_t)pixels+64);
    ret=av_image_copy_to_buffer(data+offset,pixels,(const uint8_t *const *)f->data,f->linesize,f->format,f->width,f->height,64);
    if(ret==pixels){*written=offset+pixels+64;ret=0;}else if(ret>=0)ret=-EINVAL;
end:
    yyjson_mut_doc_free(d);return ret;
}
/* Reject duplicate keys and unreasonable nesting before interpreting metadata.
 * Dictionaries have user keys; structural schemas still enforce field types. */
static int unique(yyjson_val *v,unsigned depth,unsigned *nodes) {
    if(depth>8||++*nodes>8192)return -EINVAL;
    if(yyjson_is_obj(v)) {
        if(yyjson_obj_size(v)>128)return -EINVAL;
        size_t i,max;yyjson_val *key,*value;const char *seen[128];size_t count=0;
        yyjson_obj_foreach(v,i,max,key,value) {
            const char *name=yyjson_get_str(key);if(strlen(name)!=yyjson_get_len(key))return -EINVAL;
            for(size_t k=0;k<count;k++)if(!strcmp(name,seen[k]))return -EINVAL;
            seen[count++]=name;if(unique(value,depth+1,nodes))return -EINVAL;
        }
    } else if(yyjson_is_arr(v)) {
        if(yyjson_arr_size(v)>64)return -EINVAL;
        size_t i,max;yyjson_val *value;yyjson_arr_foreach(v,i,max,value)if(unique(value,depth+1,nodes))return -EINVAL;
    }
    return 0;
}
static void readonly_owner_free(void *opaque,uint8_t *data) {
    (void)data;AVBufferRef *owner=opaque;av_buffer_unref(&owner);
}
int naki_frame_wire_unpack(AVFrame *f,AVBufferRef *owner,uint64_t limit) {
    if(!f||f->buf[0]||f->opaque_ref||!owner||owner->size<PREFIX)return -EINVAL;
    const uint8_t *data=owner->data;unsigned length=AV_RL32(data+8),pixels=AV_RL32(data+12);
    if(memcmp(data,"NKFW",4)||AV_RL32(data+4)!=NAKI_FRAME_WIRE_VERSION||length>NAKI_FRAME_WIRE_META_MAX||length>owner->size-PREFIX)return -EINVAL;
    size_t offset=(PREFIX+(size_t)length+63)&~(size_t)63;
    if(offset>owner->size||(size_t)pixels+64!=owner->size-offset)return -EINVAL;
    yyjson_doc *d=yyjson_read((const char *)data+PREFIX,length,0);if(!d)return -EINVAL;
    yyjson_val *root=yyjson_doc_get_root(d),*stamp=yyjson_obj_get(root,"stamp"),*trace=yyjson_obj_get(root,"decode_trace");int ret=-EINVAL;unsigned nodes=0;int64_t value;
    if(!yyjson_is_obj(root)||unique(root,0,&nodes)||!yyjson_is_obj(stamp)||!yyjson_is_obj(trace)||yyjson_obj_size(root)!=FRAME_FIELDS||yyjson_obj_size(stamp)!=STAMP_FIELDS||yyjson_obj_size(trace)!=TRACE_FIELDS)goto fail;
#define F(n) if(bounded(root,#n,INT_MIN,INT_MAX,&value))goto fail;f->n=(int)value;
    FRAME_I32(F)
#undef F
#define F(n) if(int64_field(root,#n,&value))goto fail;f->n=value;
    FRAME_I64(F)
#undef F
#define F(n) if(bounded(root,#n,0,NAKI_FRAME_WIRE_DIM_MAX,&value))goto fail;f->n=(size_t)value;
    FRAME_CROP(F)
#undef F
#define R(k,target) if(bounded(root,k,0,INT_MAX,&value))goto fail;target=(int)value;
    R("sar_num",f->sample_aspect_ratio.num) R("sar_den",f->sample_aspect_ratio.den) R("tb_num",f->time_base.num) R("tb_den",f->time_base.den)
#undef R
    if(shape(f,limit)!=(int)pixels||pixels>INT_MAX||f->pict_type<0||f->pict_type>AV_PICTURE_TYPE_BI||
       f->color_range<0||f->color_range>=AVCOL_RANGE_NB||f->color_primaries<0||f->color_primaries>=AVCOL_PRI_NB||
       f->color_trc<0||f->color_trc>=AVCOL_TRC_NB||f->colorspace<0||f->colorspace>=AVCOL_SPC_NB||
       f->chroma_location<0||f->chroma_location>=AVCHROMA_LOC_NB)goto fail;
    uint8_t *layout[4];int strides[4];
    if(av_image_fill_arrays(layout,strides,owner->data+offset,f->format,f->width,f->height,64)!=(int)pixels)goto fail;
    yyjson_val *planes=yyjson_obj_get(root,"planes");if(!yyjson_is_arr(planes)||yyjson_arr_size(planes)!=3)goto fail;
    for(unsigned i=0;i<3;i++){
        yyjson_val *plane=yyjson_arr_get(planes,i);int64_t position,stride,length;uint64_t rows=i?(f->height+1)/2:f->height;
        if(!yyjson_is_obj(plane)||yyjson_obj_size(plane)!=3||bounded(plane,"offset",0,pixels,&position)||
           bounded(plane,"stride",1,INT_MAX,&stride)||bounded(plane,"length",1,pixels,&length)||
           position!=(int64_t)(layout[i]-(owner->data+offset))||stride!=strides[i]||(uint64_t)length!=rows*(uint64_t)stride||
           (uint64_t)position+(uint64_t)length>pixels)goto fail;
    }
    yyjson_val *sei=yyjson_obj_get(root,"sei");if(!yyjson_is_arr(sei))goto fail;
    unsigned count=(unsigned)yyjson_arr_size(sei);size_t bytes=0,i,max;yyjson_val *v;
    yyjson_arr_foreach(sei,i,max,v) {
        yyjson_val *blob=yyjson_obj_get(v,"bytes");if(!yyjson_is_obj(v)||yyjson_obj_size(v)!=3||!yyjson_is_str(blob)||(yyjson_get_len(blob)&1)||yyjson_get_len(blob)/2>NAKI_SEI_PAYLOAD_BYTES-bytes)goto fail;
        bytes+=yyjson_get_len(blob)/2;
    }
    size_t sei_size=count?sizeof(NakiSeiBundle)+bytes:0;
    f->opaque_ref=av_buffer_allocz(sizeof(Stamp)+sei_size);if(!f->opaque_ref){ret=-ENOMEM;goto fail;}
    Stamp *s=(Stamp *)f->opaque_ref->data;s->sei_size=(uint32_t)sei_size;
#define S(n) if(int64_field(stamp,#n,&value))goto fail;s->n=value;
    SOURCE_I64(S)
#undef S
#define S(n) if(bounded(stamp,#n,INT_MIN,INT_MAX,&value))goto fail;s->n=(int)value;
    SOURCE_I32(S)
#undef S
    if(bounded(stamp,"timestamp_wraps",0,INT64_MAX,&value)||s->frame_id<=0||s->source_us==INT64_MIN||s->source_us==INT64_MAX||s->source_pts!=f->pts)goto fail;
    s->timestamp_wraps=(uint64_t)value;
    if(bounded(trace,"version",0,AV_LIVE_DECODE_TRACE_VERSION,&value))goto fail;
    s->decode_trace.version=value;
#define T(n) if(int64_field(trace,#n,&value))goto fail;s->decode_trace.n=value;
    AV_LIVE_DECODE_TRACE_FIELDS(T)
#undef T
    if(count) {
        NakiSeiBundle *b=(NakiSeiBundle *)(f->opaque_ref->data+sizeof(*s));b->count=count;b->bytes=(uint32_t)bytes;size_t position=0;
        yyjson_arr_foreach(sei,i,max,v) {
            NakiSeiMessage *m=&b->messages[i];if(bounded(v,"type",0,UINT32_MAX,&value))goto fail;m->type=(uint32_t)value;
            if(bounded(v,"suffix",0,1,&value))goto fail;
            m->suffix=(uint32_t)value;
            yyjson_val *blob=yyjson_obj_get(v,"bytes");m->size=(uint32_t)yyjson_get_len(blob)/2;m->offset=(uint32_t)position;
            if(unhex(blob,b->data+position,m->size))goto fail;
            position+=m->size;
        }
    }
    yyjson_val *sides=yyjson_obj_get(root,"side_data");if(!yyjson_is_arr(sides)||yyjson_arr_size(sides)>16)goto fail;size_t side_bytes=0;
    yyjson_arr_foreach(sides,i,max,v) {
        if(!yyjson_is_obj(v)||yyjson_obj_size(v)!=3||bounded(v,"type",0,INT_MAX,&value)||!safe_side((enum AVFrameSideDataType)value))goto fail;
        enum AVFrameSideDataType type=(enum AVFrameSideDataType)value;yyjson_val *blob=yyjson_obj_get(v,"bytes");
        if(!yyjson_is_str(blob)||(yyjson_get_len(blob)&1)||yyjson_get_len(blob)/2>SIDE_BYTES_MAX-side_bytes)goto fail;
        size_t n=yyjson_get_len(blob)/2;side_bytes+=n;AVFrameSideData *sd=av_frame_new_side_data(f,type,n);if(!sd){ret=-ENOMEM;goto fail;}
        if(unhex(blob,sd->data,n)||read_dictionary(yyjson_obj_get(v,"metadata"),&sd->metadata))goto fail;
    }
    if(read_dictionary(yyjson_obj_get(root,"metadata"),&f->metadata))goto fail;
    if(s->decode_trace.version) {
        AVFrameSideData *sd=av_frame_new_side_data(f,AV_FRAME_DATA_LIVE_DECODE_TRACE,sizeof(s->decode_trace));if(!sd){ret=-ENOMEM;goto fail;}
        memcpy(sd->data,&s->decode_trace,sizeof(s->decode_trace)); /* Local reconstruction, not wire struct copy. */
    }
    AVBufferRef *retained=av_buffer_ref(owner);if(!retained){ret=-ENOMEM;goto fail;}
    f->buf[0]=av_buffer_create(owner->data,owner->size,readonly_owner_free,retained,AV_BUFFER_FLAG_READONLY);
    if(!f->buf[0]){av_buffer_unref(&retained);ret=-ENOMEM;goto fail;}
    if(av_image_fill_arrays(f->data,f->linesize,owner->data+offset,f->format,f->width,f->height,64)!=(int)pixels)goto fail;
    yyjson_doc_free(d);return 0;
fail:
    yyjson_doc_free(d);av_frame_unref(f);return ret;
}
