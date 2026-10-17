#include "video_rate.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    NakiVideoRate r,a;const int rates[]={24,25,30,50,60};
    for(unsigned i=0;i<sizeof(rates)/sizeof(rates[0]);i++) {
        assert(!naki_video_rate_resolve((AVRational){rates[i],1},1,&r));
        assert(!r.from_fallback&&r.value.num==rates[i]&&r.value.den==1&&r.nominal_gop_frames==rates[i]*2);
        assert(r.nominal_frame_work_us==(1000000+rates[i]/2)/rates[i]);
        assert(!naki_video_rate_resolve((AVRational){rates[i]*1000,1000},240,&a));
        assert(av_cmp_q(r.value,a.value)==0&&r.nominal_gop_frames==a.nominal_gop_frames&&r.nominal_frame_work_us==a.nominal_frame_work_us);
    }
    assert(!naki_video_rate_resolve((AVRational){30000,1001},17,&r));
    assert(r.value.num==30000&&r.value.den==1001&&r.nominal_gop_frames==60&&r.nominal_frame_work_us==33367&&!r.from_fallback);
    assert(!naki_video_rate_resolve((AVRational){0,0},30,&r));assert(r.from_fallback&&r.value.num==30&&r.nominal_gop_frames==60&&r.nominal_frame_work_us==33333);
    assert(!naki_video_rate_resolve((AVRational){1,0},15,&r));assert(r.from_fallback&&r.nominal_gop_frames==30);
    assert(naki_video_rate_resolve((AVRational){0,1},0,&r)<0);
    assert(naki_video_rate_resolve((AVRational){INT_MAX,1},25,&r)<0);
    assert(!naki_video_rate_resolve((AVRational){1,10},25,&r));assert(r.nominal_gop_frames==1&&r.nominal_frame_work_us==10000000);
    puts("PASS rational effective/fallback rate, reduced fractions, GOP/work hints and overflow bounds");return 0;
}
