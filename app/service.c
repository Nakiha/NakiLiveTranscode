#include "service_internal.h"

static int number(const char *text,uint64_t min,uint64_t max,uint64_t *out) {
    char *end; errno=0; if(*text<'0' || *text>'9')return -1;
    unsigned long long n=strtoull(text,&end,10);
    if(errno || *end || n<min || n>max)return -1;
    *out=n; return 0;
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
    strcpy(s->run_dir,"out/runs"); s->state=NAKI_JOB_STATE_IDLE;
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
    if(access(s->worker_path,X_OK) || mkdir_tree(s->run_dir) || service_prepare_state(s)) {perror("Worker executable / run directory / private state lock"); return 1;}
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
    evhttp_set_gencb(http,service_handle_request,s);
    struct event *term=evsignal_new(s->base,SIGTERM,service_signal_stop,s),*intr=evsignal_new(s->base,SIGINT,service_signal_stop,s);
    struct event *timer=event_new(s->base,-1,EV_PERSIST,service_report_status,s),*watch=event_new(s->base,-1,EV_PERSIST,service_tick,s);
    struct timeval interval={(time_t)s->status_interval,0},poll_interval={0,10000};
    if(!term || !intr || !timer || !watch || event_add(term,NULL) || event_add(intr,NULL) || event_add(timer,&interval) || event_add(watch,&poll_interval))return 1;
    yyjson_mut_doc *doc=new_event(s->drain_only?"drain_started":"listening",NULL);
    json_str(doc,"bind",host); json_int(doc,"port",port); json_int(doc,"status_interval_s",s->status_interval);
    json_str(doc,"execution_mode","process_pool"); logger_submit(s->logger,NULL,doc);
    if(service_recover_workers(s)) {fprintf(stderr,"Cannot recover private worker endpoints\n"); return 1;}
    if(s->drain_only)service_signal_stop(0,0,s);
    service_tick(-1,0,s); event_base_dispatch(s->base);
    if(s->create_request) {service_error_reply(s->create_request,503,"Controller stopped before acceptance was confirmed");s->create_request=NULL;}
    if(s->stop_request) {service_error_reply(s->stop_request,503,"Controller stopped before stop acknowledgement was confirmed");s->stop_request=NULL;}
    if(s->watermark_request) {service_error_reply(s->watermark_request,503,"Controller is shutting down; inspect worker revision after recovery");s->watermark_request=NULL;}
    service_operation_unavailable(s,"controller_shutting_down_before_operation_confirmation");
    evhttp_free(http); event_free(term); event_free(intr); event_free(timer); event_free(watch); event_base_free(s->base);
    if(s->config)yyjson_doc_free(s->config);
    if(s->snapshot)yyjson_doc_free(s->snapshot);
    for(int i=0;i<CREATE_RECORDS;i++) {
        service_records_creation_clear(&s->records.items[i]);
    }
    int result=s->drain_failed?1:0;
    logger_submit(s->logger,NULL,new_event("shutdown",NULL)); logger_stop(s->logger); close(s->lock_fd); free(s); return result;
usage:
    fprintf(stderr,"Usage: naki-live [--bind 127.0.0.1] [--port 8090] [--run-dir out/runs] [--state-dir private-directory] [--spare-workers 1 (0..4)] [--worker-timeout 5] [--worker-stop-timeout 3] [--status-interval 10] [--crash-dir DIRECTORY] [--crash-timeout-ms 2000] [--crash-max-report-bytes 262144] [--crash-max-reports 32] [--crash-max-total-bytes 16777216] [--crash-max-threads 32] [--crash-max-frames 48] [--crash-max-read-bytes 1048576] [--log-queue-capacity 512] [--log-rotate-bytes 67108864] [--log-rotate-seconds 3600]\n");
    free(s); return 2;
}
