#define _DEFAULT_SOURCE
/* Test-only native failures. Never linked into the normal service. */
#include "media/media_hooks.h"
#include "media/frame_wire.h"
#include "media/rendition_message.h"
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libavutil/buffer.h>
#include <libavutil/error.h>
#include <libavformat/avformat.h>
static atomic_int target;
static const char *mode;
static AVBufferRef *held[NAKI_RENDITION_VIDEO_CREDIT];
static uint64_t held_hash[NAKI_RENDITION_VIDEO_CREDIT];
static unsigned held_count,packets;
int __real_av_write_trailer(AVFormatContext *context);
int __wrap_av_write_trailer(AVFormatContext *context) {
    int ret=__real_av_write_trailer(context);
    return atomic_load(&target)&&!strcmp(mode,"trailer_error")?AVERROR(EIO):ret;
}
static pthread_mutex_t deadlock=PTHREAD_MUTEX_INITIALIZER;
static uint64_t hash(const AVBufferRef *b){uint64_t h=UINT64_C(14695981039346656037);for(size_t i=0;i<b->size;i++)h=(h^b->data[i])*UINT64_C(1099511628211);return h;}
int __real_naki_frame_wire_unpack(AVFrame *frame,AVBufferRef *owner,uint64_t limit);
int __wrap_naki_frame_wire_unpack(AVFrame *frame,AVBufferRef *owner,uint64_t limit) {
    int ret=__real_naki_frame_wire_unpack(frame,owner,limit);
    if(ret<0||!atomic_load(&target))return ret;
    if(!strcmp(mode,"write_shared")){volatile uint8_t *pixel=frame->data[0];*pixel^=1;}
    if(!strcmp(mode,"hold_raw")&&held_count<NAKI_RENDITION_VIDEO_CREDIT){held[held_count]=av_buffer_ref(frame->buf[0]);if(!held[held_count])return AVERROR(ENOMEM);held_hash[held_count]=hash(held[held_count]);held_count++;}
    return ret;
}
int __real_naki_rendition_send(int fd,const NakiRenditionMessage *message);
int __wrap_naki_rendition_send(int fd,const NakiRenditionMessage *message) {
    if(atomic_load(&target)&&!strcmp(mode,"stop_release")&&message->kind==NAKI_RMSG_RELEASE)return 0;
    return __real_naki_rendition_send(fd,message);
}
static int video(void *opaque,const NakiVideoEncodedEvent *event,AVPacket *packet) {
    (void)opaque;(void)event;(void)packet;if(!atomic_load(&target))return 0;packets++;
    if(!strcmp(mode,"busy")&&packets==5){static atomic_uint spin;for(;;)atomic_fetch_add_explicit(&spin,1,memory_order_relaxed);}
    if(!strcmp(mode,"deadlock")&&packets==5){pthread_mutex_lock(&deadlock);pthread_mutex_lock(&deadlock);}
    if(!strcmp(mode,"hold_raw")&&held_count==NAKI_RENDITION_VIDEO_CREDIT){for(;;){for(unsigned i=0;i<held_count;i++)if(hash(held[i])!=held_hash[i])_exit(90);usleep(1000);}}
    return 0;
}
static void close_hook(void *opaque,const NakiMediaEnd *end){(void)opaque;(void)end;for(unsigned i=0;i<held_count;i++)av_buffer_unref(&held[i]);held_count=0;}
int naki_media_hooks_open(const NakiMediaJobInfo *info,NakiMediaHooks *hooks) {
    mode=getenv("NAKI_TEST_RENDITION_FAULT");if(!mode)mode="none";
    atomic_store(&target,info->output_id&&!strcmp(info->output_id,"bad"));
    *hooks=(NakiMediaHooks){.api_version=NAKI_MEDIA_HOOK_API,.video_packet=video,.close=close_hook};return 0;
}
