#ifndef NAKI_RATE_AUDIT_H
#define NAKI_RATE_AUDIT_H
#include "live_rate.h"

/* Output-owner observer, with a separately opted-in three-picture reservation
 * candidate. No I/O, allocation or pointer transport. Both default off. */
#define NAKI_RATE_AUDIT_CAPACITY 256
enum {NAKI_RATE_BOUNDARY_UNKNOWN=-1,NAKI_RATE_BOUNDARY_EMPTY=0,NAKI_RATE_BOUNDARY_TARGET=1,
      NAKI_RATE_BOUNDARY_FULL=2,NAKI_RATE_BOUNDARY_FEEDBACK=3};
typedef struct NakiRateAuditFrame {
    uint64_t seq,reserve_event,accept_event,feedback_event,write_event;
    uint64_t boundary_event,boundary_i_seq,boundary_target_seq;
    uint64_t upgrade_event,upgrade_i_seq;
    int64_t encoder_pts,reserve_before,reserve_after,feedback_before,feedback_after,write_before,write_after;
    int64_t feedback_us,actual_bits,boundary_us,boundary_old_bits,boundary_shadow_bits;
    int64_t reserved_bits,upgrade_before,upgrade_after,upgrade_old_bits,upgrade_new_bits;
    double feedback_coefficient_before,feedback_coefficient_after;
    int frames_since_i,first_p,actual_kind,boundary_group_size,boundary_status,send_eagain;
    uint64_t eof_tail_event;
} NakiRateAuditFrame;
typedef struct NakiRateAuditEntry {
    uint64_t seq;
    int64_t pts,frame_id,bits;
    unsigned generation;
    int phase,requested_qp,predicted_kind;
    int64_t predicted_bits,reserved_bits;
    NakiRateAuditFrame snapshot;
    /* phase: 1 reserved, 2 accepted, 3 AU received; freed after write */
} NakiRateAuditEntry;
typedef struct NakiRateAudit {
    int initialized,enabled,reserve_three,errors,group_phase,last_accepted_intra,last_accepted_phase,eof_group_phase;
    uint64_t event,next_seq,selected,accepted,feedback,written,last_accepted_seq;
    int64_t last_accepted_pts;
    uint64_t eof_event,eof_target_seq;
    NakiRateAuditEntry entries[NAKI_RATE_AUDIT_CAPACITY];
} NakiRateAudit;
static inline NakiRateAuditEntry *naki_rate_audit_entry(NakiRateAudit *a,uint64_t seq) {
    if(!seq)return NULL;
    NakiRateAuditEntry *e=&a->entries[(seq-1)%NAKI_RATE_AUDIT_CAPACITY];
    return e->seq==seq?e:NULL;
}
static inline void naki_rate_audit_select(NakiRateAudit *a,NakiRateAuditFrame *f,const NakiLiveRate *r,
    const NakiRateDecision *d,int64_t pts,int64_t frame_id,unsigned generation,int64_t before) {
    if(!a->enabled)return;
    *f=(NakiRateAuditFrame){.seq=++a->next_seq,.reserve_event=++a->event,.encoder_pts=pts,
        .reserve_before=before,.reserve_after=r->pending_bits,.frames_since_i=r->frames_since_i,
        .first_p=d->kind==1&&r->frames_since_i==4,.actual_kind=-1,.reserved_bits=d->reserved_bits};
    a->selected++;
    NakiRateAuditEntry *e=&a->entries[(f->seq-1)%NAKI_RATE_AUDIT_CAPACITY];
    if(e->seq){a->errors++;return;}
    *e=(NakiRateAuditEntry){.seq=f->seq,.pts=pts,.frame_id=frame_id,.generation=generation,.bits=d->reserved_bits,.phase=1,
        .requested_qp=d->qp,.predicted_kind=d->kind,.predicted_bits=d->predicted_bits,.reserved_bits=d->reserved_bits,.snapshot=*f};
    if(r->pending_bits-before!=d->reserved_bits)a->errors++;
}
static inline void naki_rate_audit_accept(NakiRateAudit *a,NakiRateAuditFrame *f,int intra) {
    if(!a->enabled)return;
    NakiRateAuditEntry *e=naki_rate_audit_entry(a,f->seq);
    if(!e||e->phase!=1){a->errors++;return;}
    e->phase=2;f->accept_event=++a->event;a->accepted++;
    a->last_accepted_seq=f->seq;a->last_accepted_pts=f->encoder_pts;
    a->last_accepted_intra=intra;a->last_accepted_phase=2;
    a->group_phase=intra?0:(a->group_phase+1)%4;
    e->snapshot=*f;
}
static inline void naki_rate_audit_feedback(NakiRateAudit *a,NakiRateAuditFrame *f,int64_t pts,
    int64_t before,int64_t after,int64_t bits,int kind,int64_t now,double coefficient_before,double coefficient_after) {
    if(!a->enabled)return;
    NakiRateAuditEntry *e=naki_rate_audit_entry(a,f->seq);
    if(!e||e->phase!=2||e->pts!=pts||e->bits!=e->reserved_bits||e->reserved_bits!=f->reserved_bits){a->errors++;return;}
    if(after-before!=bits-e->reserved_bits)a->errors++;
    e->bits=bits;e->phase=3;f->feedback_event=++a->event;a->feedback++;
    if(f->seq==a->last_accepted_seq)a->last_accepted_phase=3;
    f->feedback_before=before;f->feedback_after=after;f->actual_bits=bits;f->actual_kind=kind;f->feedback_us=now;
    f->feedback_coefficient_before=coefficient_before;f->feedback_coefficient_after=coefficient_after;
    e->snapshot=*f;
}
static inline void naki_rate_audit_written(NakiRateAudit *a,NakiRateAuditFrame *f,int64_t before,int64_t after,int64_t bits) {
    if(!a->enabled)return;
    NakiRateAuditEntry *e=naki_rate_audit_entry(a,f->seq);
    if(!e||e->phase!=3||e->bits!=bits){a->errors++;return;}
    if(after-before!=-bits)a->errors++;
    *e=(NakiRateAuditEntry){0};f->write_event=++a->event;a->written++;
    if(f->seq==a->last_accepted_seq)a->last_accepted_phase=4;
    f->write_before=before;f->write_after=after;
}
/* Caller must first match the last accepted PTS to the still-unencoded ledger.
 * The explicit I is already known; its arrival closes a 1/2/3-picture group.
 * Results distinguish empty, target, complete group, already-feedback and
 * insufficient identity. EOF tail handling is separate from this I boundary. */
static inline int naki_rate_audit_boundary(NakiRateAudit *a,NakiRateAuditFrame *target,
    const NakiRateDecision *d,const NakiLiveRate *r,int pixels,int64_t now) {
    if(!a->enabled)return NAKI_RATE_BOUNDARY_EMPTY;
    if(!a->group_phase)return a->accepted&&!a->last_accepted_intra?NAKI_RATE_BOUNDARY_FULL:NAKI_RATE_BOUNDARY_EMPTY;
    if(a->last_accepted_phase>=3)return NAKI_RATE_BOUNDARY_FEEDBACK;
    NakiRateAuditEntry *e=target?naki_rate_audit_entry(a,target->seq):NULL;
    if(!e||e->phase!=2||e->seq!=a->last_accepted_seq||e->pts!=a->last_accepted_pts||d->kind!=2||target->boundary_i_seq) {
        a->errors++;return -1;
    }
    if(pixels<=0||!isfinite(d->complexity)||d->complexity<=0||d->qp<0||d->qp>63||
       !isfinite(r->coefficient[1])||r->coefficient[1]<=0||e->bits<=0) {a->errors++;return -1;}
    double estimate=ceil(fmax(1,r->coefficient[1]*pixels*d->complexity*exp2((32-d->qp)/6.0)));
    if(!isfinite(estimate)||estimate>1000000000){a->errors++;return -1;}
    target->boundary_event=++a->event;target->boundary_i_seq=a->next_seq+1;target->boundary_us=now;
    target->boundary_group_size=a->group_phase;target->boundary_old_bits=e->bits;
    target->boundary_shadow_bits=(int64_t)fmax(e->bits,estimate);
    e->snapshot=*target;
    return NAKI_RATE_BOUNDARY_TARGET;
}

/* Only a known explicit-I boundary in the fixed GOP4 fork may opt in. The
 * shadow must already name its exact phase2 last accepted picture. A repeated
 * send attempt cannot upgrade again, even if coefficients later change. */
static inline int naki_rate_reserve_three(NakiRateAudit *a,NakiRateAuditFrame *f,NakiRateDecision *d,
    NakiLiveRate *r,int explicit_i,int fixed_group_size) {
    if(!a->enabled||!a->reserve_three||a->errors||!explicit_i||fixed_group_size!=4||
       a->group_phase!=3||a->last_accepted_phase!=2||r->since_i!=3||a->eof_event||!f||!d)return 0;
    if(a->next_seq>=INT64_MAX||a->event>=INT64_MAX){a->errors++;return -1;}
    NakiRateAuditEntry *e=naki_rate_audit_entry(a,f->seq);
    if(!e||e->phase!=2||e->seq!=a->last_accepted_seq||e->pts!=a->last_accepted_pts||e->pts!=f->encoder_pts||
       d->kind!=2||e->predicted_kind!=d->kind||e->requested_qp!=d->qp||e->predicted_bits!=d->predicted_bits||
       !f->boundary_event||f->boundary_group_size!=3||f->boundary_i_seq!=a->next_seq+1||f->eof_tail_event)return 0;
    if(f->upgrade_event)return 0;
    if(e->bits!=d->reserved_bits||e->reserved_bits!=d->reserved_bits||f->reserved_bits!=d->reserved_bits||
       f->boundary_old_bits!=d->reserved_bits) {a->errors++;return -1;}
    int64_t before=r->pending_bits,old=d->reserved_bits;
    int ret=naki_rate_upgrade_reserved(r,d,f->boundary_shadow_bits);
    if(ret<0){a->errors++;return -1;}
    if(!ret)return 0;
    f->upgrade_event=++a->event;f->upgrade_i_seq=f->boundary_i_seq;
    f->upgrade_before=before;f->upgrade_after=r->pending_bits;f->upgrade_old_bits=old;f->upgrade_new_bits=d->reserved_bits;
    f->reserved_bits=d->reserved_bits;e->bits=d->reserved_bits;e->reserved_bits=d->reserved_bits;e->snapshot=*f;
    return 1;
}
#endif
