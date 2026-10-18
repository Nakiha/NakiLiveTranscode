#include "frame_policy.h"
#include <errno.h>
#include <limits.h>
#include <string.h>

int naki_timeline_resolve(NakiTimeline *s, int64_t raw, int bits, int64_t *pts) {
    if(raw==INT64_MIN)return -EINVAL;
    int64_t offset=s->offset;
    if(s->have_pts && bits>0 && bits<63) {
        int64_t period=INT64_C(1)<<bits;
        if(raw>=0 && raw<period && s->raw_last>=0 && s->raw_last<period &&
           raw<s->raw_last && s->raw_last-raw>period/2) {
            if(offset>INT64_MAX-period)return -ERANGE;
            offset+=period;
        }
    }
    if((offset>0 && raw>INT64_MAX-offset) || (offset<0 && raw<INT64_MIN-offset))return -ERANGE;
    int64_t value=raw+offset;
    if(s->have_pts && value<=s->last)return -EINVAL;
    if(offset!=s->offset)s->wraps++;
    s->have_pts=1; s->raw_last=raw; s->last=value; s->offset=offset; *pts=value;
    return 0;
}
static int64_t elapsed(int64_t later, int64_t earlier) {
    if(later<earlier)return -1;
    uint64_t delta=(uint64_t)later-(uint64_t)earlier;
    return delta>INT64_MAX ? INT64_MAX : (int64_t)delta;
}
int naki_intra_decide(const NakiPolicyConfig *c, const NakiIntraState *s, int64_t pts, int source_i) {
    if(c->intra==NAKI_INTRA_LEGACY)return 0;
    if(!s->have_pts)return NAKI_I_FIRST | ((source_i && c->intra!=NAKI_INTRA_GRID) ? NAKI_I_SOURCE : 0);
    int reason=source_i && c->intra!=NAKI_INTRA_GRID ? NAKI_I_SOURCE : 0;
    int64_t interval=(int64_t)c->interval_ms*1000;
    if(c->intra==NAKI_INTRA_GRID && interval>0 && elapsed(pts,s->anchor_us)/interval>s->grid)reason|=NAKI_I_GRID;
    if(c->intra==NAKI_INTRA_SOURCE_MAX && interval>0 && elapsed(pts,s->last_us)>=interval)reason|=NAKI_I_MAX;
    return reason;
}
void naki_intra_accept(const NakiPolicyConfig *c, NakiIntraState *s, int64_t pts, int reason) {
    if(!reason)return;
    if(!s->have_pts)s->anchor_us=pts;
    s->last_us=pts; s->have_pts=1;
    if(c->intra==NAKI_INTRA_GRID && c->interval_ms>0)s->grid=elapsed(pts,s->anchor_us)/((int64_t)c->interval_ms*1000);
}
void naki_overload_init(NakiOverloadState *s) {
    memset(s,0,sizeof(*s)); s->recover_us=-1;
    for(int i=0;i<NAKI_OVERLOAD_LEVELS;i++)s->candidate_us[i]=-1;
}
void naki_overload_tick(const NakiPolicyConfig *c, NakiOverloadState *s, int64_t now, int64_t age) {
    if(!c->overload_levels || s->failed)return;
    for(int i=0;i<c->overload_levels;i++) {
        if(age<(int64_t)c->levels[i].enter_ms*1000)s->candidate_us[i]=-1;
        else {
            if(s->candidate_us[i]<0)s->candidate_us[i]=now;
            if(i+1>s->level && elapsed(now,s->candidate_us[i])>=(int64_t)c->levels[i].sustain_ms*1000) {
                if(!s->level)s->entered_us=now;
                s->level=i+1; s->escalations++;
            }
        }
    }
    if(!s->level)return;
    if(age<(int64_t)c->recover_below_ms*1000) {
        if(s->recover_us<0)s->recover_us=now;
        s->recovering=1;
        int64_t recovery=(int64_t)c->recover_sustain_ms*1000;
        int64_t budget=(int64_t)c->max_degraded_ms*1000;
        /* A late poll may observe both events ready. Compare their scheduled
         * completion times; exhaustion wins a tie, and recovery cannot erase it. */
        if(elapsed(now,s->recover_us)>=recovery &&
           elapsed(s->recover_us,s->entered_us)<budget-recovery) {
            uint64_t escalations=s->escalations, recoveries=s->recoveries+1;
            naki_overload_init(s); s->escalations=escalations; s->recoveries=recoveries; return;
        }
    } else {s->recover_us=-1; s->recovering=0;}
    if(elapsed(now,s->entered_us)>=(int64_t)c->max_degraded_ms*1000)s->failed=1;
}
int naki_frame_keep(const NakiPolicyConfig *c, int level, uint64_t id, int protect) {
    if(!id || level<0 || level>c->overload_levels)return -EINVAL;
    return protect || !level || (id-1)%(unsigned)c->levels[level-1].keep_one_in==0;
}
int naki_inflight_add(NakiInFlight *s, int kind, uint64_t id, int64_t ingress) {
    for(int i=0;i<NAKI_INFLIGHT_CAPACITY;i++)if(s->entries[i].kind==kind && s->entries[i].id==id)return -EINVAL;
    for(int i=0;i<NAKI_INFLIGHT_CAPACITY;i++)if(!s->entries[i].kind) {
        s->entries[i]=(NakiInFlightEntry){kind,id,ingress}; return 0;
    }
    return -ENOBUFS;
}
int naki_inflight_remove(NakiInFlight *s, int kind, uint64_t id) {
    for(int i=0;i<NAKI_INFLIGHT_CAPACITY;i++)if(s->entries[i].kind==kind && s->entries[i].id==id) {
        s->entries[i]=(NakiInFlightEntry){0}; return 1;
    }
    return 0;
}
int64_t naki_inflight_age(const NakiInFlight *s, int64_t now) {
    int64_t oldest=now;
    for(int i=0;i<NAKI_INFLIGHT_CAPACITY;i++)if(s->entries[i].kind && s->entries[i].ingress_us<oldest)oldest=s->entries[i].ingress_us;
    return elapsed(now,oldest);
}
