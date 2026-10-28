#include "media/frame_wire.h"
#include "media/shared_frame_pool.h"
#include "media/frame_trace.h"
#include "media/sei.h"
#include "control/resource_limits.h"
#include <libavutil/error.h>
#include <assert.h>
#include <unistd.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/imgutils.h>
static void returned(void *opaque,uint8_t *data){(void)data;(*(int *)opaque)++;}
static AVFrame *source(int format,int width,int height) {
    AVFrame *f=av_frame_alloc();assert(f);f->width=width;f->height=height;f->format=format;
    f->pts=90000;f->pkt_dts=87000;f->duration=1500;f->best_effort_timestamp=90000;
    f->sample_aspect_ratio=(AVRational){4,3};f->time_base=(AVRational){1,90000};
    f->pict_type=AV_PICTURE_TYPE_B;f->color_range=AVCOL_RANGE_MPEG;f->colorspace=AVCOL_SPC_BT709;
    f->color_primaries=AVCOL_PRI_BT709;f->color_trc=AVCOL_TRC_BT709;f->chroma_location=AVCHROMA_LOC_LEFT;
    assert(!av_frame_get_buffer(f,64));
    for(int plane=0;plane<3;plane++)for(int y=0;y<(plane?(height+1)/2:height);y++)
        memset(f->data[plane]+(size_t)y*f->linesize[plane],(plane+1)*51+y,(size_t)f->linesize[plane]);
    size_t sei=sizeof(NakiSeiBundle)+19;f->opaque_ref=av_buffer_allocz(sizeof(Stamp)+sei);assert(f->opaque_ref);
    Stamp *s=(Stamp *)f->opaque_ref->data;s->sequence=123;s->frame_id=82;s->source_pts=f->pts;s->source_us=1000000;
    s->submitted_us=401;s->decoded_us=503;s->ingress_us=301;s->packet_pts=f->pts;s->packet_dts=f->pkt_dts;
    s->picture_type=f->pict_type;s->timestamp_wraps=3;s->decoder_tracked=1;s->sei_size=(uint32_t)sei;
    s->decode_trace.version=AV_LIVE_DECODE_TRACE_VERSION;s->decode_trace.source_pts=f->pts;
    s->decode_trace.reconstructed_us=470;s->decode_trace.frontend_us=501;
    AVFrameSideData *live=av_frame_new_side_data(f,AV_FRAME_DATA_LIVE_DECODE_TRACE,sizeof(s->decode_trace));assert(live);memcpy(live->data,&s->decode_trace,sizeof(s->decode_trace));
    NakiSeiBundle *bundle=(NakiSeiBundle *)(f->opaque_ref->data+sizeof(*s));bundle->count=2;bundle->bytes=19;
    bundle->messages[0]=(NakiSeiMessage){.type=5,.size=16};bundle->messages[1]=(NakiSeiMessage){.type=4,.offset=16,.size=3,.suffix=1};
    for(unsigned i=0;i<19;i++)bundle->data[i]=(uint8_t)(i*7);
    AVFrameSideData *sd=av_frame_new_side_data(f,AV_FRAME_DATA_ICC_PROFILE,11);assert(sd);memcpy(sd->data,"hello\0world",11);
    assert(!av_dict_set(&sd->metadata,"scope","source",0));assert(!av_dict_set(&f->metadata,"description","round trip",0));return f;
}
static void compare_pixels(const AVFrame *a,const AVFrame *b) {
    int n=av_image_get_buffer_size(a->format,a->width,a->height,1);assert(n>0);
    uint8_t *x=av_malloc(n),*y=av_malloc(n);assert(x&&y);
    assert(av_image_copy_to_buffer(x,n,(const uint8_t *const *)a->data,a->linesize,a->format,a->width,a->height,1)==n);
    assert(av_image_copy_to_buffer(y,n,(const uint8_t *const *)b->data,b->linesize,b->format,b->width,b->height,1)==n);
    assert(!memcmp(x,y,n));av_free(x);av_free(y);
}
static void roundtrip(int format,int width,int height) {
    AVFrame *src=source(format,width,height),*dst=av_frame_alloc();assert(dst);
    NakiFramePool *pool=NULL;uint64_t pixels=(uint64_t)width*height;size_t capacity=naki_frame_wire_capacity(pixels);
    assert(capacity&&naki_frame_wire_capacity(NAKI_INPUT_MAX_PIXELS+1)==0);
    assert(!naki_pool_open(&pool,1,capacity,capacity+8192));NakiPoolToken token;uint8_t *data;
    assert(!naki_pool_acquire(pool,&token,&data));size_t written=0;
    assert(!naki_frame_wire_pack(src,pixels,data,capacity,&written)&&written>0&&written<capacity);
    assert(!naki_pool_publish(pool,&token,written));int fd=naki_pool_read_fd(pool);assert(fd>=0);
    NakiPoolView view={0};assert(!naki_pool_view_open(&view,fd,naki_pool_id(pool)));const uint8_t *blob=naki_pool_view_claim(&view,&token);assert(blob);
    int releases=0;
    /* Deliberately give unpack a non-READONLY owner with multiple refs. The
     * exported frame must still be READONLY when its own last ref remains. */
    AVBufferRef *owner=av_buffer_create((uint8_t *)blob,written,returned,&releases,0);assert(owner);
    AVBufferRef *extra=av_buffer_ref(owner);assert(extra);
    assert(!naki_frame_wire_unpack(dst,owner,pixels));assert(!av_frame_is_writable(dst));compare_pixels(src,dst);
    assert(dst->pts==src->pts&&dst->pkt_dts==src->pkt_dts&&dst->duration==src->duration&&dst->pict_type==src->pict_type);
    assert(dst->sample_aspect_ratio.num==4&&dst->sample_aspect_ratio.den==3&&dst->time_base.den==90000);
    assert(dst->color_range==src->color_range&&dst->colorspace==src->colorspace&&dst->color_primaries==src->color_primaries&&dst->color_trc==src->color_trc);
    Stamp *s=(Stamp *)dst->opaque_ref->data,*original=(Stamp *)src->opaque_ref->data;
    assert(s->sequence==123&&s->frame_id==82&&s->source_pts==src->pts&&s->submitted_us==401&&s->timestamp_wraps==3);
    assert(!s->decoder_tracked&&!s->delivery_owner&&!s->delivery_lease&&!s->branch_charge);
    assert(!memcmp(&s->decode_trace,&original->decode_trace,sizeof(s->decode_trace)));
    assert(s->sei_size==original->sei_size&&!memcmp((uint8_t *)s+sizeof(*s),(uint8_t *)original+sizeof(*s),s->sei_size));
    AVFrameSideData *sd=av_frame_get_side_data(dst,AV_FRAME_DATA_ICC_PROFILE);assert(sd&&sd->size==11&&!memcmp(sd->data,"hello\0world",11));
    assert(!strcmp(av_dict_get(sd->metadata,"scope",NULL,0)->value,"source"));
    assert(!strcmp(av_dict_get(dst->metadata,"description",NULL,0)->value,"round trip"));
    AVFrame *clone=av_frame_clone(dst);assert(clone);av_buffer_unref(&owner);av_buffer_unref(&extra);av_frame_unref(dst);
    assert(!releases&&!av_frame_is_writable(clone));size_t pixel_offset=(16+AV_RL32(blob+8)+63)&~(size_t)63;uint8_t previous=blob[pixel_offset];
    assert(!av_frame_make_writable(clone));assert(releases==1);clone->data[0][0]^=255;assert(blob[pixel_offset]==previous);av_frame_free(&clone);
    owner=av_buffer_create((uint8_t *)blob,written,returned,&releases,AV_BUFFER_FLAG_READONLY);assert(owner);
    AVBufferRef *short_owner=av_buffer_create((uint8_t *)blob,written-1,returned,&releases,AV_BUFFER_FLAG_READONLY);assert(short_owner);
    assert(naki_frame_wire_unpack(dst,short_owner,pixels)<0&&!dst->buf[0]&&!dst->opaque_ref);av_buffer_unref(&short_owner);
    AV_WL32(data+4,NAKI_FRAME_WIRE_VERSION+1);assert(naki_frame_wire_unpack(dst,owner,pixels)<0&&!dst->buf[0]);AV_WL32(data+4,NAKI_FRAME_WIRE_VERSION);
    char *json=(char *)data+16;unsigned length=AV_RL32(data+8);char *copy=malloc(length+1);assert(copy);memcpy(copy,json,length);copy[length]=0;
    char *field=strstr(copy,"\"format\"");assert(field);size_t position=(size_t)(field-copy);memcpy(json+position,"\"width\" ",8);
    assert(naki_frame_wire_unpack(dst,owner,pixels)<0&&!dst->buf[0]);memcpy(json,copy,length);
    field=strstr(copy,"tracked_packet");assert(field);position=(size_t)(field-copy);memcpy(json+position,"delivery_owner",14);
    assert(naki_frame_wire_unpack(dst,owner,pixels)<0&&!dst->opaque_ref);memcpy(json,copy,length);free(copy);
    av_buffer_unref(&owner);assert(releases==3);
    /* No process-local pointer or delivery handle may enter a source record. */
    original->delivery_lease=42;assert(naki_frame_wire_pack(src,pixels,data,capacity,&written)==-EINVAL&&written==0);original->delivery_lease=0;
    assert(av_frame_new_side_data(src,AV_FRAME_DATA_REGIONS_OF_INTEREST,32));
    assert(naki_frame_wire_pack(src,pixels,data,capacity,&written)==-ENOTSUP&&written==0);
    av_frame_free(&dst);av_frame_free(&src);naki_pool_view_close(&view);close(fd);assert(!naki_pool_producer_release(pool,&token));assert(!naki_pool_close(&pool));
}
int main(void) {
    roundtrip(AV_PIX_FMT_YUV420P,16,16);roundtrip(AV_PIX_FMT_YUV420P,17,19);
    roundtrip(AV_PIX_FMT_YUV420P10LE,16,16);roundtrip(AV_PIX_FMT_YUV420P10LE,17,19);
    puts("PASS planar 8/10-bit pixels; source identity/SEI/trace/color metadata; COW/final release; malformed and pointer-bearing records rejected");return 0;
}
