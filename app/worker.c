#define _GNU_SOURCE
#include "live.h"
#include "ipc.h"
#include "watermark.h"
#include <curl/curl.h>
#include "crash.h"
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>
#include <libavformat/avformat.h>
#include <libavutil/log.h>

static volatile sig_atomic_t stopping;
static void stop_signal(int sig) {(void)sig; stopping=1;}
typedef struct Channel {pthread_mutex_t mutex; int fd;} Channel;
static void channel_close(Channel *channel) {
    pthread_mutex_lock(&channel->mutex);
    if(channel->fd>=0)close(channel->fd);
    channel->fd=-1; pthread_mutex_unlock(&channel->mutex);
}
static int channel_send(Channel *channel,yyjson_mut_doc *doc) {
    pthread_mutex_lock(&channel->mutex);
    int ret=channel->fd<0 ? (errno=ENOTCONN,-1) : ipc_send(channel->fd,doc);
    pthread_mutex_unlock(&channel->mutex); return ret;
}
static int log_send(void *opaque,const char *text,size_t size) {
    Channel *channel=opaque;
    const char prefix[]="{\"ipc_version\":1,\"ipc_kind\":\"log\",\"record\":";
    struct iovec parts[]={{(void *)prefix,sizeof(prefix)-1},{(void *)text,size},{"}",1}};
    struct msghdr message={.msg_iov=parts,.msg_iovlen=3};
    if(size+sizeof(prefix)>IPC_MAX_BYTES)return -1;
    pthread_mutex_lock(&channel->mutex);
    ssize_t ret=channel->fd<0 ? -1 : sendmsg(channel->fd,&message,MSG_DONTWAIT|MSG_NOSIGNAL);
    pthread_mutex_unlock(&channel->mutex);
    return ret==(ssize_t)(size+sizeof(prefix)) ? 0 : -1;
}
static int start_job(Job *job, yyjson_doc *doc) {
    yyjson_val *root=yyjson_doc_get_root(doc);
    const char *id=yyjson_get_str(yyjson_obj_get(root,"job_id"));
    const char *path=yyjson_get_str(yyjson_obj_get(root,"log_path"));
    const char *request_id=yyjson_get_str(yyjson_obj_get(root,"request_id"));
    size_t size=0; char error[256];
    char *config=yyjson_val_write(yyjson_obj_get(root,"config"),0,&size);
    int valid=id && strlen(id)<sizeof(job->id) && request_id && strlen(request_id)<sizeof(job->request_id) && path && strlen(path)<sizeof(job->log_path) &&
              config && !config_parse(config,size,&job->config,error,sizeof(error));
    free(config);
    if(!valid)return -1;
    strcpy(job->id,id); strcpy(job->request_id,request_id); strcpy(job->log_path,path); strcpy(job->state,"starting");
    crash_set_job(job->id);
    job->started_us=yyjson_get_sint(yyjson_obj_get(root,"request_us"));
    naki_overload_init(&job->overload);
    job->assigned_us=yyjson_get_sint(yyjson_obj_get(root,"assigned_us"));
    job->worker_received_us=now_us(); job->window_start_us=job->started_us;
    job->prewarmed=yyjson_get_bool(yyjson_obj_get(root,"prewarmed"));
    if(job->config.frame_log && !(job->trace=logger_trace_open(job->logger,job->log_path)))return -1;
    job->watermark=naki_watermark_open(&job->config.watermark,job->started_us,&job->cancel);
    if(!job->watermark)return -1;
    for(int i=0;i<job->config.output_count;i++) {
        Job *b=calloc(1,sizeof(*b));if(!b)return -1;
        naki_output_config(&b->config,&job->config,i);b->parent=job;
        strcpy(b->output_id,job->config.outputs[i].id);strcpy(b->id,job->id);strcpy(b->request_id,job->request_id);
        strcpy(b->state,"waiting_assets");strcpy(b->log_path,job->log_path);
        b->logger=job->logger;b->trace=job->trace;b->started_us=job->started_us;b->window_start_us=job->started_us;
        b->assigned_us=job->assigned_us;b->worker_received_us=job->worker_received_us;b->worker_spawned_us=job->worker_spawned_us;
        b->worker_ready_us=job->worker_ready_us;b->prewarmed=job->prewarmed;
        b->wake_fd=eventfd(0,EFD_NONBLOCK|EFD_CLOEXEC);
        if(b->wake_fd<0||pthread_mutex_init(&b->mutex,NULL)) {if(b->wake_fd>=0)close(b->wake_fd);free(b);return -1;}
        atomic_init(&b->cancel,0);atomic_init(&b->done,0);atomic_init(&b->branch_failed,0);naki_overload_init(&b->overload);
        job->outputs[i]=b;b->watermark_snapshot=naki_watermark_status;
        b->watermark=naki_watermark_open(&b->config.watermark,b->started_us,&b->cancel);if(!b->watermark)return -1;
    }
    return pthread_create(&job->thread,NULL,pipeline_main,job) ? -1 : 0;
}

/* Private executable: fd 3 is the first controller; the private endpoint permits later takeover. */
int main(int argc,char **argv) {
    if(argc!=7)return 2;
    CrashOptions crash;
    if(crash_options_env(&crash) || crash_client_start(&crash,"worker"))return 1;
    char endpoint[108];
    if(snprintf(endpoint,sizeof(endpoint),"%s/w-%d.sock",argv[1],getpid())>=(int)sizeof(endpoint))return 2;
    int listener=ipc_listen(endpoint);
    if(listener<0)return 1;
    Channel channel={.mutex=PTHREAD_MUTEX_INITIALIZER,.fd=3};
    struct sigaction action={0}; action.sa_handler=stop_signal; sigemptyset(&action.sa_mask);
    sigaction(SIGTERM,&action,NULL); sigaction(SIGINT,&action,NULL); signal(SIGPIPE,SIG_IGN);
    av_log_set_level(AV_LOG_QUIET);
    LoggerOptions options={.queue_capacity=(unsigned)strtoul(argv[3],NULL,10),
        .rotate_seconds=(unsigned)strtoul(argv[4],NULL,10),.rotate_bytes=strtoull(argv[5],NULL,10),
        .stdout_write=log_send,.stdout_opaque=&channel};
    unsigned status_interval=(unsigned)strtoul(argv[6],NULL,10);
    if(options.queue_capacity<32 || !options.rotate_seconds || !options.rotate_bytes || !status_interval)return 2;
    Job *job=calloc(1,sizeof(*job)); NetworkStats network={0};
    if(!job)return 1;
    job->logger=logger_start(&options); job->network=&network;
    job->worker_spawned_us=strtoll(argv[2],NULL,10);
    job->wake_fd=eventfd(0,EFD_NONBLOCK|EFD_CLOEXEC);
    if(!job->logger || job->wake_fd<0 || pthread_mutex_init(&job->mutex,NULL))return 1;
    atomic_init(&job->cancel,0); atomic_init(&job->done,0);atomic_init(&job->branch_failed,0);
    network_stats_reset(&network);
    avformat_set_network_observer(network_observe,&network);
    if(avformat_network_init()<0||curl_global_init(CURL_GLOBAL_DEFAULT)!=CURLE_OK)return 1;
    job->worker_ready_us=now_us();
    int running=0,joined=0,final_ack=0,result=0;
    int64_t next_snapshot=now_us()+250000,next_report=0;
    yyjson_doc *assignment=NULL;
    yyjson_mut_doc *watermark_reply=NULL;
    int64_t disconnected_us=0,outage_us=0;
    uint64_t reconnects=0;
    uint64_t dropped=0;
    yyjson_mut_doc *ready=ipc_message("ready"); json_int(ready,"ready_us",job->worker_ready_us);
    if(channel_send(&channel,ready))next_snapshot=0;
    yyjson_mut_doc_free(ready);
    for(;;) {
        if(stopping) {if(running)request_stop(job); else break;}
        struct pollfd fds[]={{.fd=channel.fd,.events=POLLIN},{.fd=listener,.events=POLLIN}};
        int polled=poll(fds,2,50);
        if(polled<0 && errno!=EINTR) {result=1; break;}
        if(fds[1].revents&POLLIN) {
            int fd=ipc_accept(listener);
            if(fd>=0) {
                if(channel.fd>=0)close(fd);
                else {
                    pthread_mutex_lock(&channel.mutex); channel.fd=fd; pthread_mutex_unlock(&channel.mutex);
                    reconnects++;
                    if(disconnected_us) {outage_us+=now_us()-disconnected_us; disconnected_us=0;}
                    pthread_mutex_lock(&job->mutex);
                    for(int i=0;i<2;i++)if(job->first_chain[i]) {
                        yyjson_mut_doc *chain=yyjson_mut_doc_mut_copy(job->first_chain[i],NULL);
                        json_int(chain,"replayed_after_reconnect",1); logger_submit(job->logger,NULL,chain);
                    }
                    pthread_mutex_unlock(&job->mutex);
                    for(int k=0;k<job->config.output_count;k++) {
                        Job *b=job->outputs[k];pthread_mutex_lock(&b->mutex);
                        for(int i=0;i<2;i++)if(b->first_chain[i]) {
                            yyjson_mut_doc *chain=yyjson_mut_doc_mut_copy(b->first_chain[i],NULL);
                            json_int(chain,"replayed_after_reconnect",1);logger_submit(job->logger,NULL,chain);
                        }
                        pthread_mutex_unlock(&b->mutex);
                    }
                    next_snapshot=0;
                }
            }
        }
        if(fds[0].revents&POLLIN) {
            yyjson_doc *doc=NULL; int received=ipc_receive(channel.fd,&doc);
            if(received<0)channel_close(&channel);
            if(received>0) {
                if(ipc_kind(doc,"start") && !running && !stopping) {
                    if(start_job(job,doc)) {yyjson_doc_free(doc); result=1; break;}
                    assignment=doc; doc=NULL;
                    running=1; next_snapshot=0; next_report=now_us()+(int64_t)status_interval*1000000;
                } else if(ipc_kind(doc,"stop")) {
                    if(running)request_stop(job); else stopping=1;
                } else if(ipc_kind(doc,"watermark")&&running&&!joined&&!watermark_reply) {
                    yyjson_val *root=yyjson_doc_get_root(doc);
                    NakiWatermarkConfig config;uint64_t revision=0;char error[256]={0};
                    int ret=naki_watermark_update(yyjson_obj_get(root,"update"),&config,&revision,error,sizeof(error));
                    Job *target=job;const char *output_id=yyjson_get_str(yyjson_obj_get(yyjson_obj_get(root,"update"),"output_id"));
                    if(!ret&&job->config.output_count) {
                        target=NULL;for(int i=0;i<job->config.output_count;i++)if(output_id&&!strcmp(output_id,job->outputs[i]->output_id))target=job->outputs[i];
                        if(!target) {snprintf(error,sizeof(error),"output_id must name an existing branch");ret=-1;}
                    } else if(!ret&&output_id) {snprintf(error,sizeof(error),"output_id is only valid for multi-output jobs");ret=-1;}
                    if(!ret&&atomic_load(&target->done)) {snprintf(error,sizeof(error),"output branch has finished");ret=-1;}
                    if(!ret)ret=naki_watermark_request(target->watermark,&config,revision,error,sizeof(error));
                    watermark_reply=ipc_message("watermark_ack");
                    json_int(watermark_reply,"token",yyjson_get_sint(yyjson_obj_get(root,"token")));
                    json_int(watermark_reply,"revision",(int64_t)revision);
                    json_int(watermark_reply,"accepted",!ret);json_str(watermark_reply,"error",error);
                    if(output_id)json_str(watermark_reply,"output_id",output_id);
                    next_snapshot=0;
                } else if(ipc_kind(doc,"ack") && joined)final_ack=1;
                else channel_close(&channel);
                if(doc)yyjson_doc_free(doc);
            }
        }
        if(fds[0].revents&(POLLERR|POLLHUP|POLLNVAL))channel_close(&channel);
        int64_t current=now_us();
        if(watermark_reply&&channel.fd>=0&&!channel_send(&channel,watermark_reply)) {
            yyjson_mut_doc_free(watermark_reply);watermark_reply=NULL;
        }
        if(running && !joined) {
            naki_policy_tick(job,current);
            for(int i=0;i<job->config.output_count;i++)if(job->outputs[i]&&!atomic_load(&job->outputs[i]->done))naki_policy_tick(job->outputs[i],current);
        }
        if(channel.fd<0 && !disconnected_us)disconnected_us=current;
        if(running && !joined && atomic_load(&job->done)) {
            pthread_join(job->thread,NULL); joined=1; next_snapshot=0;
        }
        if(final_ack || (stopping && joined && channel.fd<0))break;
        if(running && !joined && current>=next_report) {
            yyjson_mut_doc *report=job_report(job,0);
            if(report)logger_submit(job->logger,NULL,report);
            next_report=current+(int64_t)status_interval*1000000;
        }
        if(channel.fd>=0 && current>=next_snapshot) {
            yyjson_mut_doc *doc=running ? job_snapshot(job) : ipc_message("ready");
            if(running) {
                if(!job->config.output_count)naki_watermark_status(job->watermark,doc);
                json_int(doc,"ipc_version",IPC_VERSION);
                json_str(doc,"ipc_kind",joined ? "finished" : "snapshot");
                json_int(doc,"ipc_snapshots_coalesced",(int64_t)dropped);
                json_int(doc,"control_reconnects",(int64_t)reconnects);
                json_num(doc,"control_disconnected_ms",outage_us/1000.0);
                yyjson_mut_obj_add_val(doc,yyjson_mut_doc_get_root(doc),"job_info",
                    yyjson_val_mut_copy(doc,yyjson_doc_get_root(assignment)));
            } else {
                json_int(doc,"ready_us",job->worker_ready_us);
            }
            if(channel_send(&channel,doc)) {
                if(errno!=EAGAIN && errno!=EWOULDBLOCK)channel_close(&channel);
                dropped++;
            }
            yyjson_mut_doc_free(doc);
            next_snapshot=current+250000;
        }
        /* Losing control does not cancel media. Keep final state until a controller acknowledges it. */
    }
    if(running && !joined) {request_stop(job); pthread_join(job->thread,NULL);}
    naki_watermark_close(&job->watermark);
    for(int i=0;i<job->config.output_count;i++)if(job->outputs[i]) {
        Job *b=job->outputs[i];naki_watermark_close(&b->watermark);
        for(int k=0;k<2;k++)if(b->first_chain[k])yyjson_mut_doc_free(b->first_chain[k]);
        close(b->wake_fd);pthread_mutex_destroy(&b->mutex);free(b);
    }
    if(watermark_reply)yyjson_mut_doc_free(watermark_reply);
    avformat_set_network_observer(NULL,NULL); avformat_network_deinit();
    curl_global_cleanup();
    logger_trace_release(job->logger,job->trace);
    logger_stop(job->logger);
    if(assignment)yyjson_doc_free(assignment);
    for(int i=0;i<2;i++)if(job->first_chain[i])yyjson_mut_doc_free(job->first_chain[i]);
    close(job->wake_fd); channel_close(&channel); close(listener); unlink(endpoint);
    pthread_mutex_destroy(&channel.mutex); pthread_mutex_destroy(&job->mutex); free(job);
    return result;
}
