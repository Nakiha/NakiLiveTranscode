#ifndef NAKI_DECODE_BACKEND_H
#define NAKI_DECODE_BACKEND_H
#include <libavcodec/avcodec.h>
#include <libavcodec/async.h>

/* Called once after avcodec_open2, before any input. The caller retains the
 * decoder on the synchronous path. async_create transfers it on success;
 * even an auto-enable failure leaves the async owner for ordinary cleanup. */
static inline int naki_decode_backend_init(AVCodecContext **decoder,
        AVCodecAsyncContext **async,int enabled,int automatic,int capacity,
        int minimum,int slots,void (*notify)(void *),void *opaque) {
    if(!enabled)return 0;
    int ret=avcodec_async_create(async,decoder,capacity,notify,opaque);
    if(ret<0||!automatic)return ret;
    AVCodecAsyncBudget *budget=NULL;
    ret=avcodec_async_budget_create(&budget,slots,NULL);
    if(ret>=0)ret=avcodec_async_enable_auto(*async,minimum,budget);
    avcodec_async_budget_free(&budget);
    return ret;
}
/* The same pipeline owner serializes either backend's frontend API calls.
 * Synchronous calls can wait for frame workers; no independent completion
 * notification or async auto-budget exists on that path. */
static inline int naki_decode_send(AVCodecContext *decoder,
        AVCodecAsyncContext *async,const AVPacket *packet) {
    return async?avcodec_async_send_packet(async,packet):avcodec_send_packet(decoder,packet);
}
static inline int naki_decode_receive(AVCodecContext *decoder,
        AVCodecAsyncContext *async,AVFrame *frame) {
    return async?avcodec_async_receive_frame(async,frame):avcodec_receive_frame(decoder,frame);
}
#endif
