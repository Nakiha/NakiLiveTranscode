#define _GNU_SOURCE
#include "live.h"
#include "media_hooks.h"
#include <libavformat/live_mux.h>
#include "ipc.h"
#include "crash.h"
#include "identity.h"
#include "intent.h"
#include "component_update.h"
#include "operation_cache.h"
#include <errno.h>
#include <dirent.h>
#include <event2/buffer.h>
#include <event2/event.h>
#include <event2/http.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <unistd.h>
#include <libavformat/avformat.h>
#include <libavfilter/avfilter.h>
#include <libavutil/log.h>
#include <libavutil/time.h>

#define MAX_WORKERS 5
#define CREATE_RECORDS 32
#define CONTROL_STALE_US 2000000
typedef struct Creation {
    char operation_id[129],fingerprint[65],job_id[64],origin_request_id[129],state[24];
    int accepted,outcome_known;
    int64_t retained_us;
    yyjson_doc *snapshot;
    char stop_operation_id[129],stop_origin_request_id[129],stop_state[24];
    int stop_accepted;
    NakiOperationCache *operations,*sync_operations;
} Creation;
extern char **environ;
typedef struct Worker {
    pid_t pid;
    int fd,pid_fd,owned,needs_info,ready,busy,dispatched,assigned,finished,stop_sent,kill_sent,incompatible,updates_api;
    uint64_t export_id;unsigned export_cursor;
    char instance_id[33];
    char endpoint[108];
    int64_t spawned_us,ready_us,last_seen_us,exit_deadline_us;
    int64_t next_connect_us,connect_backoff_us;
    dev_t endpoint_dev;ino_t endpoint_ino;
} Worker;
typedef struct Service {
    struct event_base *base;
    Logger *logger; LoggerOptions logging;
    Worker workers[MAX_WORKERS],*active;
    yyjson_doc *config,*snapshot;
    char run_dir[2048],worker_path[PATH_MAX],job_id[64],request_id[129],log_path[4096],state[24],error[256],pool_error[256];
    char state_dir[80]; int lock_fd,recovery_conflict;
    unsigned sequence,status_interval,spares,worker_timeout,stop_timeout,spawn_failures;
    int shutting_down,stopping,prewarmed,exit_code,exit_signal,last_recovered,exit_status_known,core_dumped;
    pid_t last_pid;
    int64_t request_us,assigned_us,snapshot_us,next_spawn_us;
    uint64_t forwarded_logs,invalid_logs;
    struct evhttp_request *watermark_request;
    int64_t watermark_request_us;
    uint64_t watermark_request_revision;
    uint64_t watermark_token;
    struct evhttp_request *create_request;
    struct evhttp_request *stop_request;
    int64_t stop_request_us;
    int64_t create_request_us;
    int drain_only,drain_failed;
    int64_t drain_deadline_us;
    char service_namespace[129];
    char event_dir[4096];
    Creation creations[CREATE_RECORDS],*creation;
    struct evhttp_request *operation_request;
    Creation *operation_creation;
    int64_t operation_request_us;
    uint64_t operation_token;
    char operation_id[129];
    int operation_is_query;
} Service;

static void set_str(yyjson_mut_doc *doc,const char *key,const char *value) {
    yyjson_mut_obj_remove_key(yyjson_mut_doc_get_root(doc),key); json_str(doc,key,value);
}
static void reply(struct evhttp_request *req,int code,yyjson_mut_doc *doc) {
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
static void error_reply(struct evhttp_request *req,int code,const char *message) {
    yyjson_mut_doc *doc=new_event("error",NULL); json_str(doc,"error",message?message:"unavailable");
    const char *fallback=code==429?"resource_budget_exhausted":code==404?"not_found":code==409?"conflict":code==503?"unavailable":"invalid_request";
    json_str(doc,"error_code",message&&naki_id_valid(message,128)?message:fallback);reply(req,code,doc);
}
static Creation *find_creation(Service *s,const char *operation) {
    for(int i=0;i<CREATE_RECORDS;i++)if(!strcmp(s->creations[i].operation_id,operation))return &s->creations[i];
    return NULL;
}
static Creation *find_job(Service *s,const char *id) {
    for(int i=0;i<CREATE_RECORDS;i++)if(!strcmp(s->creations[i].job_id,id))return &s->creations[i];
    return NULL;
}
static Creation *reserve_creation(Service *s) {
    int64_t now=now_us();
    for(int i=0;i<CREATE_RECORDS;i++) {
        Creation *r=&s->creations[i];
        if(!*r->operation_id||(r!=s->creation&&r!=s->operation_creation&&r->retained_us&&now-r->retained_us>=1800000000)) {
            if(r->snapshot)yyjson_doc_free(r->snapshot);
            naki_operation_cache_close(&r->operations);naki_operation_cache_close(&r->sync_operations);
            memset(r,0,sizeof(*r));return r;
        }
    }
    return NULL;
}
static yyjson_mut_doc *creation_status(Creation *record) {
    yyjson_mut_doc *d=new_event("operation",NULL);
    json_str(d,"kind","create");json_str(d,"operation_id",record->operation_id);
    json_str(d,"job_id",record->job_id);json_str(d,"origin_request_id",record->origin_request_id);
    json_str(d,"state",record->state);
    yyjson_mut_obj_add_bool(d,yyjson_mut_doc_get_root(d),"outcome_known",record->outcome_known);
    char path[160];snprintf(path,sizeof(path),"/v1/jobs/%s",record->job_id);json_str(d,"job_url",path);
    return d;
}
static yyjson_mut_doc *stop_status(Creation *record) {
    yyjson_mut_doc *d=new_event("operation",NULL);json_str(d,"kind","stop");
    json_str(d,"job_id",record->job_id);json_str(d,"operation_id",record->stop_operation_id);
    json_str(d,"origin_request_id",record->stop_origin_request_id);json_str(d,"state",record->stop_state);
    yyjson_mut_obj_add_bool(d,yyjson_mut_doc_get_root(d),"outcome_known",record->stop_accepted);return d;
}
static int ensure_operation_cache(Creation *record) {
    if(!record->operations)record->operations=naki_operation_cache_open(record->job_id);
    return record->operations?0:-1;
}
static yyjson_mut_doc *operation_projection(yyjson_val *record) {
    yyjson_mut_doc *d=yyjson_mut_doc_new(NULL);if(!d)return NULL;
    yyjson_mut_doc_set_root(d,yyjson_val_mut_copy(d,record));
    const char *job=yyjson_get_str(yyjson_obj_get(record,"job_id")),*operation=yyjson_get_str(yyjson_obj_get(record,"operation_id"));
    char path[256];snprintf(path,sizeof(path),"/v1/jobs/%s/operations/%s",job,operation);json_str(d,"operation_url",path);return d;
}
static void operation_unavailable(Service *s,const char *reason) {
    if(!s->operation_request)return;
    yyjson_mut_doc *cached=s->operation_creation?naki_operation_cache_get(s->operation_creation->operations,s->operation_id):NULL;
    if(cached)reply(s->operation_request,s->operation_is_query?200:503,cached);
    else error_reply(s->operation_request,503,reason);
    s->operation_request=NULL;s->operation_creation=NULL;
}
static int operation_rpc(Service *s,struct evhttp_request *req,Creation *record,const char *operation,yyjson_val *update,int64_t received) {
    if(s->operation_request) {error_reply(req,429,"operation_query_or_update_budget_exhausted");return -1;}
    Worker *w=s->active;
    if(!w||strcmp(record->job_id,s->job_id)||w->fd<0||w->needs_info||w->incompatible||!w->updates_api) {
        error_reply(req,503,"worker_operation_unavailable_or_recovering");return -1;
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
    if(ret) {error_reply(req,503,"worker_operation_not_queued");return -1;}
    evhttp_request_own(req);s->operation_request=req;s->operation_creation=record;s->operation_request_us=received;
    strcpy(s->operation_id,operation);s->operation_is_query=update==NULL;return 0;
}
static yyjson_doc *read_body(struct evhttp_request *req,size_t maximum) {
    struct evbuffer *input=evhttp_request_get_input_buffer(req);size_t size=evbuffer_get_length(input);
    if(!size||size>maximum) {error_reply(req,413,"Request body is empty or exceeds its bound");return NULL;}
    yyjson_doc *d=yyjson_read((const char *)evbuffer_pullup(input,-1),size,0);char error[128];
    if(!d||!yyjson_is_obj(yyjson_doc_get_root(d))||naki_intent_validate(yyjson_doc_get_root(d),error,sizeof(error))) {
        if(d)yyjson_doc_free(d);
        error_reply(req,400,"Invalid or duplicate-key JSON request");return NULL;
    }
    return d;
}
static int explicit_target(struct evhttp_request *req,Service *s,yyjson_doc *body) {
    yyjson_val *id=yyjson_obj_get(yyjson_doc_get_root(body),"job_id");
    if(!naki_json_id(id,63)) {error_reply(req,400,"An explicit valid job_id is required");return -1;}
    if(strcmp(yyjson_get_str(id),s->job_id)) {error_reply(req,409,"job_id does not match the assigned task");return -1;}
    return 0;
}
static void pool_json(Service *s,yyjson_mut_doc *doc) {
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
static const char *readiness_reason(Service *s,int64_t current) {
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
static const char *admission_reason(Service *s,int64_t current) {
    const char *reason=readiness_reason(s,current);if(reason)return reason;
    if(s->active)return "busy";
    int creation=0,worker=0,free_slot=0;
    for(int i=0;i<CREATE_RECORDS;i++) {
        Creation *r=&s->creations[i];
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
static void resource_observation(yyjson_mut_doc *d) {
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
static void control_health(Service *s,yyjson_mut_doc *d) {
    int64_t current=now_us();const char *ready=readiness_reason(s,current),*admit=admission_reason(s,current);
    yyjson_mut_val *root=yyjson_mut_doc_get_root(d),*live=yyjson_mut_obj(d),*readiness=yyjson_mut_obj(d),*admission=yyjson_mut_obj(d);
    yyjson_mut_obj_remove_key(root,"liveness");yyjson_mut_obj_remove_key(root,"readiness");yyjson_mut_obj_remove_key(root,"admission");
    yyjson_mut_obj_add_bool(d,live,"alive",1);yyjson_mut_obj_add_str(d,live,"boundary","controller_http_event_loop");
    yyjson_mut_obj_add_bool(d,readiness,"ready",!ready);yyjson_mut_obj_add_strcpy(d,readiness,"reason",ready?ready:"ready");
    yyjson_mut_obj_add_bool(d,admission,"open",!admit);yyjson_mut_obj_add_strcpy(d,admission,"reason",admit?admit:"open");
    yyjson_mut_obj_add_val(d,root,"liveness",live);yyjson_mut_obj_add_val(d,root,"readiness",readiness);yyjson_mut_obj_add_val(d,root,"admission",admission);
    json_int(d,"controller_observed_us",current);
}
static void snapshot_freshness(Service *s,yyjson_mut_doc *d) {
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
static yyjson_mut_doc *status(Service *s) {
    yyjson_mut_doc *doc=s->snapshot ? yyjson_doc_mut_copy(s->snapshot,NULL) : new_event("status",NULL);
    if(!doc)return NULL;
    yyjson_mut_obj_remove_key(yyjson_mut_doc_get_root(doc),"job_info");
    yyjson_mut_obj_remove_key(yyjson_mut_doc_get_root(doc),"ipc_kind");
    yyjson_mut_obj_remove_key(yyjson_mut_doc_get_root(doc),"ipc_version");
    set_str(doc,"event","status"); set_str(doc,"state",s->state);
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
    pool_json(s,doc);control_health(s,doc);snapshot_freshness(s,doc);return doc;
}
static void worker_event(Service *s,Worker *w,const char *event) {
    yyjson_mut_doc *doc=new_event(event,NULL);
    if(*w->instance_id)json_str(doc,"worker_instance_id",w->instance_id);
    if(w==s->active)json_str(doc,"job_id",s->job_id);
    json_int(doc,"worker_pid",w->pid); pool_json(s,doc); logger_submit(s->logger,NULL,doc);
}
static yyjson_mut_doc *observation(Service *s,const char *event) {
    yyjson_mut_doc *doc=new_event(event,NULL),*projection=status(s);
    if(!doc||!projection) {if(doc)yyjson_mut_doc_free(doc);if(projection)yyjson_mut_doc_free(projection);return NULL;}
    if(*s->job_id)json_str(doc,"job_id",s->job_id);
    if(*s->request_id)json_str(doc,"create_request_id",s->request_id);
    json_str(doc,"state",s->state);json_int(doc,"snapshot_received_us",s->snapshot_us);json_int(doc,"controller_observed_us",now_us());
    yyjson_mut_obj_add_val(doc,yyjson_mut_doc_get_root(doc),"snapshot",yyjson_mut_val_mut_copy(doc,yyjson_mut_doc_get_root(projection)));
    yyjson_mut_doc_free(projection);return doc;
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
static Worker *spawn_worker(Service *s) {
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
static void request_worker_stop(Service *s,Worker *w) {
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
static void dispatch(Service *s,Worker *w) {
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
    Creation *record=find_creation(s,operation);
    if(record&&(strcmp(record->fingerprint,fingerprint)||strcmp(record->job_id,id)))return -1;
    if(!record)record=reserve_creation(s);
    if(!record)return -1;
    strcpy(record->operation_id,operation);strcpy(record->fingerprint,fingerprint);strcpy(record->job_id,id);
    strcpy(record->origin_request_id,request);strcpy(record->state,"accepted");record->accepted=record->outcome_known=1;
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
        record->stop_accepted=1;strcpy(record->stop_state,"stopping");
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
        } else if(ipc_kind(doc,"operation_reply")) {
            yyjson_val *root=yyjson_doc_get_root(doc),*record=yyjson_obj_get(root,"operation");
            const char *job=yyjson_get_str(yyjson_obj_get(root,"job_id")),*requester=yyjson_get_str(yyjson_obj_get(root,"requester_instance_id"));
            Creation *creation=job?find_job(s,job):NULL;int code=yyjson_get_int(yyjson_obj_get(root,"http_status"));
            int valid=w==s->active&&creation==s->creation&&creation&&requester&&!strcmp(requester,naki_process_id());
            if(valid&&record) {
                const char *kind=yyjson_get_str(yyjson_obj_get(record,"kind"));
                if(kind&&!strcmp(kind,"update")&&(ensure_operation_cache(creation)||naki_operation_cache_put(creation->operations,record,now_us())))valid=0;
            }
            if(valid&&s->operation_request&&creation==s->operation_creation&&yyjson_get_uint(yyjson_obj_get(root,"token"))==s->operation_token) {
                if(record) {
                    const char *id=yyjson_get_str(yyjson_obj_get(record,"operation_id"));
                    yyjson_doc *newest=naki_operation_cache_newest(creation->operations,id);
                    yyjson_val *answer=newest?yyjson_doc_get_root(newest):record;
                    reply(s->operation_request,s->operation_is_query||naki_operation_result_terminal(answer)?200:202,operation_projection(answer));
                    if(newest)yyjson_doc_free(newest);
                }
                else {
                    if(!s->operation_is_query)naki_operation_cache_unreserve(creation->operations,s->operation_id);
                    error_reply(s->operation_request,code>=400&&code<=599?code:503,yyjson_get_str(yyjson_obj_get(root,"error")));
                }
                s->operation_request=NULL;s->operation_creation=NULL;
            } else if(!valid&&s->operation_request)operation_unavailable(s,"operation_reconciliation_failed");
        } else if(ipc_kind(doc,"accepted")) {
            const char *id=yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(doc),"job_id"));
            if(w==s->active&&w->dispatched&&id&&!strcmp(id,s->job_id)) {
                w->assigned=1;
                if(s->creation) {strcpy(s->creation->state,"accepted");s->creation->accepted=s->creation->outcome_known=1;}
                if(s->create_request) {reply(s->create_request,202,status(s));s->create_request=NULL;}
            }
        } else if(ipc_kind(doc,"control_rejected")) {
            worker_event(s,w,"worker_control_rejected");
        } else if(ipc_kind(doc,"stop_ack")) {
            yyjson_val *root=yyjson_doc_get_root(doc);
            const char *job=yyjson_get_str(yyjson_obj_get(root,"job_id"));
            const char *operation=yyjson_get_str(yyjson_obj_get(root,"operation_id"));
            if(w==s->active&&s->creation&&job&&!strcmp(job,s->job_id)&&operation&&!strcmp(operation,s->creation->stop_operation_id)) {
                s->creation->stop_accepted=1;strcpy(s->creation->stop_state,"stopping");
                if(s->stop_request) {reply(s->stop_request,202,stop_status(s->creation));s->stop_request=NULL;}
            }
        } else if(ipc_kind(doc,"watermark_ack")) {
            yyjson_val *root=yyjson_doc_get_root(doc);
            yyjson_val *record=yyjson_obj_get(root,"operation");
            const char *ack_job=yyjson_get_str(yyjson_obj_get(root,"job_id")),*ack_operation=yyjson_get_str(yyjson_obj_get(root,"operation_id"));
            if(w==s->active&&s->creation&&ack_job&&!strcmp(ack_job,s->job_id)&&naki_id_valid(ack_operation,128)&&
               !record&&!yyjson_get_sint(yyjson_obj_get(root,"accepted")))
                naki_operation_cache_unreserve(s->creation->operations,ack_operation);
            if(w==s->active&&s->creation&&record&&(ensure_operation_cache(s->creation)||naki_operation_cache_put(s->creation->operations,record,now_us()))) {
                w->incompatible=1;w->needs_info=1;yyjson_doc_free(doc);break;
            }
            if(w==s->active&&s->watermark_request&&yyjson_get_uint(yyjson_obj_get(root,"revision"))==s->watermark_request_revision&&
               yyjson_get_uint(yyjson_obj_get(root,"token"))==s->watermark_token) {
                yyjson_mut_doc *answer=yyjson_doc_mut_copy(doc,NULL);
                yyjson_mut_val *answer_root=yyjson_mut_doc_get_root(answer);
                yyjson_mut_obj_remove_key(answer_root,"ipc_version");yyjson_mut_obj_remove_key(answer_root,"ipc_kind");yyjson_mut_obj_remove_key(answer_root,"token");
                json_str(answer,"event",yyjson_get_sint(yyjson_obj_get(root,"accepted"))?"watermark_accepted":"watermark_rejected");json_str(answer,"job_id",s->job_id);
                reply(s->watermark_request,yyjson_get_sint(yyjson_obj_get(root,"accepted"))?202:409,answer);
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
                if(s->creation) {strcpy(s->creation->state,"accepted");s->creation->accepted=s->creation->outcome_known=1;}
                if(s->create_request) {reply(s->create_request,202,status(s));s->create_request=NULL;}
            }
            if(w!=s->active || !w->assigned) {yyjson_doc_free(doc); break;}
            const char *id=yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(doc),"job_id"));
            if(!id || strcmp(id,s->job_id)) {w->incompatible=1;w->needs_info=1;s->recovery_conflict=1;yyjson_doc_free(doc); break;}
            if(s->snapshot)yyjson_doc_free(s->snapshot);
            s->snapshot=doc; s->snapshot_us=now_us(); doc=NULL;
            const char *stop_operation=yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(s->snapshot),"stop_operation_id"));
            if(s->creation&&stop_operation&&*stop_operation&&!strcmp(stop_operation,s->creation->stop_operation_id)) {
                s->creation->stop_accepted=1;strcpy(s->creation->stop_state,"stopping");
                if(s->stop_request) {reply(s->stop_request,202,stop_status(s->creation));s->stop_request=NULL;}
            }
            if(ipc_kind(s->snapshot,"finished")) {
                w->finished=1;
                if(!w->exit_deadline_us)w->exit_deadline_us=now_us()+(int64_t)s->stop_timeout*1000000;
                yyjson_mut_doc *ack=ipc_message("ack"); ipc_send(w->fd,ack); yyjson_mut_doc_free(ack);
                strcpy(s->state,"finishing");
            } else if(!s->stopping) {
                const char *state=yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(s->snapshot),"state"));
                strcpy(s->state,state && !strcmp(state,"running") ? "running" : state && !strcmp(state,"waiting_assets") ? "waiting_assets" : "starting");
            }
        } else {
            yyjson_doc_free(doc); break;
        }
        if(doc)yyjson_doc_free(doc);
    }
}
static void reap_worker(Service *s,Worker *w,int result) {
    if(w==s->active) {
        operation_unavailable(s,"worker_exited_before_operation_confirmation");
        if(s->stop_request) {error_reply(s->stop_request,503,"Worker exited before stop acknowledgement was confirmed");s->stop_request=NULL;}
        if(s->create_request) {
            error_reply(s->create_request,503,"Worker exited before acceptance was confirmed");s->create_request=NULL;
        }
        s->last_pid=w->pid; s->exit_status_known=w->owned;
        s->exit_code=w->owned && WIFEXITED(result) ? WEXITSTATUS(result) : -1;
        s->exit_signal=w->owned && WIFSIGNALED(result) ? WTERMSIG(result) : 0;
        s->core_dumped=w->owned && WIFSIGNALED(result) && WCOREDUMP(result);
        const char *final=s->snapshot ? yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(s->snapshot),"state")) : NULL;
        if(w->finished && (s->exit_code==0 || !w->owned) && final &&
                (!strcmp(final,"completed") || !strcmp(final,"completed_partial") || !strcmp(final,"failed") || !strcmp(final,"stopped")))snprintf(s->state,sizeof(s->state),"%s",final);
        else if(s->stopping)strcpy(s->state,"stopped");
        else {
            strcpy(s->state,"failed");
            if(!*s->error)snprintf(s->error,sizeof(s->error),"Worker exited unexpectedly (code=%d signal=%d)",s->exit_code,s->exit_signal);
        }
        s->active=NULL;
        if(s->creation) {
            if(*s->creation->stop_operation_id)strcpy(s->creation->stop_state,s->creation->stop_accepted?(!strcmp(s->state,"stopped")?"stopped":"failed"):"unknown");
            yyjson_mut_doc *terminal=status(s);size_t bytes=0;char *text=terminal?yyjson_mut_write(terminal,0,&bytes):NULL;
            if(terminal)yyjson_mut_doc_free(terminal);
            if(text&&bytes<=IPC_MAX_BYTES)s->creation->snapshot=yyjson_read(text,bytes,0);
            free(text);
            strcpy(s->creation->state,s->creation->accepted?"accepted":"unknown");
            s->creation->retained_us=now_us();s->creation=NULL;
        }
        logger_submit(s->logger,NULL,observation(s,"job_observed_terminal"));
        if(!s->stopping && (!w->finished || (w->owned && s->exit_code!=0))) {
            yyjson_mut_doc *crash=observation(s,"worker_crash");
            json_str(crash,"signal_name",s->exit_signal ? strsignal(s->exit_signal) : "unknown");
            json_str(crash,"diagnostic_boundary","supervisor_observed_exit_with_last_worker_snapshot");
            logger_submit(s->logger,NULL,crash);
        }
        yyjson_val *startup=s->snapshot ? yyjson_obj_get(yyjson_doc_get_root(s->snapshot),"startup") : NULL;
        if(!startup || yyjson_is_null(yyjson_obj_get(startup,"request_to_first_video_mux_ms"))) {
            yyjson_mut_doc *incomplete=observation(s,"startup_incomplete");
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
static void tick(evutil_socket_t fd,short flags,void *opaque) {
    (void)fd; (void)flags; Service *s=opaque; int64_t current=now_us();
    if(s->operation_request&&current-s->operation_request_us>=2000000)operation_unavailable(s,"worker_operation_timeout");
    if(s->create_request&&current-s->create_request_us>=2000000) {
        error_reply(s->create_request,503,"Worker acceptance is not confirmed; do not create a different operation");s->create_request=NULL;
    }
    if(s->stop_request&&current-s->stop_request_us>=2000000) {
        error_reply(s->stop_request,503,"Stop acknowledgement is unconfirmed; query or retry the same operation");s->stop_request=NULL;
    }
    if(s->watermark_request&&current-s->watermark_request_us>=2000000) {
        error_reply(s->watermark_request,503,"Worker acknowledgement timed out; inspect accepted_revision before retrying");
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
        if((s->shutting_down || (w==s->active && s->stopping)) && !w->finished)request_worker_stop(s,w);
        if(!w->needs_info&&!w->exit_deadline_us && current-w->last_seen_us>(int64_t)s->worker_timeout*1000000) {
            if(w==s->active)snprintf(s->error,sizeof(s->error),"Worker heartbeat timeout");
            w->exit_deadline_us=current;
        }
        if(w->exit_deadline_us && current>=w->exit_deadline_us && !w->kill_sent) {
            if(w==s->active && !*s->error)snprintf(s->error,sizeof(s->error),"Worker exit deadline exceeded; forced termination");
            signal_worker(w,SIGKILL); w->kill_sent=1;
        }
        if(w==s->active)dispatch(s,w);
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
    while(idle<s->spares) {if(!spawn_worker(s))break; idle++;}
}
static void handle_request(struct evhttp_request *req,void *opaque) {
    /* Callback entry follows receipt of the complete HTTP request body. */
    int64_t received=now_us(); Service *s=opaque;
    char generated[33];const char *attempt=evhttp_find_header(evhttp_request_get_input_headers(req),"X-Request-ID");
    if(naki_id_generate(generated)) {error_reply(req,503,"Request identity unavailable");return;}
    evhttp_add_header(evhttp_request_get_output_headers(req),"X-Request-ID",attempt&&naki_id_valid(attempt,128)?attempt:generated);
    if(attempt&&!naki_id_valid(attempt,128)) {error_reply(req,400,"Invalid X-Request-ID");return;}
    const char *uri=evhttp_request_get_uri(req); enum evhttp_cmd_type method=evhttp_request_get_command(req);
    char stop_target[64]="",update_target[64]="";
    if(method==EVHTTP_REQ_POST&&!strncmp(uri,"/v1/jobs/",9)) {
        const char *tail=uri+9,*end=strchr(tail,'/');size_t length=end?(size_t)(end-tail):0;
        if(end&&!strcmp(end,"/stop")&&length&&length<sizeof(stop_target)) {
            memcpy(stop_target,tail,length);stop_target[length]=0;
            if(!naki_id_valid(stop_target,63)) {error_reply(req,400,"Invalid job target");return;}
            uri="/v1/job/stop";
        } else if(end&&!strcmp(end,"/updates")&&length&&length<sizeof(update_target)) {
            memcpy(update_target,tail,length);update_target[length]=0;
            if(!naki_id_valid(update_target,63)) {error_reply(req,400,"Invalid job target");return;}
        }
    }
    if((!strcmp(uri,"/health/live")||!strcmp(uri,"/health/ready")||!strcmp(uri,"/health/admission"))&&method==EVHTTP_REQ_GET) {
        yyjson_mut_doc *doc=new_event("health_state",NULL);control_health(s,doc);snapshot_freshness(s,doc);pool_json(s,doc);
        const char *reason=!strcmp(uri,"/health/ready")?readiness_reason(s,now_us()):!strcmp(uri,"/health/admission")?admission_reason(s,now_us()):NULL;
        reply(req,reason?503:200,doc);return;
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
        pool_json(s,doc);control_health(s,doc);snapshot_freshness(s,doc);resource_observation(doc);reply(req,200,doc);return;
    }
    if(!strncmp(uri,"/v1/jobs/",9)&&method==EVHTTP_REQ_GET) {
        const char *tail=uri+9,*separator=strchr(tail,'/');char id[64];
        size_t length=separator?(size_t)(separator-tail):strlen(tail);
        if(!length||length>=sizeof(id)) {error_reply(req,400,"Invalid job target");return;}
        memcpy(id,tail,length);id[length]=0;
        if(!naki_id_valid(id,63)) {error_reply(req,400,"Invalid job target");return;}
        if(!separator&&!strcmp(id,s->job_id)) {reply(req,200,status(s));return;}
        if(!separator)for(int i=0;i<CREATE_RECORDS;i++) {
            Creation *r=&s->creations[i];
            if(!strcmp(r->job_id,id)&&r->snapshot) {reply(req,200,yyjson_doc_mut_copy(r->snapshot,NULL));return;}
        }
        if(separator&&!strncmp(separator,"/operations/",12)&&naki_id_valid(separator+12,128)) {
            Creation *r=find_creation(s,separator+12);
            if(r&&!strcmp(r->job_id,id)) {reply(req,200,creation_status(r));return;}
            r=find_job(s,id);
            if(r&&*r->stop_operation_id&&!strcmp(r->stop_operation_id,separator+12)) {reply(req,200,stop_status(r));return;}
            if(r) {
                if(s->active&&r==s->creation&&!s->active->needs_info&&s->active->fd>=0) {
                    operation_rpc(s,req,r,separator+12,NULL,received);return;
                }
                yyjson_mut_doc *cached=naki_operation_cache_get(r->operations,separator+12);
                if(cached) {reply(req,200,cached);return;}
                if(r==s->creation&&s->active) {error_reply(req,503,"worker_operation_unavailable_or_recovering");return;}
            }
        }
        error_reply(req,404,"Job or retained operation not found");return;
    }
    if(*update_target&&method==EVHTTP_REQ_POST) {
        yyjson_doc *body=read_body(req,8192);if(!body)return;
        NakiUpdateRequest update;char error[256];
        if(naki_update_parse(yyjson_doc_get_root(body),update_target,&update,error,sizeof(error))) {yyjson_doc_free(body);error_reply(req,400,error);return;}
        Creation *record=find_job(s,update_target);
        if(!record) {yyjson_doc_free(body);error_reply(req,404,"job_not_found");return;}
        if(!strcmp(record->operation_id,update.operation_id)||(*record->stop_operation_id&&!strcmp(record->stop_operation_id,update.operation_id))) {
            yyjson_doc_free(body);error_reply(req,409,"operation_id_conflict");return;
        }
        if(ensure_operation_cache(record)) {yyjson_doc_free(body);error_reply(req,429,"controller_operation_capacity_exhausted");return;}
        int reservation=naki_operation_cache_reserve(record->operations,update.operation_id,update.fingerprint,received);
        if(reservation<0) {
            if(reservation==-ENOSPC&&s->active&&record==s->creation&&!s->active->needs_info&&s->active->fd>=0) {
                yyjson_mut_doc *sync=ipc_message("operations_sync");json_str(sync,"job_id",record->job_id);json_str(sync,"worker_instance_id",s->active->instance_id);
                if(!ipc_send(s->active->fd,sync))s->active->needs_info=1;
                yyjson_mut_doc_free(sync);
            }
            yyjson_doc_free(body);error_reply(req,reservation==-EEXIST?409:429,reservation==-EEXIST?"operation_id_conflict":"controller_operation_capacity_exhausted");return;
        }
        if(!s->active||record!=s->creation||s->active->finished||s->shutting_down||s->stopping) {
            yyjson_doc_free(body);
            if(!reservation) {
                yyjson_mut_doc *cached=naki_operation_cache_get(record->operations,update.operation_id);
                if(cached) {yyjson_mut_val *root=yyjson_mut_doc_get_root(cached);reply(req,yyjson_mut_get_bool(yyjson_mut_obj_get(root,"outcome_known"))?200:503,cached);return;}
            }
            naki_operation_cache_unreserve(record->operations,update.operation_id);error_reply(req,409,"job_closing_or_terminal");return;
        }
        if(operation_rpc(s,req,record,update.operation_id,yyjson_doc_get_root(body),received)&&reservation==1)naki_operation_cache_unreserve(record->operations,update.operation_id);
        yyjson_doc_free(body);return;
    }
    if(!strcmp(uri,"/v1/job") && method==EVHTTP_REQ_GET) {reply(req,200,status(s)); return;}
    if(!strcmp(uri,"/v1/job/watermark")&&method==EVHTTP_REQ_POST) {
        if(!s->active||!s->active->assigned||s->active->finished||s->stopping||s->shutting_down) {error_reply(req,409,"No running assigned job");return;}
        if(s->active->needs_info||!s->active->updates_api) {error_reply(req,503,"worker_operation_unavailable_or_recovering");return;}
        if(s->watermark_request) {error_reply(req,409,"Watermark acknowledgement pending");return;}
        yyjson_doc *targeted=read_body(req,8192);if(!targeted)return;
        if(explicit_target(req,s,targeted)) {yyjson_doc_free(targeted);return;}
        yyjson_mut_doc *adapted=yyjson_doc_mut_copy(targeted,NULL);yyjson_doc_free(targeted);
        if(!adapted) {error_reply(req,500,"Allocation failure");return;}
        yyjson_mut_obj_remove_key(yyjson_mut_doc_get_root(adapted),"job_id");
        size_t size=0;char *body=yyjson_mut_write(adapted,0,&size);yyjson_mut_doc_free(adapted);
        yyjson_doc *parsed=body?yyjson_read(body,size,0):NULL;free(body);
        NakiWatermarkConfig config;uint64_t revision;char error[256];
        if(!parsed||naki_watermark_update(yyjson_doc_get_root(parsed),&config,&revision,error,sizeof(error))) {
            error_reply(req,400,parsed?error:"Invalid JSON");if(parsed)yyjson_doc_free(parsed);return;
        }
        const char *output_id=yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(parsed),"output_id"));Config original;
        size_t config_size=0;char *config_text=yyjson_val_write(yyjson_doc_get_root(s->config),0,&config_size);
        if(!config_text||config_parse(config_text,config_size,&original,error,sizeof(error))) {free(config_text);yyjson_doc_free(parsed);error_reply(req,503,"assignment_configuration_unavailable");return;}
        free(config_text);if(!output_id&&!original.output_count)output_id=original.single_output_id;
        s->watermark_token=FFMAX(s->watermark_token+1,(uint64_t)received);
        char operation_id[129];snprintf(operation_id,sizeof(operation_id),"legacy_watermark_%llu",(unsigned long long)s->watermark_token);
        NakiUpdateRequest update;
        if(!output_id) {yyjson_doc_free(parsed);error_reply(req,409,"output_id is required");return;}
        if(naki_update_from_legacy(&config,revision,s->job_id,output_id,operation_id,&update,error,sizeof(error))) {yyjson_doc_free(parsed);error_reply(req,400,error);return;}
        if(ensure_operation_cache(s->creation)||naki_operation_cache_reserve(s->creation->operations,operation_id,update.fingerprint,received)<0) {yyjson_doc_free(parsed);error_reply(req,429,"controller_operation_capacity_exhausted");return;}
        yyjson_mut_doc *message=ipc_message("watermark");
        json_str(message,"job_id",s->job_id);json_str(message,"worker_instance_id",s->active->instance_id);
        json_str(message,"operation_id",operation_id);json_str(message,"origin_request_id",evhttp_find_header(evhttp_request_get_output_headers(req),"X-Request-ID"));json_int(message,"token",(int64_t)s->watermark_token);
        yyjson_mut_obj_add_val(message,yyjson_mut_doc_get_root(message),"update",yyjson_val_mut_copy(message,yyjson_doc_get_root(parsed)));
        int sent=ipc_send(s->active->fd,message);yyjson_mut_doc_free(message);yyjson_doc_free(parsed);
        if(sent) {naki_operation_cache_unreserve(s->creation->operations,operation_id);error_reply(req,503,"Worker unavailable; update not queued");return;}
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
        if(bad) {yyjson_doc_free(body);error_reply(req,400,"Stop requires an explicit job target and valid operation_id");return;}
        char operation_id[129];strcpy(operation_id,operation?yyjson_get_str(operation):generated);
        Creation *record=find_job(s,id);
        if(record&&*record->stop_operation_id&&!strcmp(record->stop_operation_id,operation_id)) {
            yyjson_doc_free(body);reply(req,record->stop_accepted?202:503,stop_status(record));return;
        }
        if(record&&!strcmp(record->operation_id,operation_id)) {yyjson_doc_free(body);error_reply(req,409,"operation_id_conflict");return;}
        if(record&&naki_operation_cache_contains(record->operations,operation_id)) {yyjson_doc_free(body);error_reply(req,409,"operation_id_conflict");return;}
        if(s->active&&s->active->needs_info) {yyjson_doc_free(body);error_reply(req,503,"worker_operation_unavailable_or_recovering");return;}
        if(!s->active||s->active->finished||strcmp(id,s->job_id)||s->shutting_down||s->stopping) {yyjson_doc_free(body);error_reply(req,409,"Job target is not available for a new stop operation");return;}
        if(!record) {yyjson_doc_free(body);error_reply(req,503,"Assignment record unavailable");return;}
        strcpy(record->stop_operation_id,operation_id);strcpy(record->stop_origin_request_id,attempt?attempt:generated);strcpy(record->stop_state,"reserved");
        yyjson_doc_free(body);
        if(*stop_target) {evhttp_request_own(req);s->stop_request=req;s->stop_request_us=received;}
        s->stopping=1; strcpy(s->state,"stopping"); request_worker_stop(s,s->active);
        if(!*stop_target)reply(req,202,status(s));
        return;
    }
    if((!strcmp(uri,"/v1/job")||!strcmp(uri,"/v1/jobs")) && method==EVHTTP_REQ_POST) {
        int canonical=!strcmp(uri,"/v1/jobs");
        if(s->shutting_down) {error_reply(req,503,"Service is shutting down"); return;}
        for(int i=0;i<MAX_WORKERS;i++)if(s->workers[i].pid && s->workers[i].needs_info) {error_reply(req,503,"Recovering existing workers"); return;}
        if(s->recovery_conflict) {error_reply(req,503,"Multiple workers require operator reconciliation"); return;}
        yyjson_doc *envelope=read_body(req,16384);if(!envelope)return;
        yyjson_val *root=yyjson_doc_get_root(envelope),*spec=root;
        char operation_id[129];
        if(canonical) {
            yyjson_val *operation=yyjson_obj_get(root,"operation_id");spec=yyjson_obj_get(root,"spec");
            if(yyjson_obj_size(root)!=2||!naki_json_id(operation,128)||!yyjson_is_obj(spec)) {
                yyjson_doc_free(envelope);error_reply(req,400,"Create requires only operation_id and spec");return;
            }
            strcpy(operation_id,yyjson_get_str(operation));
        } else strcpy(operation_id,generated);
        size_t size=0;char *body=yyjson_val_write(spec,0,&size);Config config;char error[256]="Cannot normalize create intent",fingerprint[65];
        if(!body||config_parse(body,size,&config,error,sizeof(error))||naki_config_fingerprint(&config,fingerprint,error,sizeof(error))) {
            int allocated=body!=NULL;free(body);yyjson_doc_free(envelope);error_reply(req,400,allocated?error:"Allocation failure");return;
        }
        yyjson_doc *parsed=yyjson_read(body,size,0);free(body);yyjson_doc_free(envelope);
        if(!parsed) {error_reply(req,500,"Allocation failure");return;}
        Creation *record=find_creation(s,operation_id);
        if(record) {
            yyjson_doc_free(parsed);
            if(strcmp(record->fingerprint,fingerprint))error_reply(req,409,"operation_id_conflict");
            else if(record->accepted)reply(req,202,creation_status(record));
            else reply(req,503,creation_status(record));
            return;
        }
        if(s->active) {yyjson_doc_free(parsed);error_reply(req,409,"Single-stream service already has an active job");return;}
        const char *closed=admission_reason(s,received);
        if(closed) {yyjson_doc_free(parsed);error_reply(req,503,closed);return;}
        record=reserve_creation(s);
        if(!record) {yyjson_doc_free(parsed);error_reply(req,503,"Creation retention capacity exhausted");return;}
        Worker *w=NULL;
        for(int i=0;i<MAX_WORKERS;i++)if(s->workers[i].pid && s->workers[i].ready && !s->workers[i].busy) {w=&s->workers[i]; break;}
        int prewarmed=w && w->ready_us<=received;
        if(!w)for(int i=0;i<MAX_WORKERS;i++)if(s->workers[i].pid && !s->workers[i].busy) {w=&s->workers[i]; break;}
        if(!w)w=spawn_worker(s);
        if(!w) {yyjson_doc_free(parsed); error_reply(req,503,"Worker unavailable; retry after pool recovery"); return;}
        char job_id[33];
        if(naki_id_generate(job_id)) {yyjson_doc_free(parsed);error_reply(req,503,"Job identity unavailable");return;}
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
        strcpy(record->origin_request_id,s->request_id);strcpy(record->state,"reserved");s->creation=record;
        if(config.frame_log)snprintf(s->log_path,sizeof(s->log_path),"%s/%s.trace",s->run_dir,s->job_id);
        strcpy(s->state,"starting");
        yyjson_mut_doc *accepted=new_event("request_received",NULL);
        json_str(accepted,"job_id",s->job_id); json_str(accepted,"request_id",s->request_id);
        json_int(accepted,"request_monotonic_us",received);
        json_str(accepted,"boundary","complete_http_request_handler");
        json_int(accepted,"worker_pid",w->pid); logger_submit(s->logger,NULL,accepted);
        evhttp_request_own(req);s->create_request=req;s->create_request_us=received;dispatch(s,w);return;
    }
    if(!strcmp(uri,"/health")||!strcmp(uri,"/health/live")||!strcmp(uri,"/health/ready")||!strcmp(uri,"/health/admission")||!strcmp(uri,"/v1/job")||!strcmp(uri,"/v1/job/stop")||!strcmp(uri,"/v1/job/watermark"))error_reply(req,405,"Method not allowed");
    else error_reply(req,404,"Unknown endpoint");
}
static void report_status(evutil_socket_t fd,short flags,void *opaque) {
    (void)fd; (void)flags; Service *s=opaque;
    if(s->active)return; /* Worker alone owns nonoverlapping reporting windows. */
    yyjson_mut_doc *doc=new_event("heartbeat",NULL); json_str(doc,"state","idle");
    if(*s->job_id) {json_str(doc,"last_job_id",s->job_id); json_str(doc,"last_job_state",s->state);}
    pool_json(s,doc); logger_submit(s->logger,NULL,doc);
}
static int number(const char *text,uint64_t min,uint64_t max,uint64_t *out) {
    char *end; errno=0; if(*text<'0' || *text>'9')return -1;
    unsigned long long n=strtoull(text,&end,10);
    if(errno || *end || n<min || n>max)return -1;
    *out=n; return 0;
}
static void signal_stop(evutil_socket_t sig,short flags,void *opaque) {
    (void)sig; (void)flags; Service *s=opaque; s->shutting_down=1;
    if(!s->drain_deadline_us)s->drain_deadline_us=now_us()+((int64_t)s->stop_timeout+2)*1000000;
    if(s->active) {s->stopping=1; strcpy(s->state,"stopping");}
}
static int prepare_state(Service *s) {
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
static int recover_workers(Service *s) {
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
int main(int argc,char **argv) {
    if(naki_process_init())return 1;
    crash_set_instance(naki_process_id());
    const char *host="127.0.0.1"; int port=8090;
    Service *s=calloc(1,sizeof(*s)); if(!s)return 1;
    CrashOptions crash;
    if(crash_options_env(&crash)) {fprintf(stderr,"Invalid crash capture environment\n"); free(s); return 2;}
    s->status_interval=10; s->spares=1; s->worker_timeout=5; s->stop_timeout=3;
    s->logging=(LoggerOptions){.queue_capacity=512,.rotate_seconds=3600,.rotate_bytes=64*1024*1024};
    strcpy(s->run_dir,"out/runs"); strcpy(s->state,"idle");
    for(int i=1;i<argc;i++) {
        if(!strcmp(argv[i],"--drain-only"))s->drain_only=1;
        else if(!strcmp(argv[i],"--bind") && i+1<argc)host=argv[++i];
        else if(!strcmp(argv[i],"--namespace") && i+1<argc && naki_id_valid(argv[i+1],128))strcpy(s->service_namespace,argv[++i]);
        else if(!strcmp(argv[i],"--run-dir") && i+1<argc && strlen(argv[i+1])<sizeof(s->run_dir))strcpy(s->run_dir,argv[++i]);
        else if(!strcmp(argv[i],"--state-dir") && i+1<argc && strlen(argv[i+1])<sizeof(s->state_dir))strcpy(s->state_dir,argv[++i]);
        else if(!strcmp(argv[i],"--crash-dir") && i+1<argc && strlen(argv[i+1])<sizeof(crash.directory))strcpy(crash.directory,argv[++i]);
        else if(i+1<argc) {
            const char *option=argv[i++]; uint64_t min=1,max=86400,value;
            if(!strcmp(option,"--port"))max=65535;
            else if(!strcmp(option,"--spare-workers")) {min=0; max=4;}
            else if(!strcmp(option,"--worker-timeout") || !strcmp(option,"--worker-stop-timeout"))max=60;
            else if(!strcmp(option,"--log-queue-capacity")) {min=32; max=8192;}
            else if(!strcmp(option,"--log-rotate-bytes")) {min=1024; max=UINT64_C(1)<<40;}
            else if(!strcmp(option,"--crash-timeout-ms")) {min=100; max=10000;}
            else if(!strcmp(option,"--crash-max-report-bytes")) {min=4096; max=CRASH_MAX_REPORT;}
            else if(!strcmp(option,"--crash-max-reports"))max=256;
            else if(!strcmp(option,"--crash-max-total-bytes")) {min=4096; max=UINT64_C(1)<<30;}
            else if(!strcmp(option,"--crash-max-threads"))max=CRASH_MAX_THREADS;
            else if(!strcmp(option,"--crash-max-frames"))max=CRASH_MAX_FRAMES;
            else if(!strcmp(option,"--crash-max-read-bytes")) {min=4096; max=16*1024*1024;}
            else if(strcmp(option,"--status-interval") && strcmp(option,"--log-rotate-seconds"))goto usage;
            if(number(argv[i],min,max,&value))goto usage;
            if(!strcmp(option,"--port"))port=(int)value;
            else if(!strcmp(option,"--spare-workers"))s->spares=(unsigned)value;
            else if(!strcmp(option,"--worker-timeout"))s->worker_timeout=(unsigned)value;
            else if(!strcmp(option,"--worker-stop-timeout"))s->stop_timeout=(unsigned)value;
            else if(!strcmp(option,"--status-interval"))s->status_interval=(unsigned)value;
            else if(!strcmp(option,"--log-queue-capacity"))s->logging.queue_capacity=(unsigned)value;
            else if(!strcmp(option,"--log-rotate-bytes"))s->logging.rotate_bytes=value;
            else if(!strcmp(option,"--crash-timeout-ms"))crash.timeout_ms=(unsigned)value;
            else if(!strcmp(option,"--crash-max-report-bytes"))crash.max_report_bytes=(unsigned)value;
            else if(!strcmp(option,"--crash-max-reports"))crash.max_reports=(unsigned)value;
            else if(!strcmp(option,"--crash-max-total-bytes"))crash.max_total_bytes=value;
            else if(!strcmp(option,"--crash-max-threads"))crash.max_threads=(unsigned)value;
            else if(!strcmp(option,"--crash-max-frames"))crash.max_frames=(unsigned)value;
            else if(!strcmp(option,"--crash-max-read-bytes"))crash.max_read_bytes=value;
            else s->logging.rotate_seconds=(unsigned)value;
        } else goto usage;
    }
    ssize_t length=readlink("/proc/self/exe",s->worker_path,sizeof(s->worker_path)-1);
    if(length<=0 || length>=(ssize_t)sizeof(s->worker_path)-1)return 1;
    s->worker_path[length]=0; char *slash=strrchr(s->worker_path,'/');
    if(!slash || (size_t)(slash-s->worker_path)+sizeof("/naki-live-worker")>sizeof(s->worker_path))return 1;
    strcpy(slash,"/naki-live-worker");
    if(access(s->worker_path,X_OK) || mkdir_tree(s->run_dir) || prepare_state(s)) {perror("Worker executable / run directory / private state lock"); return 1;}
    if(!*s->service_namespace) {
        yyjson_doc *namespace_doc=yyjson_read("{}",2,0);yyjson_mut_doc *namespace_mut=yyjson_doc_mut_copy(namespace_doc,NULL);
        yyjson_doc_free(namespace_doc);json_str(namespace_mut,"state_directory",s->state_dir);
        size_t bytes=0;char *text=yyjson_mut_write(namespace_mut,0,&bytes);yyjson_mut_doc_free(namespace_mut);
        namespace_doc=text?yyjson_read(text,bytes,0):NULL;free(text);
        char namespace_error[128];
        if(!namespace_doc||naki_intent_fingerprint(yyjson_doc_get_root(namespace_doc),s->service_namespace,namespace_error,sizeof(namespace_error)))return 1;
        yyjson_doc_free(namespace_doc);
    }
    int default_crash_directory=!*crash.directory;
    if(default_crash_directory && snprintf(crash.directory,sizeof(crash.directory),"%s/crashes",s->run_dir)>=(int)sizeof(crash.directory))return 1;
    if(crash.max_total_bytes<crash.max_report_bytes || (mkdir(crash.directory,0700) && errno!=EEXIST)) {perror("Crash report directory / quota"); return 1;}
    struct stat crash_directory_stat;
    if(default_crash_directory && !lstat(crash.directory,&crash_directory_stat) && (crash_directory_stat.st_mode&0777)!=0700) {
        /* Windows-mounted WSL workspaces may not implement chmod; use the private Linux state directory. */
        snprintf(crash.directory,sizeof(crash.directory),"%s/crashes",s->state_dir);
        if(mkdir(crash.directory,0700) && errno!=EEXIST)return 1;
    }
    char crash_path[PATH_MAX];
    if(!realpath(crash.directory,crash_path))return 1;
    strcpy(crash.directory,crash_path);
    if(crash_options_export(&crash) || crash_client_start(&crash,"http")) {fprintf(stderr,"Cannot initialize independent crash collector\n"); return 1;}
    if(snprintf(s->event_dir,sizeof(s->event_dir),"%s/events",s->run_dir)>=(int)sizeof(s->event_dir)||mkdir_tree(s->event_dir))return 1;
    strcpy(s->logging.role,"controller");strcpy(s->logging.instance_id,naki_process_id());
    if(snprintf(s->logging.event_prefix,sizeof(s->logging.event_prefix),"%s/controller-%s.events",s->event_dir,naki_process_id())>=(int)sizeof(s->logging.event_prefix))return 1;
    s->logger=logger_start(&s->logging); if(!s->logger) {fprintf(stderr,"Cannot initialize process event sink\n");return 1;}
    av_log_set_level(AV_LOG_QUIET); signal(SIGPIPE,SIG_IGN);
    s->base=event_base_new(); struct evhttp *http=s->base ? evhttp_new(s->base) : NULL;
    if(!http || (!s->drain_only&&evhttp_bind_socket(http,host,(ev_uint16_t)port))) {
        fprintf(stderr,"Cannot bind HTTP listener\n");
        if(http)evhttp_free(http);
        if(s->base)event_base_free(s->base);
        logger_stop(s->logger); free(s); return 1;
    }
    evhttp_set_max_body_size(http,16384); evhttp_set_max_headers_size(http,8192); evhttp_set_timeout(http,10);
    evhttp_set_gencb(http,handle_request,s);
    struct event *term=evsignal_new(s->base,SIGTERM,signal_stop,s),*intr=evsignal_new(s->base,SIGINT,signal_stop,s);
    struct event *timer=event_new(s->base,-1,EV_PERSIST,report_status,s),*watch=event_new(s->base,-1,EV_PERSIST,tick,s);
    struct timeval interval={(time_t)s->status_interval,0},poll_interval={0,10000};
    if(!term || !intr || !timer || !watch || event_add(term,NULL) || event_add(intr,NULL) || event_add(timer,&interval) || event_add(watch,&poll_interval))return 1;
    yyjson_mut_doc *doc=new_event(s->drain_only?"drain_started":"listening",NULL);
    json_str(doc,"bind",host); json_int(doc,"port",port); json_int(doc,"status_interval_s",s->status_interval);
    json_str(doc,"execution_mode","process_pool"); logger_submit(s->logger,NULL,doc);
    if(recover_workers(s)) {fprintf(stderr,"Cannot recover private worker endpoints\n"); return 1;}
    if(s->drain_only)signal_stop(0,0,s);
    tick(-1,0,s); event_base_dispatch(s->base);
    if(s->create_request) {error_reply(s->create_request,503,"Controller stopped before acceptance was confirmed");s->create_request=NULL;}
    if(s->stop_request) {error_reply(s->stop_request,503,"Controller stopped before stop acknowledgement was confirmed");s->stop_request=NULL;}
    if(s->watermark_request) {error_reply(s->watermark_request,503,"Controller is shutting down; inspect worker revision after recovery");s->watermark_request=NULL;}
    operation_unavailable(s,"controller_shutting_down_before_operation_confirmation");
    evhttp_free(http); event_free(term); event_free(intr); event_free(timer); event_free(watch); event_base_free(s->base);
    if(s->config)yyjson_doc_free(s->config);
    if(s->snapshot)yyjson_doc_free(s->snapshot);
    for(int i=0;i<CREATE_RECORDS;i++) {
        if(s->creations[i].snapshot)yyjson_doc_free(s->creations[i].snapshot);
        naki_operation_cache_close(&s->creations[i].operations);naki_operation_cache_close(&s->creations[i].sync_operations);
    }
    int result=s->drain_failed?1:0;
    logger_submit(s->logger,NULL,new_event("shutdown",NULL)); logger_stop(s->logger); close(s->lock_fd); free(s); return result;
usage:
    fprintf(stderr,"Usage: naki-live [--bind 127.0.0.1] [--port 8090] [--run-dir out/runs] [--state-dir private-directory] [--spare-workers 1 (0..4)] [--worker-timeout 5] [--worker-stop-timeout 3] [--status-interval 10] [--crash-dir DIRECTORY] [--crash-timeout-ms 2000] [--crash-max-report-bytes 262144] [--crash-max-reports 32] [--crash-max-total-bytes 16777216] [--crash-max-threads 32] [--crash-max-frames 48] [--crash-max-read-bytes 1048576] [--log-queue-capacity 512] [--log-rotate-bytes 67108864] [--log-rotate-seconds 3600]\n");
    free(s); return 2;
}
