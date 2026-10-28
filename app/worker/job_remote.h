#ifndef NAKI_JOB_REMOTE_H
#define NAKI_JOB_REMOTE_H
#include "job_media.h"
/* Factories/projections are worker-owned. Callers hold their topology owner
 * lock and prove a holder has exited/joined before retirement/destruction. */
int naki_job_remote_slots(Job *job);
void naki_job_bind_renditions(Job *job,NakiRenditions *runtime);
void naki_job_remote_configure(Job *job,const Config *session,unsigned output,const char *id);
void naki_job_remote_forget(Job *job);
void naki_job_remote_failed(Job *job,int error,const char *why);
void naki_job_remote_spawned(Job *job,int pid,uint64_t generation);
void naki_job_remote_exit_observed(Job *job,int exit_code,int signal);
void naki_job_remote_retired(Job *job,int failed,int stopping,int64_t when);
int naki_job_remote_project(Job *job,yyjson_val *snapshot,int active);
void naki_job_remote_boot(Job *job,yyjson_mut_doc *message);
void naki_job_quarantine(Job *job);
typedef struct NakiTopologyProgress {
    uint64_t desired_revision,effective_revision,source_generation,pool_bytes,pool_occupied;
    int64_t idle_deadline_us;int idle_state;
} NakiTopologyProgress;
void naki_job_topology(Job *job,const NakiTopologyProgress *progress);
typedef struct NakiRawProgress {
    unsigned video_retained,audio_retained,restarts,video_queued,audio_queued;
    uint64_t bytes,work_us;int64_t restart_at_us;
} NakiRawProgress;
void naki_job_raw_progress(Job *job,const NakiRawProgress *progress);
typedef struct NakiRenditionView {
    int pid,attached,member;
    uint64_t generation,source_generation,prepared_source_generation;
    const char *session,*instance;
} NakiRenditionView;
yyjson_mut_doc *naki_job_remote_detail(Job *job,const NakiRenditionView *view);
Job *naki_job_destination_new(Job *source,const char *output,const char *format);
void naki_job_destination_free(Job **job);
void naki_job_destinations_publish(Job *job,yyjson_mut_doc *snapshot);
void naki_job_destination_written(Job *job,int64_t when);
typedef struct NakiDestinationBaseline {uint64_t packets,audio;int64_t first_us,last_us;Metric latency;} NakiDestinationBaseline;
void naki_job_destination_baseline(Job *job,NakiDestinationBaseline *baseline);
Job *naki_job_child_new(void);
int naki_job_child_logger(Job *job,const LoggerOptions *options);
int naki_job_child_identity(Job *job,yyjson_val *bootstrap,char *error,size_t capacity);
int naki_job_child_resources(Job *job,yyjson_val *bootstrap,const NakiVideoRate *rate,uint64_t generation,uint64_t inflight_limit,int64_t accepted_us);
int naki_job_child_trace(Job *job);
void naki_job_child_stopped(Job *job);
void naki_job_child_free(Job **job);
#endif
