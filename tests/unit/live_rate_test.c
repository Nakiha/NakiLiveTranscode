#include "media/live_rate.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    NakiLiveRate bytes={0},words={0};uint8_t y8[64*48];uint16_t y10[64*48];
    for(int frame=0;frame<3;frame++) {
        for(int i=0;i<64*48;i++) {y8[i]=(uint8_t)(i*29+frame*17);y10[i]=(uint16_t)(y8[i]<<2);}
        assert(naki_rate_complexity_samples(&bytes,y8,64,64,48,1)==naki_rate_complexity(&words,y10,64,64,48));
    }
    NakiLiveRate high={0},low={0},fixed={0};NakiRateDecision a,b,c;
    assert(!naki_rate_select(&high,32,1100000,1,5000000,1000000,2000000,33333,518400,1,1,&a));
    assert(!naki_rate_select(&low,32,1100000,1,200000,1000000,2000000,33333,518400,1,1,&b));
    assert(!naki_rate_select(&fixed,32,200000,0,0,1000000,2000000,33333,518400,1,1,&c));
    assert(a.effective_bps==1100000&&b.effective_bps==200000&&b.qp>a.qp&&b.qp==c.qp);
    assert(!naki_rate_feedback(&high,&a,518400,a.predicted_bits*2,0,1933334));
    assert(high.pending_bits==a.predicted_bits*2);
    assert(!naki_rate_written(&high,1933334,1010000,a.predicted_bits*2));
    assert(!high.pending_bits);
    assert(naki_rate_written(&high,1933334,1010000,1)<0);
    for(int i=1;i<2000;i++) {
        int64_t now=1000000+(int64_t)i*33333,pts=2000000+(int64_t)i*33333;
        assert(!naki_rate_select(&high,32,1100000,0,0,now,pts,33333,518400,i%60==0,1,&a));
        assert(a.qp>=0&&a.qp<=63);
        assert(!naki_rate_feedback(&high,&a,518400,a.predicted_bits,a.kind==3?0:a.kind,pts-66666));
        assert(!naki_rate_written(&high,pts-66666,now+10000,a.predicted_bits));
        assert(!high.pending_bits&&high.count<100);
    }
    NakiLiveRate waiting={0};
    assert(!naki_rate_select(&waiting,32,1100000,0,0,1000000,2000000,33333,518400,1,1,&a));
    int64_t first=waiting.pending_bits;
    assert(!naki_rate_select(&waiting,32,1100000,0,0,1000001,2033333,33333,518400,0,1,&b));
    assert(waiting.pending_bits==first+b.predicted_bits);
    assert(!naki_rate_feedback(&waiting,&b,518400,10000,1,1966667));
    assert(waiting.pending_bits==first+10000);
    /* A short group can turn a predicted B into a P. Learn from its actual
     * type while keeping the outstanding reservation paired to this input. */
    assert(waiting.coefficient[2]==.07&&waiting.coefficient[1]!=.20);
    NakiLiveRate refresh={0};
    assert(!naki_rate_select(&refresh,32,1100000,0,0,1000000,2000000,33333,518400,1,1,&a));
    assert(a.kind==3);
    assert(!naki_rate_feedback(&refresh,&a,518400,a.predicted_bits*3,0,1933334));
    assert(refresh.coefficient[0]==.40&&refresh.coefficient[3]>.8);
    assert(!naki_rate_written(&refresh,1933334,1001000,a.predicted_bits*3));
    assert(!naki_rate_select(&refresh,32,1100000,0,0,3000000,4000000,33333,518400,1,1,&b));
    assert(b.kind==0&&b.qp<a.qp&&b.predicted_bits<=b.allowance_bits);
    assert(b.allowance_bits<=1100000*.82*33333/1000000*6);
    assert(!naki_rate_feedback(&refresh,&b,518400,b.predicted_bits/2,0,3933334));
    assert(refresh.coefficient[0]<.40&&refresh.coefficient[3]>.8);
    assert(naki_rate_feedback(&refresh,&b,518400,1000,-1,4000000)<0);
    assert(naki_rate_feedback(&refresh,&b,518400,1000,0,3933334)<0);
    /* A far-ahead display input must not retire the still-live DTS window. */
    NakiLiveRate clock={0};
    assert(!naki_rate_select(&clock,32,1100000,0,0,1000000,2000000,33333,518400,1,1,&a));
    assert(!naki_rate_feedback(&clock,&a,518400,200000,0,1933000));
    assert(!naki_rate_written(&clock,1933000,1001000,200000));
    assert(!naki_rate_select(&clock,32,1100000,0,0,1100000,4000000,33333,518400,0,1,&b));
    assert(b.media_clock_us==1933000&&clock.count==1);
    assert(b.window_room_bits==1100000*.95-200000);
    NakiLiveRate share={0};
    assert(!naki_rate_select(&share,32,1100000,0,0,1000000,2000000,33333,518400,1,1,&a));
    assert(a.followup_reserve_bits>110000&&a.allowance_bits+a.followup_reserve_bits<=a.window_room_bits);
    /* After a refresh under tight credit the following reference anchor gets
     * priority over later P anchors, without borrowing from a future window. */
    assert(!naki_rate_feedback(&share,&a,518400,800000,0,1933334));
    assert(!naki_rate_written(&share,1933334,1000100,800000));
    share.frames_since_i=3;share.since_i=3;
    assert(!naki_rate_select(&share,32,1100000,0,0,1100000,2133333,33333,518400,0,1,&b));
    assert(b.kind==1&&b.allowance_bits<=b.window_room_bits&&b.allowance_bits>80000);
    puts("live rate: independent budgets, QP response, pending reservation replacement and bounded rolling history passed");
}
