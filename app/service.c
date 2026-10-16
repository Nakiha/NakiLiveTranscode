#define _GNU_SOURCE
#include "live.h"
#include <libavformat/live_mux.h>
#include "ipc.h"
#include "crash.h"
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
#include <unistd.h>
#include <libavformat/avformat.h>
#include <libavutil/log.h>
#include <libavutil/time.h>

#define MAX_WORKERS 5
extern char **environ;
typedef struct Worker {
    pid_t pid;
    int fd,pid_fd,owned,needs_info,ready,busy,assigned,finished,stop_sent,kill_sent;
    char endpoint[108];
    int64_t spawned_us,ready_us,last_seen_us,exit_deadline_us;
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
} Service;

static void set_str(yyjson_mut_doc *doc,const char *key,const char *value) {
    yyjson_mut_obj_remove_key(yyjson_mut_doc_get_root(doc),key); json_str(doc,key,value);
}
static void reply(struct evhttp_request *req,int code,yyjson_mut_doc *doc) {
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
    yyjson_mut_doc *doc=new_event("error",NULL); json_str(doc,"error",message); reply(req,code,doc);
}
static void pool_json(Service *s,yyjson_mut_doc *doc) {
    yyjson_mut_val *pool=yyjson_mut_obj(doc),*workers=yyjson_mut_arr(doc);
    unsigned ready=0,warming=0,recovering=0;
    for(int i=0;i<MAX_WORKERS;i++) {
        Worker *w=&s->workers[i]; if(!w->pid)continue;
        if(w->needs_info)recovering++;
        if(!w->busy) {if(w->ready)ready++; else warming++;}
        yyjson_mut_val *item=yyjson_mut_obj(doc);
        yyjson_mut_obj_add_int(doc,item,"pid",w->pid);
        yyjson_mut_obj_add_str(doc,item,"state",w->busy ? "busy" : w->ready ? "ready" : "warming");
        yyjson_mut_arr_add_val(workers,item);
    }
    yyjson_mut_obj_add_uint(doc,pool,"spare_target",s->spares);
    yyjson_mut_obj_add_uint(doc,pool,"ready",ready); yyjson_mut_obj_add_uint(doc,pool,"warming",warming);
    yyjson_mut_obj_add_uint(doc,pool,"recovering",recovering);
    yyjson_mut_obj_add_val(doc,pool,"workers",workers);
    yyjson_mut_obj_add_strcpy(doc,pool,"error",s->pool_error);
    yyjson_mut_obj_add_val(doc,yyjson_mut_doc_get_root(doc),"pool",pool);
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
    pool_json(s,doc); return doc;
}
static void worker_event(Service *s,Worker *w,const char *event) {
    yyjson_mut_doc *doc=new_event(event,NULL);
    json_int(doc,"worker_pid",w->pid); pool_json(s,doc); logger_submit(s->logger,NULL,doc);
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
    char spawned[32],capacity[24],seconds[24],bytes[32],interval[24];
    int64_t started=now_us();
    snprintf(spawned,sizeof(spawned),"%lld",(long long)started);
    snprintf(capacity,sizeof(capacity),"%u",s->logging.queue_capacity);
    snprintf(seconds,sizeof(seconds),"%u",s->logging.rotate_seconds);
    snprintf(bytes,sizeof(bytes),"%llu",(unsigned long long)s->logging.rotate_bytes);
    snprintf(interval,sizeof(interval),"%u",s->status_interval);
    char *args[]={s->worker_path,s->state_dir,spawned,capacity,seconds,bytes,interval,NULL}; pid_t pid=0;
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
    yyjson_mut_doc *doc=ipc_message("stop");
    if(!ipc_send(w->fd,doc))w->stop_sent=1;
    yyjson_mut_doc_free(doc);
}
static void dispatch(Service *s,Worker *w) {
    if(w->assigned || !w->ready || s->stopping || s->shutting_down)return;
    yyjson_mut_doc *doc=ipc_message("start");
    json_str(doc,"job_id",s->job_id); json_str(doc,"log_path",s->log_path);
    json_str(doc,"request_id",s->request_id);
    json_int(doc,"request_us",s->request_us); int64_t assigned=now_us(); json_int(doc,"assigned_us",assigned);
    yyjson_mut_obj_add_bool(doc,yyjson_mut_doc_get_root(doc),"prewarmed",s->prewarmed);
    yyjson_mut_obj_add_val(doc,yyjson_mut_doc_get_root(doc),"config",yyjson_val_mut_copy(doc,yyjson_doc_get_root(s->config)));
    if(!ipc_send(w->fd,doc)) {w->assigned=1; s->assigned_us=assigned;}
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
    if(s->active && s->active!=w) {s->recovery_conflict=1; strcpy(s->pool_error,"Multiple active workers found; admission disabled"); return -1;}
    if(s->config)yyjson_doc_free(s->config);
    size_t size=0; char *text=yyjson_val_write(yyjson_obj_get(info,"config"),0,&size);
    s->config=text ? yyjson_read(text,size,0) : NULL; free(text);
    if(!s->config)return -1;
    strcpy(s->job_id,id); strcpy(s->request_id,request); strcpy(s->log_path,path);
    s->request_us=yyjson_get_sint(yyjson_obj_get(info,"request_us"));
    s->assigned_us=yyjson_get_sint(yyjson_obj_get(info,"assigned_us"));
    s->prewarmed=yyjson_get_bool(yyjson_obj_get(info,"prewarmed"));
    s->active=w; w->busy=1; w->assigned=1; s->last_pid=w->pid;
    s->last_recovered=1; s->exit_status_known=0;
    s->exit_code=-1; s->exit_signal=0; *s->error=0; s->stopping=0;
    worker_event(s,w,"worker_recovered"); return 0;
}
static void read_worker(Service *s,Worker *w) {
    for(int i=0;i<16;i++) {
        yyjson_doc *doc=NULL; int result=ipc_receive(w->fd,&doc);
        if(result<=0) {
            if(result<0) {if(w->fd>=0)close(w->fd); w->fd=-1;}
            break;
        }
        w->last_seen_us=now_us();
        if(ipc_kind(doc,"log")) {
            yyjson_val *record=yyjson_obj_get(yyjson_doc_get_root(doc),"record");
            if(yyjson_is_obj(record)) {
                yyjson_mut_doc *copy=yyjson_mut_doc_new(NULL);
                if(copy) {yyjson_mut_doc_set_root(copy,yyjson_val_mut_copy(copy,record)); logger_submit(s->logger,NULL,copy); s->forwarded_logs++;}
            } else s->invalid_logs++;
        } else if(ipc_kind(doc,"ready")) {
            w->needs_info=0;
            if(!w->ready) {
                w->ready=1; w->ready_us=yyjson_get_sint(yyjson_obj_get(yyjson_doc_get_root(doc),"ready_us"));
                worker_event(s,w,"worker_ready");
            }
        } else if(ipc_kind(doc,"snapshot") || ipc_kind(doc,"finished")) {
            if(w->needs_info && !w->assigned && recover_job(s,w,doc)) {yyjson_doc_free(doc); break;}
            w->needs_info=0;
            if(w!=s->active || !w->assigned) {yyjson_doc_free(doc); break;}
            const char *id=yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(doc),"job_id"));
            if(!id || strcmp(id,s->job_id)) {yyjson_doc_free(doc); break;}
            if(s->snapshot)yyjson_doc_free(s->snapshot);
            s->snapshot=doc; s->snapshot_us=now_us(); doc=NULL;
            if(ipc_kind(s->snapshot,"finished")) {
                w->finished=1;
                if(!w->exit_deadline_us)w->exit_deadline_us=now_us()+(int64_t)s->stop_timeout*1000000;
                yyjson_mut_doc *ack=ipc_message("ack"); ipc_send(w->fd,ack); yyjson_mut_doc_free(ack);
                strcpy(s->state,"finishing");
            } else if(!s->stopping) {
                const char *state=yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(s->snapshot),"state"));
                strcpy(s->state,state && !strcmp(state,"running") ? "running" : "starting");
            }
        } else {
            yyjson_doc_free(doc); break;
        }
        if(doc)yyjson_doc_free(doc);
    }
}
static void reap_worker(Service *s,Worker *w,int result) {
    if(w==s->active) {
        s->last_pid=w->pid; s->exit_status_known=w->owned;
        s->exit_code=w->owned && WIFEXITED(result) ? WEXITSTATUS(result) : -1;
        s->exit_signal=w->owned && WIFSIGNALED(result) ? WTERMSIG(result) : 0;
        s->core_dumped=w->owned && WIFSIGNALED(result) && WCOREDUMP(result);
        const char *final=s->snapshot ? yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(s->snapshot),"state")) : NULL;
        if(s->stopping)strcpy(s->state,"stopped");
        else if(w->finished && (s->exit_code==0 || !w->owned) && final &&
                (!strcmp(final,"completed") || !strcmp(final,"failed") || !strcmp(final,"stopped")))snprintf(s->state,sizeof(s->state),"%s",final);
        else {
            strcpy(s->state,"failed");
            if(!*s->error)snprintf(s->error,sizeof(s->error),"Worker exited unexpectedly (code=%d signal=%d)",s->exit_code,s->exit_signal);
        }
        s->active=NULL;
        logger_submit(s->logger,NULL,status(s));
        if(!s->stopping && (!w->finished || (w->owned && s->exit_code!=0))) {
            yyjson_mut_doc *crash=status(s); set_str(crash,"event","worker_crash");
            json_str(crash,"signal_name",s->exit_signal ? strsignal(s->exit_signal) : "unknown");
            json_str(crash,"diagnostic_boundary","supervisor_observed_exit_with_last_worker_snapshot");
            logger_submit(s->logger,NULL,crash);
        }
        yyjson_val *startup=s->snapshot ? yyjson_obj_get(yyjson_doc_get_root(s->snapshot),"startup") : NULL;
        if(!startup || yyjson_is_null(yyjson_obj_get(startup,"request_to_first_video_mux_ms"))) {
            yyjson_mut_doc *incomplete=status(s); set_str(incomplete,"event","startup_incomplete");
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
    unlink(w->endpoint); memset(w,0,sizeof(*w));
}
static void tick(evutil_socket_t fd,short flags,void *opaque) {
    (void)fd; (void)flags; Service *s=opaque; int64_t current=now_us();
    for(int i=0;i<MAX_WORKERS;i++) {
        Worker *w=&s->workers[i]; if(!w->pid)continue;
        read_worker(s,w);
        int result=0; pid_t reaped=w->owned ? waitpid(w->pid,&result,WNOHANG) : 0;
        if(!w->owned) {
            struct pollfd process={.fd=w->pid_fd,.events=POLLIN};
            if(poll(&process,1,0)>0) {reaped=w->pid; result=0;}
        }
        if(reaped==w->pid) {reap_worker(s,w,result); continue;}
        if((s->shutting_down || (w==s->active && s->stopping)) && !w->finished)request_worker_stop(s,w);
        if(!w->exit_deadline_us && current-w->last_seen_us>(int64_t)s->worker_timeout*1000000) {
            if(w==s->active)snprintf(s->error,sizeof(s->error),"Worker heartbeat timeout");
            w->exit_deadline_us=current;
        }
        if(w->exit_deadline_us && current>=w->exit_deadline_us && !w->kill_sent) {
            if(w==s->active && !*s->error)snprintf(s->error,sizeof(s->error),"Worker exit deadline exceeded; forced termination");
            signal_worker(w,SIGKILL); w->kill_sent=1;
        }
        if(w==s->active)dispatch(s,w);
        if(w->fd<0 && !w->finished) {
            int peer=0,connection=ipc_connect(w->endpoint,&peer);
            if(connection>=0) {if(peer==w->pid)w->fd=connection; else close(connection);}
        }
    }
    if(s->shutting_down) {
        for(int i=0;i<MAX_WORKERS;i++)if(s->workers[i].pid)return;
        event_base_loopbreak(s->base); return;
    }
    unsigned idle=0;
    for(int i=0;i<MAX_WORKERS;i++)if(s->workers[i].pid && !s->workers[i].busy)idle++;
    while(idle<s->spares) {if(!spawn_worker(s))break; idle++;}
}
static void handle_request(struct evhttp_request *req,void *opaque) {
    /* Callback entry follows receipt of the complete HTTP request body. */
    int64_t received=now_us(); Service *s=opaque;
    const char *uri=evhttp_request_get_uri(req); enum evhttp_cmd_type method=evhttp_request_get_command(req);
    if(!strcmp(uri,"/health") && method==EVHTTP_REQ_GET) {
        yyjson_mut_doc *doc=new_event("health",NULL);
        json_str(doc,"status",s->shutting_down ? "stopping" : "ok"); json_str(doc,"version","0.8.0");
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
        json_str(doc,"ffmpeg_version",av_version_info()); json_int(doc,"libavcodec_version",avcodec_version());
        pool_json(s,doc); reply(req,200,doc); return;
    }
    if(!strcmp(uri,"/v1/job") && method==EVHTTP_REQ_GET) {reply(req,200,status(s)); return;}
    if(!strcmp(uri,"/v1/job/stop") && method==EVHTTP_REQ_POST) {
        if(!s->active) {error_reply(req,409,"No running job"); return;}
        s->stopping=1; strcpy(s->state,"stopping"); request_worker_stop(s,s->active); reply(req,202,status(s)); return;
    }
    if(!strcmp(uri,"/v1/job") && method==EVHTTP_REQ_POST) {
        if(s->shutting_down) {error_reply(req,503,"Service is shutting down"); return;}
        for(int i=0;i<MAX_WORKERS;i++)if(s->workers[i].pid && s->workers[i].needs_info) {error_reply(req,503,"Recovering existing workers"); return;}
        if(s->recovery_conflict) {error_reply(req,503,"Multiple workers require operator reconciliation"); return;}
        if(s->active) {error_reply(req,409,"Single-stream service already has an active job"); return;}
        struct evbuffer *input=evhttp_request_get_input_buffer(req); size_t size=evbuffer_get_length(input);
        if(!size || size>16384) {error_reply(req,413,"Body must contain 1..16384 bytes"); return;}
        const char *body=(const char *)evbuffer_pullup(input,-1); Config config; char error[256];
        if(config_parse(body,size,&config,error,sizeof(error))) {error_reply(req,400,error); return;}
        yyjson_doc *parsed=yyjson_read(body,size,0);
        if(!parsed) {error_reply(req,500,"Allocation failure"); return;}
        Worker *w=NULL;
        for(int i=0;i<MAX_WORKERS;i++)if(s->workers[i].pid && s->workers[i].ready && !s->workers[i].busy) {w=&s->workers[i]; break;}
        int prewarmed=w && w->ready_us<=received;
        if(!w)for(int i=0;i<MAX_WORKERS;i++)if(s->workers[i].pid && !s->workers[i].busy) {w=&s->workers[i]; break;}
        if(!w)w=spawn_worker(s);
        if(!w) {yyjson_doc_free(parsed); error_reply(req,503,"Worker unavailable; retry after pool recovery"); return;}
        if(s->config)yyjson_doc_free(s->config);
        if(s->snapshot)yyjson_doc_free(s->snapshot);
        s->config=parsed; s->snapshot=NULL; s->snapshot_us=0; s->request_us=received; s->assigned_us=0;
        s->active=w; w->busy=1; s->stopping=0; s->prewarmed=prewarmed;
        s->last_recovered=0; s->exit_status_known=0;
        s->exit_code=-1; s->exit_signal=0; s->last_pid=w->pid; *s->error=0; *s->log_path=0;
        snprintf(s->job_id,sizeof(s->job_id),"%lld-%d-%u",(long long)av_gettime(),getpid(),++s->sequence);
        const char *request_id=evhttp_find_header(evhttp_request_get_input_headers(req),"X-Request-ID");
        snprintf(s->request_id,sizeof(s->request_id),"%s",request_id && strlen(request_id)<=128 ? request_id : s->job_id);
        if(config.frame_log)snprintf(s->log_path,sizeof(s->log_path),"%s/%s.trace",s->run_dir,s->job_id);
        strcpy(s->state,"starting");
        yyjson_mut_doc *accepted=new_event("request_received",NULL);
        json_str(accepted,"job_id",s->job_id); json_str(accepted,"request_id",s->request_id);
        json_int(accepted,"request_monotonic_us",received);
        json_str(accepted,"boundary","complete_http_request_handler");
        json_int(accepted,"worker_pid",w->pid); logger_submit(s->logger,NULL,accepted);
        dispatch(s,w);
        reply(req,202,status(s)); return;
    }
    if(!strcmp(uri,"/health") || !strcmp(uri,"/v1/job") || !strcmp(uri,"/v1/job/stop"))error_reply(req,405,"Method not allowed");
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
    if(s->active) {s->stopping=1; strcpy(s->state,"stopping");}
}
static int prepare_state(Service *s) {
    if(!*s->state_dir) {
        char resolved[PATH_MAX]; if(!realpath(s->run_dir,resolved))return -1;
        uint64_t hash=UINT64_C(14695981039346656037);
        for(const unsigned char *p=(const unsigned char *)resolved;*p;p++) {hash^=*p; hash*=UINT64_C(1099511628211);}
        snprintf(s->state_dir,sizeof(s->state_dir),"/tmp/naki-live-%u-%016llx",(unsigned)geteuid(),(unsigned long long)hash);
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
        int peer=0,fd=ipc_connect(endpoint,&peer);
        if(fd<0) {
            if(errno==ECONNREFUSED || errno==ENOENT)unlink(endpoint);
            continue;
        }
        if(peer!=expected || slot>=MAX_WORKERS) {close(fd); closedir(directory); return -1;}
        int process=(int)syscall(SYS_pidfd_open,peer,0);
        if(process<0) {close(fd); closedir(directory); return -1;}
        Worker *w=&s->workers[slot++];
        *w=(Worker){.pid=peer,.fd=fd,.pid_fd=process,.needs_info=1,.spawned_us=now_us(),.last_seen_us=now_us()};
        strcpy(w->endpoint,endpoint); worker_event(s,w,"worker_reattaching");
    }
    closedir(directory); return 0;
}
int main(int argc,char **argv) {
    const char *host="127.0.0.1"; int port=8090;
    Service *s=calloc(1,sizeof(*s)); if(!s)return 1;
    CrashOptions crash;
    if(crash_options_env(&crash)) {fprintf(stderr,"Invalid crash capture environment\n"); free(s); return 2;}
    s->status_interval=10; s->spares=1; s->worker_timeout=5; s->stop_timeout=3;
    s->logging=(LoggerOptions){.queue_capacity=512,.rotate_seconds=3600,.rotate_bytes=64*1024*1024};
    strcpy(s->run_dir,"out/runs"); strcpy(s->state,"idle");
    for(int i=1;i<argc;i++) {
        if(!strcmp(argv[i],"--bind") && i+1<argc)host=argv[++i];
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
    s->logger=logger_start(&s->logging); if(!s->logger)return 1;
    av_log_set_level(AV_LOG_QUIET); signal(SIGPIPE,SIG_IGN);
    s->base=event_base_new(); struct evhttp *http=s->base ? evhttp_new(s->base) : NULL;
    if(!http || evhttp_bind_socket(http,host,(ev_uint16_t)port)) {
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
    yyjson_mut_doc *doc=new_event("listening",NULL);
    json_str(doc,"bind",host); json_int(doc,"port",port); json_int(doc,"status_interval_s",s->status_interval);
    json_str(doc,"execution_mode","process_pool"); logger_submit(s->logger,NULL,doc);
    if(recover_workers(s)) {fprintf(stderr,"Cannot recover private worker endpoints\n"); return 1;}
    tick(-1,0,s); event_base_dispatch(s->base);
    evhttp_free(http); event_free(term); event_free(intr); event_free(timer); event_free(watch); event_base_free(s->base);
    if(s->config)yyjson_doc_free(s->config);
    if(s->snapshot)yyjson_doc_free(s->snapshot);
    logger_submit(s->logger,NULL,new_event("shutdown",NULL)); logger_stop(s->logger); close(s->lock_fd); free(s); return 0;
usage:
    fprintf(stderr,"Usage: naki-live [--bind 127.0.0.1] [--port 8090] [--run-dir out/runs] [--state-dir private-directory] [--spare-workers 1 (0..4)] [--worker-timeout 5] [--worker-stop-timeout 3] [--status-interval 10] [--crash-dir DIRECTORY] [--crash-timeout-ms 2000] [--crash-max-report-bytes 262144] [--crash-max-reports 32] [--crash-max-total-bytes 16777216] [--crash-max-threads 32] [--crash-max-frames 48] [--crash-max-read-bytes 1048576] [--log-queue-capacity 512] [--log-rotate-bytes 67108864] [--log-rotate-seconds 3600]\n");
    free(s); return 2;
}
