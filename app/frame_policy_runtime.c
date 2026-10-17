#include "live.h"

int naki_policy_track(Job *job, int kind, uint64_t id, int64_t ingress) {
    pthread_mutex_lock(&job->mutex);
    int ret=naki_inflight_add(&job->policy_inflight,kind,id,ingress);
    pthread_mutex_unlock(&job->mutex); return ret;
}
int naki_policy_untrack(Job *job, int kind, uint64_t id) {
    pthread_mutex_lock(&job->mutex);
    int found=naki_inflight_remove(&job->policy_inflight,kind,id);
    pthread_mutex_unlock(&job->mutex); return found;
}
void naki_policy_tick(Job *job, int64_t current) {
    if(!job->config.policy.overload_levels)return;
    pthread_mutex_lock(&job->mutex);
    if(job->policy_closing) {pthread_mutex_unlock(&job->mutex); return;}
    int old_level=job->overload.level, old_recovering=job->overload.recovering, old_failed=job->overload.failed;
    job->oldest_ingress_age_us=naki_inflight_age(&job->policy_inflight,current);
    naki_overload_tick(&job->config.policy,&job->overload,current,job->oldest_ingress_age_us);
    int level=job->overload.level, recovering=job->overload.recovering, failed=job->overload.failed;
    if(failed && !old_failed) {
        job->overload_fail_write_us=job->write_started_us ? current-job->write_started_us : 0;
        snprintf(job->overload_fail_write_phase,sizeof(job->overload_fail_write_phase),"%s",job->write_phase);
    }
    int64_t age=job->oldest_ingress_age_us;
    pthread_mutex_unlock(&job->mutex);
    if(level!=old_level || recovering!=old_recovering || failed!=old_failed) {
        yyjson_mut_doc *event=new_event("overload_transition",job);
        json_str(event,"state",failed ? "failed" : recovering ? "recovering" : level ? "degraded" : "normal");
        json_int(event,"level",level); json_num(event,"oldest_ingress_age_ms",age/1000.0);
        logger_submit(job->logger,NULL,event);
    }
    if(failed)request_stop(job);
}
