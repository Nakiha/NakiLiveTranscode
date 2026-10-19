#include "pipeline_internal.h"

void pipeline_unified_event(Pipeline *p,const char *action,uint64_t seq,int64_t amount) {
    PipelineUnified *s=p->unified;NakiUnifiedRate *u=&s->budget;
    yyjson_mut_doc *d=new_event("unified_rate",p->job);
    json_int(d,"schema",3);json_int(d,"event_seq",(int64_t)++s->event);json_str(d,"action",action);
    json_int(d,"admission_seq",(int64_t)seq);json_int(d,"amount",amount);json_int(d,"clock_us",u->clock);
    json_int(d,"C",u->C);json_int(d,"D",u->D);json_int(d,"pending",p->rate.pending_bits);
    json_int(d,"version",(int64_t)u->version);json_int(d,"admitted",(int64_t)u->admitted);
    json_int(d,"real_room",u->real_room);json_int(d,"soft_room",u->soft_room);json_int(d,"model_minimum",u->minimum);
    json_int(d,"margin_used",u->margin_used);json_int(d,"margin_uses",(int64_t)u->margin_uses);
    json_int(d,"source_credit_bit_us",u->source_credit);json_int(d,"absolute_credit_bit_us",u->absolute_credit);
    json_int(d,"source_rate",u->source_rate);json_int(d,"stage_peak",u->stage_peak);json_int(d,"native_peak",u->native_peak);
    json_int(d,"active_peak",u->active_peak);json_int(d,"future_peak",u->future_peak);json_str(d,"reason",u->reason);
    json_int(d,"accepted",(int64_t)u->accepted);json_int(d,"feedback",(int64_t)u->feedback);json_int(d,"written",(int64_t)u->written);
    uint64_t stage_bytes=0,oldest=0;int active=0,stage=0,native=0;
    for(int i=0;i<NAKI_UNIFIED_SLOTS;i++) {
        NakiUnifiedItem *a=&u->item[i];
        if(a->state&&a->state<=NAKI_U_ACCEPT) {active++;if(!oldest||a->seq<oldest)oldest=a->seq;
            if(a->state==NAKI_U_ACCEPT)native++;else stage++;}
    }
    for(int i=0;i<4;i++)if(s->frames[i])for(int b=0;b<AV_NUM_DATA_POINTERS;b++)if(s->frames[i]->buf[b]) {
        int duplicate=0;uint8_t *data=s->frames[i]->buf[b]->data;
        for(int j=0;j<=i;j++)if(s->frames[j])for(int c=0;c<AV_NUM_DATA_POINTERS;c++) {
            if(j==i&&c>=b)break;
            if(s->frames[j]->buf[c]&&s->frames[j]->buf[c]->data==data)duplicate=1;
        }
        if(!duplicate)stage_bytes+=s->frames[i]->buf[b]->size;
    }
    json_int(d,"active_frames",active);json_int(d,"stage_frames",stage);json_int(d,"native_frames",native);
    json_int(d,"oldest_active_seq",(int64_t)oldest);json_int(d,"stage_buffer_bytes",(int64_t)stage_bytes);
    if(!strcmp(action,"init")) {
        char fingerprint[65],error[256];
        if(naki_config_fingerprint(&p->job->config,fingerprint,error,sizeof(error))>=0)json_str(d,"config_fingerprint",fingerprint);
        json_int(d,"maxrate",u->cap);json_str(d,"profile",p->job->config.encoder_profile);
    }
    if(!strcmp(action,"summary")) {
        LoggerStats stats;logger_get_stats(p->job->logger,&stats);
        json_int(d,"logger_dropped_frames",(int64_t)stats.dropped_frames);json_int(d,"logger_dropped_status",(int64_t)stats.dropped_status);
        json_int(d,"logger_io_errors",(int64_t)stats.io_errors);json_int(d,"ledger_check",naki_unified_check(u));
    }
    NakiUnifiedItem *f=naki_unified_find(u,seq);
    if(f) {json_int(d,"phase",f->state);json_int(d,"quota",f->quota);json_int(d,"predicted",f->decision.predicted_bits);
        json_int(d,"bits",f->bits);json_int(d,"kind",f->kind);json_int(d,"qp",f->decision.qp);
        json_int(d,"encoder_pts_us",f->pts);json_int(d,"filtered_us",f->filtered_us);
        json_int(d,"consulted_high_seq",(int64_t)f->consulted);json_num(d,"coefficient",f->decision.model_coefficient);}
    logger_submit(p->job->logger,NULL,d);
}
static int refresh_at(Pipeline *p,int64_t at) {
    PipelineUnified *s=p->unified;int count=0;
    int ret=pipeline_reader_unified_snapshot(p,&s->source_cursor,s->snapshot,&count);
    if(ret<0)return ret;
    for(int i=0;i<count;i++)if(naki_unified_source(&s->budget,&s->snapshot[i])<0)return AVERROR_INVALIDDATA;
    return naki_unified_clock(&s->budget,at)<0?AVERROR_INVALIDDATA:0;
}
int pipeline_unified_refresh(Pipeline *p) {return refresh_at(p,now_us());}
static int receive(void *opaque) {return pipeline_receive_encoded(opaque);}
static int send(void *opaque,const void *frame) {return pipeline_encoder_send(opaque,frame);}
static uint64_t progress(void *opaque) {return ((Pipeline *)opaque)->unified->budget.feedback;}
static int drain_null(Pipeline *p) {
    PipelineUnified *s=p->unified;NakiCodecProgress io={p,send,receive,progress};
    int ret=naki_codec_submit(&io,NULL,AVERROR(EAGAIN),&s->null_sent);
    pipeline_unified_event(p,ret<0?"null_failed":"null_accepted",0,ret);
    return ret;
}
static int solve(Pipeline *p,int recheck) {
    PipelineUnified *s=p->unified;uint64_t ids[4];int kinds[4],n=0;
    for(int i=0;i<s->count;i++) {NakiUnifiedItem *f=naki_unified_find(&s->budget,s->ids[i]);
        if(f&&(f->state==NAKI_U_OPEN||f->state==NAKI_U_PLAN)) {ids[n]=f->seq;kinds[n++]=f->kind;}}
    if(!n)return 0;
    int ret=pipeline_unified_refresh(p);if(ret<0)return ret;
    ret=naki_unified_plan(&s->budget,ids,kinds,n);
    if(ret<0&&recheck&&!strcmp(s->budget.reason,"MODEL_PREFLIGHT_FAIL_REAL")) {
        /* Exactly one nonblocking progress/clock/source refresh, no wait for income. */
        if((ret=pipeline_receive_encoded(p))<0||(ret=pipeline_drain_mux(p,0))<0||(ret=pipeline_unified_refresh(p))<0)return ret;
        ret=naki_unified_plan(&s->budget,ids,kinds,n);
        pipeline_unified_event(p,"real_recheck",0,ret);
    }
    if(ret<0) {p->stage=s->budget.reason;pipeline_unified_event(p,"plan_failed",0,ret);return AVERROR_INVALIDDATA;}
    for(int i=0;i<n;i++)pipeline_unified_event(p,"plan",ids[i],0);
    return 0;
}
static int flush_batch(Pipeline *p,int eof) {
    PipelineUnified *s=p->unified;int ret;
    if(!s->count)return eof?drain_null(p):0;
    int inter=s->stamps[s->count-1].intra_reason?s->count-1:s->count;
    for(int i=0;i<s->count;i++) {
        NakiUnifiedItem *f=naki_unified_find(&s->budget,s->ids[i]);if(!f)return AVERROR_INVALIDDATA;
        f->kind=i==inter?(f->seq==1?3:0):(i==inter-1?1:2);
    }
    if((ret=solve(p,1))<0)return ret;
    int64_t closed=now_us();
    for(int i=0;i<s->count;i++) {
        NakiUnifiedItem *f=naki_unified_find(&s->budget,s->ids[i]);if(!f)return AVERROR_INVALIDDATA;
        Stamp stamp=s->stamps[i];stamp.rate=f->decision;stamp.unified_version=f->version;
        stamp.unified_quota=f->quota;stamp.unified_consulted=f->consulted;stamp.unified_stage_done_us=closed;
        if(naki_unified_try(&s->budget,f->seq)<0)return AVERROR_INVALIDDATA;
        pipeline_unified_event(p,"try",f->seq,0);
        if((ret=pipeline_encode_selected(p,s->frames[i],stamp))<0)return ret;
        av_frame_free(&s->frames[i]);
    }
    if(eof&&(ret=drain_null(p))<0)return ret;
    if((ret=pipeline_receive_encoded(p))<0)return ret;
    for(int i=0;i<s->count;i++) {NakiUnifiedItem *f=naki_unified_find(&s->budget,s->ids[i]);
        if(f&&f->state<=NAKI_U_ACCEPT) {p->stage="unified codec no progress; fifth frame forbidden";return AVERROR(EDEADLK);}}
    s->count=0;return 0;
}
int pipeline_unified_frame(Pipeline *p,AVFrame *frame) {
    const Config *c=&p->job->config;
    if(!p->unified) {
        if(p->job->parent||c->output_count||!c->constrained_crf||strcmp(c->encoder,"libvvenc")||
           !c->policy.intra||c->maxrate!=1100000||!c->source_rate_limit||c->encoder_max_future_frames!=4||
           c->width!=960||c->height!=540||strcmp(c->encoder_profile,"live_3b_2s_rdoq2_sel2")) {
            p->stage="unified opt-in requires the frozen single-output VVenC experiment";return AVERROR(EINVAL);
        }
        p->unified=calloc(1,sizeof(*p->unified));if(!p->unified)return AVERROR(ENOMEM);
        int64_t period=av_rescale_q(1,av_inv_q(p->encoder->framerate),AV_TIME_BASE_Q);
        naki_unified_init(&p->unified->budget,&p->rate,c->maxrate,c->source_rate_limit,p->encoder->width*p->encoder->height,c->qp,period,now_us());
        p->unified->budget.config_bits=8*(int64_t)p->encoder->extradata_size;
        pipeline_unified_event(p,"init",0,0);
    }
    PipelineUnified *s=p->unified;if(s->count==4)return AVERROR(ENOBUFS);
    if(!pipeline_stamp_valid(frame->opaque_ref))return AVERROR_INVALIDDATA;
    Stamp stamp=*(Stamp *)frame->opaque_ref->data;stamp.filtered_us=now_us();
    int byte_input=frame->format==AV_PIX_FMT_YUV420P;
    if(!byte_input&&frame->format!=AV_PIX_FMT_YUV420P10LE)return AVERROR_INVALIDDATA;
    double activity=naki_rate_complexity_samples(&p->rate,frame->data[0],frame->linesize[0]/(byte_input?1:2),frame->width,frame->height,byte_input);
    int64_t pts=av_rescale_q(frame->pts,av_buffersink_get_time_base(p->sink),AV_TIME_BASE_Q);uint64_t seq;
    if(naki_unified_admit(&s->budget,pts,stamp.filtered_us,activity,&seq)<0)return AVERROR_INVALIDDATA;
    AVFrame *copy=av_frame_clone(frame);if(!copy)return AVERROR(ENOMEM);
    stamp.unified_seq=seq;s->frames[s->count]=copy;s->stamps[s->count]=stamp;s->ids[s->count++]=seq;
    pipeline_unified_event(p,"admit",seq,0);
    return stamp.intra_reason||s->count==4?flush_batch(p,0):0;
}
int pipeline_unified_eof(Pipeline *p) {p->unified->flushing=1;return flush_batch(p,1);}
int pipeline_unified_feedback(Pipeline *p,Stamp *stamp,int64_t bits,int kind,int64_t dts) {
    PipelineUnified *s=p->unified;int ret=naki_unified_actual(&s->budget,stamp->unified_seq,bits,kind,dts);
    stamp->unified_future=(int)(s->budget.admitted-stamp->unified_seq);
    pipeline_unified_event(p,ret<0?"actual_failed":"actual",stamp->unified_seq,bits);
    if(ret<0) {p->stage=s->budget.reason;return AVERROR_INVALIDDATA;}
    return solve(p,0);
}
int pipeline_unified_prewrite(Pipeline *p,const Stamp *stamp,int64_t dts) {
    int ret=pipeline_unified_refresh(p);if(ret<0)return ret;
    ret=naki_unified_can_write(&p->unified->budget,stamp->unified_seq,dts);
    if(ret<0) { /* one fresh source/clock observation; never recurse into mux */
        if((ret=pipeline_unified_refresh(p))<0)return ret;
        ret=naki_unified_can_write(&p->unified->budget,stamp->unified_seq,dts);
        pipeline_unified_event(p,"write_real_recheck",stamp->unified_seq,ret);
    }
    if(ret<0)p->stage=p->unified->budget.reason;
    return ret<0?AVERROR_INVALIDDATA:0;
}
int pipeline_unified_written(Pipeline *p,const Stamp *stamp,int64_t bits,int64_t dts,int64_t wall) {
    /* Settle at the real flush time. Sources published after that time remain
     * future events; never accrue clock/source income after the write boundary. */
    int ret=refresh_at(p,wall);if(ret<0)return ret;
    ret=naki_unified_written(&p->unified->budget,stamp->unified_seq,dts,wall);
    pipeline_unified_event(p,ret<0?"write_failed":"write",stamp->unified_seq,bits);
    if(ret<0)p->stage=p->unified->budget.reason;
    return ret<0?AVERROR_INVALIDDATA:0;
}
void pipeline_unified_release(Pipeline *p) {
    if(!p->unified)return;
    for(int i=0;i<4;i++)av_frame_free(&p->unified->frames[i]);
    naki_unified_cancel(&p->unified->budget);pipeline_unified_event(p,"release_unsubmitted",0,0);
}
void pipeline_unified_finish(Pipeline *p) {
    if(!p->unified)return;
    for(int i=0;i<NAKI_UNIFIED_SLOTS;i++)if(p->unified->budget.item[i].state)
        pipeline_unified_event(p,"outstanding",p->unified->budget.item[i].seq,p->unified->budget.item[i].bits);
    pipeline_unified_event(p,"summary",0,0);
    free(p->unified);p->unified=NULL;
}
