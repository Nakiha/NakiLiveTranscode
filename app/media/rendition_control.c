#include "rendition_runtime_internal.h"
#include "control/intent.h"
#include "worker/job_remote.h"
static const char *actions[]={"watermark","add_rendition","remove_rendition","restart_rendition","add_destination","remove_destination","request_random_access"};
static unsigned members(NakiRenditions *m){unsigned n=0;for(unsigned i=0;i<NAKI_OUTPUT_MAX;i++)n+=m->slots[i].member!=0;return n;}
static int reject(char *error,size_t cap,int code,const char *reason){snprintf(error,cap,"%s",reason);return code;}
static int candidate(Job *job,yyjson_val *spec,Config *config,OutputConfig *output,char **json,char *error,size_t cap) {
    yyjson_doc *source=yyjson_read(naki_job_public_config(job),strlen(naki_job_public_config(job)),0);yyjson_mut_doc *d=source?yyjson_mut_doc_new(NULL):NULL;int ret=-EINVAL;
    if(!d)goto done;
    yyjson_mut_val *root=yyjson_val_mut_copy(d,yyjson_doc_get_root(source)),*list=yyjson_mut_arr(d);yyjson_mut_doc_set_root(d,root);
    yyjson_mut_obj_remove_key(root,"outputs");yyjson_mut_obj_remove_key(root,"output");yyjson_mut_obj_remove_key(root,"watermark");
    yyjson_mut_arr_add_val(list,yyjson_val_mut_copy(d,spec));yyjson_mut_obj_add_val(d,root,"outputs",list);
    size_t bytes;char *text=yyjson_mut_write(d,0,&bytes);if(!text){ret=-ENOMEM;goto done;}
    ret=config_parse(text,bytes,config,error,cap);free(text);if(ret)goto done;
    *output=config->outputs[0];
    *json=naki_rendition_config_json(naki_job_public_config(job),strlen(naki_job_public_config(job)),spec,error,cap);if(!*json)ret=-EINVAL;
done:if(d)yyjson_mut_doc_free(d);if(source)yyjson_doc_free(source);return ret;
}
int naki_renditions_submit(Job *job,yyjson_val *request,const char *origin,yyjson_mut_doc **result,char *error,size_t cap) {
    *result=NULL;NakiUpdateRequest u;if(!naki_id_valid(origin,128)||naki_update_parse(request,naki_job_id(job),&u,error,cap))return -EINVAL;
    NakiRenditions *m=naki_job_renditions(job);if(!m)return reject(error,cap,-ENOTSUP,"rendition_runtime_required");
    NakiOperationIntent in={.kind=u.action?NAKI_OP_TOPOLOGY:NAKI_OP_UPDATE,.job_id=naki_job_id(job),.operation_id=u.operation_id,.output_id=u.output_id,
        .fingerprint=u.fingerprint,.origin_request_id=origin,.expected_revision=u.expected_revision,.revision=u.expected_revision+1,
        .component=u.action?(u.action==NAKI_DESTINATION_ADD||u.action==NAKI_DESTINATION_REMOVE?"destination":"rendition"):"watermark",.action=u.action?actions[u.action]:NULL,.destination_id=*u.destination_id?u.destination_id:NULL};
    int ret=naki_operations_replay(naki_job_operations(job),&in,result);if(ret!=-ENOENT)return ret;
    pthread_mutex_lock(&m->mutex);
    ret=-ESHUTDOWN;int64_t now=now_us();
    if(!m->ready||m->finishing||naki_job_cancelled(job)||m->idle_expired||(m->idle.state==NAKI_SESSION_IDLE&&now>=m->idle.deadline_us)){reject(error,cap,ret,"session_not_ready_or_closing");goto done;}
    unsigned slot=NAKI_OUTPUT_MAX;
    for(unsigned i=0;i<NAKI_OUTPUT_MAX;i++)if(!strcmp(m->slots[i].spec.id,u.output_id)&&*u.output_id){slot=i;break;}
    if(!u.action||u.action>=NAKI_DESTINATION_ADD) {
        if(slot==NAKI_OUTPUT_MAX||!m->slots[slot].member){ret=reject(error,cap,-ENOENT,"rendition_not_found");goto done;}
        Rendition *r=&m->slots[slot];
        if(!u.expected_generation||u.expected_generation!=r->generation){ret=reject(error,cap,-ESTALE,"expected_generation_required_or_stale");goto done;}
        if(!r->prepared||r->failed||r->stopping||!r->pid){ret=reject(error,cap,-EPIPE,"rendition_not_active");goto done;}
        if(u.action){
            if(m->transition.operation){ret=reject(error,cap,-EAGAIN,"topology_transition_in_progress");goto done;}
            if(u.expected_revision!=m->desired_revision){ret=reject(error,cap,-ESTALE,"topology_revision_conflict");goto done;}
            ret=naki_destinations_validate(naki_job_config(r->job),&u,error,cap);if(ret<0)goto done;
        }
        if(m->command_count==8){ret=reject(error,cap,-ENOSPC,"rendition_command_capacity");goto done;}
        size_t bytes;char *text=yyjson_val_write(request,0,&bytes);yyjson_doc *copy=text?yyjson_read(text,bytes,0):NULL;free(text);if(!copy){ret=-ENOMEM;goto done;}
        NakiOperationHandle op;ret=naki_operations_reserve(naki_job_operations(job),&in,now,&op);if(ret!=1){yyjson_doc_free(copy);if(!ret)*result=naki_operations_get(naki_job_operations(job),op);goto done;}
        if(u.action)m->desired_revision++;
        unsigned at=(m->command_head+m->command_count++)%8;m->commands[at].request=copy;m->commands[at].operation=op;m->commands[at].slot=slot;m->commands[at].generation=r->generation;strcpy(m->commands[at].origin,origin);
        *result=naki_operations_get(naki_job_operations(job),op);ret=0;wake_job(job);goto done;
    }
    if(m->transition.operation){ret=reject(error,cap,-EAGAIN,"topology_transition_in_progress");goto done;}
    if(u.expected_revision!=m->desired_revision){ret=reject(error,cap,-ESTALE,"topology_revision_conflict");goto done;}
    Config cfg={0};OutputConfig spec={0};char *json=NULL;
    if(u.action==NAKI_RENDITION_ADD) {
        if(slot!=NAKI_OUTPUT_MAX){ret=reject(error,cap,-EEXIST,"rendition_id_reserved");goto done;}
        if(!naki_json_id(yyjson_obj_get(u.spec,"id"),32)||strcmp(yyjson_get_str(yyjson_obj_get(u.spec,"id")),u.output_id)){ret=reject(error,cap,-EINVAL,"spec_id_must_match_target");goto done;}
        ret=candidate(job,u.spec,&cfg,&spec,&json,error,cap);if(ret<0)goto done;
        if(naki_job_config(job)->rendition_backend==1&&!strncmp(spec.output,"srt://",6)){free(json);ret=reject(error,cap,-ENOTSUP,"process_holder_sendmsg_unavailable_for_SRT; use thread backend");goto done;}
        unsigned threads=(unsigned)spec.encoder_threads;
        for(unsigned i=0;i<NAKI_OUTPUT_MAX;i++){Rendition *r=&m->slots[i];if(*r->spec.id){threads+=r->spec.encoder_threads;if(!strcmp(r->spec.output,spec.output)){free(json);ret=reject(error,cap,-EEXIST,"duplicate_destination");goto done;}}else if(!r->pid&&slot==NAKI_OUTPUT_MAX)slot=i;}
        if(slot==NAKI_OUTPUT_MAX||threads>16){free(json);ret=reject(error,cap,-ENOSPC,"rendition_process_or_thread_budget");goto done;}
    } else {
        if(slot==NAKI_OUTPUT_MAX||!m->slots[slot].member){ret=reject(error,cap,-ENOENT,"rendition_not_found");goto done;}
        Rendition *r=&m->slots[slot];if(u.expected_generation&&u.expected_generation!=r->generation){ret=reject(error,cap,-ESTALE,"rendition_generation_conflict");goto done;}
        if(u.action==NAKI_RENDITION_RESTART&&!r->failed){ret=reject(error,cap,-EINVAL,"manual_restart_requires_failed_rendition");goto done;}
    }
    NakiOperationHandle op;ret=naki_operations_reserve(naki_job_operations(job),&in,now,&op);if(ret!=1){free(json);if(!ret)*result=naki_operations_get(naki_job_operations(job),op);goto done;}
    m->desired_revision++;
    m->transition.accepted_us=now;m->transition.deadline_us=now+12000000;m->transition.action=u.action;m->transition.slot=slot;m->transition.operation=op;m->transition.revision=m->desired_revision;m->transition.config=cfg;m->transition.spec=spec;m->transition.config_json=json;
    if(u.action!=NAKI_RENDITION_REMOVE)m->transition.deadline_us=naki_rendition_prepare_deadline(u.action==NAKI_RENDITION_ADD?&spec:&m->slots[slot].spec,now);
    naki_operations_transition(naki_job_operations(job),op,NAKI_OP_PREPARING,0,0,NULL,now);
    *result=naki_operations_get(naki_job_operations(job),op);ret=0;wake_job(job);
done:
    pthread_mutex_unlock(&m->mutex);return ret;
}
static void transition_clear(NakiRenditions *m){free(m->transition.config_json);memset(&m->transition,0,sizeof(m->transition));}
static void transition_fail(NakiRenditions *m,int code,const char *why){naki_operations_transition(naki_job_operations(m->job),m->transition.operation,NAKI_OP_FAILED,1,code,why,now_us());transition_clear(m);}
static void stop_child(Rendition *r,int drain) {
    if(!r->pid)return;
    r->stopping=1;r->remove_draining=drain;r->stop_deadline_us=now_us()+2000000;
    if(drain){r->eof_sent=0;return;}
    yyjson_mut_doc *d=ipc_message("rendition_stop");json_str(d,"rendition_instance",r->instance);ipc_send(r->control,d);yyjson_mut_doc_free(d);
}
void naki_rendition_reaped(Pipeline *p,unsigned slot) {
    NakiRenditions *m=naki_job_renditions(p->job);Rendition *r=&m->slots[slot];
    for(unsigned k=0;k<256;k++)if(r->remote_operations[k]){naki_operations_holder_exited(naki_job_operations(p->job),r->remote_operations[k],now_us());r->remote_operations[k]=0;}
    if(r->attach_operation){naki_operations_holder_exited(naki_job_operations(p->job),r->attach_operation,now_us());r->attach_operation=0;}
    if(r->remove_operation){naki_operations_transition(naki_job_operations(p->job),r->remove_operation,NAKI_OP_PREPARED,0,0,NULL,now_us());naki_operations_transition(naki_job_operations(p->job),r->remove_operation,NAKI_OP_APPLYING,1,0,NULL,now_us());naki_operations_applied(naki_job_operations(p->job),r->remove_operation,&r->detach_identity,0,now_us());naki_operations_retire(naki_job_operations(p->job),r->remove_operation,now_us());r->remove_operation=0;}
    if(r->removed||!r->member){naki_job_remote_forget(r->job);memset(&r->spec,0,sizeof(r->spec));free(r->config_json);r->config_json=NULL;r->removed=0;}
}
static int subscriptions(Pipeline *p) {
    NakiRenditions *m=naki_job_renditions(p->job);NakiSeiConfig sei={0};int audio=0;
    for(unsigned i=0;i<NAKI_OUTPUT_MAX;i++){Rendition *r=&m->slots[i];if(!r->member&&!(m->transition.operation&&m->transition.started&&m->transition.action==NAKI_RENDITION_ADD&&m->transition.slot==i))continue;if(naki_sei_union(&sei,&r->spec.sei))return -ENOSPC;audio|=r->spec.audio_copy&&strcmp(r->spec.output_format,"mp4");}
    int64_t generation;int ret=pipeline_reader_subscribe(p,&sei,audio,&generation);
    if(!ret&&m->transition.operation)m->slots[m->transition.slot].required_subscription_generation=generation;
    return ret;
}
static void automatic_restart(Pipeline *p,int64_t now) {
    NakiRenditions *m=naki_job_renditions(p->job);Job *j=p->job;
    if(!naki_job_config(j)->rendition_auto_restart||m->finishing||m->transition.operation||m->idle_expired||naki_job_cancelled(j))return;
    for(unsigned i=0;i<NAKI_OUTPUT_MAX;i++) {
        Rendition *r=&m->slots[i];if(!r->member||!r->failed||r->pid||r->automatic_attempts>=(unsigned)naki_job_config(j)->rendition_restart_limit)continue;
        if(!r->restart_at_us){int64_t delay=(int64_t)naki_job_config(j)->rendition_restart_backoff_ms*1000*(1U<<r->automatic_attempts);r->restart_at_us=now+FFMIN(delay,60000000);}
        if(now<r->restart_at_us)continue;
        char operation_id[33];if(naki_id_generate(operation_id))return;
        yyjson_mut_doc *d=yyjson_mut_doc_new(NULL);if(!d)return;yyjson_mut_val *root=yyjson_mut_obj(d),*target=yyjson_mut_obj(d);yyjson_mut_doc_set_root(d,root);
        yyjson_mut_obj_add_str(d,root,"operation_id",operation_id);yyjson_mut_obj_add_str(d,root,"action","restart_rendition");yyjson_mut_obj_add_uint(d,root,"expected_revision",m->desired_revision);
        yyjson_mut_obj_add_uint(d,root,"expected_generation",r->generation);yyjson_mut_obj_add_str(d,target,"component","rendition");yyjson_mut_obj_add_strcpy(d,target,"output_id",r->spec.id);yyjson_mut_obj_add_val(d,root,"target",target);
        size_t bytes;char *text=yyjson_mut_write(d,0,&bytes);yyjson_mut_doc_free(d);yyjson_doc *v=text?yyjson_read(text,bytes,0):NULL;free(text);
        NakiUpdateRequest request;char error[256];int ret=v?naki_update_parse(yyjson_doc_get_root(v),naki_job_id(j),&request,error,sizeof(error)):-ENOMEM;
        if(v)yyjson_doc_free(v);
        if(ret)return;
        NakiOperationIntent in={.kind=NAKI_OP_TOPOLOGY,.job_id=naki_job_id(j),.operation_id=operation_id,.output_id=r->spec.id,.fingerprint=request.fingerprint,
            .origin_request_id="automatic_restart",.component="rendition",.action="restart_rendition",.expected_revision=m->desired_revision,.revision=m->desired_revision+1};
        NakiOperationHandle op;ret=naki_operations_reserve(naki_job_operations(j),&in,now,&op);r->automatic_attempts++;r->restart_at_us=0;
        if(ret!=1)return;
        m->desired_revision++;m->transition.action=NAKI_RENDITION_RESTART;m->transition.automatic=1;m->transition.operation=op;m->transition.slot=i;m->transition.revision=m->desired_revision;m->transition.accepted_us=now;m->transition.deadline_us=now+12000000;
        m->transition.deadline_us=naki_rendition_prepare_deadline(&r->spec,now);
        naki_operations_transition(naki_job_operations(j),op,NAKI_OP_PREPARING,0,0,NULL,now);
        yyjson_mut_doc *event=new_event("rendition_automatic_restart",j);json_str(event,"output_id",r->spec.id);json_str(event,"operation_id",operation_id);json_int(event,"attempt",r->automatic_attempts);logger_submit(naki_job_logger(j),NULL,event);return;
    }
}
int naki_rendition_control_tick(Pipeline *p) {
    NakiRenditions *m=naki_job_renditions(p->job);Job *j=p->job;int64_t now=now_us();
    if(m->finishing||naki_job_cancelled(j)) {
        if(m->transition.operation){naki_operations_transition(naki_job_operations(j),m->transition.operation,NAKI_OP_CANCELLED,1,0,"session closing before attach",now);transition_clear(m);}
        naki_session_idle_finish(&m->idle);
    } else if(naki_session_idle_poll(&m->idle,now)>0){m->idle_expired=1;return 0;}
    if(naki_job_config(j)->source_rate_limit&&now>=m->next_source_rate_us){int64_t bps=pipeline_source_rate(p,now);for(unsigned i=0;i<NAKI_OUTPUT_MAX;i++){Rendition *r=&m->slots[i];if(r->pid&&!r->failed&&!r->stopping){yyjson_mut_doc *d=ipc_message("rendition_source_rate");json_str(d,"rendition_instance",r->instance);json_int(d,"bps",bps);ipc_send(r->control,d);yyjson_mut_doc_free(d);}}m->next_source_rate_us=now+50000;}
    for(unsigned i=0;i<NAKI_OUTPUT_MAX;i++) {
        Rendition *r=&m->slots[i];
        if(r->remove_draining&&r->pid&&!r->eof_sent){NakiRenditionMessage eof={.kind=NAKI_RMSG_EOF,.rendition_generation=r->generation};strcpy(eof.session,naki_process_id());strcpy(eof.instance,r->instance);if(!naki_rendition_send(r->media,&eof))r->eof_sent=1;}
    }
    for(unsigned count=0;count<8&&m->command_count;count++) {
        unsigned at=m->command_head;Rendition *r=&m->slots[m->commands[at].slot];NakiOperationHandle op=m->commands[at].operation;int done=0;
        if(m->finishing||naki_job_cancelled(j)||!r->pid||r->failed||r->generation!=m->commands[at].generation){naki_operations_transition(naki_job_operations(j),op,NAKI_OP_CANCELLED,1,0,"rendition unavailable before forwarding",now);done=1;}
        else {
            yyjson_mut_doc *message=ipc_message("rendition_update");json_str(message,"rendition_instance",r->instance);json_str(message,"origin_request_id",m->commands[at].origin);
            yyjson_val *request=yyjson_doc_get_root(m->commands[at].request);json_str(message,"operation_id",yyjson_get_str(yyjson_obj_get(request,"operation_id")));yyjson_mut_obj_add_val(message,yyjson_mut_doc_get_root(message),"update",yyjson_val_mut_copy(message,request));
            if(!ipc_send(r->control,message)){
                for(unsigned k=0;k<256;k++){yyjson_mut_doc *old=r->remote_operations[k]?naki_operations_get(naki_job_operations(j),r->remote_operations[k]):NULL;int free_slot=!old;if(old)yyjson_mut_doc_free(old);if(free_slot){r->remote_operations[k]=op;break;}}
                done=1;
            }else if(errno!=EAGAIN&&errno!=EWOULDBLOCK){naki_rendition_fail(r,-errno,"watermark command transport");}
            yyjson_mut_doc_free(message);
        }
        if(!done)break;
        yyjson_doc_free(m->commands[at].request);memset(&m->commands[at],0,sizeof(m->commands[at]));m->command_head=(at+1)%8;m->command_count--;
    }
    automatic_restart(p,now);
    if(!m->transition.operation){return subscriptions(p);}
    Rendition *r=&m->slots[m->transition.slot];
    if(!m->transition.started) {
        m->transition.started=1;
        if(m->transition.action==NAKI_RENDITION_ADD) {
            r->spec=m->transition.spec;free(r->config_json);r->config_json=m->transition.config_json;m->transition.config_json=NULL;r->member=r->attached=0;r->removed=r->restarting=0;r->incarnations=0;
            naki_job_remote_configure(r->job,&m->transition.config,0,r->spec.id);
            int ret=subscriptions(p);if(ret<0){if(!r->pid)naki_rendition_reaped(p,m->transition.slot);transition_fail(m,ret,"subscription union budget");return 0;}
            ret=naki_rendition_spawn(p,m->transition.slot);if(ret<0){if(!r->pid)naki_rendition_reaped(p,m->transition.slot);transition_fail(m,ret,"candidate spawn failed");return 0;}
        } else if(m->transition.action==NAKI_RENDITION_REMOVE) {
            r->member=r->attached=0;r->removed=1;r->remove_operation=m->transition.operation;r->detach_identity=m->last_frame;
            naki_session_idle_outputs(&m->idle,members(m),now);m->effective_revision=m->transition.revision;
            if(r->pid)stop_child(r,1);else naki_rendition_reaped(p,m->transition.slot);
            transition_clear(m);return 0;
        } else {r->attached=0;r->restarting=1;if(!m->transition.automatic)r->automatic_attempts=0;r->restart_at_us=0;if(r->pid)stop_child(r,0);}
    }
    if(m->transition.action==NAKI_RENDITION_RESTART&&!r->pid&&r->restarting){int ret=naki_rendition_spawn(p,m->transition.slot);r->restarting=0;if(ret<0){transition_fail(m,ret,"restart spawn failed");return 0;}}
    if(now>=m->transition.deadline_us){naki_rendition_fail(r,-ETIMEDOUT,"candidate attach deadline");transition_fail(m,-ETIMEDOUT,"candidate attach deadline");return 0;}
    if(r->restarting&&r->pid)return 0;
    if(r->failed&&!r->restarting){transition_fail(m,-EIO,"candidate rendition failed before attach");return 0;}
    if(r->prepared&&r->prepare_source_generation!=m->source_generation){naki_rendition_fail(r,-ESTALE,"source format changed during prepare");transition_fail(m,-ESTALE,"source_format_generation changed during prepare");return 0;}
    if(r->prepared){naki_operations_transition(naki_job_operations(j),m->transition.operation,NAKI_OP_PREPARED,0,0,NULL,now);}
    return 0;
}
int naki_rendition_attach(Pipeline *p,AVFrame *frame) {
    NakiRenditions *m=naki_job_renditions(p->job);
    /* An HTTP/control-thread reservation is not an owner-prepared attachment.
     * In particular a pending remove still names a live, prepared old holder. */
    if(!m->transition.operation||!m->transition.started||
       (m->transition.action!=NAKI_RENDITION_ADD&&m->transition.action!=NAKI_RENDITION_RESTART)||
       m->finishing||naki_job_cancelled(p->job))return 0;
    Rendition *r=&m->slots[m->transition.slot];
    if(!r->pid||!r->prepared||r->failed||r->stopping)return 0;
    if(r->prepare_source_generation!=m->source_generation)return 0;
    const Stamp *source=(const Stamp *)frame->opaque_ref->data;
    if(source->subscription_generation<r->required_subscription_generation||!pipeline_reader_epoch_ready(p,r->required_subscription_generation))return 0;
    if(naki_session_idle_outputs(&m->idle,members(m)+(r->member?0:1),now_us())<0){stop_child(r,0);transition_fail(m,-ESHUTDOWN,"idle deadline reached before attachment");return 0;}
    Stamp *s=(Stamp *)frame->opaque_ref->data;r->attach_identity=pipeline_frame_identity(p,s,0,(AVRational){0,1});
    naki_operations_transition(naki_job_operations(p->job),m->transition.operation,NAKI_OP_APPLYING,1,0,NULL,now_us());
    int ret=naki_operations_applied(naki_job_operations(p->job),m->transition.operation,&r->attach_identity,1,now_us());if(ret<0)return ret;
    r->member=r->attached=1;r->attach_operation=m->transition.operation;m->effective_revision=m->transition.revision;transition_clear(m);return 0;
}
void naki_rendition_publish(Pipeline *p) {
    NakiRenditions *m=naki_job_renditions(p->job);Job *j=p->job;NakiPoolStats video={0},audio={0};naki_pool_stats(m->video,&video);naki_pool_stats(m->audio,&audio);
    NakiTopologyProgress progress={.desired_revision=m->desired_revision,.effective_revision=m->effective_revision,
        .source_generation=m->source_generation,.pool_bytes=video.mapped_bytes+audio.mapped_bytes,
        .pool_occupied=video.occupied+audio.occupied,.idle_deadline_us=m->idle.deadline_us,.idle_state=m->idle.state};
    naki_job_topology(j,&progress);
    for(unsigned i=0;i<NAKI_OUTPUT_MAX;i++){
        Rendition *r=&m->slots[i];NakiRawProgress raw={.video_retained=video.retained[i],.audio_retained=audio.retained[i],
            .restarts=r->automatic_attempts,.restart_at_us=r->restart_at_us,.video_queued=r->queued_video,
            .audio_queued=r->queued_audio,.bytes=r->queued_bytes,.work_us=r->queued_work_us};
        naki_job_raw_progress(r->job,&raw);
    }
}

void naki_rendition_observe(Pipeline *p,Rendition *r,yyjson_val *record) {
    const char *id=yyjson_get_str(yyjson_obj_get(record,"operation_id")),*output=yyjson_get_str(yyjson_obj_get(yyjson_obj_get(record,"target"),"output_id"));
    if(!id||!output||strcmp(output,r->spec.id))return;
    NakiOperationHandle op;if(naki_operations_lookup(naki_job_operations(p->job),naki_job_id(p->job),id,&op))return;
    int authorized=0;for(unsigned k=0;k<256;k++)if(r->remote_operations[k]==op){authorized=1;break;}
    if(authorized){
        (void)naki_operations_observe(naki_job_operations(p->job),op,record,now_us());
        const char *state=yyjson_get_str(yyjson_obj_get(record,"state")),*action=yyjson_get_str(yyjson_obj_get(record,"action"));
        if(state&&!strcmp(state,"applied")&&action&&(!strcmp(action,"add_destination")||!strcmp(action,"remove_destination")||!strcmp(action,"request_random_access"))){uint64_t revision=yyjson_get_uint(yyjson_obj_get(record,"accepted_revision"));if(revision>naki_job_renditions(p->job)->effective_revision)naki_job_renditions(p->job)->effective_revision=revision;}
    }
}
void naki_rendition_written(Pipeline *p,Rendition *r,yyjson_val *snapshot) {
    if(!r->attach_operation)return;
    yyjson_val *first=yyjson_arr_get(yyjson_obj_get(snapshot,"first_frame_checkpoints"),0);
    if(!yyjson_is_obj(first))return;
    uint64_t id=yyjson_get_uint(yyjson_obj_get(first,"frame_id"));
    if(id!=r->attach_identity.frame_id){naki_rendition_fail(r,-EPROTO,"first written identity differs from attach frame");return;}
    NakiFrameIdentity written=r->attach_identity;written.output_pts=yyjson_get_sint(yyjson_obj_get(first,"pts_us"));written.output_tb_num=1;written.output_tb_den=1000000;
    int64_t when=yyjson_get_sint(yyjson_obj_get(first,"mux_flush_done_monotonic_us"));
    if(when<=0)return;
    if(!naki_operations_written(naki_job_operations(p->job),r->attach_operation,&written,when)){
        naki_operations_frame_release(naki_job_operations(p->job),r->attach_operation,now_us());r->attach_operation=0;r->written_generation=r->generation;
    }
}

yyjson_mut_doc *naki_renditions_snapshot(Job *job,const char *output) {
    NakiRenditions *m=naki_job_renditions(job);if(!m||!naki_id_valid(output,32))return NULL;yyjson_mut_doc *d=NULL;
    pthread_mutex_lock(&m->mutex);
    for(unsigned i=0;i<NAKI_OUTPUT_MAX;i++){
        Rendition *r=&m->slots[i];if(strcmp(r->spec.id,output))continue;
        NakiRenditionView view={.pid=r->pid,.attached=r->attached,.member=r->member,.generation=r->generation,
            .source_generation=m->source_generation,.prepared_source_generation=r->prepare_source_generation,
            .session=naki_process_id(),.instance=r->instance};
        d=naki_job_remote_detail(r->job,&view);break;
    }
    pthread_mutex_unlock(&m->mutex);return d;
}
