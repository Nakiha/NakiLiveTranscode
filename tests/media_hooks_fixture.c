/* Test-only factory: no meter or SEI implementation; packet lifetime and guards. */
#include "media_hooks.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavutil/time.h>

typedef struct Context {
    char path[4096], mode[32];
    pthread_t owner;
    uint64_t audio, video, keys, known_types, reordered_pts;
    int64_t previous_pts, previous_encoded_us, first_encoded_us;
    AVPacket *held;
    uint64_t held_hash;
} Context;

static uint64_t hash(const AVPacket *packet) {
    uint64_t value=UINT64_C(14695981039346656037);
    for(int i=0;i<packet->size;i++)value=(value^packet->data[i])*UINT64_C(1099511628211);
    return value;
}
static int audio(void *opaque,const NakiAudioPacketEvent *event) {
    Context *c=opaque;
    assert(pthread_equal(c->owner,pthread_self()));
    assert(event->packet->size>0 && event->parameters->codec_type==AVMEDIA_TYPE_AUDIO);
    assert(event->input_stream_index==event->packet->stream_index);
    assert(event->time_base.num>0 && event->time_base.den>0);
    assert(event->demuxed_us>0 && event->demuxed_us<=event->dispatched_us);
    c->audio++;
    if(!c->held) {c->held=av_packet_clone(event->packet);assert(c->held);c->held_hash=hash(c->held);}
    return !strcmp(c->mode,"audio_error") ? AVERROR_EXTERNAL : 0;
}
static int video(void *opaque,const NakiVideoEncodedEvent *event,AVPacket *packet) {
    Context *c=opaque;
    assert(pthread_equal(c->owner,pthread_self()));
    assert(event->time_base.num>0 && event->time_base.den>0 && event->input_sequence>=0);
    assert(event->encoded_us>=c->previous_encoded_us && event->encoded_us<=av_gettime_relative());
    assert(event->input_picture_type>=AV_PICTURE_TYPE_I && event->input_picture_type<=AV_PICTURE_TYPE_B);
    assert(event->output_key==!!(packet->flags&AV_PKT_FLAG_KEY));
    if(!c->video)c->first_encoded_us=event->encoded_us;
    if(c->video && packet->pts<c->previous_pts)c->reordered_pts++;
    c->previous_pts=packet->pts;c->previous_encoded_us=event->encoded_us;
    c->video++;c->keys+=event->output_key;c->known_types+=event->output_picture_type!=AV_PICTURE_TYPE_NONE;
    if(!strcmp(c->mode,"video_error"))return AVERROR_EXTERNAL;
    if(!strcmp(c->mode,"rewrite_pts"))packet->pts++;
    if(!strcmp(c->mode,"growth"))return av_grow_packet(packet,NAKI_MEDIA_HOOK_MAX_ADDED_BYTES+1);
    /* Exercise the writable-payload extension point without adding metadata. */
    return av_packet_make_writable(packet);
}
static void close_hook(void *opaque,const NakiMediaEnd *event) {
    Context *c=opaque;
    assert(pthread_equal(c->owner,pthread_self()));
    if(c->held)assert(hash(c->held)==c->held_hash);
    FILE *report=fopen(c->path,"w");assert(report);
    fprintf(report,"{\"audio\":%llu,\"video\":%llu,\"keys\":%llu,\"known_types\":%llu,"
        "\"reordered_pts\":%llu,\"first_encoded_us\":%lld,\"last_encoded_us\":%lld,"
        "\"cancelled\":%d,\"error_code\":%d}\n",
        (unsigned long long)c->audio,(unsigned long long)c->video,(unsigned long long)c->keys,
        (unsigned long long)c->known_types,(unsigned long long)c->reordered_pts,
        (long long)c->first_encoded_us,(long long)c->previous_encoded_us,event->cancelled,event->error_code);
    assert(!fclose(report));av_packet_free(&c->held);free(c);
}
int naki_media_hooks_open(const NakiMediaJobInfo *info,NakiMediaHooks *hooks) {
    const char *directory=getenv("NAKI_TEST_MEDIA_HOOK_DIR"),*mode=getenv("NAKI_TEST_MEDIA_HOOK_MODE");
    if(!directory)return AVERROR(EINVAL);
    Context *c=calloc(1,sizeof(*c));if(!c)return AVERROR(ENOMEM);
    if(snprintf(c->path,sizeof(c->path),"%s/%s.json",directory,info->job_id)>=(int)sizeof(c->path)) {free(c);return AVERROR(ENAMETOOLONG);}
    snprintf(c->mode,sizeof(c->mode),"%s",mode ? mode : "valid");
    c->owner=pthread_self();
    *hooks=(NakiMediaHooks){.api_version=NAKI_MEDIA_HOOK_API,.opaque=c,.audio_packet=audio,.video_packet=video,.close=close_hook};
    return !strcmp(c->mode,"open_error") ? AVERROR_EXTERNAL : 0;
}
