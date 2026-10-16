#include "live.h"
#include <errno.h>
#include <event2/buffer.h>
#include <event2/event.h>
#include <event2/http.h>
#include <event2/keyvalq_struct.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <libavformat/avformat.h>
#include <libavutil/log.h>
#include <libavutil/time.h>

typedef struct Service { Job *job; Logger *logger; NetworkStats network; char run_dir[2048]; unsigned sequence, status_interval; } Service;

static void reply(struct evhttp_request *req, int code, yyjson_mut_doc *doc) {
    struct evbuffer *buf = evbuffer_new();
    size_t len = 0;
    char *text = doc ? yyjson_mut_write(doc, 0, &len) : NULL;
    if (doc) yyjson_mut_doc_free(doc);
    if (!buf || !text) {
        if (buf) evbuffer_free(buf);
        free(text); evhttp_send_error(req, 500, "Allocation failure"); return;
    }
    evbuffer_add(buf, text, len); evbuffer_add(buf, "\n", 1);
    evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", "application/json");
    evhttp_add_header(evhttp_request_get_output_headers(req), "Cache-Control", "no-store");
    evhttp_send_reply(req, code, NULL, buf);
    free(text); evbuffer_free(buf);
}
static void error_reply(struct evhttp_request *req, int code, const char *message) {
    yyjson_mut_doc *doc = new_event("error", NULL);
    json_str(doc, "error", message); reply(req, code, doc);
}
static void destroy_job(Job *job) {
    if (!job) return;
    request_stop(job);
    pthread_join(job->thread, NULL);
    logger_trace_release(job->logger,job->trace); close(job->wake_fd);
    pthread_mutex_destroy(&job->mutex); free(job);
}
static void handle_request(struct evhttp_request *req, void *opaque) {
    Service *s = opaque;
    const char *uri = evhttp_request_get_uri(req);
    enum evhttp_cmd_type method = evhttp_request_get_command(req);
    if (!strcmp(uri,"/health") && method==EVHTTP_REQ_GET) {
        yyjson_mut_doc *doc = new_event("health",NULL);
        json_str(doc,"status","ok"); json_str(doc,"version","0.2.0");
        json_int(doc,"status_interval_s",s->status_interval);
        json_str(doc,"http_library",event_get_version());
        json_str(doc,"json_library",yyjson_version() ? YYJSON_VERSION_STRING : "unknown");
        json_int(doc,"async_api",AVCODEC_ASYNC_API_VERSION);
        json_int(doc,"network_observer_api",AVFORMAT_NETWORK_OBSERVER_VERSION);
        json_str(doc,"ffmpeg_version",av_version_info());
        json_int(doc,"libavcodec_version",avcodec_version());
        reply(req,200,doc); return;
    }
    if (!strcmp(uri,"/v1/job") && method==EVHTTP_REQ_GET) {
        if (s->job) reply(req,200,job_snapshot(s->job));
        else { yyjson_mut_doc *doc = new_event("status",NULL); json_str(doc,"state","idle"); reply(req,200,doc); }
        return;
    }
    if (!strcmp(uri,"/v1/job/stop") && method==EVHTTP_REQ_POST) {
        if (!s->job || atomic_load(&s->job->done)) {error_reply(req,409,"No running job"); return;}
        request_stop(s->job); reply(req,202,job_snapshot(s->job)); return;
    }
    if (!strcmp(uri,"/v1/job") && method==EVHTTP_REQ_POST) {
        if (s->job && !atomic_load(&s->job->done)) {error_reply(req,409,"Single-stream service already has an active job"); return;}
        struct evbuffer *input=evhttp_request_get_input_buffer(req);
        size_t size=evbuffer_get_length(input);
        if (!size || size>16384) {error_reply(req,413,"Body must contain 1..16384 bytes"); return;}
        Config config; char error[256];
        if (config_parse((const char *)evbuffer_pullup(input,-1),size,&config,error,sizeof(error))) {error_reply(req,400,error); return;}
        Job *job=calloc(1,sizeof(*job));
        if (!job) {error_reply(req,500,"Allocation failure"); return;}
        job->config=config; job->started_us=now_us(); job->window_start_us=job->started_us;
        job->logger=s->logger;
        job->network=&s->network;
        snprintf(job->id,sizeof(job->id),"%lld-%d-%u",(long long)av_gettime(),(int)getpid(),++s->sequence);
        if(config.frame_log)snprintf(job->log_path,sizeof(job->log_path),"%s/%s.trace",s->run_dir,job->id);
        strcpy(job->state,"starting");
        atomic_init(&job->cancel,0); atomic_init(&job->done,0);
        job->wake_fd=eventfd(0,EFD_NONBLOCK|EFD_CLOEXEC);
        if (job->wake_fd<0 || pthread_mutex_init(&job->mutex,NULL)) {
            if (job->wake_fd>=0) close(job->wake_fd);
            free(job); error_reply(req,500,"Cannot create job event/mutex"); return;
        }
        if(config.frame_log && !(job->trace=logger_trace_open(s->logger,job->log_path))) {
            close(job->wake_fd); pthread_mutex_destroy(&job->mutex); free(job);
            error_reply(req,500,"Cannot allocate trace sink"); return;
        }
        network_stats_reset(&s->network);
        int ret=pthread_create(&job->thread,NULL,pipeline_main,job);
        if (ret) {
            logger_trace_release(s->logger,job->trace); close(job->wake_fd); pthread_mutex_destroy(&job->mutex); free(job);
            error_reply(req,500,"Cannot start pipeline worker"); return;
        }
        destroy_job(s->job); s->job=job;
        reply(req,202,job_snapshot(job)); return;
    }
    if (!strcmp(uri,"/health") || !strcmp(uri,"/v1/job") || !strcmp(uri,"/v1/job/stop")) error_reply(req,405,"Method not allowed");
    else error_reply(req,404,"Unknown endpoint");
}
static void report_status(evutil_socket_t fd, short flags, void *opaque) {
    (void)fd; (void)flags;
    Service *s=opaque;
    yyjson_mut_doc *doc;
    if(s->job && !atomic_load(&s->job->done))doc=job_report(s->job,0);
    else {
        doc=new_event("heartbeat",NULL); json_str(doc,"state","idle");
        if(s->job) {
            json_str(doc,"last_job_id",s->job->id);
            pthread_mutex_lock(&s->job->mutex); json_str(doc,"last_job_state",s->job->state); pthread_mutex_unlock(&s->job->mutex);
        }
    }
    if(doc)logger_submit(s->logger,NULL,doc);
}
static int number(const char *text, uint64_t min, uint64_t max, uint64_t *out) {
    char *end; errno=0;
    if(*text<'0' || *text>'9')return -1;
    unsigned long long n=strtoull(text,&end,10);
    if(errno || *end || n<min || n>max)return -1;
    *out=n; return 0;
}
static void signal_stop(evutil_socket_t sig, short flags, void *opaque) {
    (void)sig; (void)flags; event_base_loopbreak(opaque);
}
int main(int argc, char **argv) {
    const char *host="127.0.0.1"; int port=8090;
    Service s={.status_interval=10}; strcpy(s.run_dir,"out/runs");
    LoggerOptions logging={.queue_capacity=512,.rotate_seconds=3600,.rotate_bytes=64*1024*1024};
    for (int i=1;i<argc;i++) {
        if (!strcmp(argv[i],"--port") && i+1<argc) {
            char *end; long value=strtol(argv[++i],&end,10);
            if (*end || value<1 || value>65535) {fprintf(stderr,"Invalid port\n"); return 2;}
            port=(int)value;
        } else if ((!strcmp(argv[i],"--status-interval") || !strcmp(argv[i],"--log-queue-capacity") ||
                    !strcmp(argv[i],"--log-rotate-bytes") || !strcmp(argv[i],"--log-rotate-seconds")) && i+1<argc) {
            const char *option=argv[i++]; uint64_t value;
            uint64_t min=!strcmp(option,"--log-queue-capacity") ? 32 : !strcmp(option,"--log-rotate-bytes") ? 1024 : 1;
            uint64_t max=!strcmp(option,"--log-queue-capacity") ? 8192 : !strcmp(option,"--log-rotate-bytes") ? (UINT64_C(1)<<40) : 86400;
            if(number(argv[i],min,max,&value)) {fprintf(stderr,"Invalid logging option\n"); return 2;}
            if(!strcmp(option,"--status-interval"))s.status_interval=(unsigned)value;
            else if(!strcmp(option,"--log-queue-capacity"))logging.queue_capacity=(unsigned)value;
            else if(!strcmp(option,"--log-rotate-bytes"))logging.rotate_bytes=value;
            else logging.rotate_seconds=(unsigned)value;
        } else if (!strcmp(argv[i],"--bind") && i+1<argc) host=argv[++i];
        else if (!strcmp(argv[i],"--run-dir") && i+1<argc && strlen(argv[i+1])<sizeof(s.run_dir)) strcpy(s.run_dir,argv[++i]);
        else {fprintf(stderr,"Usage: naki-live [--bind 127.0.0.1] [--port 8090] [--run-dir out/runs] [--status-interval 10] [--log-queue-capacity 512] [--log-rotate-bytes 67108864] [--log-rotate-seconds 3600]\n"); return 2;}
    }
    if (mkdir_tree(s.run_dir)) {perror("run directory"); return 1;}
    s.logger=logger_start(&logging);
    if(!s.logger) {fprintf(stderr,"Cannot start logger\n"); return 1;}
    /* FFmpeg diagnostics must not inject plain text into our JSONL stream. */
    av_log_set_level(AV_LOG_QUIET);
    signal(SIGPIPE,SIG_IGN);
    network_stats_reset(&s.network);
    avformat_set_network_observer(network_observe,&s.network);
    avformat_network_init();
    struct event_base *base=event_base_new();
    struct evhttp *http=base ? evhttp_new(base) : NULL;
    if (!http || evhttp_bind_socket(http,host,(ev_uint16_t)port)) {fprintf(stderr,"Cannot bind HTTP listener\n"); if(http)evhttp_free(http); if(base)event_base_free(base); logger_stop(s.logger); return 1;}
    evhttp_set_max_body_size(http,16384);
    evhttp_set_max_headers_size(http,8192);
    evhttp_set_timeout(http,10);
    evhttp_set_gencb(http,handle_request,&s);
    struct event *term=evsignal_new(base,SIGTERM,signal_stop,base);
    struct event *intr=evsignal_new(base,SIGINT,signal_stop,base);
    struct event *timer=event_new(base,-1,EV_PERSIST,report_status,&s);
    struct timeval interval={(time_t)s.status_interval,0};
    if (!term || !intr || !timer || event_add(term,NULL) || event_add(intr,NULL) || event_add(timer,&interval)) {fprintf(stderr,"Cannot install events\n"); return 1;}
    yyjson_mut_doc *doc=new_event("listening",NULL);
    json_str(doc,"bind",host); json_int(doc,"port",port); json_int(doc,"status_interval_s",s.status_interval);
    logger_submit(s.logger,NULL,doc);
    event_base_dispatch(base);
    evhttp_free(http);
    destroy_job(s.job);
    avformat_set_network_observer(NULL,NULL);
    event_free(term); event_free(intr); event_free(timer); event_base_free(base);
    avformat_network_deinit();
    logger_submit(s.logger,NULL,new_event("shutdown",NULL));
    logger_stop(s.logger);
    return 0;
}
