#ifndef NAKI_SERVICE_RECORDS_H
#define NAKI_SERVICE_RECORDS_H
#include "worker/job_state.h"
#include <stdint.h>
#include <yyjson.h>
#include "control/operation_cache.h"
#define CREATE_RECORDS 32
typedef struct Creation {
    char operation_id[129],fingerprint[65],job_id[64],origin_request_id[129];
    NakiRecordState state;
    int accepted,outcome_known;
    int64_t retained_us;
    yyjson_doc *snapshot;
    char stop_operation_id[129],stop_origin_request_id[129];
    NakiRecordState stop_state;
    int stop_accepted;
    NakiOperationCache *operations,*sync_operations;
} Creation;
/* Event-loop owned, bounded retention. Active and pending records cannot expire. */
typedef struct CreationStore { Creation items[CREATE_RECORDS]; } CreationStore;
Creation *service_records_find_creation(CreationStore *store,const char *operation);
Creation *service_records_find_job(CreationStore *store,const char *id);
Creation *service_records_reserve_creation(CreationStore *store,Creation *active,Creation *pending);
int service_records_ensure_operation_cache(Creation *record);
void service_records_creation_clear(Creation *record);
#endif
