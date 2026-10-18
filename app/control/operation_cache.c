#include "operation_cache.h"
#include "identity.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#define CACHE_RECORDS 256
#define CACHE_RESULT_BYTES 6144
typedef struct CachedOperation {char id[129],fingerprint[65],json[CACHE_RESULT_BYTES];int64_t evict_after,observed_us;uint64_t result_version;int terminal;} CachedOperation;
struct NakiOperationCache {char job_id[64];CachedOperation records[CACHE_RECORDS];};
_Static_assert(sizeof(NakiOperationCache)<=2*1024*1024,"Controller operation cache exceeds per-job budget");
static CachedOperation *find(NakiOperationCache *cache,const char *id) {
    for(unsigned i=0;i<CACHE_RECORDS;i++)if(!strcmp(cache->records[i].id,id))return &cache->records[i];
    return NULL;
}
NakiOperationCache *naki_operation_cache_open(const char *job) {
    if(!naki_id_valid(job,63))return NULL;
    NakiOperationCache *cache=calloc(1,sizeof(*cache));if(cache)strcpy(cache->job_id,job);return cache;
}
void naki_operation_cache_close(NakiOperationCache **cache) {free(*cache);*cache=NULL;}
int naki_operation_cache_contains(NakiOperationCache *cache,const char *id) {return cache&&find(cache,id)!=NULL;}
int naki_operation_cache_reserve(NakiOperationCache *cache,const char *id,const char *fingerprint,int64_t now) {
    if(!cache||!naki_id_valid(id,128)||!fingerprint||strlen(fingerprint)!=64)return -EINVAL;
    CachedOperation *record=find(cache,id);
    if(record)return strcmp(record->fingerprint,fingerprint)?-EEXIST:0;
    for(unsigned i=0;i<CACHE_RECORDS;i++) {
        record=&cache->records[i];
        if(!*record->id||(record->evict_after&&now>=record->evict_after)) {
            memset(record,0,sizeof(*record));strcpy(record->id,id);strcpy(record->fingerprint,fingerprint);return 1;
        }
    }
    return -ENOSPC;
}
void naki_operation_cache_unreserve(NakiOperationCache *cache,const char *id) {
    CachedOperation *record=cache?find(cache,id):NULL;if(record&&!*record->json)memset(record,0,sizeof(*record));
}
int naki_operation_result_terminal(yyjson_val *record) {
    const char *state=yyjson_get_str(yyjson_obj_get(record,"state"));if(!state)return 0;
    if(!strcmp(state,"failed")||!strcmp(state,"superseded")||!strcmp(state,"cancelled")||!strcmp(state,"stopped"))return 1;
    const char *delivery=yyjson_get_str(yyjson_obj_get(yyjson_obj_get(record,"delivery"),"state"));
    return !strcmp(state,"applied")&&delivery&&(!strcmp(delivery,"first_written")||!strcmp(delivery,"not_written"));
}
int naki_operation_cache_put(NakiOperationCache *cache,yyjson_val *root,int64_t now) {
    const char *job=yyjson_get_str(yyjson_obj_get(root,"job_id")),*kind=yyjson_get_str(yyjson_obj_get(root,"kind"));
    if(!cache||!job||strcmp(job,cache->job_id)||!kind)return -EINVAL;
    if(strcmp(kind,"update"))return 0;
    const char *id=yyjson_get_str(yyjson_obj_get(root,"operation_id")),*fingerprint=yyjson_get_str(yyjson_obj_get(root,"intent_fingerprint"));
    if(!naki_id_valid(id,128)||!fingerprint||strlen(fingerprint)!=64)return -EINVAL;
    yyjson_val *version=yyjson_obj_get(root,"result_version");
    if(!yyjson_is_uint(version)||!yyjson_get_uint(version))return -EINVAL;
    CachedOperation *existing=find(cache,id);
    if(existing&&strcmp(existing->fingerprint,fingerprint))return -EEXIST;
    /* Transport order is not capture order: final history can precede an older
     * pending RPC. Keep the entire newest result, not selected terminal fields. */
    if(existing&&existing->result_version>=yyjson_get_uint(version))return 0;
    size_t bytes=0;char *text=yyjson_val_write(root,0,&bytes);
    if(!text)return -ENOMEM;
    if(bytes>=CACHE_RESULT_BYTES) {free(text);return -EFBIG;}
    int ret=naki_operation_cache_reserve(cache,id,fingerprint,now);if(ret<0) {free(text);return ret;}
    CachedOperation *record=find(cache,id);memcpy(record->json,text,bytes+1);free(text);record->result_version=yyjson_get_uint(version);record->terminal=naki_operation_result_terminal(root);record->observed_us=now;
    int64_t terminal=yyjson_get_sint(yyjson_obj_get(root,"terminal_us"));
    uint64_t pins=yyjson_get_uint(yyjson_obj_get(root,"retention_pins"));
    uint64_t references=yyjson_get_uint(yyjson_obj_get(yyjson_obj_get(root,"delivery"),"inflight_frame_leases"));
    record->evict_after=record->terminal&&!pins&&!references&&terminal?terminal+1800000000:0;
    return 0;
}
yyjson_doc *naki_operation_cache_newest(NakiOperationCache *cache,const char *id) {
    CachedOperation *record=cache?find(cache,id):NULL;
    return record&&*record->json?yyjson_read(record->json,strlen(record->json),0):NULL;
}
int naki_operation_cache_reconcile(NakiOperationCache *staging,NakiOperationCache *live) {
    if(!staging||!live||strcmp(staging->job_id,live->job_id))return -EINVAL;
    for(unsigned i=0;i<CACHE_RECORDS;i++) {
        CachedOperation *record=&live->records[i];
        if(!*record->json||!find(staging,record->id))continue;
        yyjson_doc *d=naki_operation_cache_newest(live,record->id);if(!d)return -ENOMEM;
        int result=naki_operation_cache_put(staging,yyjson_doc_get_root(d),record->observed_us);yyjson_doc_free(d);
        if(result)return result;
    }
    return 0;
}
yyjson_mut_doc *naki_operation_cache_get(NakiOperationCache *cache,const char *id) {
    CachedOperation *record=cache?find(cache,id):NULL;if(!record)return NULL;
    yyjson_doc *observed=*record->json?yyjson_read(record->json,strlen(record->json),0):NULL;
    if(record->terminal&&!observed)return NULL;
    yyjson_mut_doc *d=record->terminal&&observed?yyjson_doc_mut_copy(observed,NULL):yyjson_mut_doc_new(NULL);
    if(!d) {if(observed)yyjson_doc_free(observed);return NULL;}
    yyjson_mut_val *root=record->terminal?yyjson_mut_doc_get_root(d):yyjson_mut_obj(d);yyjson_mut_doc_set_root(d,root);
    if(!record->terminal) {
        yyjson_mut_obj_add_str(d,root,"kind","update");yyjson_mut_obj_add_strcpy(d,root,"job_id",cache->job_id);yyjson_mut_obj_add_strcpy(d,root,"operation_id",record->id);
        yyjson_mut_obj_add_str(d,root,"state","unknown");yyjson_mut_obj_add_bool(d,root,"outcome_known",0);
        yyjson_mut_obj_add_str(d,root,"reason","operation_outcome_unknown");
        if(observed) {
            yyjson_mut_obj_add_val(d,root,"last_observed",yyjson_val_mut_copy(d,yyjson_doc_get_root(observed)));
            yyjson_val *checkpoint=yyjson_obj_get(yyjson_doc_get_root(observed),"applied_checkpoint");
            if(checkpoint)yyjson_mut_obj_add_val(d,root,"applied_checkpoint",yyjson_val_mut_copy(d,checkpoint));
        } else yyjson_mut_obj_add_null(d,root,"last_observed");
    }
    yyjson_mut_obj_remove_key(root,"result_source");yyjson_mut_obj_add_str(d,root,"result_source","controller_history");
    yyjson_mut_obj_add_sint(d,root,"last_observed_received_monotonic_us",record->observed_us);
    if(observed)yyjson_doc_free(observed);
    return d;
}
