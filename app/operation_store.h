#ifndef NAKI_OPERATION_STORE_H
#define NAKI_OPERATION_STORE_H
#include <stddef.h>
#include <stdint.h>
#include <yyjson.h>
typedef struct NakiOperations NakiOperations;
typedef uint64_t NakiOperationHandle;
typedef enum NakiOperationKind {NAKI_OP_CREATE,NAKI_OP_UPDATE,NAKI_OP_STOP} NakiOperationKind;
typedef enum NakiOperationState {
    NAKI_OP_ACCEPTED,NAKI_OP_PREPARING,NAKI_OP_PREPARED,NAKI_OP_APPLYING,NAKI_OP_APPLIED,
    NAKI_OP_FAILED,NAKI_OP_SUPERSEDED,NAKI_OP_CANCELLED,NAKI_OP_STOPPING,NAKI_OP_STOPPED
} NakiOperationState;
enum {NAKI_OP_PIN_DESIRED=1,NAKI_OP_PIN_EFFECTIVE=2,NAKI_OP_PIN_CLAIMED=4,NAKI_OP_PIN_OWNERSHIP=8};
typedef struct NakiFrameIdentity {
    uint64_t frame_id;
    int64_t source_pts,source_us,output_pts;
    int source_tb_num,source_tb_den,output_tb_num,output_tb_den;
} NakiFrameIdentity;
typedef struct NakiOperationIntent {
    NakiOperationKind kind;
    const char *job_id,*operation_id,*output_id,*fingerprint,*origin_request_id;
    uint64_t expected_revision,revision;
} NakiOperationIntent;
NakiOperations *naki_operations_open(unsigned update_capacity,size_t byte_limit,int64_t retention_us);
void naki_operations_close(NakiOperations **store);
/* 1=new reservation, 0=replay, negative errno=conflict/capacity/invalid.
 * Create/stop use two slots reserved independently of update saturation. */
int naki_operations_reserve(NakiOperations *store,const NakiOperationIntent *intent,int64_t now_us,NakiOperationHandle *handle);
int naki_operations_lookup(NakiOperations *store,const char *job_id,const char *operation_id,NakiOperationHandle *handle);
/* Snapshot an existing intent atomically; no reservation or other side effect. */
int naki_operations_replay(NakiOperations *store,const NakiOperationIntent *intent,yyjson_mut_doc **result);
int naki_operations_pin(NakiOperations *store,NakiOperationHandle handle,unsigned set,unsigned clear);
int naki_operations_transition(NakiOperations *store,NakiOperationHandle handle,NakiOperationState state,
                               int owner_decision,int error_code,const char *reason,int64_t now_us);
/* Commit applied checkpoint and optionally acquire exactly one frame lease atomically. */
int naki_operations_applied(NakiOperations *store,NakiOperationHandle handle,const NakiFrameIdentity *frame,int acquire_lease,int64_t now_us);
int naki_operations_frame_acquire(NakiOperations *store,NakiOperationHandle handle);
int naki_operations_frame_release(NakiOperations *store,NakiOperationHandle handle,int64_t now_us);
int naki_operations_written(NakiOperations *store,NakiOperationHandle handle,const NakiFrameIdentity *frame,int64_t written_us);
int naki_operations_written_once(NakiOperations *store,NakiOperationHandle handle,const NakiFrameIdentity *frame,int64_t written_us,int *first);
int naki_operations_retire(NakiOperations *store,NakiOperationHandle handle,int64_t now_us);
void naki_operations_cancel_unclaimed(NakiOperations *store,int64_t now_us);
yyjson_mut_doc *naki_operations_get(NakiOperations *store,NakiOperationHandle handle);
/* Bounded export index, stable while reservations are pinned by the caller's job lifecycle. */
yyjson_mut_doc *naki_operations_page(NakiOperations *store,unsigned cursor,unsigned maximum,unsigned *next);
size_t naki_operations_record_bytes(void);
#endif
