#ifndef NAKI_OPERATION_CACHE_H
#define NAKI_OPERATION_CACHE_H
#include <stdint.h>
#include <yyjson.h>
typedef struct NakiOperationCache NakiOperationCache;
NakiOperationCache *naki_operation_cache_open(const char *job_id);
void naki_operation_cache_close(NakiOperationCache **cache);
int naki_operation_cache_reserve(NakiOperationCache *cache,const char *operation_id,const char *fingerprint,int64_t now_us);
int naki_operation_cache_contains(NakiOperationCache *cache,const char *operation_id);
void naki_operation_cache_unreserve(NakiOperationCache *cache,const char *operation_id);
int naki_operation_cache_put(NakiOperationCache *cache,yyjson_val *record,int64_t now_us);
/* Greatest worker result; caller owns the document. Does not invent a live
 * outcome for a reservation that has never received a worker result. */
yyjson_doc *naki_operation_cache_newest(NakiOperationCache *cache,const char *operation_id);
/* Preserve newer live replies for IDs also present in a completed sync page
 * set, without resurrecting worker-evicted IDs or empty reservations. */
int naki_operation_cache_reconcile(NakiOperationCache *staging,NakiOperationCache *live);
yyjson_mut_doc *naki_operation_cache_get(NakiOperationCache *cache,const char *operation_id);
int naki_operation_result_terminal(yyjson_val *record);
#endif
