#include "pipeline_internal.h"

void pipeline_rate_audit_init(Pipeline *p) {
    NakiRateAudit *a=&p->rate_audit;
    if(a->initialized)return;
    a->initialized=1;
    const char *flag=getenv("NAKI_RATE_AUDIT");
    a->enabled=flag&&!strcmp(flag,"1");
    if(!a->enabled)return;
    const Config *c=&p->job->config;
    const char *candidate=getenv("NAKI_RATE_RESERVE_THREE");
    int requested=candidate&&!strcmp(candidate,"1");
    a->reserve_three=requested&&c->policy.intra==NAKI_INTRA_SOURCE&&p->encoder->codec_id==AV_CODEC_ID_VVC&&
        strcmp(c->encoder_profile,"reference")&&c->encoder_max_future_frames==4;
    if(requested&&!a->reserve_three)a->errors++;
    /* Capture detail requires an existing complete trace sink. Do not change
     * sampling/configuration or media behavior merely to enable the observer. */
    if(!p->job->trace||(!c->network_trace&&!c->codec_trace&&(!c->frame_log||c->frame_log_every!=1)))a->errors++;
    yyjson_mut_doc *doc=new_event("rate_audit_init",p->job);
    json_int(doc,"audit_version",2);json_int(doc,"model_epoch",1);json_int(doc,"seed_version",1);
    json_int(doc,"reserve_three_enabled",a->reserve_three);
    json_str(doc,"encoder_profile",c->encoder_profile);json_str(doc,"encoder_preset",c->encoder_preset);
    json_int(doc,"width",p->encoder->width);json_int(doc,"height",p->encoder->height);
    json_int(doc,"encoder_bit_depth",c->encoder_bit_depth);json_int(doc,"encoder_input_bit_depth",c->encoder_input_bit_depth);
    json_int(doc,"encoder_max_future_frames",c->encoder_max_future_frames);json_int(doc,"fixed_group_size",4);
    json_int(doc,"explicit_source_i",c->policy.intra==NAKI_INTRA_SOURCE);
    json_int(doc,"crf",c->qp);json_int(doc,"maxrate",c->maxrate);json_int(doc,"source_rate_limit",c->source_rate_limit);
    json_num(doc,"seed_startup_i",.8);json_num(doc,"seed_i",.4);json_num(doc,"seed_p",.2);json_num(doc,"seed_b",.07);
    char fingerprint[65],error[256];
    if(naki_config_fingerprint(c,fingerprint,error,sizeof(error))<0)a->errors++;
    else json_str(doc,"config_fingerprint",fingerprint);
    json_int(doc,"capture_errors",a->errors);logger_submit(p->job->logger,NULL,doc);
}

uint64_t pipeline_rate_audit_boundary(Pipeline *p,int64_t now,int *status) {
    NakiRateAudit *a=&p->rate_audit;*status=0;
    if(!a->enabled)return 0;
    if(!a->group_phase||a->last_accepted_phase>=3) {
        *status=naki_rate_audit_boundary(a,NULL,NULL,&p->rate,0,now);return 0;
    }
    for(int i=0;i<LEDGER_CAPACITY;i++) {
        Ledger *l=&p->ledger[i];
        if(l->used&&l->pts==a->last_accepted_pts&&l->stamp.rate_audit.seq==a->last_accepted_seq) {
            *status=naki_rate_audit_boundary(a,&l->stamp.rate_audit,&l->stamp.rate,&p->rate,
                p->encoder->width*p->encoder->height,now);
            /* Called only for the explicit I in pipeline_encode_frame, before
             * its rate_select, and outside the encoder EAGAIN retry loop. */
            if(*status==NAKI_RATE_BOUNDARY_TARGET&&naki_rate_reserve_three(a,&l->stamp.rate_audit,&l->stamp.rate,&p->rate,1,4)<0) {
                *status=NAKI_RATE_BOUNDARY_UNKNOWN;return 0;
            }
            return *status==1?l->stamp.rate_audit.seq:0;
        }
    }
    a->errors++;*status=-1;return 0;
}

void pipeline_rate_audit_eof(Pipeline *p) {
    NakiRateAudit *a=&p->rate_audit;if(!a->enabled)return;
    a->eof_event=++a->event;a->eof_group_phase=a->group_phase;
    if(!a->group_phase||a->last_accepted_phase>=3)return;
    for(int i=0;i<LEDGER_CAPACITY;i++) {
        Ledger *l=&p->ledger[i];
        if(l->used&&l->pts==a->last_accepted_pts&&l->stamp.rate_audit.seq==a->last_accepted_seq) {
            a->eof_target_seq=l->stamp.rate_audit.seq;l->stamp.rate_audit.eof_tail_event=a->eof_event;
            NakiRateAuditEntry *e=naki_rate_audit_entry(a,a->eof_target_seq);
            if(e)e->snapshot=l->stamp.rate_audit;else a->errors++;
            return;
        }
    }
    a->errors++;
}

void pipeline_rate_audit_json(yyjson_mut_doc *doc,const NakiRateAuditFrame *f) {
    json_int(doc,"rc_audit_version",2);
#define FIELD(name) json_int(doc,"rc_audit_"#name,(int64_t)f->name)
    FIELD(seq);FIELD(encoder_pts);FIELD(reserve_event);FIELD(accept_event);FIELD(feedback_event);FIELD(write_event);
    FIELD(reserve_before);FIELD(reserve_after);FIELD(feedback_before);FIELD(feedback_after);FIELD(write_before);FIELD(write_after);
    FIELD(frames_since_i);FIELD(first_p);FIELD(actual_kind);FIELD(actual_bits);FIELD(feedback_us);
    FIELD(boundary_status);FIELD(boundary_target_seq);FIELD(boundary_event);FIELD(boundary_i_seq);
    FIELD(boundary_group_size);FIELD(boundary_us);FIELD(boundary_old_bits);FIELD(boundary_shadow_bits);
    FIELD(send_eagain);FIELD(eof_tail_event);
    FIELD(reserved_bits);FIELD(upgrade_event);FIELD(upgrade_i_seq);
    FIELD(upgrade_before);FIELD(upgrade_after);FIELD(upgrade_old_bits);FIELD(upgrade_new_bits);
#undef FIELD
    json_num(doc,"rc_audit_coefficient_before",f->feedback_coefficient_before);
    json_num(doc,"rc_audit_coefficient_after",f->feedback_coefficient_after);
}

void pipeline_rate_audit_finish(Pipeline *p) {
    NakiRateAudit *a=&p->rate_audit;if(!a->enabled)return;
    int outstanding=0;int64_t bits=0;
    for(int i=0;i<NAKI_RATE_AUDIT_CAPACITY;i++) {
        NakiRateAuditEntry *e=&a->entries[i];if(!e->seq)continue;
        outstanding++;bits+=e->bits;
        yyjson_mut_doc *doc=new_event("rate_audit_outstanding",p->job);
        json_int(doc,"decision_seq",(int64_t)e->seq);json_int(doc,"encoder_pts",e->pts);
        json_int(doc,"frame_id",e->frame_id);json_int(doc,"generation",e->generation);
        json_int(doc,"phase",e->phase);json_int(doc,"reserved_or_actual_bits",e->bits);
        json_int(doc,"rc_qp",e->requested_qp);json_int(doc,"rc_model_kind",e->predicted_kind);json_int(doc,"rc_predicted_bits",e->predicted_bits);
        pipeline_rate_audit_json(doc,&e->snapshot);
        logger_submit(p->job->logger,NULL,doc);
    }
    if(bits!=p->rate.pending_bits)a->errors++;
    LoggerStats stats;logger_get_stats(p->job->logger,&stats);
    yyjson_mut_doc *doc=new_event("rate_audit_summary",p->job);
    json_int(doc,"audit_version",2);json_int(doc,"selected",(int64_t)a->selected);
    json_int(doc,"accepted",(int64_t)a->accepted);json_int(doc,"feedback",(int64_t)a->feedback);json_int(doc,"written",(int64_t)a->written);
    json_int(doc,"last_event",(int64_t)a->event);json_int(doc,"capture_errors",a->errors);
    json_int(doc,"eof_event",(int64_t)a->eof_event);json_int(doc,"eof_group_size",a->eof_group_phase);
    json_int(doc,"eof_target_seq",(int64_t)a->eof_target_seq);
    json_int(doc,"outstanding",outstanding);json_int(doc,"outstanding_bits",bits);json_int(doc,"pending_bits",p->rate.pending_bits);
    json_int(doc,"logger_dropped_frames",(int64_t)stats.dropped_frames);json_int(doc,"logger_dropped_status",(int64_t)stats.dropped_status);
    json_int(doc,"logger_io_errors",(int64_t)stats.io_errors);
    logger_submit(p->job->logger,NULL,doc);
}
