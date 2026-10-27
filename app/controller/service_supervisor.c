#include "service_internal.h"

/* One libevent owner controls verified private endpoints and pidfds. Recovery
 * reconciliation precedes admission; IPC acceptance and stop ACKs keep their
 * original boundaries. Only intentional service shutdown stops all workers. */
static void worker_event(Service *s,Worker *w,const char *event) {
    yyjson_mut_doc *doc=new_event(event,NULL);
    if(*w->instance_id)json_str(doc,"worker_instance_id",w->instance_id);
    if(w==s->active)json_str(doc,"job_id",s->job_id);
    json_int(doc,"worker_pid",w->pid); service_pool_json(s,doc); logger_submit(s->logger,NULL,doc);
}

static void backoff(Service *s,const char *reason) {
    snprintf(s->pool_error,sizeof(s->pool_error),"%s",reason);
    if(s->spawn_failures<5)s->spawn_failures++;
    s->next_spawn_us=now_us()+((int64_t)1<<(s->spawn_failures-1))*1000000;
}

static void signal_worker(Worker *w,int sig) {
    if(w->pid_fd>=0)syscall(SYS_pidfd_send_signal,w->pid_fd,sig,NULL,0);
    else if(w->owned)kill(w->pid,sig);
}

static int same_endpoint(Worker *w) {
    struct stat st;return !lstat(w->endpoint,&st)&&st.st_dev==w->endpoint_dev&&st.st_ino==w->endpoint_ino;
}

/* 1 connected and peer/pidfd verified; 0 unresolved; -1 confirmed dead. */
static int reconnect_worker(Service *s,Worker *w,int64_t now) {
    if(w->fd>=0||now<w->next_connect_us)return 0;
    if(!w->connect_backoff_us)w->connect_backoff_us=100000;
    w->next_connect_us=now+w->connect_backoff_us;w->connect_backoff_us=FFMIN(w->connect_backoff_us*2,1000000);
    struct stat endpoint;
    if(w->endpoint_ino&&!lstat(w->endpoint,&endpoint)&&(endpoint.st_dev!=w->endpoint_dev||endpoint.st_ino!=w->endpoint_ino)) {
        w->incompatible=1;s->recovery_conflict=1;
        snprintf(s->pool_error,sizeof(s->pool_error),"Worker endpoint identity changed; admission disabled");return 0;
    }
    int peer=0,peer_process=-1,fd=ipc_connect(w->endpoint,&peer,&peer_process);
    if(fd<0) {
        int connect_error=errno;
        int probe=(int)syscall(SYS_pidfd_open,w->pid,0);
        if(probe>=0) {struct pollfd gone={.fd=probe,.events=POLLIN};int dead=poll(&gone,1,0)>0;close(probe);if(dead) {if(same_endpoint(w))unlink(w->endpoint);return -1;}}
        else if(errno==ESRCH) {if(same_endpoint(w))unlink(w->endpoint);return -1;}
        if(connect_error==ENOPROTOOPT)snprintf(s->pool_error,sizeof(s->pool_error),"Recovery requires kernel SO_PEERPIDFD; admission disabled");
        else snprintf(s->pool_error,sizeof(s->pool_error),"Worker %d remains unresolved; admission disabled",w->pid);
        return 0;
    }
    if(peer!=w->pid) {close(peer_process);close(fd);w->incompatible=1;snprintf(s->pool_error,sizeof(s->pool_error),"Worker endpoint peer mismatch; admission disabled");return 0;}
    if(w->pid_fd<0) {
        w->pid_fd=peer_process;
    } else close(peer_process);
    w->fd=fd;w->needs_info=1;w->connect_backoff_us=100000;return 1;
}

Worker *service_spawn_worker(Service *s) {
    if(now_us()<s->next_spawn_us)return NULL;
    Worker *w=NULL;
    for(int i=0;i<MAX_WORKERS;i++)if(!s->workers[i].pid) {w=&s->workers[i]; break;}
    if(!w)return NULL;
    int pair[2]={-1,-1},child_fd=-1,error=0;
    if(socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC|SOCK_NONBLOCK,0,pair))goto failed;
    child_fd=fcntl(pair[1],F_DUPFD_CLOEXEC,10);
    if(child_fd<0)goto failed;
    posix_spawn_file_actions_t actions; posix_spawnattr_t attributes;
    if((error=posix_spawn_file_actions_init(&actions)))goto failed;
    if((error=posix_spawnattr_init(&attributes))) {posix_spawn_file_actions_destroy(&actions); goto failed;}
    sigset_t empty,defaults; sigemptyset(&empty); sigemptyset(&defaults);
    sigaddset(&defaults,SIGTERM); sigaddset(&defaults,SIGINT); sigaddset(&defaults,SIGPIPE);
    error=posix_spawnattr_setsigmask(&attributes,&empty);
    if(!error)error=posix_spawnattr_setsigdefault(&attributes,&defaults);
    if(!error)error=posix_spawnattr_setflags(&attributes,POSIX_SPAWN_SETSIGMASK|POSIX_SPAWN_SETSIGDEF);
    if(!error)error=posix_spawn_file_actions_adddup2(&actions,child_fd,3);
    if(!error)error=posix_spawn_file_actions_addopen(&actions,STDOUT_FILENO,"/dev/null",O_WRONLY,0);
    if(!error)error=posix_spawn_file_actions_addclosefrom_np(&actions,4);
    char spawned[32],capacity[24],seconds[24],bytes[32],interval[24],stop_timeout[24];
    int64_t started=now_us();
    snprintf(spawned,sizeof(spawned),"%lld",(long long)started);
    snprintf(capacity,sizeof(capacity),"%u",s->logging.queue_capacity);
    snprintf(seconds,sizeof(seconds),"%u",s->logging.rotate_seconds);
    snprintf(bytes,sizeof(bytes),"%llu",(unsigned long long)s->logging.rotate_bytes);
    snprintf(interval,sizeof(interval),"%u",s->status_interval);
    snprintf(stop_timeout,sizeof(stop_timeout),"%u",s->stop_timeout);
    char *args[]={s->worker_path,s->state_dir,spawned,capacity,seconds,bytes,interval,stop_timeout,s->event_dir,NULL}; pid_t pid=0;
    if(!error)error=posix_spawn(&pid,s->worker_path,&actions,&attributes,args,environ);
    posix_spawn_file_actions_destroy(&actions); posix_spawnattr_destroy(&attributes);
    if(error)goto failed;
    close(child_fd); close(pair[1]);
    *w=(Worker){.pid=pid,.fd=pair[0],.pid_fd=(int)syscall(SYS_pidfd_open,pid,0),
        .owned=1,.spawned_us=started,.last_seen_us=started};
    snprintf(w->endpoint,sizeof(w->endpoint),"%s/w-%d.sock",s->state_dir,pid);
    worker_event(s,w,"worker_spawned"); return w;
failed:
    if(child_fd>=0)close(child_fd);
    for(int i=0;i<2;i++)if(pair[i]>=0)close(pair[i]);
    backoff(s,error ? strerror(error) : strerror(errno)); return NULL;
}

void service_request_worker_stop(Service *s,Worker *w) {
    if(!w->exit_deadline_us)w->exit_deadline_us=now_us()+(int64_t)s->stop_timeout*1000000;
    if(w->stop_sent)return;
    if(!*w->instance_id)return;
    yyjson_mut_doc *doc=ipc_message(s->shutting_down?"drain":w->busy?"stop":"standby_stop");
    json_str(doc,"worker_instance_id",w->instance_id);
    json_int(doc,"stop_requested_us",w->exit_deadline_us-(int64_t)s->stop_timeout*1000000);
    json_int(doc,"stop_deadline_us",w->exit_deadline_us);
    if(w->busy)json_str(doc,"job_id",s->job_id);
    if(!s->shutting_down&&w->busy&&s->creation) {
        json_str(doc,"operation_id",s->creation->stop_operation_id);json_str(doc,"origin_request_id",s->creation->stop_origin_request_id);
    }
    if(!ipc_send(w->fd,doc))w->stop_sent=1;
    yyjson_mut_doc_free(doc);
}

void service_dispatch(Service *s,Worker *w) {
    if(w->dispatched || !w->ready || w->incompatible || s->stopping || s->shutting_down)return;
    yyjson_mut_doc *doc=ipc_message("start");
    json_str(doc,"job_id",s->job_id); json_str(doc,"log_path",s->log_path);
    json_str(doc,"request_id",s->request_id);
    json_str(doc,"service_namespace",s->service_namespace);
    json_str(doc,"creation_operation_id",s->creation->operation_id);
    json_str(doc,"spec_fingerprint",s->creation->fingerprint);
    json_int(doc,"request_us",s->request_us); int64_t assigned=now_us(); json_int(doc,"assigned_us",assigned);
    yyjson_mut_obj_add_bool(doc,yyjson_mut_doc_get_root(doc),"prewarmed",s->prewarmed);
    yyjson_mut_obj_add_val(doc,yyjson_mut_doc_get_root(doc),"config",yyjson_val_mut_copy(doc,yyjson_doc_get_root(s->config)));
    if(!ipc_send(w->fd,doc)) {w->dispatched=1;w->needs_info=1; s->assigned_us=assigned;}
    else if(errno!=EAGAIN && errno!=EWOULDBLOCK) {
        snprintf(s->error,sizeof(s->error),"Worker dispatch failed"); signal_worker(w,SIGKILL); w->kill_sent=1;
    }
    yyjson_mut_doc_free(doc);
}

static int recover_job(Service *s,Worker *w,yyjson_doc *doc) {
    yyjson_val *info=yyjson_obj_get(yyjson_doc_get_root(doc),"job_info");
    const char *id=yyjson_get_str(yyjson_obj_get(info,"job_id"));
    const char *request=yyjson_get_str(yyjson_obj_get(info,"request_id"));
    const char *path=yyjson_get_str(yyjson_obj_get(info,"log_path"));
    if(!id || strlen(id)>=sizeof(s->job_id) || !request || strlen(request)>=sizeof(s->request_id) || !path || strlen(path)>=sizeof(s->log_path))return -1;
    const char *namespace=yyjson_get_str(yyjson_obj_get(info,"service_namespace"));
    const char *operation=yyjson_get_str(yyjson_obj_get(info,"creation_operation_id"));
    const char *fingerprint=yyjson_get_str(yyjson_obj_get(info,"spec_fingerprint"));
    if(!namespace||strcmp(namespace,s->service_namespace)||!naki_id_valid(operation,128)||!fingerprint||strlen(fingerprint)!=64)return -1;
    Creation *record=service_records_find_creation(&s->records,operation);
    if(record&&(strcmp(record->fingerprint,fingerprint)||strcmp(record->job_id,id)))return -1;
    if(!record)record=service_records_reserve_creation(&s->records,s->creation,s->operation_creation);
    if(!record)return -1;
    strcpy(record->operation_id,operation);strcpy(record->fingerprint,fingerprint);strcpy(record->job_id,id);
    strcpy(record->origin_request_id,request);record->state=NAKI_RECORD_STATE_ACCEPTED;record->accepted=record->outcome_known=1;
    if(s->active && s->active!=w) {s->recovery_conflict=1; strcpy(s->pool_error,"Multiple active workers found; admission disabled"); return -1;}
    if(s->config)yyjson_doc_free(s->config);
    size_t size=0; char *text=yyjson_val_write(yyjson_obj_get(info,"config"),0,&size);
    s->config=text ? yyjson_read(text,size,0) : NULL; free(text);
    if(!s->config)return -1;
    strcpy(s->job_id,id); strcpy(s->request_id,request); strcpy(s->log_path,path);
    s->request_us=yyjson_get_sint(yyjson_obj_get(info,"request_us"));
    s->assigned_us=yyjson_get_sint(yyjson_obj_get(info,"assigned_us"));
    s->prewarmed=yyjson_get_bool(yyjson_obj_get(info,"prewarmed"));
    s->active=w; w->busy=1; w->assigned=1;w->dispatched=1; s->last_pid=w->pid;
    s->creation=record;
    s->last_recovered=1; s->exit_status_known=0;
    s->exit_code=-1; s->exit_signal=0; *s->error=0;
    yyjson_val *snapshot=yyjson_doc_get_root(doc);
    int64_t stop_deadline=yyjson_get_sint(yyjson_obj_get(snapshot,"stop_deadline_us"));
    s->stopping=yyjson_get_bool(yyjson_obj_get(snapshot,"stop_intent"));
    const char *stop_operation=yyjson_get_str(yyjson_obj_get(snapshot,"stop_operation_id"));
    const char *stop_origin=yyjson_get_str(yyjson_obj_get(snapshot,"stop_origin_request_id"));
    if(s->stopping&&naki_id_valid(stop_operation,128)&&naki_id_valid(stop_origin,128)) {
        strcpy(record->stop_operation_id,stop_operation);strcpy(record->stop_origin_request_id,stop_origin);
        record->stop_accepted=1;record->stop_state=NAKI_RECORD_STATE_STOPPING;
    }
    if(s->stopping&&stop_deadline>0)w->exit_deadline_us=stop_deadline;
    worker_event(s,w,"worker_recovered"); return 0;
}

static void read_worker(Service *s,Worker *w) {
    if(w->fd<0||w->incompatible)return;
    for(int i=0;i<16;i++) {
        yyjson_doc *doc=NULL; int result=ipc_receive(w->fd,&doc);
        if(result==IPC_VERSION_MISMATCH) {
            w->incompatible=1;w->needs_info=1;
            snprintf(s->pool_error,sizeof(s->pool_error),"Live worker protocol is incompatible; admission disabled");
            yyjson_doc_free(doc);break;
        }
        if(result<=0) {
            if(result<0) {if(w->fd>=0)close(w->fd); w->fd=-1;w->needs_info=1;}
            break;
        }
        w->last_seen_us=now_us();
        const char *instance=yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(doc),"instance_id"));
        if(*w->instance_id&&strcmp(w->instance_id,instance)) {w->incompatible=1;w->needs_info=1;yyjson_doc_free(doc);break;}
        strcpy(w->instance_id,instance);
        if(ipc_kind(doc,"ready")) {
            yyjson_val *capabilities=yyjson_doc_get_root(doc);
            if(yyjson_get_int(yyjson_obj_get(capabilities,"component_updates_api"))!=1||yyjson_get_int(yyjson_obj_get(capabilities,"operation_query_api"))!=1||yyjson_get_int(yyjson_obj_get(capabilities,"operation_result_version_api"))!=1) {
                w->incompatible=1;w->needs_info=1;snprintf(s->pool_error,sizeof(s->pool_error),"Worker operation capabilities are incompatible; admission disabled");yyjson_doc_free(doc);break;
            }
            w->updates_api=1;
            if(!w->busy||!w->dispatched)w->needs_info=0;
            struct stat st;if(!lstat(w->endpoint,&st)) {w->endpoint_dev=st.st_dev;w->endpoint_ino=st.st_ino;}
            if(!w->ready) {
                w->ready=1; w->ready_us=yyjson_get_sint(yyjson_obj_get(yyjson_doc_get_root(doc),"ready_us"));
                worker_event(s,w,"worker_ready");
            }
        } else if(ipc_kind(doc,"owner_info")) {
            yyjson_val *root=yyjson_doc_get_root(doc);
            const char *owner=yyjson_get_str(yyjson_obj_get(root,"job_id"));
            if(yyjson_get_int(yyjson_obj_get(root,"component_updates_api"))!=1||yyjson_get_int(yyjson_obj_get(root,"operation_query_api"))!=1||yyjson_get_int(yyjson_obj_get(root,"operation_result_version_api"))!=1||
               (!w->assigned&&recover_job(s,w,doc))||w!=s->active||!s->creation||!owner||strcmp(owner,s->job_id)) {
                w->incompatible=1;w->needs_info=1;snprintf(s->pool_error,sizeof(s->pool_error),"Worker ownership/capability reconciliation failed");yyjson_doc_free(doc);break;
            }
            w->updates_api=1;w->needs_info=1;w->export_cursor=0;w->export_id=0;
            naki_operation_cache_close(&s->creation->sync_operations);
            s->creation->sync_operations=naki_operation_cache_open(s->job_id);
        } else if(ipc_kind(doc,"operations_page")) {
            yyjson_val *root=yyjson_doc_get_root(doc),*records=yyjson_obj_get(root,"operations");
            const char *job=yyjson_get_str(yyjson_obj_get(root,"job_id"));
            uint64_t epoch=yyjson_get_uint(yyjson_obj_get(root,"export_id"));unsigned cursor=(unsigned)yyjson_get_uint(yyjson_obj_get(root,"cursor"));
            int invalid=w!=s->active||!s->creation||!job||strcmp(job,s->job_id)||!yyjson_is_arr(records)||yyjson_arr_size(records)>8;
            if(!invalid&&cursor==0&&epoch!=w->export_id) {
                naki_operation_cache_close(&s->creation->sync_operations);s->creation->sync_operations=naki_operation_cache_open(job);
                w->export_id=epoch;w->export_cursor=0;w->needs_info=1;
            }
            invalid=invalid||!s->creation||!s->creation->sync_operations||epoch!=w->export_id||cursor!=w->export_cursor;
            size_t index,max;yyjson_val *record;
            if(!invalid)yyjson_arr_foreach(records,index,max,record)if(naki_operation_cache_put(s->creation->sync_operations,record,now_us())) {invalid=1;break;}
            if(invalid) {w->incompatible=1;w->needs_info=1;snprintf(s->pool_error,sizeof(s->pool_error),"Worker operation history cannot be reconciled within its bound");yyjson_doc_free(doc);break;}
            w->export_cursor=(unsigned)yyjson_get_uint(yyjson_obj_get(root,"next_cursor"));
            if(!w->export_cursor) {
                if(s->creation->operations&&naki_operation_cache_reconcile(s->creation->sync_operations,s->creation->operations)) {
                    w->incompatible=1;w->needs_info=1;snprintf(s->pool_error,sizeof(s->pool_error),"Worker operation result versions conflict during reconciliation");yyjson_doc_free(doc);break;
                }
                naki_operation_cache_close(&s->creation->operations);s->creation->operations=s->creation->sync_operations;s->creation->sync_operations=NULL;
                w->needs_info=0;
            }
        } else if(ipc_kind(doc,"rendition_reply")) {
            yyjson_val *root=yyjson_doc_get_root(doc),*record=yyjson_obj_get(root,"rendition");
            const char *job=yyjson_get_str(yyjson_obj_get(root,"job_id")),*requester=yyjson_get_str(yyjson_obj_get(root,"requester_instance_id"));
            int valid=w==s->active&&job&&!strcmp(job,s->job_id)&&requester&&!strcmp(requester,naki_process_id())&&s->operation_request&&s->operation_is_query==2&&yyjson_get_uint(yyjson_obj_get(root,"token"))==s->operation_token;
            if(valid){if(record){yyjson_mut_doc *answer=yyjson_mut_doc_new(NULL);if(answer){yyjson_mut_doc_set_root(answer,yyjson_val_mut_copy(answer,record));service_reply(s->operation_request,200,answer);}else service_error_reply(s->operation_request,503,"allocation_failure");}
                else service_error_reply(s->operation_request,yyjson_get_int(yyjson_obj_get(root,"http_status")),"rendition_not_found_or_unavailable");
                s->operation_request=NULL;s->operation_creation=NULL;}
        } else if(ipc_kind(doc,"operation_reply")) {
            yyjson_val *root=yyjson_doc_get_root(doc),*record=yyjson_obj_get(root,"operation");
            const char *job=yyjson_get_str(yyjson_obj_get(root,"job_id")),*requester=yyjson_get_str(yyjson_obj_get(root,"requester_instance_id"));
            Creation *creation=job?service_records_find_job(&s->records,job):NULL;int code=yyjson_get_int(yyjson_obj_get(root,"http_status"));
            int valid=w==s->active&&creation==s->creation&&creation&&requester&&!strcmp(requester,naki_process_id());
            if(valid&&record) {
                const char *kind=yyjson_get_str(yyjson_obj_get(record,"kind"));
                if(kind&&(!strcmp(kind,"update")||!strcmp(kind,"topology"))&&(service_records_ensure_operation_cache(creation)||naki_operation_cache_put(creation->operations,record,now_us())))valid=0;
            }
            if(valid&&s->operation_request&&creation==s->operation_creation&&yyjson_get_uint(yyjson_obj_get(root,"token"))==s->operation_token) {
                if(record) {
                    const char *id=yyjson_get_str(yyjson_obj_get(record,"operation_id"));
                    yyjson_doc *newest=naki_operation_cache_newest(creation->operations,id);
                    yyjson_val *answer=newest?yyjson_doc_get_root(newest):record;
                    service_reply(s->operation_request,s->operation_is_query||naki_operation_result_terminal(answer)?200:202,service_operation_projection(answer));
                    if(newest)yyjson_doc_free(newest);
                }
                else {
                    if(!s->operation_is_query)naki_operation_cache_unreserve(creation->operations,s->operation_id);
                    service_error_reply(s->operation_request,code>=400&&code<=599?code:503,yyjson_get_str(yyjson_obj_get(root,"error")));
                }
                s->operation_request=NULL;s->operation_creation=NULL;
            } else if(!valid&&s->operation_request)service_operation_unavailable(s,"operation_reconciliation_failed");
        } else if(ipc_kind(doc,"accepted")) {
            const char *id=yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(doc),"job_id"));
            if(w==s->active&&w->dispatched&&id&&!strcmp(id,s->job_id)) {
                w->assigned=1;
                if(s->creation) {s->creation->state=NAKI_RECORD_STATE_ACCEPTED;s->creation->accepted=s->creation->outcome_known=1;}
                if(s->create_request) {service_reply(s->create_request,202,service_status(s));s->create_request=NULL;}
            }
        } else if(ipc_kind(doc,"control_rejected")) {
            worker_event(s,w,"worker_control_rejected");
        } else if(ipc_kind(doc,"stop_ack")) {
            yyjson_val *root=yyjson_doc_get_root(doc);
            const char *job=yyjson_get_str(yyjson_obj_get(root,"job_id"));
            const char *operation=yyjson_get_str(yyjson_obj_get(root,"operation_id"));
            if(w==s->active&&s->creation&&job&&!strcmp(job,s->job_id)&&operation&&!strcmp(operation,s->creation->stop_operation_id)) {
                s->creation->stop_accepted=1;s->creation->stop_state=NAKI_RECORD_STATE_STOPPING;
                if(s->stop_request) {service_reply(s->stop_request,202,service_stop_status(s->creation));s->stop_request=NULL;}
            }
        } else if(ipc_kind(doc,"watermark_ack")) {
            yyjson_val *root=yyjson_doc_get_root(doc);
            yyjson_val *record=yyjson_obj_get(root,"operation");
            const char *ack_job=yyjson_get_str(yyjson_obj_get(root,"job_id")),*ack_operation=yyjson_get_str(yyjson_obj_get(root,"operation_id"));
            if(w==s->active&&s->creation&&ack_job&&!strcmp(ack_job,s->job_id)&&naki_id_valid(ack_operation,128)&&
               !record&&!yyjson_get_sint(yyjson_obj_get(root,"accepted")))
                naki_operation_cache_unreserve(s->creation->operations,ack_operation);
            if(w==s->active&&s->creation&&record&&(service_records_ensure_operation_cache(s->creation)||naki_operation_cache_put(s->creation->operations,record,now_us()))) {
                w->incompatible=1;w->needs_info=1;yyjson_doc_free(doc);break;
            }
            if(w==s->active&&s->watermark_request&&yyjson_get_uint(yyjson_obj_get(root,"revision"))==s->watermark_request_revision&&
               yyjson_get_uint(yyjson_obj_get(root,"token"))==s->watermark_token) {
                yyjson_mut_doc *answer=yyjson_doc_mut_copy(doc,NULL);
                yyjson_mut_val *answer_root=yyjson_mut_doc_get_root(answer);
                yyjson_mut_obj_remove_key(answer_root,"ipc_version");yyjson_mut_obj_remove_key(answer_root,"ipc_kind");yyjson_mut_obj_remove_key(answer_root,"token");
                json_str(answer,"event",yyjson_get_sint(yyjson_obj_get(root,"accepted"))?"watermark_accepted":"watermark_rejected");json_str(answer,"job_id",s->job_id);
                service_reply(s->watermark_request,yyjson_get_sint(yyjson_obj_get(root,"accepted"))?202:409,answer);
                s->watermark_request=NULL;
            }
        } else if(ipc_kind(doc,"snapshot") || ipc_kind(doc,"finished")) {
            if(w->needs_info||!w->updates_api||!w->assigned) {
                w->incompatible=1;snprintf(s->pool_error,sizeof(s->pool_error),"Worker assignment namespace or ownership conflicts; admission disabled");
                yyjson_doc_free(doc);break;
            }
            const char *snapshot_id=yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(doc),"job_id"));
            if(w==s->active&&w->dispatched&&snapshot_id&&!strcmp(snapshot_id,s->job_id)) {
                w->assigned=1;
                if(s->creation) {s->creation->state=NAKI_RECORD_STATE_ACCEPTED;s->creation->accepted=s->creation->outcome_known=1;}
                if(s->create_request) {service_reply(s->create_request,202,service_status(s));s->create_request=NULL;}
            }
            if(w!=s->active || !w->assigned) {yyjson_doc_free(doc); break;}
            const char *id=yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(doc),"job_id"));
            if(!id || strcmp(id,s->job_id)) {w->incompatible=1;w->needs_info=1;s->recovery_conflict=1;yyjson_doc_free(doc); break;}
            if(s->snapshot)yyjson_doc_free(s->snapshot);
            s->snapshot=doc; s->snapshot_us=now_us(); doc=NULL;
            const char *stop_operation=yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(s->snapshot),"stop_operation_id"));
            if(s->creation&&stop_operation&&*stop_operation&&!strcmp(stop_operation,s->creation->stop_operation_id)) {
                s->creation->stop_accepted=1;s->creation->stop_state=NAKI_RECORD_STATE_STOPPING;
                if(s->stop_request) {service_reply(s->stop_request,202,service_stop_status(s->creation));s->stop_request=NULL;}
            }
            if(ipc_kind(s->snapshot,"finished")) {
                w->finished=1;
                if(!w->exit_deadline_us)w->exit_deadline_us=now_us()+(int64_t)s->stop_timeout*1000000;
                yyjson_mut_doc *ack=ipc_message("ack"); ipc_send(w->fd,ack); yyjson_mut_doc_free(ack);
                s->state=NAKI_JOB_STATE_FINISHING;
            } else if(!s->stopping) {
                const char *state=yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(s->snapshot),"state"));
                s->state=state && !strcmp(state,"running") ? NAKI_JOB_STATE_RUNNING : state && !strcmp(state,"waiting_assets") ? NAKI_JOB_STATE_WAITING_ASSETS : NAKI_JOB_STATE_STARTING;
            }
        } else {
            yyjson_doc_free(doc); break;
        }
        if(doc)yyjson_doc_free(doc);
    }
}

static void reap_worker(Service *s,Worker *w,int result) {
    if(w==s->active) {
        service_operation_unavailable(s,"worker_exited_before_operation_confirmation");
        if(s->stop_request) {service_error_reply(s->stop_request,503,"Worker exited before stop acknowledgement was confirmed");s->stop_request=NULL;}
        if(s->create_request) {
            service_error_reply(s->create_request,503,"Worker exited before acceptance was confirmed");s->create_request=NULL;
        }
        s->last_pid=w->pid; s->exit_status_known=w->owned;
        s->exit_code=w->owned && WIFEXITED(result) ? WEXITSTATUS(result) : -1;
        s->exit_signal=w->owned && WIFSIGNALED(result) ? WTERMSIG(result) : 0;
        s->core_dumped=w->owned && WIFSIGNALED(result) && WCOREDUMP(result);
        const char *final=s->snapshot ? yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(s->snapshot),"state")) : NULL;
        if(w->finished && (s->exit_code==0 || !w->owned) && final &&
                (!strcmp(final,"completed") || !strcmp(final,"completed_partial") || !strcmp(final,"failed") || !strcmp(final,"stopped")))s->state=naki_job_state_parse(final);
        else if(s->stopping)s->state=NAKI_JOB_STATE_STOPPED;
        else {
            s->state=NAKI_JOB_STATE_FAILED;
            if(!*s->error)snprintf(s->error,sizeof(s->error),"Worker exited unexpectedly (code=%d signal=%d)",s->exit_code,s->exit_signal);
        }
        s->active=NULL;
        if(s->creation) {
            if(*s->creation->stop_operation_id)s->creation->stop_state=s->creation->stop_accepted?((s->state==NAKI_JOB_STATE_STOPPED)?NAKI_RECORD_STATE_STOPPED:NAKI_RECORD_STATE_FAILED):NAKI_RECORD_STATE_UNKNOWN;
            yyjson_mut_doc *terminal=service_status(s);size_t bytes=0;char *text=terminal?yyjson_mut_write(terminal,0,&bytes):NULL;
            if(terminal)yyjson_mut_doc_free(terminal);
            if(text&&bytes<=IPC_MAX_BYTES)s->creation->snapshot=yyjson_read(text,bytes,0);
            free(text);
            s->creation->state=s->creation->accepted?NAKI_RECORD_STATE_ACCEPTED:NAKI_RECORD_STATE_UNKNOWN;
            s->creation->retained_us=now_us();s->creation=NULL;
        }
        logger_submit(s->logger,NULL,service_observation(s,"job_observed_terminal"));
        if(!s->stopping && (!w->finished || (w->owned && s->exit_code!=0))) {
            yyjson_mut_doc *crash=service_observation(s,"worker_crash");
            json_str(crash,"signal_name",s->exit_signal ? strsignal(s->exit_signal) : "unknown");
            json_str(crash,"diagnostic_boundary","supervisor_observed_exit_with_last_worker_snapshot");
            logger_submit(s->logger,NULL,crash);
        }
        yyjson_val *startup=s->snapshot ? yyjson_obj_get(yyjson_doc_get_root(s->snapshot),"startup") : NULL;
        if(!startup || yyjson_is_null(yyjson_obj_get(startup,"request_to_first_video_mux_ms"))) {
            yyjson_mut_doc *incomplete=service_observation(s,"startup_incomplete");
            json_num(incomplete,"request_to_exit_ms",(now_us()-s->request_us)/1000.0);
            logger_submit(s->logger,NULL,incomplete);
        }
    }
    yyjson_mut_doc *exit=new_event("worker_exited",NULL);
    json_int(exit,"worker_pid",w->pid); json_int(exit,"exit_status_known",w->owned);
    if(w->owned) {
        json_int(exit,"exit_code",WIFEXITED(result) ? WEXITSTATUS(result) : -1);
        json_int(exit,"exit_signal",WIFSIGNALED(result) ? WTERMSIG(result) : 0);
        json_int(exit,"core_dumped",WIFSIGNALED(result) && WCOREDUMP(result));
    }
    logger_submit(s->logger,NULL,exit);
    if(!s->shutting_down && !w->busy && !w->stop_sent)backoff(s,"Standby worker exited unexpectedly");
    if(now_us()-w->spawned_us>10000000) {s->spawn_failures=0; s->next_spawn_us=0; *s->pool_error=0;}
    if(w->fd>=0)close(w->fd);
    if(w->pid_fd>=0)close(w->pid_fd);
    if(same_endpoint(w))unlink(w->endpoint);
    else if(!access(w->endpoint,F_OK)) {s->recovery_conflict=1;snprintf(s->pool_error,sizeof(s->pool_error),"Worker endpoint identity changed; operator reconciliation required");}
    memset(w,0,sizeof(*w));
}

void service_tick(evutil_socket_t fd,short flags,void *opaque) {
    (void)fd; (void)flags; Service *s=opaque; int64_t current=now_us();
    if(s->operation_request&&current-s->operation_request_us>=2000000)service_operation_unavailable(s,"worker_operation_timeout");
    if(s->create_request&&current-s->create_request_us>=2000000) {
        service_error_reply(s->create_request,503,"Worker acceptance is not confirmed; do not create a different operation");s->create_request=NULL;
    }
    if(s->stop_request&&current-s->stop_request_us>=2000000) {
        service_error_reply(s->stop_request,503,"Stop acknowledgement is unconfirmed; query or retry the same operation");s->stop_request=NULL;
    }
    if(s->watermark_request&&current-s->watermark_request_us>=2000000) {
        service_error_reply(s->watermark_request,503,"Worker acknowledgement timed out; inspect accepted_revision before retrying");
        s->watermark_request=NULL;
    }
    for(int i=0;i<MAX_WORKERS;i++) {
        Worker *w=&s->workers[i]; if(!w->pid)continue;
        read_worker(s,w);
        if(w->owned&&!w->endpoint_ino) {
            struct stat endpoint;
            if(!lstat(w->endpoint,&endpoint)&&S_ISSOCK(endpoint.st_mode)&&endpoint.st_uid==geteuid()) {
                w->endpoint_dev=endpoint.st_dev;w->endpoint_ino=endpoint.st_ino;
            }
        }
        int result=0; pid_t reaped=w->owned ? waitpid(w->pid,&result,WNOHANG) : 0;
        if(!w->owned) {
            struct pollfd process={.fd=w->pid_fd,.events=POLLIN};
            if(poll(&process,1,0)>0) {reaped=w->pid; result=0;}
        }
        if(reaped==w->pid) {reap_worker(s,w,result); continue;}
        if(w->fd<0&&reconnect_worker(s,w,current)<0) {
            /* Owned children must be reaped by waitpid, never a fabricated status. */
            if(w->owned) {
                reaped=waitpid(w->pid,&result,WNOHANG);
                if(reaped!=w->pid)continue;
            }
            reap_worker(s,w,result);continue;
        }
        if((s->shutting_down || (w==s->active && s->stopping)) && !w->finished)service_request_worker_stop(s,w);
        if(!w->needs_info&&!w->exit_deadline_us && current-w->last_seen_us>(int64_t)s->worker_timeout*1000000) {
            if(w==s->active)snprintf(s->error,sizeof(s->error),"Worker heartbeat timeout");
            w->exit_deadline_us=current;
        }
        if(w->exit_deadline_us && current>=w->exit_deadline_us && !w->kill_sent) {
            if(w==s->active && !*s->error)snprintf(s->error,sizeof(s->error),"Worker exit deadline exceeded; forced termination");
            signal_worker(w,SIGKILL); w->kill_sent=1;
        }
        if(w==s->active)service_dispatch(s,w);
    }
    if(s->shutting_down) {
        if(current>=s->drain_deadline_us) {
            for(int i=0;i<MAX_WORKERS;i++)if(s->workers[i].pid)s->drain_failed=1;
            if(s->recovery_conflict)s->drain_failed=1;
            event_base_loopbreak(s->base);return;
        }
        for(int i=0;i<MAX_WORKERS;i++)if(s->workers[i].pid)return;
        if(s->recovery_conflict)s->drain_failed=1;
        event_base_loopbreak(s->base); return;
    }
    for(int i=0;i<MAX_WORKERS;i++)if(s->workers[i].pid&&s->workers[i].needs_info)return;
    if(!s->recovery_conflict&&!s->spawn_failures)*s->pool_error=0;
    unsigned idle=0;
    for(int i=0;i<MAX_WORKERS;i++)if(s->workers[i].pid && !s->workers[i].busy)idle++;
    while(idle<s->spares) {if(!service_spawn_worker(s))break; idle++;}
}

void service_signal_stop(evutil_socket_t sig,short flags,void *opaque) {
    (void)sig; (void)flags; Service *s=opaque; s->shutting_down=1;
    if(!s->drain_deadline_us)s->drain_deadline_us=now_us()+((int64_t)s->stop_timeout+2)*1000000;
    if(s->active) {s->stopping=1; s->state=NAKI_JOB_STATE_STOPPING;}
}

int service_prepare_state(Service *s) {
    if(!*s->state_dir) {
        char resolved[PATH_MAX]; if(!realpath(s->run_dir,resolved))return -1;
        uint64_t hash=UINT64_C(14695981039346656037);
        for(const unsigned char *p=(const unsigned char *)resolved;*p;p++) {hash^=*p; hash*=UINT64_C(1099511628211);}
        const char *temporary=getenv("TMPDIR");if(!temporary||!*temporary)temporary="/tmp";
        if(*temporary!='/') {errno=EINVAL;return -1;}
        int length=snprintf(s->state_dir,sizeof(s->state_dir),"%s/naki-live-%u-%016llx",temporary,(unsigned)geteuid(),(unsigned long long)hash);
        if(length<0||(size_t)length>=sizeof(s->state_dir)) {errno=ENAMETOOLONG;return -1;}
    }
    if(mkdir(s->state_dir,0700) && errno!=EEXIST)return -1;
    struct stat st;
    if(lstat(s->state_dir,&st) || !S_ISDIR(st.st_mode) || st.st_uid!=geteuid() || (st.st_mode&0777)!=0700) {errno=EACCES; return -1;}
    char resolved[PATH_MAX];
    if(!realpath(s->state_dir,resolved) || strlen(resolved)>=sizeof(s->state_dir)) {errno=ENAMETOOLONG; return -1;}
    strcpy(s->state_dir,resolved);
    char lock[112]; snprintf(lock,sizeof(lock),"%s/supervisor.lock",s->state_dir);
    s->lock_fd=open(lock,O_CREAT|O_RDWR|O_CLOEXEC|O_NOFOLLOW,0600);
    return s->lock_fd<0 || flock(s->lock_fd,LOCK_EX|LOCK_NB) ? -1 : 0;
}

int service_recover_workers(Service *s) {
    DIR *directory=opendir(s->state_dir); if(!directory)return -1;
    struct dirent *entry; int slot=0;
    while((entry=readdir(directory))) {
        int expected=0,used=0;
        if(sscanf(entry->d_name,"w-%d.sock%n",&expected,&used)!=1 || !used || entry->d_name[used] || expected<=0)continue;
        char endpoint[108];
        if(snprintf(endpoint,sizeof(endpoint),"%s/%s",s->state_dir,entry->d_name)>=(int)sizeof(endpoint))continue;
        struct stat st;
        if(lstat(endpoint,&st) || !S_ISSOCK(st.st_mode) || st.st_uid!=geteuid())continue;
        if(slot>=MAX_WORKERS) {s->recovery_conflict=1;snprintf(s->pool_error,sizeof(s->pool_error),"Recovery candidate capacity exceeded; admission disabled");continue;}
        Worker *w=&s->workers[slot];
        *w=(Worker){.pid=expected,.fd=-1,.pid_fd=-1,.needs_info=1,.spawned_us=now_us(),.last_seen_us=now_us(),.endpoint_dev=st.st_dev,.endpoint_ino=st.st_ino};
        strcpy(w->endpoint,endpoint);
        if(reconnect_worker(s,w,now_us())<0) {memset(w,0,sizeof(*w));continue;}
        slot++;worker_event(s,w,"worker_reattaching");
    }
    closedir(directory); return 0;
}
