#include "service_internal.h"
#include <libavcodec/avcodec.h>
#include <libavcodec/async.h>
#include <libavformat/network_observer.h>

/* HTTP/control JSON only. Own a pending request only after IPC enqueue succeeds;
 * supervisor ACK processing completes it on this same libevent thread. */
void service_reply(struct evhttp_request *req,int code,yyjson_mut_doc *doc) {
    const char *attempt=evhttp_find_header(evhttp_request_get_output_headers(req),"X-Request-ID");
    if(attempt&&doc)json_str(doc,"request_id",attempt);
    struct evbuffer *buf=evbuffer_new(); size_t size=0;
    char *text=doc ? yyjson_mut_write(doc,0,&size) : NULL;
    if(doc)yyjson_mut_doc_free(doc);
    if(!buf || !text) {
        if(buf)evbuffer_free(buf);
        free(text); evhttp_send_error(req,500,"Allocation failure"); return;
    }
    evbuffer_add(buf,text,size); evbuffer_add(buf,"\n",1);
    evhttp_add_header(evhttp_request_get_output_headers(req),"Content-Type","application/json");
    evhttp_add_header(evhttp_request_get_output_headers(req),"Cache-Control","no-store");
    evhttp_send_reply(req,code,NULL,buf); free(text); evbuffer_free(buf);
}

void service_error_reply(struct evhttp_request *req,int code,const char *message) {
    yyjson_mut_doc *doc=new_event("error",NULL); json_str(doc,"error",message?message:"unavailable");
    const char *fallback=code==429?"resource_budget_exhausted":code==404?"not_found":code==409?"conflict":code==503?"unavailable":"invalid_request";
    json_str(doc,"error_code",message&&naki_id_valid(message,128)?message:fallback);service_reply(req,code,doc);
}

void service_operation_unavailable(Service *s,const char *reason) {
    if(!s->operation_request)return;
    yyjson_mut_doc *cached=s->operation_creation?naki_operation_cache_get(s->operation_creation->operations,s->operation_id):NULL;
    if(cached)service_reply(s->operation_request,s->operation_is_query?200:503,cached);
    else service_error_reply(s->operation_request,503,reason);
    s->operation_request=NULL;s->operation_creation=NULL;
}

static int operation_rpc(Service *s,struct evhttp_request *req,Creation *record,const char *operation,yyjson_val *update,int64_t received) {
    if(s->operation_request) {service_error_reply(req,429,"operation_query_or_update_budget_exhausted");return -1;}
    Worker *w=s->active;
    if(!w||strcmp(record->job_id,s->job_id)||w->fd<0||w->needs_info||w->incompatible||!w->updates_api) {
        service_error_reply(req,503,"worker_operation_unavailable_or_recovering");return -1;
    }
    yyjson_mut_doc *message=ipc_message(update?"update":"operation_get");json_str(message,"job_id",record->job_id);
    json_str(message,"worker_instance_id",w->instance_id);json_str(message,"operation_id",operation);
    s->operation_token=FFMAX(s->operation_token+1,(uint64_t)received);json_int(message,"token",(int64_t)s->operation_token);
    const char *origin=evhttp_find_header(evhttp_request_get_output_headers(req),"X-Request-ID");json_str(message,"origin_request_id",origin);
    if(update)yyjson_mut_obj_add_val(message,yyjson_mut_doc_get_root(message),"update",yyjson_val_mut_copy(message,update));
    if(update) {
        yyjson_mut_doc *event=new_event("update_received",NULL);json_str(event,"job_id",record->job_id);json_str(event,"operation_id",operation);
        json_str(event,"request_id",origin);json_str(event,"create_request_id",record->origin_request_id);
        yyjson_val *target=yyjson_obj_get(update,"target");json_str(event,"output_id",yyjson_get_str(yyjson_obj_get(target,"output_id")));json_str(event,"component","watermark");
        logger_submit(s->logger,NULL,event);
    }
    int ret=ipc_send(w->fd,message);yyjson_mut_doc_free(message);
    if(ret) {service_error_reply(req,503,"worker_operation_not_queued");return -1;}
    evhttp_request_own(req);s->operation_request=req;s->operation_creation=record;s->operation_request_us=received;
    strcpy(s->operation_id,operation);s->operation_is_query=update==NULL;return 0;
}

static yyjson_doc *read_body(struct evhttp_request *req,size_t maximum) {
    struct evbuffer *input=evhttp_request_get_input_buffer(req);size_t size=evbuffer_get_length(input);
    if(!size||size>maximum) {service_error_reply(req,413,"Request body is empty or exceeds its bound");return NULL;}
    yyjson_doc *d=yyjson_read((const char *)evbuffer_pullup(input,-1),size,0);char error[128];
    if(!d||!yyjson_is_obj(yyjson_doc_get_root(d))||naki_intent_validate(yyjson_doc_get_root(d),error,sizeof(error))) {
        if(d)yyjson_doc_free(d);
        service_error_reply(req,400,"Invalid or duplicate-key JSON request");return NULL;
    }
    return d;
}

static int explicit_target(struct evhttp_request *req,Service *s,yyjson_doc *body) {
    yyjson_val *id=yyjson_obj_get(yyjson_doc_get_root(body),"job_id");
    if(!naki_json_id(id,63)) {service_error_reply(req,400,"An explicit valid job_id is required");return -1;}
    if(strcmp(yyjson_get_str(id),s->job_id)) {service_error_reply(req,409,"job_id does not match the assigned task");return -1;}
    return 0;
}

void service_handle_request(struct evhttp_request *req,void *opaque) {
    /* Callback entry follows receipt of the complete HTTP request body. */
    int64_t received=now_us(); Service *s=opaque;
    char generated[33];const char *attempt=evhttp_find_header(evhttp_request_get_input_headers(req),"X-Request-ID");
    if(naki_id_generate(generated)) {service_error_reply(req,503,"Request identity unavailable");return;}
    evhttp_add_header(evhttp_request_get_output_headers(req),"X-Request-ID",attempt&&naki_id_valid(attempt,128)?attempt:generated);
    if(attempt&&!naki_id_valid(attempt,128)) {service_error_reply(req,400,"Invalid X-Request-ID");return;}
    const char *uri=evhttp_request_get_uri(req); enum evhttp_cmd_type method=evhttp_request_get_command(req);
    char stop_target[64]="",update_target[64]="";
    if(method==EVHTTP_REQ_POST&&!strncmp(uri,"/v1/jobs/",9)) {
        const char *tail=uri+9,*end=strchr(tail,'/');size_t length=end?(size_t)(end-tail):0;
        if(end&&!strcmp(end,"/stop")&&length&&length<sizeof(stop_target)) {
            memcpy(stop_target,tail,length);stop_target[length]=0;
            if(!naki_id_valid(stop_target,63)) {service_error_reply(req,400,"Invalid job target");return;}
            uri="/v1/job/stop";
        } else if(end&&!strcmp(end,"/updates")&&length&&length<sizeof(update_target)) {
            memcpy(update_target,tail,length);update_target[length]=0;
            if(!naki_id_valid(update_target,63)) {service_error_reply(req,400,"Invalid job target");return;}
        }
    }
    if((!strcmp(uri,"/health/live")||!strcmp(uri,"/health/ready")||!strcmp(uri,"/health/admission"))&&method==EVHTTP_REQ_GET) {
        yyjson_mut_doc *doc=new_event("health_state",NULL);service_control_health(s,doc);service_snapshot_freshness(s,doc);service_pool_json(s,doc);
        const char *reason=!strcmp(uri,"/health/ready")?service_readiness_reason(s,now_us()):!strcmp(uri,"/health/admission")?service_admission_reason(s,now_us()):NULL;
        service_reply(req,reason?503:200,doc);return;
    }
    if(!strcmp(uri,"/health") && method==EVHTTP_REQ_GET) {
        yyjson_mut_doc *doc=new_event("health",NULL);
        json_str(doc,"status",s->shutting_down ? "stopping" : "ok"); json_str(doc,"version","0.9.0");
        json_int(doc,"srt_enabled",avio_find_protocol_name("srt://127.0.0.1:1")!=NULL);
        json_int(doc,"live_mux_api",AVFORMAT_LIVE_MUX_VERSION);
        json_int(doc,"vvenc_enabled",avcodec_find_encoder_by_name("libvvenc")!=NULL);
        json_int(doc,"crash_collector_pid",crash_collector_pid());
        json_int(doc,"crash_capture_available",crash_client_enabled());
        json_int(doc,"crash_ptracer_registered",crash_ptracer_registered());
        json_str(doc,"crash_report_dir",crash_report_directory());
        json_str(doc,"execution_mode","process_pool"); json_int(doc,"supervisor_pid",getpid());
        json_int(doc,"status_interval_s",s->status_interval); json_int(doc,"worker_timeout_s",s->worker_timeout);
        json_int(doc,"worker_stop_timeout_s",s->stop_timeout);
        json_str(doc,"http_library",event_get_version()); json_str(doc,"json_library",YYJSON_VERSION_STRING);
        json_int(doc,"async_api",AVCODEC_ASYNC_API_VERSION); json_int(doc,"network_observer_api",AVFORMAT_NETWORK_OBSERVER_VERSION);
        json_int(doc,"frame_policy_api",1);
        json_int(doc,"watermark_api",1);
        json_int(doc,"component_updates_api",1);json_int(doc,"operation_query_api",1);
        json_int(doc,"operation_result_version_api",1);
        json_int(doc,"max_outputs",4);json_str(doc,"dynamic_video","fixed_output_filter_rebuild");
        json_int(doc,"media_hooks_api",NAKI_MEDIA_HOOK_API);
        json_int(doc,"watermark_codecs",avcodec_find_decoder(AV_CODEC_ID_PNG)&&avcodec_find_decoder(AV_CODEC_ID_MJPEG)&&
            avcodec_find_decoder(AV_CODEC_ID_GIF)&&avfilter_get_by_name("overlay"));
#ifdef AVCODEC_LIVE_FRAME_INTRA_VERSION
        json_int(doc,"frame_intra_api",AVCODEC_LIVE_FRAME_INTRA_VERSION);
#else
        json_int(doc,"frame_intra_api",0);
#endif
        json_str(doc,"ffmpeg_version",av_version_info()); json_int(doc,"libavcodec_version",avcodec_version());
        service_pool_json(s,doc);service_control_health(s,doc);service_snapshot_freshness(s,doc);service_resource_observation(doc);service_reply(req,200,doc);return;
    }
    if(!strncmp(uri,"/v1/jobs/",9)&&method==EVHTTP_REQ_GET) {
        const char *tail=uri+9,*separator=strchr(tail,'/');char id[64];
        size_t length=separator?(size_t)(separator-tail):strlen(tail);
        if(!length||length>=sizeof(id)) {service_error_reply(req,400,"Invalid job target");return;}
        memcpy(id,tail,length);id[length]=0;
        if(!naki_id_valid(id,63)) {service_error_reply(req,400,"Invalid job target");return;}
        if(!separator&&!strcmp(id,s->job_id)) {service_reply(req,200,service_status(s));return;}
        if(!separator)for(int i=0;i<CREATE_RECORDS;i++) {
            Creation *r=&s->records.items[i];
            if(!strcmp(r->job_id,id)&&r->snapshot) {service_reply(req,200,yyjson_doc_mut_copy(r->snapshot,NULL));return;}
        }
        if(separator&&!strncmp(separator,"/operations/",12)&&naki_id_valid(separator+12,128)) {
            Creation *r=service_records_find_creation(&s->records,separator+12);
            if(r&&!strcmp(r->job_id,id)) {service_reply(req,200,service_creation_status(r));return;}
            r=service_records_find_job(&s->records,id);
            if(r&&*r->stop_operation_id&&!strcmp(r->stop_operation_id,separator+12)) {service_reply(req,200,service_stop_status(r));return;}
            if(r) {
                if(s->active&&r==s->creation&&!s->active->needs_info&&s->active->fd>=0) {
                    operation_rpc(s,req,r,separator+12,NULL,received);return;
                }
                yyjson_mut_doc *cached=naki_operation_cache_get(r->operations,separator+12);
                if(cached) {service_reply(req,200,cached);return;}
                if(r==s->creation&&s->active) {service_error_reply(req,503,"worker_operation_unavailable_or_recovering");return;}
            }
        }
        service_error_reply(req,404,"Job or retained operation not found");return;
    }
    if(*update_target&&method==EVHTTP_REQ_POST) {
        yyjson_doc *body=read_body(req,8192);if(!body)return;
        NakiUpdateRequest update;char error[256];
        if(naki_update_parse(yyjson_doc_get_root(body),update_target,&update,error,sizeof(error))) {yyjson_doc_free(body);service_error_reply(req,400,error);return;}
        Creation *record=service_records_find_job(&s->records,update_target);
        if(!record) {yyjson_doc_free(body);service_error_reply(req,404,"job_not_found");return;}
        if(!strcmp(record->operation_id,update.operation_id)||(*record->stop_operation_id&&!strcmp(record->stop_operation_id,update.operation_id))) {
            yyjson_doc_free(body);service_error_reply(req,409,"operation_id_conflict");return;
        }
        if(service_records_ensure_operation_cache(record)) {yyjson_doc_free(body);service_error_reply(req,429,"controller_operation_capacity_exhausted");return;}
        int reservation=naki_operation_cache_reserve(record->operations,update.operation_id,update.fingerprint,received);
        if(reservation<0) {
            if(reservation==-ENOSPC&&s->active&&record==s->creation&&!s->active->needs_info&&s->active->fd>=0) {
                yyjson_mut_doc *sync=ipc_message("operations_sync");json_str(sync,"job_id",record->job_id);json_str(sync,"worker_instance_id",s->active->instance_id);
                if(!ipc_send(s->active->fd,sync))s->active->needs_info=1;
                yyjson_mut_doc_free(sync);
            }
            yyjson_doc_free(body);service_error_reply(req,reservation==-EEXIST?409:429,reservation==-EEXIST?"operation_id_conflict":"controller_operation_capacity_exhausted");return;
        }
        if(!s->active||record!=s->creation||s->active->finished||s->shutting_down||s->stopping) {
            yyjson_doc_free(body);
            if(!reservation) {
                yyjson_mut_doc *cached=naki_operation_cache_get(record->operations,update.operation_id);
                if(cached) {yyjson_mut_val *root=yyjson_mut_doc_get_root(cached);service_reply(req,yyjson_mut_get_bool(yyjson_mut_obj_get(root,"outcome_known"))?200:503,cached);return;}
            }
            naki_operation_cache_unreserve(record->operations,update.operation_id);service_error_reply(req,409,"job_closing_or_terminal");return;
        }
        if(operation_rpc(s,req,record,update.operation_id,yyjson_doc_get_root(body),received)&&reservation==1)naki_operation_cache_unreserve(record->operations,update.operation_id);
        yyjson_doc_free(body);return;
    }
    if(!strcmp(uri,"/v1/job") && method==EVHTTP_REQ_GET) {service_reply(req,200,service_status(s)); return;}
    if(!strcmp(uri,"/v1/job/watermark")&&method==EVHTTP_REQ_POST) {
        if(!s->active||!s->active->assigned||s->active->finished||s->stopping||s->shutting_down) {service_error_reply(req,409,"No running assigned job");return;}
        if(s->active->needs_info||!s->active->updates_api) {service_error_reply(req,503,"worker_operation_unavailable_or_recovering");return;}
        if(s->watermark_request) {service_error_reply(req,409,"Watermark acknowledgement pending");return;}
        yyjson_doc *targeted=read_body(req,8192);if(!targeted)return;
        if(explicit_target(req,s,targeted)) {yyjson_doc_free(targeted);return;}
        yyjson_mut_doc *adapted=yyjson_doc_mut_copy(targeted,NULL);yyjson_doc_free(targeted);
        if(!adapted) {service_error_reply(req,500,"Allocation failure");return;}
        yyjson_mut_obj_remove_key(yyjson_mut_doc_get_root(adapted),"job_id");
        size_t size=0;char *body=yyjson_mut_write(adapted,0,&size);yyjson_mut_doc_free(adapted);
        yyjson_doc *parsed=body?yyjson_read(body,size,0):NULL;free(body);
        NakiWatermarkConfig config;uint64_t revision;char error[256];
        if(!parsed||naki_watermark_update(yyjson_doc_get_root(parsed),&config,&revision,error,sizeof(error))) {
            service_error_reply(req,400,parsed?error:"Invalid JSON");if(parsed)yyjson_doc_free(parsed);return;
        }
        const char *output_id=yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(parsed),"output_id"));Config original;
        size_t config_size=0;char *config_text=yyjson_val_write(yyjson_doc_get_root(s->config),0,&config_size);
        if(!config_text||config_parse(config_text,config_size,&original,error,sizeof(error))) {free(config_text);yyjson_doc_free(parsed);service_error_reply(req,503,"assignment_configuration_unavailable");return;}
        free(config_text);if(!output_id&&!original.output_count)output_id=original.single_output_id;
        s->watermark_token=FFMAX(s->watermark_token+1,(uint64_t)received);
        char operation_id[129];snprintf(operation_id,sizeof(operation_id),"legacy_watermark_%llu",(unsigned long long)s->watermark_token);
        NakiUpdateRequest update;
        if(!output_id) {yyjson_doc_free(parsed);service_error_reply(req,409,"output_id is required");return;}
        if(naki_update_from_legacy(&config,revision,s->job_id,output_id,operation_id,&update,error,sizeof(error))) {yyjson_doc_free(parsed);service_error_reply(req,400,error);return;}
        if(service_records_ensure_operation_cache(s->creation)||naki_operation_cache_reserve(s->creation->operations,operation_id,update.fingerprint,received)<0) {yyjson_doc_free(parsed);service_error_reply(req,429,"controller_operation_capacity_exhausted");return;}
        yyjson_mut_doc *message=ipc_message("watermark");
        json_str(message,"job_id",s->job_id);json_str(message,"worker_instance_id",s->active->instance_id);
        json_str(message,"operation_id",operation_id);json_str(message,"origin_request_id",evhttp_find_header(evhttp_request_get_output_headers(req),"X-Request-ID"));json_int(message,"token",(int64_t)s->watermark_token);
        yyjson_mut_obj_add_val(message,yyjson_mut_doc_get_root(message),"update",yyjson_val_mut_copy(message,yyjson_doc_get_root(parsed)));
        int sent=ipc_send(s->active->fd,message);yyjson_mut_doc_free(message);yyjson_doc_free(parsed);
        if(sent) {naki_operation_cache_unreserve(s->creation->operations,operation_id);service_error_reply(req,503,"Worker unavailable; update not queued");return;}
        evhttp_request_own(req);s->watermark_request=req;s->watermark_request_us=received;s->watermark_request_revision=revision;return;
    }
    if(!strcmp(uri,"/v1/job/stop") && method==EVHTTP_REQ_POST) {
        yyjson_doc *body=read_body(req,1024);if(!body)return;
        yyjson_val *root=yyjson_doc_get_root(body);size_t index,max;yyjson_val *key,*value;
        const char *id=*stop_target?stop_target:yyjson_get_str(yyjson_obj_get(root,"job_id"));
        yyjson_val *operation=yyjson_obj_get(root,"operation_id");
        int bad=!naki_id_valid(id,63)||(*stop_target&&!naki_json_id(operation,128))||(operation&&!naki_json_id(operation,128));
        yyjson_obj_foreach(root,index,max,key,value) {
            const char *field=yyjson_get_str(key);
            if(strcmp(field,"operation_id")&&(*stop_target||strcmp(field,"job_id")))bad=1;
        }
        if(bad) {yyjson_doc_free(body);service_error_reply(req,400,"Stop requires an explicit job target and valid operation_id");return;}
        char operation_id[129];strcpy(operation_id,operation?yyjson_get_str(operation):generated);
        Creation *record=service_records_find_job(&s->records,id);
        if(record&&*record->stop_operation_id&&!strcmp(record->stop_operation_id,operation_id)) {
            yyjson_doc_free(body);service_reply(req,record->stop_accepted?202:503,service_stop_status(record));return;
        }
        if(record&&!strcmp(record->operation_id,operation_id)) {yyjson_doc_free(body);service_error_reply(req,409,"operation_id_conflict");return;}
        if(record&&naki_operation_cache_contains(record->operations,operation_id)) {yyjson_doc_free(body);service_error_reply(req,409,"operation_id_conflict");return;}
        if(s->active&&s->active->needs_info) {yyjson_doc_free(body);service_error_reply(req,503,"worker_operation_unavailable_or_recovering");return;}
        if(!s->active||s->active->finished||strcmp(id,s->job_id)||s->shutting_down||s->stopping) {yyjson_doc_free(body);service_error_reply(req,409,"Job target is not available for a new stop operation");return;}
        if(!record) {yyjson_doc_free(body);service_error_reply(req,503,"Assignment record unavailable");return;}
        strcpy(record->stop_operation_id,operation_id);strcpy(record->stop_origin_request_id,attempt?attempt:generated);record->stop_state=NAKI_RECORD_STATE_RESERVED;
        yyjson_doc_free(body);
        if(*stop_target) {evhttp_request_own(req);s->stop_request=req;s->stop_request_us=received;}
        s->stopping=1; s->state=NAKI_JOB_STATE_STOPPING; service_request_worker_stop(s,s->active);
        if(!*stop_target)service_reply(req,202,service_status(s));
        return;
    }
    if((!strcmp(uri,"/v1/job")||!strcmp(uri,"/v1/jobs")) && method==EVHTTP_REQ_POST) {
        int canonical=!strcmp(uri,"/v1/jobs");
        if(s->shutting_down) {service_error_reply(req,503,"Service is shutting down"); return;}
        yyjson_doc *envelope=read_body(req,16384);if(!envelope)return;
        yyjson_val *root=yyjson_doc_get_root(envelope),*spec=root;
        char operation_id[129];
        if(canonical) {
            yyjson_val *operation=yyjson_obj_get(root,"operation_id");spec=yyjson_obj_get(root,"spec");
            if(yyjson_obj_size(root)!=2||!naki_json_id(operation,128)||!yyjson_is_obj(spec)) {
                yyjson_doc_free(envelope);service_error_reply(req,400,"Create requires only operation_id and spec");return;
            }
            strcpy(operation_id,yyjson_get_str(operation));
        } else strcpy(operation_id,generated);
        size_t size=0;char *body=yyjson_val_write(spec,0,&size);Config config;char error[256]="Cannot normalize create intent",fingerprint[65];
        if(!body||config_parse(body,size,&config,error,sizeof(error))||naki_config_fingerprint(&config,fingerprint,error,sizeof(error))) {
            int allocated=body!=NULL;free(body);yyjson_doc_free(envelope);service_error_reply(req,400,allocated?error:"Allocation failure");return;
        }
        yyjson_doc *parsed=yyjson_read(body,size,0);free(body);yyjson_doc_free(envelope);
        if(!parsed) {service_error_reply(req,500,"Allocation failure");return;}
        Creation *record=service_records_find_creation(&s->records,operation_id);
        if(record) {
            yyjson_doc_free(parsed);
            if(strcmp(record->fingerprint,fingerprint))service_error_reply(req,409,"operation_id_conflict");
            else if(record->accepted)service_reply(req,202,service_creation_status(record));
            else service_reply(req,503,service_creation_status(record));
            return;
        }
        /* A matching retained create is a read of its known outcome, not new
         * admission. An unrelated standby can still be reconciling after the
         * active assignment has been recovered. Keep all new work gated. */
        for(int i=0;i<MAX_WORKERS;i++)if(s->workers[i].pid && s->workers[i].needs_info) {yyjson_doc_free(parsed);service_error_reply(req,503,"Recovering existing workers"); return;}
        if(s->recovery_conflict) {yyjson_doc_free(parsed);service_error_reply(req,503,"Multiple workers require operator reconciliation"); return;}
        if(s->active) {yyjson_doc_free(parsed);service_error_reply(req,409,"Single-stream service already has an active job");return;}
        const char *closed=service_admission_reason(s,received);
        if(closed) {yyjson_doc_free(parsed);service_error_reply(req,503,closed);return;}
        record=service_records_reserve_creation(&s->records,s->creation,s->operation_creation);
        if(!record) {yyjson_doc_free(parsed);service_error_reply(req,503,"Creation retention capacity exhausted");return;}
        Worker *w=NULL;
        for(int i=0;i<MAX_WORKERS;i++)if(s->workers[i].pid && s->workers[i].ready && !s->workers[i].busy) {w=&s->workers[i]; break;}
        int prewarmed=w && w->ready_us<=received;
        if(!w)for(int i=0;i<MAX_WORKERS;i++)if(s->workers[i].pid && !s->workers[i].busy) {w=&s->workers[i]; break;}
        if(!w)w=service_spawn_worker(s);
        if(!w) {yyjson_doc_free(parsed); service_error_reply(req,503,"Worker unavailable; retry after pool recovery"); return;}
        char job_id[33];
        if(naki_id_generate(job_id)) {yyjson_doc_free(parsed);service_error_reply(req,503,"Job identity unavailable");return;}
        if(s->config)yyjson_doc_free(s->config);
        if(s->snapshot)yyjson_doc_free(s->snapshot);
        s->config=parsed; s->snapshot=NULL; s->snapshot_us=0; s->request_us=received; s->assigned_us=0;
        s->active=w; w->busy=1; s->stopping=0; s->prewarmed=prewarmed;
        s->last_recovered=0; s->exit_status_known=0;
        s->exit_code=-1; s->exit_signal=0; s->last_pid=w->pid; *s->error=0; *s->log_path=0;
        strcpy(s->job_id,job_id);
        const char *request_id=evhttp_find_header(evhttp_request_get_input_headers(req),"X-Request-ID");
        snprintf(s->request_id,sizeof(s->request_id),"%s",request_id ? request_id : generated);
        strcpy(record->operation_id,operation_id);strcpy(record->fingerprint,fingerprint);strcpy(record->job_id,job_id);
        strcpy(record->origin_request_id,s->request_id);record->state=NAKI_RECORD_STATE_RESERVED;s->creation=record;
        if(config.frame_log||config.network_trace||config.codec_trace)snprintf(s->log_path,sizeof(s->log_path),"%s/%s.trace",s->run_dir,s->job_id);
        s->state=NAKI_JOB_STATE_STARTING;
        yyjson_mut_doc *accepted=new_event("request_received",NULL);
        json_str(accepted,"job_id",s->job_id); json_str(accepted,"request_id",s->request_id);
        json_int(accepted,"request_monotonic_us",received);
        json_str(accepted,"boundary","complete_http_request_handler");
        json_int(accepted,"worker_pid",w->pid); logger_submit(s->logger,NULL,accepted);
        evhttp_request_own(req);s->create_request=req;s->create_request_us=received;service_dispatch(s,w);return;
    }
    if(!strcmp(uri,"/health")||!strcmp(uri,"/health/live")||!strcmp(uri,"/health/ready")||!strcmp(uri,"/health/admission")||!strcmp(uri,"/v1/job")||!strcmp(uri,"/v1/job/stop")||!strcmp(uri,"/v1/job/watermark"))service_error_reply(req,405,"Method not allowed");
    else service_error_reply(req,404,"Unknown endpoint");
}
