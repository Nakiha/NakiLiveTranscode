#define _GNU_SOURCE
#include "job.h"
#include "job_remote.h"
#include "platform/runtime.h"
#include "control/identity.h"
#include "media/watermark.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/eventfd.h>

static Job *remote_slot(Job *parent) {
    Job *j=calloc(1,sizeof(*j));if(!j)return NULL;
    j->allocation_kind=NAKI_JOB_ALLOCATION_REMOTE_SLOT;
    j->parent=parent;j->logger=parent->logger;j->trace=parent->trace;j->remote_rendition=1;
    strcpy(j->id,parent->id);strcpy(j->request_id,parent->request_id);strcpy(j->log_path,parent->log_path);
    j->lifecycle.started_us=parent->lifecycle.started_us;j->progress.window_start_us=parent->lifecycle.started_us;
    j->wake_fd=eventfd(0,EFD_NONBLOCK|EFD_CLOEXEC);
    if(j->wake_fd<0){free(j);return NULL;}
    if(pthread_mutex_init(&j->mutex,NULL)){close(j->wake_fd);free(j);return NULL;}
    atomic_init(&j->lifecycle.done,1);return j;
}
int naki_job_remote_slots(Job *j) {
    unsigned created=0;
    for(unsigned i=0;i<NAKI_OUTPUT_MAX;i++) {
        if(!j->outputs[i]){
            j->outputs[i]=remote_slot(j);
            if(!j->outputs[i]){
                for(unsigned k=0;k<i;k++)if(created&(1U<<k)){Job *slot=j->outputs[k];close(slot->wake_fd);pthread_mutex_destroy(&slot->mutex);free(slot);j->outputs[k]=NULL;}
                return -ENOMEM;
            }
            created|=1U<<i;
        }
    }
    for(unsigned i=0;i<NAKI_OUTPUT_MAX;i++)j->outputs[i]->remote_rendition=1;
    j->config.output_count=NAKI_OUTPUT_MAX;return 0;
}
void naki_job_bind_renditions(Job *j,NakiRenditions *m){j->renditions=m;}
void naki_job_remote_configure(Job *j,const Config *session,unsigned output,const char *id){pthread_mutex_lock(&j->mutex);naki_output_config(&j->config,session,(int)output);snprintf(j->output_id,sizeof(j->output_id),"%s",id);pthread_mutex_unlock(&j->mutex);}
void naki_job_remote_forget(Job *j){pthread_mutex_lock(&j->mutex);j->output_id[0]=0;pthread_mutex_unlock(&j->mutex);}
void naki_job_remote_failed(Job *j,int error,const char *why){pthread_mutex_lock(&j->mutex);j->lifecycle.branch_failed=1;j->lifecycle.state=NAKI_JOB_STATE_FAILING;snprintf(j->lifecycle.error,sizeof(j->lifecycle.error),"%s: %d",why,error);pthread_mutex_unlock(&j->mutex);}
void naki_job_remote_spawned(Job *j,int pid,uint64_t generation) {
    pthread_mutex_lock(&j->mutex);j->rendition_pid=pid;j->rendition_generation=generation;
    if(j->remote_snapshot){yyjson_doc_free(j->remote_snapshot);j->remote_snapshot=NULL;}
    memset(&j->stats,0,sizeof(j->stats));j->lifecycle.error[0]=0;j->lifecycle.ended_us=0;
    j->progress.rendition_exit_known=0;j->progress.rendition_exit_code=-1;j->progress.rendition_exit_signal=0;pthread_mutex_unlock(&j->mutex);
    atomic_store(&j->lifecycle.done,0);atomic_store(&j->lifecycle.cancel,0);atomic_store(&j->lifecycle.branch_failed,0);
    pthread_mutex_lock(&j->mutex);j->lifecycle.state=NAKI_JOB_STATE_STARTING;pthread_mutex_unlock(&j->mutex);
}
void naki_job_remote_exit_observed(Job *j,int exit_code,int signal){pthread_mutex_lock(&j->mutex);j->progress.rendition_exit_known=1;j->progress.rendition_exit_code=exit_code;j->progress.rendition_exit_signal=signal;pthread_mutex_unlock(&j->mutex);}
void naki_job_remote_retired(Job *j,int failed,int stopping,int64_t when){pthread_mutex_lock(&j->mutex);j->lifecycle.state=failed?NAKI_JOB_STATE_FAILED:stopping?NAKI_JOB_STATE_STOPPED:NAKI_JOB_STATE_COMPLETED;j->lifecycle.ended_us=when;j->rendition_pid=0;pthread_mutex_unlock(&j->mutex);atomic_store(&j->lifecycle.done,1);}
int naki_job_remote_project(Job *j,yyjson_val *snapshot,int active) {
    size_t bytes;char *text=yyjson_val_write(snapshot,0,&bytes);yyjson_doc *copy=text?yyjson_read(text,bytes,0):NULL;free(text);if(!copy)return -ENOMEM;
    pthread_mutex_lock(&j->mutex);
    uint64_t packets=yyjson_get_uint(yyjson_obj_get(snapshot,"packets_out")),written=yyjson_get_uint(yyjson_obj_get(snapshot,"bytes_out")),filtered=yyjson_get_uint(yyjson_obj_get(snapshot,"frames_filtered"));
    uint64_t dp=packets>=j->stats.packets_out?packets-j->stats.packets_out:0,db=written>=j->stats.bytes_out?written-j->stats.bytes_out:0,df=filtered>=j->stats.frames_filtered?filtered-j->stats.frames_filtered:0;
    if(j->remote_snapshot)yyjson_doc_free(j->remote_snapshot);
    j->remote_snapshot=copy;
#define I(k) j->stats.k=yyjson_get_uint(yyjson_obj_get(snapshot,#k))
    I(packets_out);I(bytes_out);I(frames_filtered);I(audio_packets_out);I(audio_tracks);I(audio_dropped);I(sei_written);
#undef I
    j->stats.sei_protected=yyjson_get_uint(yyjson_obj_get(snapshot,"sei_protected_frames"));
    j->progress.last_output_us=yyjson_get_sint(yyjson_obj_get(yyjson_obj_get(snapshot,"progress"),"last_written_us"));
    if(active)j->lifecycle.state=NAKI_JOB_STATE_RUNNING;
    pthread_mutex_unlock(&j->mutex);
    pthread_mutex_lock(&j->parent->mutex);j->parent->stats.packets_out+=dp;j->parent->stats.bytes_out+=db;j->parent->stats.frames_filtered+=df;pthread_mutex_unlock(&j->parent->mutex);
    return 0;
}
void naki_job_remote_boot(Job *j,yyjson_mut_doc *d) {
    NakiJobStartup s=naki_job_startup(j);json_int(d,"request_us",s.started_us);
#define I(k) json_int(d,#k,s.k)
    I(input_open_started_us);I(input_open_done_us);I(input_probe_done_us);I(decoder_ready_us);I(assigned_us);I(worker_received_us);I(worker_spawned_us);I(worker_ready_us);I(prewarmed);
#undef I
}
void naki_job_quarantine(Job *j){pthread_mutex_lock(&j->mutex);j->lifecycle.state=NAKI_JOB_STATE_FAILING;snprintf(j->lifecycle.error,sizeof(j->lifecycle.error),"rendition holder quarantine: exit proof pending");pthread_mutex_unlock(&j->mutex);}
void naki_job_topology(Job *j,const NakiTopologyProgress *s){pthread_mutex_lock(&j->mutex);j->progress.topology_revision=s->desired_revision;j->progress.effective_topology_revision=s->effective_revision;j->progress.source_format_generation=s->source_generation;j->progress.idle_deadline_us=s->idle_deadline_us;j->progress.idle_state=s->idle_state;j->progress.shared_pool_bytes=s->pool_bytes;j->progress.shared_pool_occupied=s->pool_occupied;pthread_mutex_unlock(&j->mutex);}
void naki_job_raw_progress(Job *j,const NakiRawProgress *s){pthread_mutex_lock(&j->mutex);j->progress.raw_video_retained=s->video_retained;j->progress.raw_audio_retained=s->audio_retained;j->progress.automatic_restart_attempts=s->restarts;j->progress.automatic_restart_at_us=s->restart_at_us;j->stats.branch_video_queued=s->video_queued;j->stats.branch_audio_queued=s->audio_queued;j->stats.branch_queue_bytes=s->bytes;j->stats.branch_queue_media_us=s->work_us;pthread_mutex_unlock(&j->mutex);}
yyjson_mut_doc *naki_job_remote_detail(Job *j,const NakiRenditionView *v) {
    pthread_mutex_lock(&j->mutex);yyjson_mut_doc *d=j->remote_snapshot?yyjson_doc_mut_copy(j->remote_snapshot,NULL):new_event("rendition_status",j);
    if(d){const char *overrides[]={"state","detail_scope","rendition_pid","rendition_generation","attached","logical_member","source_format_generation","prepared_source_format_generation","input_session_instance","rendition_instance"};for(unsigned k=0;k<sizeof(overrides)/sizeof(*overrides);k++)yyjson_mut_obj_remove_key(yyjson_mut_doc_get_root(d),overrides[k]);if(*j->lifecycle.error)yyjson_mut_obj_remove_key(yyjson_mut_doc_get_root(d),"error");json_str(d,"state",naki_job_state_name(j->lifecycle.state));if(*j->lifecycle.error)json_str(d,"error",j->lifecycle.error);json_str(d,"detail_scope","full");json_int(d,"rendition_pid",v->pid);json_int(d,"rendition_generation",v->generation);json_int(d,"attached",v->attached);json_int(d,"logical_member",v->member);json_int(d,"source_format_generation",v->source_generation);json_int(d,"prepared_source_format_generation",v->prepared_source_generation);json_str(d,"input_session_instance",v->session);json_str(d,"rendition_instance",v->instance);}
    pthread_mutex_unlock(&j->mutex);return d;
}
Job *naki_job_destination_new(Job *source,const char *output,const char *format) {
    Job *j=calloc(1,sizeof(*j));if(!j)return NULL;
    j->allocation_kind=NAKI_JOB_ALLOCATION_DESTINATION;
    j->config=source->config;snprintf(j->config.output,sizeof(j->config.output),"%s",output);snprintf(j->config.output_format,sizeof(j->config.output_format),"%s",format);
    strcpy(j->id,source->id);strcpy(j->request_id,source->request_id);strcpy(j->output_id,source->output_id);
    j->logger=source->logger;j->wake_fd=-1;j->lifecycle.started_us=source->lifecycle.started_us;
    if(pthread_mutex_init(&j->mutex,NULL)){free(j);return NULL;}return j;
}
void naki_job_destination_free(Job **job){Job *j=*job;if(!j)return;if(j->allocation_kind!=NAKI_JOB_ALLOCATION_DESTINATION)abort();pthread_mutex_destroy(&j->mutex);free(j);*job=NULL;}
void naki_job_destinations_publish(Job *j,yyjson_mut_doc *snapshot){pthread_mutex_lock(&j->mutex);if(j->destination_snapshot)yyjson_mut_doc_free(j->destination_snapshot);j->destination_snapshot=snapshot;pthread_mutex_unlock(&j->mutex);}
void naki_job_destination_written(Job *j,int64_t when){pthread_mutex_lock(&j->mutex);j->progress.last_output_us=when;pthread_mutex_unlock(&j->mutex);}
void naki_job_destination_baseline(Job *j,NakiDestinationBaseline *b){pthread_mutex_lock(&j->mutex);b->packets=j->stats.packets_out;b->audio=j->stats.audio_packets_out;b->first_us=j->progress.first_video_output_us;b->last_us=j->progress.last_output_us;b->latency=j->stats.lifetime.ingress_to_mux;pthread_mutex_unlock(&j->mutex);}
Job *naki_job_child_new(void) {
    Job *j=calloc(1,sizeof(*j)),*parent=calloc(1,sizeof(*parent));if(!j||!parent){free(j);free(parent);return NULL;}
    j->allocation_kind=NAKI_JOB_ALLOCATION_CHILD;
    j->wake_fd=parent->wake_fd=-1;j->parent=parent;j->remote_rendition=2;
    if(pthread_mutex_init(&j->mutex,NULL)){free(j);free(parent);return NULL;}
    if(pthread_mutex_init(&parent->mutex,NULL)){pthread_mutex_destroy(&j->mutex);free(j);free(parent);return NULL;}
    j->wake_fd=eventfd(0,EFD_NONBLOCK|EFD_CLOEXEC);
    if(j->wake_fd<0){pthread_mutex_destroy(&j->mutex);pthread_mutex_destroy(&parent->mutex);free(j);free(parent);return NULL;}
    return j;
}
int naki_job_child_logger(Job *j,const LoggerOptions *options){j->logger=logger_start(options);return j->logger?0:-ENOMEM;}
int naki_job_child_identity(Job *j,yyjson_val *v,char *error,size_t cap) {
    size_t bytes;char *text=yyjson_val_write(yyjson_obj_get(v,"config"),0,&bytes);
    if(!text){snprintf(error,cap,"config allocation");return -ENOMEM;}
    int ret=config_parse(text,bytes,&j->config,error,cap);free(text);if(ret)return -EINVAL;
    const char *id=yyjson_get_str(yyjson_obj_get(v,"job_id")),*request=yyjson_get_str(yyjson_obj_get(v,"request_id")),*output=yyjson_get_str(yyjson_obj_get(v,"output_id")),*log=yyjson_get_str(yyjson_obj_get(v,"log_path"));
    if(!naki_id_valid(id,63)||!naki_id_valid(request,128)||!naki_id_valid(output,32)||!log||strlen(log)>4000)return -EINVAL;
    strcpy(j->id,id);strcpy(j->request_id,request);strcpy(j->output_id,output);strcpy(j->log_path,log);return 0;
}
int naki_job_child_resources(Job *j,yyjson_val *v,const NakiVideoRate *rate,uint64_t generation,uint64_t limit,int64_t accepted) {
#define I(k) yyjson_get_sint(yyjson_obj_get(v,k))
    j->lifecycle.started_us=I("request_us");j->progress.window_start_us=j->lifecycle.started_us;
    j->progress.input_open_started_us=I("input_open_started_us");j->progress.input_open_done_us=I("input_open_done_us");j->progress.input_probe_done_us=I("input_probe_done_us");j->progress.decoder_ready_us=I("decoder_ready_us");j->progress.video_rate=*rate;
    j->progress.assigned_us=I("assigned_us");j->progress.worker_received_us=I("worker_received_us");j->progress.worker_spawned_us=I("worker_spawned_us");j->progress.worker_ready_us=I("worker_ready_us");j->lifecycle.prewarmed=I("prewarmed");
#undef I
    j->progress.rendition_inflight_limit=limit;j->operations=naki_operations_open(256,2*1024*1024,1800000000);if(!j->operations)return -ENOMEM;
    j->watermark=naki_watermark_open(&j->config.watermark,accepted,&j->lifecycle.cancel);
    if(!j->watermark||naki_watermark_bind(j->watermark,j->operations,j->id,j->output_id,j->request_id,j->logger))return -EINVAL;
    j->watermark_snapshot=naki_watermark_status;j->lifecycle.state=NAKI_JOB_STATE_STARTING;naki_overload_init(&j->flow.overload);
    j->rendition_generation=generation;j->rendition_pid=getpid();return 0;
}
int naki_job_child_trace(Job *j){if(j->config.frame_log||j->config.network_trace||j->config.codec_trace){j->trace=logger_trace_open(j->logger,j->log_path);if(!j->trace)return -ENOMEM;}return 0;}
void naki_job_child_stopped(Job *j){if(naki_job_cancelled(j)){pthread_mutex_lock(&j->mutex);if(!j->lifecycle.branch_failed)j->lifecycle.state=NAKI_JOB_STATE_STOPPED;pthread_mutex_unlock(&j->mutex);}}
void naki_job_child_free(Job **job) {
    Job *j=*job;if(!j)return;
    if(j->allocation_kind!=NAKI_JOB_ALLOCATION_CHILD)abort();
    naki_watermark_close(&j->watermark);naki_operations_close(&j->operations);
    for(unsigned i=0;i<2;i++)if(j->progress.first_chain[i])yyjson_mut_doc_free(j->progress.first_chain[i]);
    if(j->destination_snapshot)yyjson_mut_doc_free(j->destination_snapshot);
    if(j->logger){logger_trace_release(j->logger,j->trace);logger_stop(j->logger);}
    if(j->wake_fd>=0)close(j->wake_fd);
    pthread_mutex_destroy(&j->mutex);pthread_mutex_destroy(&j->parent->mutex);free(j->parent);free(j);*job=NULL;
}
