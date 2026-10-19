#include "media/rate_audit.h"
#include <assert.h>
#include <float.h>
#include <stdio.h>

typedef struct Fixture {
    NakiLiveRate rate;
    NakiRateAudit audit;
    NakiRateDecision decisions[5];
    NakiRateAuditFrame frames[5];
} Fixture;
static void setup(Fixture *f,int length) {
    memset(f,0,sizeof(*f));f->audit.enabled=1;f->audit.reserve_three=1;
    for(int i=0;i<=length;i++) {
        int64_t before=f->rate.pending_bits;
        assert(!naki_rate_select(&f->rate,32,1100000,0,0,1000000+i*10,100000+i*33333,33333,518400,!i,1,&f->decisions[i]));
        /* Source IDs deliberately run backwards; identity is seq + PTS. */
        naki_rate_audit_select(&f->audit,&f->frames[i],&f->rate,&f->decisions[i],100000+i*33333,1000-i*2,9,before);
        naki_rate_audit_accept(&f->audit,&f->frames[i],!i);
        if(!i) {
            before=f->rate.pending_bits;double old=f->rate.coefficient[3];
            assert(!naki_rate_feedback(&f->rate,&f->decisions[0],518400,905000,0,0));
            naki_rate_audit_feedback(&f->audit,&f->frames[0],100000,before,f->rate.pending_bits,905000,0,1000001,old,f->rate.coefficient[3]);
            before=f->rate.pending_bits;assert(!naki_rate_written(&f->rate,0,1000002,905000));
            naki_rate_audit_written(&f->audit,&f->frames[0],before,f->rate.pending_bits,905000);
        }
    }
    assert(!f->audit.errors);
}
static int shadow(Fixture *f,int length) {
    return naki_rate_audit_boundary(&f->audit,&f->frames[length],&f->decisions[length],&f->rate,518400,1000100);
}
static int upgrade(Fixture *f,int explicit_i,int group) {
    return naki_rate_reserve_three(&f->audit,&f->frames[3],&f->decisions[3],&f->rate,explicit_i,group);
}
static void assert_no_control_change(Fixture *f,int explicit_i,int group,int expected) {
    NakiLiveRate saved=f->rate;NakiRateDecision decisions[5];memcpy(decisions,f->decisions,sizeof(decisions));
    int result=upgrade(f,explicit_i,group);assert(result==expected);
    assert(!memcmp(&saved,&f->rate,sizeof(saved))&&!memcmp(decisions,f->decisions,sizeof(decisions)));
}
static void skip_cases(void) {
    Fixture f;
    for(int length=1;length<=4;length++)if(length!=3) {
        setup(&f,length);assert(shadow(&f,length)==(length==4?NAKI_RATE_BOUNDARY_FULL:NAKI_RATE_BOUNDARY_TARGET));
        NakiLiveRate saved=f.rate;NakiRateDecision d=f.decisions[length];
        assert(!naki_rate_reserve_three(&f.audit,&f.frames[length],&f.decisions[length],&f.rate,1,4));
        assert(!memcmp(&saved,&f.rate,sizeof(saved))&&!memcmp(&d,&f.decisions[length],sizeof(d)));
    }
    for(int which=0;which<12;which++) {
        setup(&f,3);assert(shadow(&f,3)==NAKI_RATE_BOUNDARY_TARGET);
        NakiRateAuditEntry *e=naki_rate_audit_entry(&f.audit,f.frames[3].seq);
        if(which==0)f.audit.reserve_three=0;
        if(which==1)f.audit.enabled=0;
        if(which==2)e->phase=1; /* selected but send not accepted */
        if(which==3)e->phase=3;
        if(which==4)f.audit.last_accepted_pts++;
        if(which==5)f.frames[3].seq++;
        if(which==6)f.audit.eof_event=99;
        if(which==7)f.frames[3].eof_tail_event=99;
        if(which==8)f.audit.errors=1;
        if(which==9)f.audit.last_accepted_phase=3;
        if(which==10)e->requested_qp++;
        if(which==11)f.frames[3].boundary_i_seq++;
        assert_no_control_change(&f,1,4,0);
    }
    setup(&f,3);assert(shadow(&f,3)==1);assert_no_control_change(&f,0,4,0);assert_no_control_change(&f,1,8,0);
    setup(&f,3);assert(naki_rate_audit_boundary(&f.audit,NULL,NULL,&f.rate,518400,1000100)<0);
    assert_no_control_change(&f,1,4,0); /* unknown identity */
}
static void numeric_cases(void) {
    Fixture f;
    const double bad[]={NAN,INFINITY,DBL_MAX};
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++) {
        setup(&f,3);f.rate.coefficient[1]=bad[i];NakiLiveRate saved=f.rate;
        assert(shadow(&f,3)<0&&!memcmp(&saved,&f.rate,sizeof(saved)));
        assert_no_control_change(&f,1,4,0);
        setup(&f,3);f.decisions[3].complexity=bad[i];saved=f.rate;
        assert(shadow(&f,3)<0&&!memcmp(&saved,&f.rate,sizeof(saved)));
    }
    for(int which=0;which<5;which++) {
        setup(&f,3);assert(shadow(&f,3)==1);
        if(which==0)f.rate.pending_bits=INT64_MAX;
        if(which==1)f.frames[3].boundary_shadow_bits=1000000001;
        if(which==2)f.frames[3].boundary_shadow_bits=0;
        if(which==3)f.rate.pending_bits=f.decisions[3].reserved_bits-1;
        if(which==4)f.audit.event=INT64_MAX;
        assert_no_control_change(&f,1,4,-1);assert(f.audit.errors==1&&!f.frames[3].upgrade_event);
    }
    setup(&f,3);NakiRateDecision *d=&f.decisions[3];
    NakiLiveRate saved=f.rate;NakiRateDecision original=*d;
    assert(naki_rate_upgrade_reserved(&f.rate,d,d->reserved_bits)==0);
    assert(naki_rate_upgrade_reserved(&f.rate,d,d->reserved_bits-1)==0);
    assert(!memcmp(&saved,&f.rate,sizeof(saved))&&!memcmp(&original,d,sizeof(original)));
}
static void transaction(int send_fails) {
    Fixture f;setup(&f,3);assert(shadow(&f,3)==1);
    NakiRateDecision untouched[2]={f.decisions[1],f.decisions[2]};
    NakiRateAuditFrame untouched_audit[2]={f.frames[1],f.frames[2]};
    NakiLiveRate control=f.rate;NakiRateDecision control_i,input_i;
    assert(!naki_rate_select(&control,32,1100000,0,0,1000200,233332,33333,518400,1,1,&control_i));
    int64_t old_pending=f.rate.pending_bits,pred=f.decisions[3].predicted_bits;
    assert(upgrade(&f,1,4)==1);
    int64_t delta=f.decisions[3].reserved_bits-pred;assert(delta>0&&f.rate.pending_bits==old_pending+delta);
    assert(f.decisions[3].predicted_bits==pred&&f.frames[3].upgrade_new_bits==f.decisions[3].reserved_bits);
    NakiRateAuditEntry *target=naki_rate_audit_entry(&f.audit,f.frames[3].seq);
    assert(target->bits==target->reserved_bits&&target->reserved_bits==f.decisions[3].reserved_bits);
    assert(target->predicted_bits==pred&&target->snapshot.upgrade_event==f.frames[3].upgrade_event);
    uint64_t event=f.audit.event;
    assert(upgrade(&f,1,4)==0&&f.audit.event==event&&f.rate.pending_bits==old_pending+delta);
    assert(!memcmp(untouched,&f.decisions[1],sizeof(untouched))&&!memcmp(untouched_audit,&f.frames[1],sizeof(untouched_audit)));
    int64_t before_i=f.rate.pending_bits;
    assert(!naki_rate_select(&f.rate,32,1100000,0,0,1000200,233332,33333,518400,1,1,&input_i));
    assert(control_i.window_room_bits-input_i.window_room_bits==delta);
    assert(input_i.allowance_bits<control_i.allowance_bits&&input_i.qp>control_i.qp); /* effective before I select */
    naki_rate_audit_select(&f.audit,&f.frames[4],&f.rate,&input_i,233332,1,9,before_i);
    assert(f.frames[3].boundary_event<f.frames[3].upgrade_event&&f.frames[3].upgrade_event<f.frames[4].reserve_event);
    int64_t after_i=f.rate.pending_bits;
    /* Retry the same selected I twice without selecting/accepting it again. */
    for(int retry=0;retry<2;retry++) {
        f.frames[4].send_eagain++;assert(!upgrade(&f,1,4));
        assert(f.rate.pending_bits==after_i&&f.audit.selected==5&&f.audit.accepted==4);
    }
    if(send_fails) {
        NakiRateAuditEntry *failed=naki_rate_audit_entry(&f.audit,f.frames[4].seq);
        assert(failed->phase==1&&!f.frames[4].accept_event&&target->phase==2);
        int64_t sum=0;for(int i=0;i<NAKI_RATE_AUDIT_CAPACITY;i++)if(f.audit.entries[i].seq)sum+=f.audit.entries[i].bits;
        assert(sum==f.rate.pending_bits&&!f.audit.errors);return;
    }
    naki_rate_audit_accept(&f.audit,&f.frames[4],1);
    int64_t actual=f.decisions[3].reserved_bits/2,before=f.rate.pending_bits;
    double coefficient=f.rate.coefficient[1];
    assert(!naki_rate_feedback(&f.rate,&f.decisions[3],518400,actual,1,1));
    assert(f.rate.pending_bits==before+actual-f.decisions[3].reserved_bits);
    naki_rate_audit_feedback(&f.audit,&f.frames[3],f.frames[3].encoder_pts,before,f.rate.pending_bits,actual,1,1000300,coefficient,f.rate.coefficient[1]);
    assert(f.frames[4].accept_event<f.frames[3].feedback_event);
    before=f.rate.pending_bits;assert(!naki_rate_written(&f.rate,1,1000301,actual));
    naki_rate_audit_written(&f.audit,&f.frames[3],before,f.rate.pending_bits,actual);
    assert(f.rate.pending_bits==old_pending-pred+input_i.reserved_bits&&!f.audit.errors);
    /* The controller check must use the raised reservation, not the forecast. */
    NakiLiveRate insufficient=f.rate;insufficient.pending_bits=pred;
    NakiLiveRate saved=insufficient;
    assert(naki_rate_feedback(&insufficient,&f.decisions[3],518400,actual,1,2)<0);
    assert(!memcmp(&saved,&insufficient,sizeof(saved)));
    insufficient=f.rate;insufficient.pending_bits=INT64_MAX;saved=insufficient;
    assert(naki_rate_feedback(&insufficient,&f.decisions[3],518400,1000000000,1,2)<0);
    assert(!memcmp(&saved,&insufficient,sizeof(saved)));
    printf("three-frame reservation: +%lld bits before I, I allowance %.1f -> %.1f, QP %d -> %d; exact replacement passed\n",
        (long long)delta,control_i.allowance_bits,input_i.allowance_bits,control_i.qp,input_i.qp);
}
int main(void) {
    skip_cases();numeric_cases();transaction(0);transaction(1);
    puts("rate reservation: restricted identity, unchanged non-targets, retry idempotence, failed send, finite/overflow and conservation passed");
}
