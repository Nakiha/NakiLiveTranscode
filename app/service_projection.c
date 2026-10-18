#include "service_internal.h"

/* Response/log projections run on the controller event loop. Worker snapshots
 * remain authoritative; this layer adds controller observations without
 * sampling or resetting the worker's nonoverlapping metric windows. */
static void set_str(yyjson_mut_doc *doc,const char *key,const char *value) {
    yyjson_mut_obj_remove_key(yyjson_mut_doc_get_root(doc),key); json_str(doc,key,value);
}

yyjson_mut_doc *service_creation_status(Creation *record) {
    yyjson_mut_doc *d=new_event("operation",NULL);
    json_str(d,"kind","create");json_str(d,"operation_id",record->operation_id);
    json_str(d,"job_id",record->job_id);json_str(d,"origin_request_id",record->origin_request_id);
    json_str(d,"state",naki_record_state_name(record->state));
    yyjson_mut_obj_add_bool(d,yyjson_mut_doc_get_root(d),"outcome_known",record->outcome_known);
    char path[160];snprintf(path,sizeof(path),"/v1/jobs/%s",record->job_id);json_str(d,"job_url",path);
    return d;
}

yyjson_mut_doc *service_stop_status(Creation *record) {
    yyjson_mut_doc *d=new_event("operation",NULL);json_str(d,"kind","stop");
    json_str(d,"job_id",record->job_id);json_str(d,"operation_id",record->stop_operation_id);
    json_str(d,"origin_request_id",record->stop_origin_request_id);json_str(d,"state",naki_record_state_name(record->stop_state));
    yyjson_mut_obj_add_bool(d,yyjson_mut_doc_get_root(d),"outcome_known",record->stop_accepted);return d;
}

yyjson_mut_doc *service_operation_projection(yyjson_val *record) {
    yyjson_mut_doc *d=yyjson_mut_doc_new(NULL);if(!d)return NULL;
    yyjson_mut_doc_set_root(d,yyjson_val_mut_copy(d,record));
    const char *job=yyjson_get_str(yyjson_obj_get(record,"job_id")),*operation=yyjson_get_str(yyjson_obj_get(record,"operation_id"));
    char path[256];snprintf(path,sizeof(path),"/v1/jobs/%s/operations/%s",job,operation);json_str(d,"operation_url",path);return d;
}

void service_pool_json(Service *s,yyjson_mut_doc *doc) {
    yyjson_mut_val *pool=yyjson_mut_obj(doc),*workers=yyjson_mut_arr(doc);
    unsigned ready=0,warming=0,recovering=0;
    for(int i=0;i<MAX_WORKERS;i++) {
        Worker *w=&s->workers[i]; if(!w->pid)continue;
        if(w->needs_info)recovering++;
        if(!w->busy&&!w->needs_info) {if(w->ready)ready++; else warming++;}
        yyjson_mut_val *item=yyjson_mut_obj(doc);
        yyjson_mut_obj_add_int(doc,item,"pid",w->pid);
        yyjson_mut_obj_add_strcpy(doc,item,"instance_id",w->instance_id);
        yyjson_mut_obj_add_str(doc,item,"state",w->incompatible?"incompatible":w->needs_info?"unresolved":w->busy?"busy":w->ready?"ready":"warming");
        yyjson_mut_arr_add_val(workers,item);
    }
    yyjson_mut_obj_add_uint(doc,pool,"spare_target",s->spares);
    yyjson_mut_obj_add_uint(doc,pool,"ready",ready); yyjson_mut_obj_add_uint(doc,pool,"warming",warming);
    yyjson_mut_obj_add_uint(doc,pool,"recovering",recovering);
    yyjson_mut_obj_add_val(doc,pool,"workers",workers);
    yyjson_mut_obj_add_strcpy(doc,pool,"error",s->pool_error);
    yyjson_mut_obj_add_val(doc,yyjson_mut_doc_get_root(doc),"pool",pool);
}

const char *service_readiness_reason(Service *s,int64_t current) {
    if(s->shutting_down)return "shutting_down";
    if(s->recovery_conflict)return "ownership_conflict";
    for(int i=0;i<MAX_WORKERS;i++)if(s->workers[i].pid&&(s->workers[i].needs_info||s->workers[i].incompatible))return "worker_reconciliation_pending";
    if(s->active) {
        Worker *w=s->active;
        if(w->fd<0||!w->updates_api)return "worker_control_unavailable";
        if(current-w->last_seen_us>CONTROL_STALE_US)return "worker_control_stale";
    }
    return NULL;
}

const char *service_admission_reason(Service *s,int64_t current) {
    const char *reason=service_readiness_reason(s,current);if(reason)return reason;
    if(s->active)return "busy";
    int creation=0,worker=0,free_slot=0;
    for(int i=0;i<CREATE_RECORDS;i++) {
        Creation *r=&s->records.items[i];
        if(!*r->operation_id||(r!=s->creation&&r!=s->operation_creation&&r->retained_us&&current-r->retained_us>=1800000000))creation=1;
    }
    if(!creation)return "creation_retention_capacity";
    for(int i=0;i<MAX_WORKERS;i++) {
        Worker *w=&s->workers[i];if(!w->pid)free_slot=1;
        else if(!w->busy) {if(current-w->last_seen_us>CONTROL_STALE_US)return "worker_control_stale";worker=1;}
    }
    if(!worker&&current<s->next_spawn_us)return "pool_backoff";
    if(!worker&&!free_slot)return "worker_capacity";
    return NULL;
}

void service_resource_observation(yyjson_mut_doc *d) {
    yyjson_mut_val *root=yyjson_mut_doc_get_root(d),*v=yyjson_mut_obj(d);struct rlimit files;
    yyjson_mut_obj_add_str(d,v,"actions","observe_only_existing_hard_media_bounds_and_deadlines_unchanged");
    yyjson_mut_obj_add_sint(d,v,"observed_monotonic_us",now_us());
    if(!getrlimit(RLIMIT_NOFILE,&files)) {
        if(files.rlim_cur==RLIM_INFINITY)yyjson_mut_obj_add_null(d,v,"nofile_soft");else yyjson_mut_obj_add_uint(d,v,"nofile_soft",files.rlim_cur);
        if(files.rlim_max==RLIM_INFINITY)yyjson_mut_obj_add_null(d,v,"nofile_hard");else yyjson_mut_obj_add_uint(d,v,"nofile_hard",files.rlim_max);
    }
    DIR *directory=opendir("/proc/self/fd");unsigned count=0;int capped=0;
    if(directory) {
        struct dirent *entry;
        while((entry=readdir(directory)))if(strcmp(entry->d_name,".")&&strcmp(entry->d_name,"..")) {if(count==4096) {capped=1;break;}count++;}
        closedir(directory);yyjson_mut_obj_add_uint(d,v,"controller_fd_count",count);yyjson_mut_obj_add_bool(d,v,"controller_fd_count_capped",capped);
    }
    const char *names[]={"memory.current","memory.max","pids.current","pids.max","cpu.max"};
    yyjson_mut_val *cgroup=yyjson_mut_obj(d);yyjson_mut_obj_add_str(d,cgroup,"scope","mounted_cgroup_v2_root_not_process_rss");
    for(unsigned i=0;i<sizeof(names)/sizeof(names[0]);i++) {
        char path[96],value[96];snprintf(path,sizeof(path),"/sys/fs/cgroup/%s",names[i]);int fd=open(path,O_RDONLY|O_CLOEXEC);
        ssize_t bytes=fd>=0?read(fd,value,sizeof(value)-1):-1;if(fd>=0)close(fd);
        if(bytes>0) {while(bytes>0&&(value[bytes-1]=='\n'||value[bytes-1]=='\r'))bytes--;value[bytes]=0;yyjson_mut_obj_add_strcpy(d,cgroup,names[i],value);}
        else yyjson_mut_obj_add_null(d,cgroup,names[i]);
    }
    yyjson_mut_obj_add_val(d,v,"cgroup",cgroup);yyjson_mut_obj_add_val(d,root,"resource_observation",v);
}

void service_control_health(Service *s,yyjson_mut_doc *d) {
    int64_t current=now_us();const char *ready=service_readiness_reason(s,current),*admit=service_admission_reason(s,current);
    yyjson_mut_val *root=yyjson_mut_doc_get_root(d),*live=yyjson_mut_obj(d),*readiness=yyjson_mut_obj(d),*admission=yyjson_mut_obj(d);
    yyjson_mut_obj_remove_key(root,"liveness");yyjson_mut_obj_remove_key(root,"readiness");yyjson_mut_obj_remove_key(root,"admission");
    yyjson_mut_obj_add_bool(d,live,"alive",1);yyjson_mut_obj_add_str(d,live,"boundary","controller_http_event_loop");
    yyjson_mut_obj_add_bool(d,readiness,"ready",!ready);yyjson_mut_obj_add_strcpy(d,readiness,"reason",ready?ready:"ready");
    yyjson_mut_obj_add_bool(d,admission,"open",!admit);yyjson_mut_obj_add_strcpy(d,admission,"reason",admit?admit:"open");
    yyjson_mut_obj_add_val(d,root,"liveness",live);yyjson_mut_obj_add_val(d,root,"readiness",readiness);yyjson_mut_obj_add_val(d,root,"admission",admission);
    json_int(d,"controller_observed_us",current);
}

void service_snapshot_freshness(Service *s,yyjson_mut_doc *d) {
    int64_t current=now_us();yyjson_mut_val *root=yyjson_mut_doc_get_root(d),*v=yyjson_mut_obj(d);
    yyjson_val *source=s->snapshot?yyjson_doc_get_root(s->snapshot):NULL;
    yyjson_mut_obj_add_sint(d,v,"controller_observed_monotonic_us",current);
    yyjson_mut_obj_add_sint(d,v,"stale_after_ms",CONTROL_STALE_US/1000);
    yyjson_mut_obj_add_bool(d,v,"media_worker_active",s->active!=NULL);
    yyjson_mut_obj_add_bool(d,v,"worker_snapshot_available",s->snapshot!=NULL);
    yyjson_mut_obj_add_uint(d,v,"worker_snapshot_generation",yyjson_get_uint(yyjson_obj_get(source,"snapshot_generation")));
    if(s->snapshot_us) {
        yyjson_mut_obj_add_sint(d,v,"worker_generated_monotonic_us",yyjson_get_sint(yyjson_obj_get(source,"snapshot_generated_us")));
        yyjson_mut_obj_add_sint(d,v,"controller_received_monotonic_us",s->snapshot_us);
        yyjson_mut_obj_add_real(d,v,"receipt_age_ms",(current-s->snapshot_us)/1000.0);
    } else {
        yyjson_mut_obj_add_null(d,v,"worker_generated_monotonic_us");yyjson_mut_obj_add_null(d,v,"controller_received_monotonic_us");yyjson_mut_obj_add_null(d,v,"receipt_age_ms");
    }
    yyjson_mut_obj_add_bool(d,v,"stale",s->snapshot_us&&current-s->snapshot_us>CONTROL_STALE_US);
    yyjson_mut_obj_remove_key(root,"freshness");yyjson_mut_obj_add_val(d,root,"freshness",v);
}

yyjson_mut_doc *service_status(Service *s) {
    yyjson_mut_doc *doc=s->snapshot ? yyjson_doc_mut_copy(s->snapshot,NULL) : new_event("status",NULL);
    if(!doc)return NULL;
    yyjson_mut_obj_remove_key(yyjson_mut_doc_get_root(doc),"job_info");
    yyjson_mut_obj_remove_key(yyjson_mut_doc_get_root(doc),"ipc_kind");
    yyjson_mut_obj_remove_key(yyjson_mut_doc_get_root(doc),"ipc_version");
    set_str(doc,"event","status"); set_str(doc,"state",naki_job_state_name(s->state));
    if(*s->job_id)set_str(doc,"job_id",s->job_id);
    if(*s->request_id)set_str(doc,"request_id",s->request_id);
    if(*s->request_id)set_str(doc,"create_request_id",s->request_id);
    if(s->creation)json_str(doc,"creation_operation_id",s->creation->operation_id);
    json_str(doc,"service_namespace",s->service_namespace);
    if(*s->error || !s->snapshot)set_str(doc,"error",s->error);
    if(!s->snapshot) {
        json_str(doc,"log_path",s->log_path); json_int(doc,"packets_out",0); json_int(doc,"frames_decoded",0);
        json_num(doc,"write_in_progress_ms",0); json_str(doc,"write_phase","");
        yyjson_mut_val *startup=yyjson_mut_obj(doc);
        yyjson_mut_obj_add_bool(doc,startup,"prewarmed",s->prewarmed);
        yyjson_mut_obj_add_sint(doc,startup,"request_monotonic_us",s->request_us);
        yyjson_mut_obj_add_null(doc,startup,"request_to_first_video_mux_ms");
        yyjson_mut_obj_add_null(doc,startup,"request_to_first_key_mux_ms");
        yyjson_mut_obj_add_val(doc,yyjson_mut_doc_get_root(doc),"startup",startup);
    }
    yyjson_mut_val *control=yyjson_mut_obj(doc);
    yyjson_mut_obj_add_int(doc,control,"pid",getpid());
    yyjson_mut_obj_add_int(doc,control,"worker_pid",s->active ? s->active->pid : s->last_pid);
    yyjson_mut_obj_add_bool(doc,control,"active",s->active!=NULL);
    yyjson_mut_obj_add_bool(doc,control,"recovered",s->last_recovered);
    yyjson_mut_obj_add_bool(doc,control,"exit_status_known",s->exit_status_known);
    if(s->snapshot_us)yyjson_mut_obj_add_real(doc,control,"snapshot_age_ms",(now_us()-s->snapshot_us)/1000.0);
    else yyjson_mut_obj_add_null(doc,control,"snapshot_age_ms");
    yyjson_mut_obj_add_int(doc,control,"exit_code",s->exit_code);
    yyjson_mut_obj_add_int(doc,control,"exit_signal",s->exit_signal);
    if(s->exit_status_known)yyjson_mut_obj_add_bool(doc,control,"core_dumped",s->core_dumped);
    else yyjson_mut_obj_add_null(doc,control,"core_dumped");
    yyjson_mut_obj_add_uint(doc,control,"forwarded_logs",s->forwarded_logs);
    yyjson_mut_obj_add_uint(doc,control,"invalid_logs",s->invalid_logs);
    LoggerStats logs; logger_get_stats(s->logger,&logs);
    yyjson_mut_obj_add_uint(doc,control,"log_dropped_status",logs.dropped_status);
    yyjson_mut_obj_add_uint(doc,control,"log_io_errors",logs.io_errors);
    yyjson_mut_obj_add_val(doc,yyjson_mut_doc_get_root(doc),"supervisor",control);
    service_pool_json(s,doc);service_control_health(s,doc);service_snapshot_freshness(s,doc);return doc;
}

yyjson_mut_doc *service_observation(Service *s,const char *event) {
    yyjson_mut_doc *doc=new_event(event,NULL),*projection=service_status(s);
    if(!doc||!projection) {if(doc)yyjson_mut_doc_free(doc);if(projection)yyjson_mut_doc_free(projection);return NULL;}
    if(*s->job_id)json_str(doc,"job_id",s->job_id);
    if(*s->request_id)json_str(doc,"create_request_id",s->request_id);
    json_str(doc,"state",naki_job_state_name(s->state));json_int(doc,"snapshot_received_us",s->snapshot_us);json_int(doc,"controller_observed_us",now_us());
    yyjson_mut_obj_add_val(doc,yyjson_mut_doc_get_root(doc),"snapshot",yyjson_mut_val_mut_copy(doc,yyjson_mut_doc_get_root(projection)));
    yyjson_mut_doc_free(projection);return doc;
}

void service_report_status(evutil_socket_t fd,short flags,void *opaque) {
    (void)fd; (void)flags; Service *s=opaque;
    if(s->active)return; /* Worker alone owns nonoverlapping reporting windows. */
    yyjson_mut_doc *doc=new_event("heartbeat",NULL); json_str(doc,"state","idle");
    if(*s->job_id) {json_str(doc,"last_job_id",s->job_id); json_str(doc,"last_job_state",naki_job_state_name(s->state));}
    service_pool_json(s,doc); logger_submit(s->logger,NULL,doc);
}
