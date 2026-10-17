#ifndef NAKI_BRANCH_QUEUE_H
#define NAKI_BRANCH_QUEUE_H
#include <pthread.h>
#include <stdint.h>
#include <libavcodec/avcodec.h>
#define NAKI_BRANCH_VIDEO_MAX 16
#define NAKI_BRANCH_AUDIO_MAX 256
#define NAKI_BRANCH_QUEUE_MAX NAKI_BRANCH_VIDEO_MAX /* Legacy shared-cap init. */
#define NAKI_BRANCH_QUEUE_STORAGE_MAX (NAKI_BRANCH_VIDEO_MAX+NAKI_BRANCH_AUDIO_MAX)
enum NakiBranchReason {NAKI_BRANCH_NONE,NAKI_BRANCH_VIDEO_COUNT,NAKI_BRANCH_AUDIO_COUNT,NAKI_BRANCH_BYTES,NAKI_BRANCH_WORK,NAKI_BRANCH_LAG,NAKI_BRANCH_REASONS};
static inline const char *naki_branch_reason_name(unsigned reason) {
    static const char *names[]={"none","video_count","audio_count","bytes","work","lag"};
    return reason<NAKI_BRANCH_REASONS?names[reason]:"invalid";
}
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
    NakiBranchItem items[NAKI_BRANCH_QUEUE_STORAGE_MAX];
    unsigned capacity,head,count,high_water;
    unsigned video_limit,audio_limit,video_count,audio_count,video_high,audio_high,rejection_reason;
    uint64_t rejected[NAKI_BRANCH_REASONS];
    uint64_t byte_limit,bytes,high_bytes,media_us;
    int64_t media_limit_us,lag_limit_us;
    int64_t admission_deadline_us;
    uint64_t admission_retries,admission_timeouts;
    int eof,aborted;
} NakiBranchQueue;
void naki_branch_item_free(NakiBranchItem *item);
/* Conservative retained-buffer charge, including side data and opaque_ref.
 * Shared buffers may be charged more than once; this is not a process RSS cap. */
int naki_branch_item_charge(NakiBranchItem *item);
int naki_branch_queue_init(NakiBranchQueue *queue,unsigned capacity,uint64_t byte_limit,
    int64_t media_limit_us,int64_t lag_limit_us);
/* Production mixed FIFO: separate type quotas and matching total storage. */
int naki_branch_queue_init_limits(NakiBranchQueue *queue,unsigned video_items,unsigned audio_packets,uint64_t byte_limit,
    int64_t media_limit_us,int64_t lag_limit_us);
/* Nonblocking push transfers ownership only on success. Full -> EAGAIN;
 * single-item byte/media oversize -> EFBIG; cumulative media/bytes -> EAGAIN;
 * ingress age -> ETIMEDOUT. media_us is conservative retained data duration,
 * not last PTS minus first PTS: legal forward timestamp gaps cost no memory.
 * Caller supplies a positive conservative duration, including a bounded
 * codec/frame-rate fallback when the source duration is unavailable.
 * No drop/fail policy is selected here. */
int naki_branch_queue_push(NakiBranchQueue *queue,NakiBranchItem *item,int64_t now_us);
/* Same hard bounds/ownership as push. Transient fullness starts one bounded
 * pressure window; a successful single push does not reset it. Only a timely
 * consumer drain strictly below all half-water marks recovers admission;
 * capacity 1 or 2 must empty. The caller may retain just this current item
 * outside the queue, within the same single-item byte/media limits. */
int naki_branch_queue_admit(NakiBranchQueue *queue,NakiBranchItem *item,int64_t now_us,int64_t grace_us);
typedef struct NakiBranchOffer {
    NakiBranchQueue *queue;
    NakiBranchItem *item;
    int result; /* EAGAIN = pending, 0 = transferred, other <0 = caller-owned. */
} NakiBranchOffer;
/* Offer to every healthy branch before waiting; at most one caller-owned item
 * per branch, no extra queue. Common retry loop checks cancellation/watchdogs
 * without locks, and never waits serial grace periods for individual branches.
 * Later items for healthy branches can wait for this common pressure window. */
int naki_branch_admit_many(NakiBranchOffer *offers,unsigned count,int64_t grace_us,
    int (*check)(void *),void *opaque);
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
