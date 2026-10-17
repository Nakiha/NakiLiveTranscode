#ifndef NAKI_BRANCH_QUEUE_H
#define NAKI_BRANCH_QUEUE_H
#include <pthread.h>
#include <stdint.h>
#include <libavcodec/avcodec.h>
#define NAKI_BRANCH_QUEUE_MAX 16
/* In-process ownership only, never serialized into worker IPC. Each item has
 * either a decoded frame or an audio packet + immutable parameter snapshot. */
typedef struct NakiBranchItem {
    AVFrame *frame;
    AVPacket *packet;
    AVCodecParameters *parameters;
    AVRational time_base;
    int64_t sequence,source_us,ingress_us;
    uint64_t charged_bytes,media_us;
} NakiBranchItem;
typedef struct NakiBranchQueue {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    NakiBranchItem items[NAKI_BRANCH_QUEUE_MAX];
    unsigned capacity,head,count,high_water;
    uint64_t byte_limit,bytes,high_bytes,media_us;
    int64_t media_limit_us,lag_limit_us;
    int eof,aborted;
} NakiBranchQueue;
void naki_branch_item_free(NakiBranchItem *item);
/* Conservative retained-buffer charge, including side data and opaque_ref.
 * Shared buffers may be charged more than once; this is not a process RSS cap. */
int naki_branch_item_charge(NakiBranchItem *item);
int naki_branch_queue_init(NakiBranchQueue *queue,unsigned capacity,uint64_t byte_limit,
    int64_t media_limit_us,int64_t lag_limit_us);
/* Nonblocking push transfers ownership only on success. Full -> EAGAIN;
 * single-item byte/media oversize -> EFBIG; cumulative media/bytes -> EAGAIN;
 * ingress age -> ETIMEDOUT. media_us is conservative retained data duration,
 * not last PTS minus first PTS: legal forward timestamp gaps cost no memory.
 * Caller supplies a positive conservative duration, including a bounded
 * codec/frame-rate fallback when the source duration is unavailable.
 * No drop/fail policy is selected here. */
int naki_branch_queue_push(NakiBranchQueue *queue,NakiBranchItem *item,int64_t now_us);
/* deadline is absolute CLOCK_MONOTONIC microseconds. 0 means poll. */
int naki_branch_queue_pop(NakiBranchQueue *queue,NakiBranchItem *item,int64_t deadline_us);
/* Periodic watchdog can detect retained input age even with no new arrivals. */
int naki_branch_queue_check(NakiBranchQueue *queue,int64_t now_us);
void naki_branch_queue_finish(NakiBranchQueue *queue);
/* Abort releases queued references and wakes consumers; owner I/O cancellation
 * and bounds on filter/encoder/mux buffers remain the caller's responsibility. */
void naki_branch_queue_abort(NakiBranchQueue *queue);
void naki_branch_queue_destroy(NakiBranchQueue *queue);
#endif
