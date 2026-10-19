#ifndef NAKI_UNIFIED_RATE_H
#define NAKI_UNIFIED_RATE_H
#include "live_rate.h"
#define NAKI_UNIFIED_SLOTS 256
#define NAKI_UNIFIED_SOURCES 1024
enum { NAKI_U_FREE,NAKI_U_OPEN,NAKI_U_PLAN,NAKI_U_TRY,NAKI_U_ACCEPT,NAKI_U_ACTUAL };
typedef struct NakiUnifiedItem {
    uint64_t seq,version,consulted;
    int state,kind;
    int64_t pts,filtered_us,quota,bits;
    NakiRateDecision decision;
} NakiUnifiedItem;
typedef struct NakiUnifiedSource {int64_t published,complete,bits;} NakiUnifiedSource;
typedef struct NakiUnifiedRate {
    NakiLiveRate *model;
    NakiUnifiedItem item[NAKI_UNIFIED_SLOTS];
    struct {int64_t start,end,bits;int phase;} source[NAKI_UNIFIED_SOURCES];
    int source_count,source_limit,wrote,cap,pixels,crf;
    int64_t config_bits;
    int64_t period,clock,source_rate,source_credit,absolute_credit;
    uint64_t admitted,version,accepted,feedback,written,margin_uses;
    int active_peak,stage_peak,native_peak,future_peak,margin_used;
    int64_t C,D,real_room,soft_room,minimum;
    const char *reason;
} NakiUnifiedRate;
void naki_unified_init(NakiUnifiedRate *,NakiLiveRate *,int,int,int,int,int64_t,int64_t);
NakiUnifiedItem *naki_unified_find(NakiUnifiedRate *,uint64_t);
int naki_unified_source(NakiUnifiedRate *,const NakiUnifiedSource *);
int naki_unified_clock(NakiUnifiedRate *,int64_t);
int naki_unified_admit(NakiUnifiedRate *,int64_t,int64_t,double,uint64_t *);
int naki_unified_plan(NakiUnifiedRate *,const uint64_t *,const int *,int);
int naki_unified_try(NakiUnifiedRate *,uint64_t);
int naki_unified_accept(NakiUnifiedRate *,uint64_t);
int naki_unified_actual(NakiUnifiedRate *,uint64_t,int64_t,int,int64_t);
int naki_unified_can_write(NakiUnifiedRate *,uint64_t,int64_t);
int naki_unified_written(NakiUnifiedRate *,uint64_t,int64_t,int64_t);
int naki_unified_cancel(NakiUnifiedRate *);
int naki_unified_check(NakiUnifiedRate *);
#endif
