#include "media/rate_audit.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    NakiRateAudit disabled={0};NakiRateAuditFrame f={0};NakiLiveRate r={0};NakiRateDecision d;
    assert(!naki_rate_select(&r,32,1100000,0,0,1000000,0,33333,518400,1,1,&d));
    naki_rate_audit_select(&disabled,&f,&r,&d,0,1,0,0);
    assert(!disabled.next_seq&&!f.seq&&!disabled.errors);
    for(int length=1;length<=4;length++) {
        NakiRateAudit a={.enabled=1};NakiLiveRate rate={0};NakiRateDecision decisions[5];NakiRateAuditFrame frames[5]={{0}};
        int64_t before=0;
        for(int i=0;i<=length;i++) {
            before=rate.pending_bits;
            assert(!naki_rate_select(&rate,32,1100000,0,0,1000000+i,33333*i,33333,518400,!i,1,&decisions[i]));
            naki_rate_audit_select(&a,&frames[i],&rate,&decisions[i],33333*i,i,7,before);
            naki_rate_audit_accept(&a,&frames[i],!i);
        }
        before=rate.pending_bits;
        int result=naki_rate_audit_boundary(&a,&frames[length],&decisions[length],&rate,518400,1000100);
        assert(result==(length<4?NAKI_RATE_BOUNDARY_TARGET:NAKI_RATE_BOUNDARY_FULL));assert(rate.pending_bits==before);
        if(length<4) {
            assert(frames[length].boundary_i_seq==(uint64_t)length+2);
            assert(frames[length].boundary_group_size==length);
            assert(frames[length].boundary_event<++a.event);
            assert(frames[length].boundary_shadow_bits>=frames[length].boundary_old_bits);
            int errors=a.errors;
            assert(naki_rate_audit_boundary(&a,&frames[length],&decisions[length],&rate,518400,1000101)<0);
            assert(a.errors==errors+1&&rate.pending_bits==before);a.errors=errors;
        }
        /* Receive in coded order, not display order. A promoted P retains the
         * same immutable decision and requested QP, with actual replacement. */
        int order[5]={0,length,1,2,3};
        for(int n=0;n<=length;n++) {
            int i=order[n];int kind=!i?0:i==length?1:2;
            int target=!i?3:kind;double old=rate.coefficient[target];
            int64_t bits=decisions[i].predicted_bits+100,before_feedback=rate.pending_bits;
            assert(!naki_rate_feedback(&rate,&decisions[i],518400,bits,kind,n));
            naki_rate_audit_feedback(&a,&frames[i],33333*i,before_feedback,rate.pending_bits,bits,kind,1000200+n,old,rate.coefficient[target]);
            int64_t before_write=rate.pending_bits;
            assert(!naki_rate_written(&rate,n,1000300+n,bits));
            naki_rate_audit_written(&a,&frames[i],before_write,rate.pending_bits,bits);
            assert(frames[i].reserve_event<frames[i].accept_event&&frames[i].accept_event<frames[i].feedback_event&&frames[i].feedback_event<frames[i].write_event);
        }
        assert(!a.errors&&!rate.pending_bits&&a.selected==a.written&&a.written==a.feedback&&a.accepted==a.selected);
        naki_rate_audit_written(&a,&frames[0],0,0,1);assert(a.errors==1); /* duplicate fails the audit */
    }
    NakiRateAudit missing={.enabled=1,.group_phase=1};
    assert(naki_rate_audit_boundary(&missing,NULL,&d,&r,518400,1000100)<0&&missing.errors==1);
    missing.last_accepted_phase=3;
    assert(naki_rate_audit_boundary(&missing,NULL,NULL,&r,0,1000100)==NAKI_RATE_BOUNDARY_FEEDBACK&&missing.errors==1);
    /* A send failure leaves reserved phase 1, distinct from accepted phase 2. */
    NakiRateAudit failed={.enabled=1};NakiRateAuditFrame failed_frame={0};
    naki_rate_audit_select(&failed,&failed_frame,&r,&d,0,1,0,0);
    assert(naki_rate_audit_entry(&failed,1)->phase==1&&!failed.accepted&&!failed.last_accepted_seq);
    puts("rate audit: disabled observer, short/full groups, immutable identity, reorder, replacement and duplicate detection passed");
}
