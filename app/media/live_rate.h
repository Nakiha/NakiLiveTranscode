#ifndef NAKI_LIVE_RATE_H
#define NAKI_LIVE_RATE_H
#include <stdint.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

/* Experimental quality-first controller. Only its output owner mutates it.
 * No pacing, dropping or retrying a coded picture. Outstanding pictures reserve
 * predicted bits; actual AU bits replace the prediction before mux residence. */
#define NAKI_RATE_HISTORY 512
typedef struct NakiRateEntry { int64_t dts,wall,bits; } NakiRateEntry;
typedef struct NakiLiveRate {
    NakiRateEntry history[NAKI_RATE_HISTORY];
    int head,count,since_i,started,have_coded,frames_since_i;
    int64_t pending_bits,first_pts,last_coded_dts;
    /* Normal I / P / B / startup IDR have separate requested-QP models. */
    double coefficient[4];
    uint16_t previous[4096];
    int samples;
} NakiLiveRate;
typedef struct NakiRateDecision {
    int qp,kind,saturated;
    int64_t source_bps,effective_bps,predicted_bits,decision_us,media_clock_us;
    double complexity,quality_qp,allowance_bits,model_coefficient,window_room_bits,followup_reserve_bits;
} NakiRateDecision;
static inline double naki_rate_clip(double low,double high,double v) {return v<low?low:v>high?high:v;}
static inline double naki_rate_complexity_samples(NakiLiveRate *r,const void *samples,int stride,int width,int height,int byte_input) {
    const uint16_t *y=samples;const uint8_t *y8=samples;
    int step=16;
    while((width/step+1)*(height/step+1)>4096)step++;
    double spatial=0,motion=0;int n=0;
    for(int row=1;row<height;row+=step)for(int col=1;col<width;col+=step) {
        /* Keep the controller activity domain at 10-bit for either input. */
        int v=byte_input?(y8[row*stride+col]<<2):y[row*stride+col];
        int left=byte_input?(y8[row*stride+col-1]<<2):y[row*stride+col-1];
        int above=byte_input?(y8[(row-1)*stride+col]<<2):y[(row-1)*stride+col];
        spatial+=abs(v-left)+abs(v-above);
        if(n<r->samples)motion+=abs(v-r->previous[n]);
        r->previous[n++]=(uint16_t)v;
    }
    int have=r->samples==n;r->samples=n;
    return naki_rate_clip(.25,4,(16+spatial/fmax(1,n)*.5+(have?motion/fmax(1,n)*.25:0))/64);
}
static inline double naki_rate_complexity(NakiLiveRate *r,const uint16_t *y,int stride,int width,int height) {
    return naki_rate_complexity_samples(r,y,stride,width,height,0);
}
static inline int naki_rate_select(NakiLiveRate *r,int crf,int maxrate,int source_limit,int64_t source_bps,
    int64_t now,int64_t pts,int64_t period,int pixels,int intra,double complexity,NakiRateDecision *d) {
    if(period<=0||pixels<=0||source_bps<0||now<=0)return -1;
    int first=!r->started;
    if(first) {
        r->coefficient[0]=.8;r->coefficient[1]=.20;r->coefficient[2]=.07;
        /* A startup IDR has no temporal history. Normal TRAIL I should not
         * inherit its bootstrap estimate or its first-picture QPA offset. */
        r->coefficient[3]=r->coefficient[0];r->coefficient[0]=.40;
        r->first_pts=pts;r->started=1;
    }
    int64_t rate=maxrate>0?maxrate:100000000;
    if(source_limit&&source_bps<rate)rate=source_bps;
    int kind=intra?(first?3:0):((r->since_i+1)%4==0?1:2);
    if(intra)r->since_i=0;else r->since_i=(r->since_i+1)%4;
    if(intra)r->frames_since_i=0;else if(r->frames_since_i<8)r->frames_since_i++;
    /* Input display PTS is ahead of the next coded DTS with 3B. Advancing a
     * rolling cutoff with PTS can prematurely credit a still-live startup I.
     * Only a completed coded AU may advance this conservative media clock.
     * Encoded-but-not-written AUs remain reserved in pending_bits. */
    int64_t media=r->have_coded?r->last_coded_dts:r->first_pts-2*period,media_bits=0,wall_bits=0;
    while(r->count) {
        NakiRateEntry *e=&r->history[r->head];
        if(e->dts>media-1000000||e->wall>now-1000000)break;
        r->head=(r->head+1)%NAKI_RATE_HISTORY;r->count--;
    }
    for(int i=0;i<r->count;i++) {
        NakiRateEntry *e=&r->history[(r->head+i)%NAKI_RATE_HISTORY];
        if(e->dts>media-1000000)media_bits+=e->bits;
        if(e->wall>now-1000000)wall_bits+=e->bits;
    }
    double weight=intra?6:kind==1?3.2:.2;
    double target=fmax(1,rate*.82*period/1000000.0*weight);
    double room=fmin(rate*.95-media_bits,rate*.95-wall_bits)-r->pending_bits;
    /* Refresh and the next reference anchor share the available window.
     * Protect a nominal P+3B allowance before spending on I. This is a budget
     * forecast, not future-picture analysis or new token credit. A constrained
     * window may still force either picture above its quality anchor. */
    double followup=intra?fmax(0,fmin(room*.6,rate*.82*period/1000000.0*(3.2+3*.2))):0;
    double usable=intra?room-followup:room*((kind==1&&r->frames_since_i==4)?.85:.4);
    double allowance=fmax(1,fmin(target,fmax(rate*period/10000000.0,usable)));
    double quality=naki_rate_clip(0,63,crf+naki_rate_clip(-3,3,1.5*log2(complexity)));
    double at32=r->coefficient[kind]*pixels*complexity;
    double needed=32+6*log2(fmax(1,at32)/allowance);
    int qp=(int)ceil(naki_rate_clip(0,63,fmax(quality,needed)));
    int64_t predicted=(int64_t)ceil(fmax(1,at32*exp2((32-qp)/6.0)));
    if(predicted>1000000000||r->pending_bits>INT64_MAX-predicted)return -1;
    r->pending_bits+=predicted;
    *d=(NakiRateDecision){.qp=qp,.kind=kind,.saturated=needed>63,.source_bps=source_bps,
        .effective_bps=rate,.predicted_bits=predicted,.decision_us=now,.media_clock_us=media,.complexity=complexity,
        .quality_qp=quality,.allowance_bits=allowance,.model_coefficient=r->coefficient[kind],.window_room_bits=room,
        .followup_reserve_bits=followup};
    return 0;
}
static inline int naki_rate_feedback(NakiLiveRate *r,const NakiRateDecision *d,int pixels,int64_t bits,int actual_kind,int64_t dts) {
    if(bits<=0||bits>1000000000||pixels<=0||r->pending_bits<d->predicted_bits||actual_kind<0||actual_kind>2)return -1;
    if(r->have_coded&&dts<=r->last_coded_dts)return -1;
    int kind=actual_kind==0&&d->kind==3?3:actual_kind;
    double observed=bits*exp2((d->qp-32)/6.0)/(pixels*d->complexity);
    double old=r->coefficient[kind],alpha=kind==0?.5:.2;
    r->coefficient[kind]=old*(1-alpha)+naki_rate_clip(old*.25,old*4,observed)*alpha;
    r->pending_bits+=bits-d->predicted_bits;
    r->have_coded=1;r->last_coded_dts=dts;
    return 0;
}
static inline int naki_rate_written(NakiLiveRate *r,int64_t dts,int64_t wall,int64_t bits) {
    if(bits<=0||r->pending_bits<bits||r->count==NAKI_RATE_HISTORY)return -1;
    if(r->count) {
        NakiRateEntry *last=&r->history[(r->head+r->count-1)%NAKI_RATE_HISTORY];
        if(dts<=last->dts||wall<last->wall)return -1;
    }
    r->pending_bits-=bits;
    r->history[(r->head+r->count++)%NAKI_RATE_HISTORY]=(NakiRateEntry){dts,wall,bits};
    return 0;
}
#endif
