#include "job.h"
#include "platform/runtime.h"
#include "crash/crash.h"
#include "control/identity.h"
#include <errno.h>
#include <unistd.h>
#include <libavutil/time.h>
void wake_job(void *opaque) {
    Job *job = opaque;
    uint64_t one = 1;
    ssize_t n;
    do { n = write(job->wake_fd, &one, sizeof(one)); } while (n < 0 && errno == EINTR);
    /* EAGAIN means an existing level-triggered wakeup is pending. */
}
void request_stop(Job *job) {
    atomic_store(&job->lifecycle.cancel,1);wake_job(job);
    for(int i=0;i<job->config.output_count;i++)if(job->outputs[i])request_stop(job->outputs[i]);
}
void naki_branch_release(Job *job,uint64_t id,uint64_t bytes) {
    if(!job->parent)return;
    pthread_mutex_lock(&job->mutex);
    if(naki_inflight_remove(&job->flow.branch_pending,NAKI_TRACK_FRAME,id)) {
        if(job->stats.branch_inflight_bytes>=bytes)job->stats.branch_inflight_bytes-=bytes;
        if(job->stats.branch_inflight_frames)job->stats.branch_inflight_frames--;
    }
    pthread_mutex_unlock(&job->mutex);
}
yyjson_mut_doc *new_event(const char *event, const Job *job) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) return NULL;
    yyjson_mut_val *obj = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, obj);
    json_str(doc, "event", event);
    json_int(doc, "unix_us", av_gettime());
    json_int(doc, "monotonic_us", now_us());
    json_int(doc,"process_id",getpid());
    if(*naki_process_id())json_str(doc,"instance_id",naki_process_id());
    if (job) {json_str(doc, "job_id", job->id); json_str(doc,"request_id",job->request_id);json_str(doc,"create_request_id",job->request_id);if(*job->output_id)json_str(doc,"output_id",job->output_id);}
    return doc;
}
