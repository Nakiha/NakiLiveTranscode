#define _GNU_SOURCE
#include "rendition_runtime_internal.h"
#include "pipeline_internal.h"
#include "worker/job_remote.h"
#include "rendition_message.h"
#include "frame_wire.h"
#include "audio_wire.h"
#include "session_idle.h"
#include "platform/ipc.h"
#include "control/identity.h"
#include <spawn.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/eventfd.h>
extern char **environ;
int naki_renditions_create(Job *j) {
    NakiRenditions *m=calloc(1,sizeof(*m));if(!m)return -ENOMEM;m->job=j;m->initial_count=naki_job_config(j)->output_count;m->source_generation=1;m->desired_revision=m->effective_revision=1;
    if(pthread_mutex_init(&m->mutex,NULL)){free(m);return -ENOMEM;}
    naki_job_bind_renditions(j,m);
    int slots=naki_job_remote_slots(j);if(slots<0)return slots;
    for(unsigned i=0;i<NAKI_OUTPUT_MAX;i++) {
        Rendition *r=&m->slots[i];r->control=r->media=r->completion=r->pidfd=-1;
        r->job=naki_job_output(j,i);
        if(i<m->initial_count){r->spec=naki_job_config(j)->outputs[i];r->member=r->attached=1;}
    }
    return naki_session_idle_init(&m->idle,naki_job_config(j)->idle_timeout_ms,m->initial_count,now_us());
}
static void property_copy(AVFrame *dst,const AVFrame *src) {
    *dst=(AVFrame){.width=src->width,.height=src->height,.format=src->format,.color_range=src->color_range,.colorspace=src->colorspace,
        .color_primaries=src->color_primaries,.color_trc=src->color_trc,.chroma_location=src->chroma_location,.sample_aspect_ratio=src->sample_aspect_ratio};
}
static yyjson_mut_doc *boot_message(Pipeline *p,Rendition *r) {
    NakiRenditions *m=naki_job_renditions(p->job);Job *j=p->job;
    yyjson_mut_doc *d=ipc_message("rendition_init");if(!d)return NULL;
    yyjson_doc *cfg=yyjson_read(r->config_json,strlen(r->config_json),0);if(!cfg){yyjson_mut_doc_free(d);return NULL;}
    yyjson_mut_val *configuration=yyjson_val_mut_copy(d,yyjson_doc_get_root(cfg));
    if(r->incarnations>1&&!strchr(r->spec.output,':')){char output[4096];int n=snprintf(output,sizeof(output),"%s.g%llu",r->spec.output,(unsigned long long)r->generation);if(n<0||(size_t)n>=sizeof(output)){yyjson_doc_free(cfg);yyjson_mut_doc_free(d);return NULL;}yyjson_mut_obj_remove_key(configuration,"output");yyjson_mut_obj_add_strcpy(d,configuration,"output",output);}
    yyjson_mut_obj_add_val(d,yyjson_mut_doc_get_root(d),"config",configuration);yyjson_doc_free(cfg);
    json_str(d,"job_id",naki_job_id(j));json_str(d,"request_id",naki_job_request_id(j));json_str(d,"output_id",r->spec.id);json_str(d,"session",naki_process_id());json_str(d,"rendition_instance",r->instance);
    char trace[4096]={0},event[4096]={0};int a=0,b=0;
    if(*naki_job_log_path(j))a=snprintf(trace,sizeof(trace),"%s.%s.g%llu",naki_job_log_path(j),r->spec.id,(unsigned long long)r->generation);
    const char *events=logger_event_prefix(naki_job_logger(j));
    if(*events)b=snprintf(event,sizeof(event),"%s.%s.g%llu",events,r->spec.id,(unsigned long long)r->generation);
    if(a<0||b<0||(size_t)a>=sizeof(trace)||(size_t)b>=sizeof(event)){yyjson_mut_doc_free(d);return NULL;}json_str(d,"log_path",trace);json_str(d,"event_prefix",event);
    json_int(d,"rendition_generation",r->generation);json_int(d,"source_generation",m->source_generation);json_int(d,"inflight_bytes",r->spec.inflight_bytes);
    json_int(d,"prepare_accepted_us",r->prepare_accepted_us);json_int(d,"io_timeout_ms",r->spec.io_timeout_ms);
    json_int(d,"video_pool",naki_pool_id(m->video));json_int(d,"audio_pool",naki_pool_id(m->audio));
#define I(k) json_int(d,#k,m->prototype.k)
    I(width);I(height);I(format);I(color_range);I(colorspace);I(color_primaries);I(color_trc);I(chroma_location);
#undef I
    json_int(d,"sar_num",m->prototype.sample_aspect_ratio.num);json_int(d,"sar_den",m->prototype.sample_aspect_ratio.den);
    json_int(d,"tb_num",p->input_tb.num);json_int(d,"tb_den",p->input_tb.den);json_int(d,"fps_num",p->video_rate.value.num);json_int(d,"fps_den",p->video_rate.value.den);json_int(d,"wrap_bits",p->input_wrap_bits);
    naki_job_remote_boot(j,d);
    return d;
}
void naki_rendition_fail(Rendition *r,int error,const char *why) {
    if(!r->pid||r->failed)return;
    Job *j=r->job;r->failed=1;r->stopping=1;r->stop_deadline_us=now_us()+2000000;
    naki_job_remote_failed(r->job,error,why);
    /* Stop new offers immediately. A bounded EOF gives already admitted
     * frames a chance to drain; native hangs still reach the pidfd deadline. */
    r->remove_draining=1;
    NakiRenditionMessage eof={.kind=NAKI_RMSG_EOF,.rendition_generation=r->generation};strcpy(eof.session,naki_process_id());strcpy(eof.instance,r->instance);
    if(!naki_rendition_send(r->media,&eof))r->eof_sent=1;
    unsigned retained=0;for(unsigned k=0;k<LEASES;k++)retained+=r->leases[k].used!=0;
    yyjson_mut_doc *event=new_event("rendition_failed",j);json_str(event,"stage",why);json_int(event,"error_code",error);json_int(event,"retained_raw_leases",retained);json_int(event,"rendition_pid",r->pid);json_str(event,"rendition_instance",r->instance);json_str(event,"input_session_instance",naki_process_id());logger_submit(naki_job_logger(j),NULL,event);
}
int naki_rendition_spawn(Pipeline *p,unsigned index) {
    NakiRenditions *m=naki_job_renditions(p->job);Rendition *r=&m->slots[index];
    if(r->pid||!r->config_json)return -EBUSY;
    if(m->next_instance_generation==UINT64_MAX)return -EOVERFLOW;
    r->prepare_accepted_us=m->transition.operation&&m->transition.slot==index?m->transition.accepted_us:naki_job_started_us(p->job);
    int sockets[3][2];for(unsigned i=0;i<3;i++)sockets[i][0]=sockets[i][1]=-1;
    int poolfd[2]={-1,-1},ret=0;posix_spawn_file_actions_t actions;int initialized=0;
    for(unsigned i=0;i<3;i++)if(socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_NONBLOCK|SOCK_CLOEXEC,0,sockets[i])){ret=-errno;goto done;}
    poolfd[0]=naki_pool_read_fd(m->video);poolfd[1]=naki_pool_read_fd(m->audio);if(poolfd[0]<0||poolfd[1]<0){ret=-EMFILE;goto done;}
    if(naki_id_generate(r->instance)){ret=-EIO;goto done;}
    r->local_thread=naki_job_config(p->job)->rendition_backend==2;
    if(r->local_thread){
        int fds[5]={sockets[0][1],sockets[1][1],sockets[2][1],poolfd[0],poolfd[1]};
        ret=naki_rendition_thread_start(fds,r->instance,&r->thread,&r->thread_done);if(ret)goto done;
        for(unsigned i=0;i<3;i++)sockets[i][1]=-1;
        poolfd[0]=poolfd[1]=-1;r->pid=getpid();r->pidfd=-1;
    }else{
        /* Move sources above the preserved range before constructing dup2. */
        int sources[5]={sockets[0][1],sockets[1][1],sockets[2][1],poolfd[0],poolfd[1]};
        for(unsigned i=0;i<5;i++){int fd=fcntl(sources[i],F_DUPFD_CLOEXEC,16);if(fd<0){ret=-errno;for(unsigned j=0;j<i;j++)close(sources[j]);goto done;}sources[i]=fd;}
        ret=posix_spawn_file_actions_init(&actions);if(ret){ret=-ret;for(unsigned i=0;i<5;i++)close(sources[i]);goto done;}initialized=1;
        for(unsigned i=0;i<5&&!ret;i++)ret=posix_spawn_file_actions_adddup2(&actions,sources[i],3+(int)i);
        if(!ret)ret=posix_spawn_file_actions_addclosefrom_np(&actions,8);
        char executable[4096];ssize_t bytes=readlink("/proc/self/exe",executable,sizeof(executable)-1);
        if(bytes<0)ret=errno;else executable[bytes]=0;
        char parent[32];snprintf(parent,sizeof(parent),"%d",getpid());char *args[]={executable,"--rendition",parent,r->instance,NULL};
        if(!ret)ret=posix_spawn(&r->pid,executable,&actions,NULL,args,environ);
        for(unsigned i=0;i<5;i++)close(sources[i]);
        if(ret){ret=-ret;goto done;}
        r->pidfd=(int)syscall(SYS_pidfd_open,r->pid,0);
        if(r->pidfd<0){int error=errno;/* Own unreaped spawn, no reuse. */kill(r->pid,SIGKILL);waitpid(r->pid,NULL,0);r->pid=0;ret=-error;goto done;}
    }
    r->control=sockets[0][0];sockets[0][0]=-1;r->media=sockets[1][0];sockets[1][0]=-1;r->completion=sockets[2][0];sockets[2][0]=-1;
    r->prepare_source_generation=m->source_generation;
    r->generation=++m->next_instance_generation;r->incarnations++;r->watermark_cached_revision=0;
    naki_job_remote_spawned(r->job,r->pid,r->generation);
    r->prepared=r->finished=r->failed=r->stopping=r->eof_sent=r->remove_draining=0;r->stop_deadline_us=0;
    r->prepared_deadline_us=naki_rendition_prepare_deadline(&r->spec,r->prepare_accepted_us);
    ret=r->local_thread?naki_pool_join_local(m->video,index,r->generation,NAKI_RENDITION_VIDEO_CREDIT):naki_pool_join(m->video,index,r->generation,NAKI_RENDITION_VIDEO_CREDIT,r->pidfd);
    if(!ret)ret=r->local_thread?naki_pool_join_local(m->audio,index,r->generation,NAKI_RENDITION_AUDIO_CREDIT):naki_pool_join(m->audio,index,r->generation,NAKI_RENDITION_AUDIO_CREDIT,r->pidfd);
    yyjson_mut_doc *boot=ret?NULL:boot_message(p,r);if(!boot&&!ret)ret=-ENOMEM;
    if(boot){if(ipc_send(r->control,boot))ret=-errno;yyjson_mut_doc_free(boot);}
    if(ret)naki_rendition_fail(r,ret,"rendition preparation");
done:
    if(initialized)posix_spawn_file_actions_destroy(&actions);
    for(unsigned i=0;i<3;i++)for(unsigned j=0;j<2;j++)if(sockets[i][j]>=0)close(sockets[i][j]);
    for(unsigned i=0;i<2;i++)if(poolfd[i]>=0)close(poolfd[i]);
    return ret;
}
void naki_rendition_cache_desired(Rendition *r,yyjson_val *desired,uint64_t revision){
    if(!yyjson_is_obj(desired)||!revision||revision<=r->watermark_cached_revision)return;
    NakiWatermarkConfig value;char error[256];if(naki_watermark_config(desired,&value,error,sizeof(error)))return;
    yyjson_doc *config=yyjson_read(r->config_json,strlen(r->config_json),0);yyjson_mut_doc *updated=config?yyjson_doc_mut_copy(config,NULL):NULL;
    if(updated){yyjson_mut_val *root=yyjson_mut_doc_get_root(updated);yyjson_mut_obj_remove_key(root,"watermark");yyjson_mut_obj_add_val(updated,root,"watermark",yyjson_val_mut_copy(updated,desired));
        char *text=yyjson_mut_write(updated,0,NULL);if(text){free(r->config_json);r->config_json=text;r->watermark_cached_revision=revision;}yyjson_mut_doc_free(updated);}
    if(config)yyjson_doc_free(config);
}
static void project(Rendition *r,yyjson_val *snapshot) {
    if(naki_job_remote_project(r->job,snapshot,!r->failed&&!r->stopping)<0)return;
    yyjson_val *component=yyjson_obj_get(yyjson_obj_get(snapshot,"components"),"watermark");
    naki_rendition_cache_desired(r,yyjson_obj_get(component,"desired"),yyjson_get_uint(yyjson_obj_get(component,"desired_revision")));
}
static int same_token(const NakiPoolToken *a,const NakiPoolToken *b){return a->pool_id==b->pool_id&&a->slot==b->slot&&a->generation==b->generation&&a->bytes==b->bytes;}
static int poll_locked(Pipeline *p,int finishing) {
    NakiRenditions *m=naki_job_renditions(p->job);if(!m)return -EINVAL;
    int64_t now=now_us();
    for(unsigned i=0;i<NAKI_OUTPUT_MAX;i++) {
        Rendition *r=&m->slots[i];if(!r->pid)continue;int completion_closed=0;
        for(unsigned n=0;n<128;n++) {
            NakiRenditionMessage message;int ret=naki_rendition_receive(r->completion,&message);if(ret==-EAGAIN||ret==-EWOULDBLOCK)break;
            if(ret<0){if(ret==-EPIPE||ret==-ECONNRESET)completion_closed=ret;else if(!r->finished)naki_rendition_fail(r,ret,"completion channel");break;}
            if(strcmp(message.session,naki_process_id())||strcmp(message.instance,r->instance)||message.rendition_generation!=r->generation){naki_rendition_fail(r,-EPROTO,"stale completion identity");break;}
            Lease *lease=NULL;for(unsigned k=0;k<LEASES;k++)if(r->leases[k].used&&same_token(&r->leases[k].message.token,&message.token)&&r->leases[k].message.media==message.media){lease=&r->leases[k];break;}
            if(!lease){naki_rendition_fail(r,-ESTALE,"unknown or duplicate raw completion");break;}
            if(message.kind==NAKI_RMSG_ACCEPTED) {
                if(!lease->queued){naki_rendition_fail(r,-ESTALE,"duplicate accepted completion");break;}
                lease->queued=0;r->queued_work_us-=lease->work_us;r->queued_bytes-=message.token.bytes;if(message.media==NAKI_RMSG_VIDEO)r->queued_video--;else r->queued_audio--;
            } else if(message.kind==NAKI_RMSG_RELEASE) {
                if(lease->queued){naki_rendition_fail(r,-EPROTO,"release before admission");break;}
                ret=naki_pool_release(message.media==NAKI_RMSG_VIDEO?m->video:m->audio,&message.token,i,r->generation);
                if(ret<0){naki_rendition_fail(r,ret,"raw lease release");break;}lease->used=0;
            } else {naki_rendition_fail(r,-EPROTO,"invalid completion kind");break;}
        }
        for(unsigned n=0;n<16;n++) {
            yyjson_doc *doc=NULL;int ret=ipc_receive(r->control,&doc);if(!ret)break;
            if(ret<0){if(!r->finished)naki_rendition_fail(r,ret,"rendition control disconnected");break;}
            yyjson_val *root=yyjson_doc_get_root(doc);const char *instance=yyjson_get_str(yyjson_obj_get(root,"rendition_instance"));
            if(instance&&strcmp(instance,r->instance))naki_rendition_fail(r,-EPROTO,"rendition instance mismatch");
            else if(ipc_kind(doc,"rendition_ready")){int error=yyjson_get_sint(yyjson_obj_get(root,"error"));if(error<0){const char *why=yyjson_get_str(yyjson_obj_get(root,"error_reason"));naki_rendition_fail(r,error,why?why:"rendition prepare failed");}else r->prepared=1;}
            else if(ipc_kind(doc,"rendition_status")||ipc_kind(doc,"rendition_finished")) {
                yyjson_val *snapshot=yyjson_obj_get(root,"snapshot");if(yyjson_is_obj(snapshot)) {
                    project(r,snapshot);naki_rendition_written(p,r,snapshot);
                    yyjson_val *progress=yyjson_obj_get(snapshot,"progress");yyjson_val *age=yyjson_obj_get(progress,"oldest_video_inflight_age_ms");
                    if(yyjson_is_num(age)&&yyjson_get_num(age)>r->spec.max_lag_ms)naki_rendition_fail(r,-ETIMEDOUT,"private frame delivery deadline");
                    const char *state=yyjson_get_str(yyjson_obj_get(snapshot,"state"));if(state&&!strcmp(state,"failed"))naki_rendition_fail(r,-EIO,"rendition media failed");
                }
                if(ipc_kind(doc,"rendition_finished")){r->finished=1;if(!r->stop_deadline_us)r->stop_deadline_us=now+2000000;yyjson_mut_doc *ack=ipc_message("rendition_ack");json_str(ack,"rendition_instance",r->instance);ipc_send(r->control,ack);yyjson_mut_doc_free(ack);}
            }
            else if(ipc_kind(doc,"rendition_history")) {naki_rendition_cache_desired(r,yyjson_obj_get(root,"watermark_desired"),yyjson_get_uint(yyjson_obj_get(root,"watermark_desired_revision")));size_t x,n;yyjson_val *record;yyjson_arr_foreach(yyjson_obj_get(root,"operations"),x,n,record)naki_rendition_observe(p,r,record);}
            else if(ipc_kind(doc,"rendition_operation")) {
                naki_rendition_cache_desired(r,yyjson_obj_get(root,"watermark_desired"),yyjson_get_uint(yyjson_obj_get(root,"watermark_desired_revision")));
                yyjson_val *record=yyjson_obj_get(root,"operation");if(yyjson_is_obj(record))naki_rendition_observe(p,r,record);
                else {const char *id=yyjson_get_str(yyjson_obj_get(root,"operation_id"));NakiOperationHandle op;if(id&&!naki_operations_lookup(naki_job_operations(p->job),naki_job_id(p->job),id,&op))naki_operations_transition(naki_job_operations(p->job),op,NAKI_OP_FAILED,1,yyjson_get_int(yyjson_obj_get(root,"error")),"rendition update rejected",now_us());}
            }
            yyjson_doc_free(doc);
        }
        if(completion_closed&&!r->finished&&!r->failed)naki_rendition_fail(r,completion_closed,"completion channel closed");
        if(!r->prepared&&now>=r->prepared_deadline_us)naki_rendition_fail(r,-ETIMEDOUT,"rendition prepare deadline");
        if(!r->stopping)for(unsigned k=0;k<LEASES;k++)if(r->leases[k].used&&now-r->leases[k].message.ingress_us>(int64_t)r->spec.max_lag_ms*1000){naki_rendition_fail(r,-ETIMEDOUT,"raw frame retention deadline");break;}
        if((naki_job_cancelled(p->job)||finishing)&&!r->stopping&&!r->finished){r->stopping=1;r->stop_deadline_us=now+2000000;yyjson_mut_doc *d=ipc_message("rendition_stop");json_str(d,"rendition_instance",r->instance);ipc_send(r->control,d);yyjson_mut_doc_free(d);}
        if(r->stop_deadline_us&&now>=r->stop_deadline_us){naki_rendition_fail(r,-ETIMEDOUT,"rendition exit proof deadline");if(!r->local_thread)(void)syscall(SYS_pidfd_send_signal,r->pidfd,SIGKILL,NULL,0);}
        int exited=0,status=0;
        if(r->local_thread){
            if(atomic_load(&r->thread_done)&&!pthread_tryjoin_np(r->thread,NULL))exited=1;
        }else{
            struct pollfd fd={.fd=r->pidfd,.events=POLLIN};
            if(poll(&fd,1,0)>0&&(fd.revents&POLLIN)&&waitpid(r->pid,&status,WNOHANG)==r->pid)exited=1;
        }
        if(exited) {
            if(!r->finished&&!r->stopping)naki_rendition_fail(r,-EPIPE,"rendition exited without final media state");
            naki_job_remote_exit_observed(r->job,WIFEXITED(status)?WEXITSTATUS(status):-1,WIFSIGNALED(status)?WTERMSIG(status):0);
            int a=r->local_thread?naki_pool_reap_local_joined(m->video,i,r->generation):naki_pool_reap(m->video,i,r->generation),b=r->local_thread?naki_pool_reap_local_joined(m->audio,i,r->generation):naki_pool_reap(m->audio,i,r->generation);if(a&&a!=-ESTALE)return a;if(b&&b!=-ESTALE)return b;
            memset(r->leases,0,sizeof(r->leases));r->queued_audio=r->queued_video=0;r->queued_bytes=r->queued_work_us=0;
            naki_job_remote_retired(r->job,r->failed,r->stopping,now);
            if(r->pidfd>=0)close(r->pidfd);
            close(r->control);close(r->media);close(r->completion);r->pidfd=r->control=r->media=r->completion=-1;r->pid=0;
            naki_rendition_reaped(p,i);
        }
    }
    return 0;
}
int naki_renditions_open(Pipeline *p) {
    NakiRenditions *m=naki_job_renditions(p->job);if(!m)return -EINVAL;
    for(unsigned i=0;i<m->initial_count;i++)if(naki_job_config(p->job)->rendition_backend==1&&!strncmp(m->slots[i].spec.output,"srt://",6)){p->stage="process holder v1 denies sendmsg; SRT requires legacy/thread backend";return -ENOTSUP;}
    AVFrame properties=pipeline_input_properties(p);
    if(properties.format!=AV_PIX_FMT_YUV420P&&properties.format!=AV_PIX_FMT_YUV420P10LE){p->stage="shared frame v2 supports planar YUV420 8/10-bit only";return -ENOTSUP;}
    int ret=naki_pool_open(&m->video,1+NAKI_OUTPUT_MAX*NAKI_RENDITION_VIDEO_CREDIT,naki_frame_wire_capacity(NAKI_INPUT_MAX_PIXELS),UINT64_C(4)*1024*1024*1024);
    if(!ret)ret=naki_pool_open(&m->audio,1+NAKI_OUTPUT_MAX*NAKI_RENDITION_AUDIO_CREDIT,NAKI_AUDIO_WIRE_CAPACITY,UINT64_C(640)*1024*1024);
    if(ret<0)return ret;
    property_copy(&m->prototype,&properties);
    yyjson_doc *public=yyjson_read(naki_job_public_config(p->job),strlen(naki_job_public_config(p->job)),0);if(!public)return -EINVAL;yyjson_val *outputs=yyjson_obj_get(yyjson_doc_get_root(public),"outputs");
    for(unsigned i=0;i<m->initial_count;i++){Rendition *r=&m->slots[i];char error[256]={0};r->config_json=naki_rendition_config_json(naki_job_public_config(p->job),strlen(naki_job_public_config(p->job)),yyjson_arr_get(outputs,i),error,sizeof(error));if(!r->config_json){ret=-EINVAL;break;}ret=naki_rendition_spawn(p,i);if(ret<0)break;}
    yyjson_doc_free(public);if(ret<0)return ret;
    for(;;){unsigned waiting=0,ready=0;ret=naki_renditions_poll(p,0);if(ret<0)return ret;for(unsigned i=0;i<m->initial_count;i++){Rendition *r=&m->slots[i];if(r->prepared&&!r->failed)ready++;else if(r->pid&&!r->failed)waiting++;}if(!waiting){pthread_mutex_lock(&m->mutex);m->ready=ready>0;pthread_mutex_unlock(&m->mutex);return ready?0:-EPIPE;}if(naki_job_cancelled(p->job))return AVERROR_EXIT;struct pollfd fd={.fd=naki_job_wake_fd(p->job),.events=POLLIN};poll(&fd,1,5);}
}
static int available(const Rendition *r,int audio) {
    if(!r->attached||!r->pid||!r->prepared||r->failed||r->stopping||r->finished)return 0;
    unsigned retained=0;for(unsigned i=0;i<LEASES;i++)if(r->leases[i].used&&r->leases[i].message.media==(audio?NAKI_RMSG_AUDIO:NAKI_RMSG_VIDEO))retained++;
    return retained<(audio?NAKI_RENDITION_AUDIO_CREDIT:NAKI_RENDITION_VIDEO_CREDIT)&&(audio?r->queued_audio<(unsigned)r->spec.audio_packets:r->queued_video<(unsigned)r->spec.queue_items);
}
int naki_renditions_capacity(Pipeline *p){NakiRenditions *m=naki_job_renditions(p->job);unsigned live=0;for(unsigned i=0;i<NAKI_OUTPUT_MAX;i++){Rendition *r=&m->slots[i];if(available(r,0))return 1;if(r->attached&&r->pid&&!r->failed&&!r->stopping)live++;}return !live;}
static int distribute(Pipeline *p,const NakiPoolToken *token,int audio,uint64_t frame_id,int64_t ingress,uint64_t work_us) {
    NakiRenditions *m=naki_job_renditions(p->job);NakiFramePool *pool=audio?m->audio:m->video;
    for(unsigned i=0;i<NAKI_OUTPUT_MAX;i++) {
        Rendition *r=&m->slots[i];if(!r->attached||!r->pid||!r->prepared||r->failed||r->stopping||r->finished)continue;if(audio&&(!r->spec.audio_copy||!strcmp(r->spec.output_format,"mp4")))continue;
        if(!available(r,audio)||r->queued_bytes>r->spec.queue_bytes||token->bytes>r->spec.queue_bytes-r->queued_bytes||r->queued_work_us>(uint64_t)r->spec.queue_media_ms*1000||work_us>(uint64_t)r->spec.queue_media_ms*1000-r->queued_work_us){naki_rendition_fail(r,-ENOBUFS,"independent rendition admission budget");continue;}
        unsigned k;for(k=0;k<LEASES;k++)if(!r->leases[k].used)break;if(k==LEASES){naki_rendition_fail(r,-ENOBUFS,"raw lease ledger");continue;}
        NakiRenditionMessage message={.kind=audio?NAKI_RMSG_AUDIO:NAKI_RMSG_VIDEO,.media=audio?NAKI_RMSG_AUDIO:NAKI_RMSG_VIDEO,.rendition_generation=r->generation,.source_generation=m->source_generation,.frame_id=frame_id,.ingress_us=ingress,.token=*token};strcpy(message.session,naki_process_id());strcpy(message.instance,r->instance);
        int ret=naki_pool_offer(pool,token,i,r->generation);if(!ret)ret=naki_rendition_send(r->media,&message);
        if(ret<0){/* No descriptor was queued on send failure; release the local offer. */naki_pool_release(pool,token,i,r->generation);naki_rendition_fail(r,ret,"nonblocking media offer");continue;}
        r->leases[k]=(Lease){.used=1,.queued=1,.work_us=work_us,.message=message};if(audio)r->queued_audio++;else r->queued_video++;r->queued_bytes+=token->bytes;r->queued_work_us+=work_us;
    }
    return naki_pool_producer_release(pool,token);
}
static int frame_locked(Pipeline *p,AVFrame *frame) {
    NakiRenditions *m=naki_job_renditions(p->job);int ret=naki_job_config(p->job)->decoder_pump?0:pipeline_prepare_decoded_frame(p,frame);if(ret<0)return ret;
    Stamp *s=(Stamp *)frame->opaque_ref->data;
    if((ret=pipeline_reader_epoch_consumed(p,s->sequence))<0)return ret;
    s->source_rate_bps=naki_job_config(p->job)->source_rate_limit?pipeline_source_rate(p,now_us()):0;
    if(s->decoder_tracked){if(!naki_policy_untrack(p->job,NAKI_TRACK_FRAME,(uint64_t)s->frame_id))return AVERROR_INVALIDDATA;s->decoder_tracked=0;}
    naki_job_shared_decoded(p->job,s,0);
    if(frame->width!=m->prototype.width||frame->height!=m->prototype.height||frame->format!=m->prototype.format||frame->colorspace!=m->prototype.colorspace||frame->color_range!=m->prototype.color_range||frame->color_primaries!=m->prototype.color_primaries||frame->color_trc!=m->prototype.color_trc||frame->chroma_location!=m->prototype.chroma_location||frame->sample_aspect_ratio.num!=m->prototype.sample_aspect_ratio.num||frame->sample_aspect_ratio.den!=m->prototype.sample_aspect_ratio.den){if(m->source_generation==UINT64_MAX)return -EOVERFLOW;m->source_generation++;property_copy(&m->prototype,frame);}
    m->last_frame=pipeline_frame_identity(p,s,0,(AVRational){0,1});
    if((ret=naki_rendition_attach(p,frame))<0)return ret;
    unsigned live=0;for(unsigned i=0;i<NAKI_OUTPUT_MAX;i++){Rendition *r=&m->slots[i];live+=r->attached&&r->pid&&r->prepared&&!r->failed&&!r->stopping&&!r->finished;}if(!live)return 0;
    NakiPoolToken token;uint8_t *data;ret=naki_pool_acquire(m->video,&token,&data);if(ret<0)return ret;size_t bytes=0;
    ret=naki_frame_wire_pack(frame,NAKI_INPUT_MAX_PIXELS,data,naki_pool_stride(m->video),&bytes);if(!ret)ret=naki_pool_publish(m->video,&token,bytes);
    if(ret<0){naki_pool_producer_release(m->video,&token);return ret;}
    return distribute(p,&token,0,(uint64_t)s->frame_id,s->ingress_us,(uint64_t)p->video_rate.nominal_frame_work_us);
}
static int audio_locked(Pipeline *p,const InputItem *audio) {
    NakiRenditions *m=naki_job_renditions(p->job);unsigned interested=0,live=0,ready=0;
    for(unsigned i=0;i<NAKI_OUTPUT_MAX;i++){Rendition *r=&m->slots[i];if(!r->attached||!r->pid||!r->prepared||r->failed||r->stopping||r->finished)continue;live++;if(r->spec.audio_copy&&strcmp(r->spec.output_format,"mp4")){interested++;ready+=available(r,1);}}
    if(!interested)return 0;
    if(!ready&&interested==live)return AVERROR(EAGAIN);
    NakiPoolToken token;uint8_t *data;int ret=naki_pool_acquire(m->audio,&token,&data);if(ret<0)return ret;size_t bytes=0;
    const char *previous_stage=p->stage;p->stage="rendition audio wire";
    ret=naki_audio_wire_pack(audio,data,naki_pool_stride(m->audio),&bytes);if(!ret)ret=naki_pool_publish(m->audio,&token,bytes);
    if(ret<0){naki_pool_producer_release(m->audio,&token);return ret;}
    p->stage=previous_stage;
    return distribute(p,&token,1,(uint64_t)audio->demuxed_us,audio->demuxed_us,(uint64_t)FFMAX(1,av_rescale_q(audio->packet->duration,audio->tb,AV_TIME_BASE_Q)));
}
int naki_renditions_finish(Pipeline *p,int stopped) {
    NakiRenditions *m=naki_job_renditions(p->job);if(!m)return 0;pthread_mutex_lock(&m->mutex);m->finishing=1;naki_rendition_control_tick(p);pthread_mutex_unlock(&m->mutex);int64_t until=now_us()+15000000;int closing_error=0;
    for(;;) {
        unsigned live=0;for(unsigned i=0;i<NAKI_OUTPUT_MAX;i++){Rendition *r=&m->slots[i];if(!r->pid)continue;live++;if(!stopped&&!r->stopping&&!r->eof_sent){NakiRenditionMessage eof={.kind=NAKI_RMSG_EOF,.rendition_generation=r->generation};strcpy(eof.session,naki_process_id());strcpy(eof.instance,r->instance);if(!naki_rendition_send(r->media,&eof))r->eof_sent=1;}if(now_us()>=until)naki_rendition_fail(r,-ETIMEDOUT,"rendition terminal drain deadline");}
        if(!live){
            unsigned desired=0,completed=0;for(unsigned i=0;i<NAKI_OUTPUT_MAX;i++){desired+=m->slots[i].member!=0;completed+=m->slots[i].member&&naki_job_state(m->slots[i].job)==NAKI_JOB_STATE_COMPLETED;}
            return closing_error?closing_error:!stopped&&desired&&!completed?AVERROR(EPIPE):0;
        }
        int ret=naki_renditions_poll(p,stopped);if(ret<0){if(!closing_error)closing_error=ret;for(unsigned i=0;i<NAKI_OUTPUT_MAX;i++)naki_rendition_fail(&m->slots[i],ret,"terminal completion protocol failed");}
        if(now_us()>=until+3000000){
            /* Keep admission closed while a holder cannot be proven dead. An
             * explicit whole-service stop still has its supervisor deadline. */
            naki_job_quarantine(p->job);
        }
        poll(NULL,0,5);
    }
    return 0;
}
void naki_renditions_close(Job *job) {
    NakiRenditions *m=naki_job_renditions(job);if(!m)return;
    for(unsigned i=0;i<NAKI_OUTPUT_MAX;i++)if(m->slots[i].pid)return;
    if(naki_pool_close(&m->video)||naki_pool_close(&m->audio))return;
    for(unsigned i=0;i<NAKI_OUTPUT_MAX;i++)free(m->slots[i].config_json);
    free(m->transition.config_json);
    pthread_mutex_destroy(&m->mutex);free(m);naki_job_bind_renditions(job,NULL);
}

int naki_renditions_poll(Pipeline *p,int finishing) {
    NakiRenditions *m=naki_job_renditions(p->job);if(!m)return -EINVAL;pthread_mutex_lock(&m->mutex);
    int ret=poll_locked(p,finishing);if(ret>=0&&m->ready)ret=naki_rendition_control_tick(p);naki_rendition_publish(p);if(ret>=0&&m->idle_expired&&!m->finishing)ret=AVERROR_EOF;
    pthread_mutex_unlock(&m->mutex);return ret;
}
int naki_renditions_frame(Pipeline *p,AVFrame *frame) {
    NakiRenditions *m=naki_job_renditions(p->job);pthread_mutex_lock(&m->mutex);int ret=frame_locked(p,frame);pthread_mutex_unlock(&m->mutex);return ret;
}
int naki_renditions_audio(Pipeline *p,const InputItem *audio) {
    NakiRenditions *m=naki_job_renditions(p->job);pthread_mutex_lock(&m->mutex);int ret=audio_locked(p,audio);pthread_mutex_unlock(&m->mutex);return ret;
}
