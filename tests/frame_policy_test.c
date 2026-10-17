#include "frame_policy.h"
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>

int main(void) {
    NakiTimeline t={0}; int64_t pts;
    assert(naki_timeline_resolve(&t,INT64_MIN,33,&pts)==-EINVAL);
    assert(!naki_timeline_resolve(&t,(INT64_C(1)<<33)-10,33,&pts));
    assert(!naki_timeline_resolve(&t,20,33,&pts));
    assert(pts==(INT64_C(1)<<33)+20 && t.wraps==1);
    assert(naki_timeline_resolve(&t,20,33,&pts)==-EINVAL);
    assert(naki_timeline_resolve(&t,19,33,&pts)==-EINVAL);
    assert(!naki_timeline_resolve(&t,INT64_C(900000000),33,&pts));
    assert(pts==(INT64_C(1)<<33)+900000000); /* Keep a large forward hole. */
    NakiPolicyConfig c={.intra=NAKI_INTRA_SOURCE_MAX,.interval_ms=3000}; NakiIntraState intra={0};
    assert(naki_intra_decide(&c,&intra,0,0)==NAKI_I_FIRST);
    naki_intra_accept(&c,&intra,0,NAKI_I_FIRST);
    assert(!naki_intra_decide(&c,&intra,1199999,0));
    assert(naki_intra_decide(&c,&intra,1200000,1)==NAKI_I_SOURCE);
    naki_intra_accept(&c,&intra,1200000,NAKI_I_SOURCE);
    assert(!naki_intra_decide(&c,&intra,4199999,0));
    assert(naki_intra_decide(&c,&intra,4200000,0)==NAKI_I_MAX);
    assert(naki_intra_decide(&c,&intra,6000000,1)==(NAKI_I_MAX|NAKI_I_SOURCE));
    /* Planning without encoder acceptance does not reset a deadline. */
    assert(intra.last_us==1200000);
    c.intra=NAKI_INTRA_GRID; c.interval_ms=2000; intra=(NakiIntraState){0};
    naki_intra_accept(&c,&intra,700000,NAKI_I_FIRST);
    assert(!naki_intra_decide(&c,&intra,1900000,1));
    assert(naki_intra_decide(&c,&intra,2700000,0)==NAKI_I_GRID);
    naki_intra_accept(&c,&intra,6700000,NAKI_I_GRID);
    assert(!naki_intra_decide(&c,&intra,6700001,0)); /* No burst after source holes. */
    c.intra=NAKI_INTRA_SOURCE;
    assert(!naki_intra_decide(&c,&intra,INT64_C(900000000),0));
    assert(naki_intra_decide(&c,&intra,INT64_C(900000000),1)==NAKI_I_SOURCE);

    c.overload_levels=2; c.levels[0]=(NakiOverloadLevel){100,20,2};
    c.levels[1]=(NakiOverloadLevel){200,20,3};
    c.recover_below_ms=50; c.recover_sustain_ms=30; c.max_degraded_ms=100;
    assert(naki_frame_keep(&c,1,1,0)==1 && naki_frame_keep(&c,1,2,0)==0);
    assert(naki_frame_keep(&c,2,2,1)==1 && naki_frame_keep(&c,0,2,0)==1);
    assert(naki_frame_keep(&c,2,3,0)==0 && naki_frame_keep(&c,2,4,0)==1);
    NakiOverloadState overload; naki_overload_init(&overload);
    naki_overload_tick(&c,&overload,0,150000); assert(!overload.level);
    naki_overload_tick(&c,&overload,19000,150000); assert(!overload.level);
    naki_overload_tick(&c,&overload,20000,150000); assert(overload.level==1);
    naki_overload_tick(&c,&overload,30000,250000);
    naki_overload_tick(&c,&overload,50000,250000); assert(overload.level==2 && overload.entered_us==20000);
    naki_overload_tick(&c,&overload,60000,0); assert(overload.recovering);
    naki_overload_tick(&c,&overload,70000,150000); assert(!overload.recovering);
    naki_overload_tick(&c,&overload,119000,150000); assert(!overload.failed);
    naki_overload_tick(&c,&overload,120000,150000); assert(overload.failed);
    naki_overload_init(&overload);
    naki_overload_tick(&c,&overload,0,150000);
    naki_overload_tick(&c,&overload,20000,150000);
    naki_overload_tick(&c,&overload,30000,0);
    naki_overload_tick(&c,&overload,60000,0);
    assert(!overload.level && overload.recoveries==1 && !overload.failed);
    /* Entered 20ms, deadline 120ms. Late polls compare recovery's planned
     * completion with that deadline; equality belongs to exhaustion. */
    const int64_t recovery_starts[]={110000,90000,89000};
    for(int i=0;i<3;i++) {
        naki_overload_init(&overload);
        naki_overload_tick(&c,&overload,0,150000);
        naki_overload_tick(&c,&overload,20000,150000);
        naki_overload_tick(&c,&overload,recovery_starts[i],0);
        naki_overload_tick(&c,&overload,140000,0);
        if(i<2)assert(overload.failed && overload.level==1 && !overload.recoveries);
        else assert(!overload.failed && !overload.level && overload.recoveries==1);
    }

    NakiInFlight active={0};
    assert(!naki_inflight_add(&active,NAKI_TRACK_PACKET,1,100));
    assert(naki_inflight_add(&active,NAKI_TRACK_PACKET,1,100)==-EINVAL);
    assert(!naki_inflight_add(&active,NAKI_TRACK_FRAME,1,200));
    assert(naki_inflight_age(&active,1000)==900);
    assert(naki_inflight_remove(&active,NAKI_TRACK_PACKET,1));
    assert(!naki_inflight_remove(&active,NAKI_TRACK_PACKET,1)); /* Old packet ref cannot retire a frame. */
    assert(naki_inflight_age(&active,1000)==800);
    for(int i=2;i<=NAKI_INFLIGHT_CAPACITY;i++)assert(!naki_inflight_add(&active,NAKI_TRACK_FRAME,i,200));
    assert(naki_inflight_add(&active,NAKI_TRACK_FRAME,NAKI_INFLIGHT_CAPACITY+1,200)==-ENOBUFS);
    puts("PASS strict timeline/wrap/holes, accepted I deadlines, overload hysteresis/exhaustion, bounded identities");
    return 0;
}
