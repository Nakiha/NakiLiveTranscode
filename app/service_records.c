#include "service_records.h"
#include <string.h>

Creation *service_records_find_creation(CreationStore *store,const char *operation) {
    for(int i=0;i<CREATE_RECORDS;i++)if(!strcmp(store->items[i].operation_id,operation))return &store->items[i];
    return NULL;
}

Creation *service_records_find_job(CreationStore *store,const char *id) {
    for(int i=0;i<CREATE_RECORDS;i++)if(!strcmp(store->items[i].job_id,id))return &store->items[i];
    return NULL;
}

Creation *service_records_reserve_creation(CreationStore *store,Creation *active,Creation *pending) {
    int64_t now=now_us();
    for(int i=0;i<CREATE_RECORDS;i++) {
        Creation *r=&store->items[i];
        if(!*r->operation_id||(r!=active&&r!=pending&&r->retained_us&&now-r->retained_us>=1800000000)) {
            service_records_creation_clear(r);
            return r;
        }
    }
    return NULL;
}

int service_records_ensure_operation_cache(Creation *record) {
    if(!record->operations)record->operations=naki_operation_cache_open(record->job_id);
    return record->operations?0:-1;
}

void service_records_creation_clear(Creation *r) {
    if(r->snapshot)yyjson_doc_free(r->snapshot);
    naki_operation_cache_close(&r->operations);
    naki_operation_cache_close(&r->sync_operations);
    memset(r,0,sizeof(*r));
}
