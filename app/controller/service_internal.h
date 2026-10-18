#ifndef NAKI_SERVICE_INTERNAL_H
#define NAKI_SERVICE_INTERNAL_H
#define _GNU_SOURCE
#include "control/config.h"
#include "worker/job_api.h"
#include "platform/runtime.h"
#include "worker/job_state.h"
#include "observability/logger.h"
#include "media/media_hooks.h"
#include <libavformat/live_mux.h>
#include "platform/ipc.h"
#include "crash/crash.h"
#include "control/identity.h"
#include "control/intent.h"
#include "control/component_update.h"
#include "service_records.h"
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
#define CONTROL_STALE_US 2000000
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
    char run_dir[2048],worker_path[PATH_MAX],job_id[64],request_id[129],log_path[4096],error[256],pool_error[256];
    NakiJobState state;
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
    CreationStore records;
    Creation *creation;
    struct evhttp_request *operation_request;
    Creation *operation_creation;
    int64_t operation_request_us;
    uint64_t operation_token;
    char operation_id[129];
    int operation_is_query;
} Service;

/* All controller state is confined to its libevent thread. Worker IPC ACKs
 * remain the acceptance boundary; loss of this controller never stops media. */
/* MODULE INTERFACES */
/* service_http.c */
void service_reply(struct evhttp_request *req,int code,yyjson_mut_doc *doc);
void service_error_reply(struct evhttp_request *req,int code,const char *message);
void service_operation_unavailable(Service *s,const char *reason);
void service_handle_request(struct evhttp_request *req,void *opaque);

/* service_projection.c */
yyjson_mut_doc *service_creation_status(Creation *record);
yyjson_mut_doc *service_stop_status(Creation *record);
yyjson_mut_doc *service_operation_projection(yyjson_val *record);
void service_pool_json(Service *s,yyjson_mut_doc *doc);
const char *service_readiness_reason(Service *s,int64_t current);
const char *service_admission_reason(Service *s,int64_t current);
void service_resource_observation(yyjson_mut_doc *d);
void service_control_health(Service *s,yyjson_mut_doc *d);
void service_snapshot_freshness(Service *s,yyjson_mut_doc *d);
yyjson_mut_doc *service_status(Service *s);
yyjson_mut_doc *service_observation(Service *s,const char *event);
void service_report_status(evutil_socket_t fd,short flags,void *opaque);

/* service_supervisor.c */
Worker *service_spawn_worker(Service *s);
void service_request_worker_stop(Service *s,Worker *w);
void service_dispatch(Service *s,Worker *w);
void service_tick(evutil_socket_t fd,short flags,void *opaque);
void service_signal_stop(evutil_socket_t sig,short flags,void *opaque);
int service_prepare_state(Service *s);
int service_recover_workers(Service *s);

#endif
