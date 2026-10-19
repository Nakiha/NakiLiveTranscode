#include "unified_rate.h"
#include <errno.h>
#include <limits.h>
#define BAD(u,why) ((u)->reason=(why),-EINVAL)
void naki_unified_init(NakiUnifiedRate *u,NakiLiveRate *m,int cap,int source_limit,int pixels,int crf,int64_t period,int64_t now) {
    memset(u,0,sizeof(*u));u->model=m;u->cap=cap;u->source_limit=source_limit;
    u->pixels=pixels;u->crf=crf;u->period=period;u->clock=now;
    u->absolute_credit=(int64_t)cap*1000000;u->reason="ok";
    m->coefficient[0]=.4;m->coefficient[1]=.2;m->coefficient[2]=.07;m->coefficient[3]=.8;
}
NakiUnifiedItem *naki_unified_find(NakiUnifiedRate *u,uint64_t seq) {
    for(int i=0;i<NAKI_UNIFIED_SLOTS;i++)if(u->item[i].state&&u->item[i].seq==seq)return &u->item[i];
    return NULL;
}
int naki_unified_check(NakiUnifiedRate *u) {
    int stage=0,native=0;int64_t C=0,D=0;
    for(int i=0;i<NAKI_UNIFIED_SLOTS;i++) {
        NakiUnifiedItem *f=&u->item[i];
        if(f->state>=NAKI_U_OPEN&&f->state<=NAKI_U_TRY)stage++;
        if(f->state==NAKI_U_ACCEPT)native++;
        if(f->state==NAKI_U_PLAN||f->state==NAKI_U_TRY)C+=f->quota;
        if(f->state==NAKI_U_ACCEPT||f->state==NAKI_U_ACTUAL)D+=f->bits;
    }
    if(stage+native>4||C<0||D<0||C!=u->C||D!=u->D||C+D!=u->model->pending_bits)return BAD(u,"ledger_or_admission_gate");
    if(stage+native>u->active_peak)u->active_peak=stage+native;
    if(stage>u->stage_peak)u->stage_peak=stage;
    if(native>u->native_peak)u->native_peak=native;
    return 0;
}
int naki_unified_source(NakiUnifiedRate *u,const NakiUnifiedSource *s) {
    if(s->bits<=0||s->bits>1000000000||s->complete<=0||s->published<s->complete||s->complete>INT64_MAX-1000000)return BAD(u,"source_domain");
    int64_t start=s->published>u->clock?s->published:u->clock,end=s->complete+1000000;
    if(end<=start)return 0; /* Delayed publication earns no retroactive credit. */
    if(u->source_count==NAKI_UNIFIED_SOURCES)return BAD(u,"source_journal_full");
    int i=u->source_count++;u->source[i].start=start;u->source[i].end=end;u->source[i].bits=s->bits;u->source[i].phase=0;
    return 0;
}
static int accrue(NakiUnifiedRate *u,int64_t to) {
    int64_t elapsed=to-u->clock,limit=(int64_t)u->cap*1000000;
    if(elapsed<0||u->source_rate<0)return BAD(u,"clock_or_source_order");
    int64_t *credits[2]={&u->source_credit,&u->absolute_credit},rates[2]={u->source_rate,u->cap};
    for(int i=0;i<2;i++)if(rates[i]&&elapsed) {
        int64_t room=limit-*credits[i];
        if(room<0)return BAD(u,"credit_above_burst");
        if(elapsed>=room/rates[i]+!!(room%rates[i]))*credits[i]=limit;
        else *credits[i]+=elapsed*rates[i];
    }
    u->clock=to;return 0;
}
int naki_unified_clock(NakiUnifiedRate *u,int64_t now) {
    if(now<u->clock)return BAD(u,"clock_rewind");
    for(int rounds=0;rounds<=2*NAKI_UNIFIED_SOURCES;rounds++) {
        int64_t next=now;int found=0;
        for(int i=0;i<u->source_count;i++) {
            int64_t when=u->source[i].phase?u->source[i].end:u->source[i].start;
            if(when<=next) {next=when;found=1;}
        }
        if(accrue(u,next)<0)return -EINVAL;
        if(!found)return 0;
        for(int i=0;i<u->source_count;) {
            if(!u->source[i].phase&&u->source[i].start==next) {
                if(u->source_rate>INT64_MAX-u->source[i].bits)return BAD(u,"source_rate_overflow");
                u->source_rate+=u->source[i].bits;u->source[i].phase=1;
            }
            if(u->source[i].phase&&u->source[i].end==next) {
                u->source_rate-=u->source[i].bits;u->source[i]=u->source[--u->source_count];continue;
            }
            i++;
        }
    }
    return BAD(u,"source_event_bound");
}
int naki_unified_admit(NakiUnifiedRate *u,int64_t pts,int64_t filtered,double activity,uint64_t *seq) {
    if(!isfinite(activity)||activity<.25||activity>4||pts<0||filtered<=0||u->admitted==UINT64_MAX)return BAD(u,"admission_domain");
    int active=0,slot=-1;
    for(int i=0;i<NAKI_UNIFIED_SLOTS;i++) {int state=u->item[i].state;if(state&&state<=NAKI_U_ACCEPT)active++;if(!state)slot=i;}
    if(active>=4||slot<0)return BAD(u,"no_fifth_frame");
    NakiUnifiedItem *f=&u->item[slot];*f=(NakiUnifiedItem){.seq=++u->admitted,.state=NAKI_U_OPEN,.pts=pts,.filtered_us=filtered};
    f->decision.complexity=activity;*seq=f->seq;
    if(!u->model->started) {u->model->started=1;u->model->first_pts=pts;}
    return naki_unified_check(u);
}
static int64_t history_bits(NakiUnifiedRate *u,int media,int64_t when) {
    NakiLiveRate *r=u->model;int64_t sum=0;
    for(int i=0;i<r->count;i++) {NakiRateEntry *e=&r->history[(r->head+i)%NAKI_RATE_HISTORY];if((media?e->dts:e->wall)>when-1000000)sum+=e->bits;}
    return sum;
}
static int64_t source_room(NakiUnifiedRate *u) {
    if(!u->source_limit)return u->cap;
    return u->wrote?u->source_credit/1000000:(u->source_rate<u->cap?u->source_rate:u->cap);
}
int naki_unified_plan(NakiUnifiedRate *u,const uint64_t *seq,const int *kind,int count) {
    u->reason="ok";
    if(count<1||count>4||u->cap<=0||u->cap>100000000||u->pixels<=0||u->period<=0)return BAD(u,"plan_domain");
    NakiLiveRate *r=u->model;double minimum[4],desired[4],ideal[4],frac[4];int64_t quota[4];
    NakiUnifiedItem *item[4];int64_t previous=0,mins=0;
    int64_t media=r->have_coded?r->last_coded_dts:r->first_pts-2*u->period;
    while(r->count) {NakiRateEntry *e=&r->history[r->head];if(e->dts>media-1000000||e->wall>u->clock-1000000)break;r->head=(r->head+1)%NAKI_RATE_HISTORY;r->count--;}
    for(int i=0;i<count;i++) {
        item[i]=naki_unified_find(u,seq[i]);
        if(!item[i]||(item[i]->state!=NAKI_U_OPEN&&item[i]->state!=NAKI_U_PLAN)||kind[i]<0||kind[i]>3)return BAD(u,"plan_state");
        for(int j=0;j<i;j++)if(seq[i]==seq[j])return BAD(u,"duplicate_plan_id");
        if(item[i]->state==NAKI_U_PLAN)previous+=item[i]->quota;
        double lo=ceil(fmax(1,r->coefficient[kind[i]]*u->pixels*4*exp2((32-63)/6.0)))+(seq[i]==1?u->config_bits:0);
        if(!isfinite(lo)||lo>1000000000)return BAD(u,"model_domain");
        minimum[i]=lo;mins+=(int64_t)lo;
    }
    int64_t wall=history_bits(u,0,u->clock),dts=history_bits(u,1,media),locked=u->C-previous;
    int64_t supply=source_room(u),absolute=u->absolute_credit/1000000;
    int64_t real=(int64_t)fmin(fmin(u->cap-wall,u->cap-dts),fmin(supply,absolute))-u->D-locked;
    int64_t soft=(int64_t)floor(fmin(fmin(.95*u->cap-wall,.95*u->cap-dts),fmin(supply,absolute)))-u->D-locked;
    u->real_room=real;u->soft_room=soft;u->minimum=mins;u->margin_used=0;
    if(mins>real)return BAD(u,"MODEL_PREFLIGHT_FAIL_REAL");
    int64_t available=soft;
    if(mins>soft) {available=real;u->margin_used=1;u->margin_uses++;} /* one explicit fallback, whole unfrozen set */
    int64_t rate=u->source_limit&&u->source_rate<u->cap?u->source_rate:u->cap;
    double extra=0;
    for(int i=0;i<count;i++) {double weight=kind[i]==0||kind[i]==3?6:kind[i]==1?3.2:.2;desired[i]=fmax(minimum[i],rate*.82*u->period/1000000.*weight);extra+=desired[i]-minimum[i];}
    double alpha=extra>0?fmin(1,(available-mins)/extra):0;int64_t total=0;
    for(int i=0;i<count;i++) {ideal[i]=minimum[i]+alpha*(desired[i]-minimum[i]);quota[i]=(int64_t)floor(ideal[i]);frac[i]=ideal[i]-quota[i];total+=quota[i];}
    int64_t target=(int64_t)floor(fmin((double)available,mins+extra));
    for(int64_t n=total;n<target;n++) {int best=0;for(int i=1;i<count;i++)if(frac[i]>frac[best])best=i;quota[best]++;frac[best]=-1;}
    u->C-=previous;r->pending_bits-=previous;u->version++;
    for(int i=0;i<count;i++) {
        NakiUnifiedItem *f=item[i];double activity=f->decision.complexity;
        double quality=naki_rate_clip(0,63,u->crf+naki_rate_clip(-3,3,1.5*log2(activity)));
        double at32=r->coefficient[kind[i]]*u->pixels*activity;
        int qp=(int)ceil(quality);int64_t predicted=0;
        for(;qp<=63;qp++) {double pred=ceil(fmax(1,at32*exp2((32-qp)/6.)))+(f->seq==1?u->config_bits:0);if(!isfinite(pred)||pred>1000000000)return BAD(u,"prediction_domain");predicted=(int64_t)pred;if(predicted<=quota[i])break;}
        if(qp>63)return BAD(u,"model_preflight_mismatch");
        f->kind=kind[i];f->quota=quota[i];f->version=u->version;f->state=NAKI_U_PLAN;f->consulted=u->admitted;
        f->decision=(NakiRateDecision){.qp=qp,.kind=kind[i],.source_bps=u->source_rate,.effective_bps=rate,.predicted_bits=predicted,
            .decision_us=u->clock,.media_clock_us=media,.complexity=activity,.quality_qp=quality,.allowance_bits=quota[i],
            .model_coefficient=r->coefficient[kind[i]],.window_room_bits=available};
        u->C+=quota[i];r->pending_bits+=quota[i];
    }
    return naki_unified_check(u);
}
int naki_unified_try(NakiUnifiedRate *u,uint64_t seq) {
    NakiUnifiedItem *f=naki_unified_find(u,seq);if(!f||f->state!=NAKI_U_PLAN)return BAD(u,"try_state");f->state=NAKI_U_TRY;return naki_unified_check(u);
}
int naki_unified_accept(NakiUnifiedRate *u,uint64_t seq) {
    NakiUnifiedItem *f=naki_unified_find(u,seq);if(!f||f->state!=NAKI_U_TRY)return BAD(u,"accept_state");
    f->state=NAKI_U_ACCEPT;f->bits=f->quota;u->C-=f->quota;u->D+=f->quota;u->accepted++;return naki_unified_check(u);
}
int naki_unified_actual(NakiUnifiedRate *u,uint64_t seq,int64_t bits,int kind,int64_t dts) {
    NakiUnifiedItem *f=naki_unified_find(u,seq);if(!f||f->state!=NAKI_U_ACCEPT||kind<0||kind>2||kind!=(f->kind==3?0:f->kind))return BAD(u,"actual_role_or_state");
    if(u->admitted<seq||u->admitted-seq>4)return BAD(u,"future_union");
    if((int)(u->admitted-seq)>u->future_peak)u->future_peak=(int)(u->admitted-seq);
    NakiRateDecision reserved=f->decision;reserved.predicted_bits=f->quota;
    if(naki_rate_feedback(u->model,&reserved,u->pixels,bits,kind,dts)<0)return BAD(u,"actual_model_feedback");
    u->D+=bits-f->quota;f->bits=bits;f->state=NAKI_U_ACTUAL;u->feedback++;return naki_unified_check(u);
}
int naki_unified_can_write(NakiUnifiedRate *u,uint64_t seq,int64_t dts) {
    NakiUnifiedItem *f=naki_unified_find(u,seq);if(!f||f->state!=NAKI_U_ACTUAL)return BAD(u,"write_state");
    int64_t room=(int64_t)fmin(fmin(u->cap-history_bits(u,0,u->clock),u->cap-history_bits(u,1,dts)),fmin(source_room(u),u->absolute_credit/1000000));
    if(f->bits>room)return BAD(u,"ACTUAL_CAP");
    return 0;
}
int naki_unified_written(NakiUnifiedRate *u,uint64_t seq,int64_t dts,int64_t wall) {
    NakiUnifiedItem *f=naki_unified_find(u,seq);if(!f||f->state!=NAKI_U_ACTUAL)return BAD(u,"written_state");
    int violation=naki_unified_can_write(u,seq,dts)<0;int64_t bits=f->bits;
    if(!u->wrote)u->source_credit=(u->source_rate<u->cap?u->source_rate:u->cap)*1000000;
    if(naki_rate_written(u->model,dts,wall,bits)<0)return BAD(u,"written_model");
    u->source_credit-=bits*1000000;u->absolute_credit-=bits*1000000;u->wrote=1;u->D-=bits;u->written++;f->state=NAKI_U_FREE;
    if(naki_unified_check(u)<0)return -EINVAL;
    return violation?BAD(u,"ACTUAL_CAP_AFTER_FLUSH"):0;
}
int naki_unified_cancel(NakiUnifiedRate *u) {
    for(int i=0;i<NAKI_UNIFIED_SLOTS;i++) {NakiUnifiedItem *f=&u->item[i];if(f->state&&f->state<=NAKI_U_TRY) {if(f->state!=NAKI_U_OPEN) {u->C-=f->quota;u->model->pending_bits-=f->quota;}f->state=NAKI_U_FREE;}}
    return naki_unified_check(u);
}
