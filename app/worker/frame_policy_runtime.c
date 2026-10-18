#include "job.h"
#include "platform/runtime.h"
#include "frame_policy_runtime.h"

int naki_policy_track(Job *job, int kind, uint64_t id, int64_t ingress) {
    pthread_mutex_lock(&job->mutex);
    int ret=naki_inflight_add(&job->flow.policy_inflight,kind,id,ingress);
    pthread_mutex_unlock(&job->mutex); return ret;
}
int naki_policy_untrack(Job *job, int kind, uint64_t id) {
    pthread_mutex_lock(&job->mutex);
    int found=naki_inflight_remove(&job->flow.policy_inflight,kind,id);
    pthread_mutex_unlock(&job->mutex); return found;
}
void naki_policy_tick(Job *job, int64_t current) {
    if(!job->config.policy.overload_levels)return;
    pthread_mutex_lock(&job->mutex);
    if(job->flow.policy_closing) {pthread_mutex_unlock(&job->mutex); return;}
    int old_level=job->flow.overload.level, old_recovering=job->flow.overload.recovering, old_failed=job->flow.overload.failed;
    job->flow.oldest_ingress_age_us=naki_inflight_age(&job->flow.policy_inflight,current);
    naki_overload_tick(&job->config.policy,&job->flow.overload,current,job->flow.oldest_ingress_age_us);
    int level=job->flow.overload.level, recovering=job->flow.overload.recovering, failed=job->flow.overload.failed;
    if(failed && !old_failed) {
        job->flow.overload_fail_write_us=job->progress.write_started_us ? current-job->progress.write_started_us : 0;
        job->flow.overload_fail_write_phase=job->progress.write_phase;
    }
    int64_t age=job->flow.oldest_ingress_age_us;
    pthread_mutex_unlock(&job->mutex);
    if(level!=old_level || recovering!=old_recovering || failed!=old_failed) {
        yyjson_mut_doc *event=new_event("overload_transition",job);
        json_str(event,"state",failed ? "failed" : recovering ? "recovering" : level ? "degraded" : "normal");
        json_int(event,"level",level); json_num(event,"oldest_ingress_age_ms",age/1000.0);
        logger_submit(job->logger,NULL,event);
    }
    if(failed)request_stop(job);
}
