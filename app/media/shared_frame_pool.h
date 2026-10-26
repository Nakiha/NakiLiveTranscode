#ifndef NAKI_SHARED_FRAME_POOL_H
#define NAKI_SHARED_FRAME_POOL_H
#include <stddef.h>
#include <stdint.h>
#define NAKI_POOL_MEMBERS 4U
#define NAKI_POOL_MAX_SLOTS 1024U
/* All bookkeeping and the short mutex live only in the producer process.
 * Consumers receive a sealed read-only FD, never this object or a writable FD.
 * One producer reservation at a time. Offers/releases may run on parent threads.
 * The context lifetime itself must be serialized with close. */
typedef struct NakiFramePool NakiFramePool;
typedef struct NakiPoolToken {
    uint64_t pool_id, generation, bytes;
    uint32_t slot;
} NakiPoolToken;
typedef struct NakiPoolView {
    const uint8_t *mapping;
    size_t mapping_bytes, data_offset, slot_bytes;
    uint64_t pool_id;
    unsigned slots;
    uint64_t seen[NAKI_POOL_MAX_SLOTS]; /* Consumer-private duplicate/old descriptor guard. */
} NakiPoolView;
typedef struct NakiPoolStats {
    unsigned slots, occupied, producer_reserved;
    unsigned credits[NAKI_POOL_MEMBERS], retained[NAKI_POOL_MEMBERS];
    uint64_t mapped_bytes;
} NakiPoolStats;
int naki_pool_open(NakiFramePool **out,unsigned slots,size_t slot_bytes,uint64_t max_bytes);
int naki_pool_read_fd(NakiFramePool *pool); /* New CLOEXEC O_RDONLY FD; caller closes. */
uint64_t naki_pool_id(const NakiFramePool *pool);
size_t naki_pool_stride(const NakiFramePool *pool);
/* Membership reserves credit before media. Sum(credits)+1 <= slots; a stalled
 * member holding its entire allowance cannot exhaust the producer's reserve.
 * pidfd must already identify the authorized, independently exec'd holder. */
int naki_pool_join(NakiFramePool *pool,unsigned member,uint64_t instance,unsigned credit,int pidfd);
/* Same-process comparison backend only. The owner MUST join every local
 * consumer/codec thread and unmap its views before calling reap_local_joined.
 * These functions cannot admit or reclaim a process member. */
int naki_pool_join_local(NakiFramePool *pool,unsigned member,uint64_t instance,unsigned credit);
int naki_pool_reap_local_joined(NakiFramePool *pool,unsigned member,uint64_t instance);
int naki_pool_acquire(NakiFramePool *pool,NakiPoolToken *token,uint8_t **pixels);
int naki_pool_publish(NakiFramePool *pool,NakiPoolToken *token,size_t bytes);
int naki_pool_offer(NakiFramePool *pool,const NakiPoolToken *token,unsigned member,uint64_t instance);
int naki_pool_release(NakiFramePool *pool,const NakiPoolToken *token,unsigned member,uint64_t instance);
int naki_pool_producer_release(NakiFramePool *pool,const NakiPoolToken *token);
/* A timeout/disconnect is not an exit proof. Only the bound pidfd becoming
 * readable permits reclaiming outstanding leases and removing membership.
 * v1 holder contract: no descendants or FD/mapping transfer to another holder. */
int naki_pool_reap(NakiFramePool *pool,unsigned member,uint64_t instance);
void naki_pool_stats(NakiFramePool *pool,NakiPoolStats *stats);
int naki_pool_close(NakiFramePool **pool); /* EBUSY while any holder is authorized. */
int naki_pool_view_open(NakiPoolView *view,int fd,uint64_t expected_id);
const uint8_t *naki_pool_view_data(const NakiPoolView *view,const NakiPoolToken *token);
const uint8_t *naki_pool_view_claim(NakiPoolView *view,const NakiPoolToken *token);
void naki_pool_view_close(NakiPoolView *view);
#endif
