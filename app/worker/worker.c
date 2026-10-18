#define _GNU_SOURCE
#include "job.h"
#include "platform/runtime.h"
#include "frame_policy_runtime.h"
#include "media/pipeline.h"
#include "platform/ipc.h"
#include "media/watermark.h"
#include <curl/curl.h>
#include "crash/crash.h"
#include "control/identity.h"
#include "control/intent.h"
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
static int start_job(Job *job, yyjson_doc *doc) {
    yyjson_val *root=yyjson_doc_get_root(doc);
    const char *id=yyjson_get_str(yyjson_obj_get(root,"job_id"));
    const char *path=yyjson_get_str(yyjson_obj_get(root,"log_path"));
    const char *request_id=yyjson_get_str(yyjson_obj_get(root,"request_id"));
    size_t size=0; char error[256];
    char *config=yyjson_val_write(yyjson_obj_get(root,"config"),0,&size);
    int valid=naki_id_valid(id,sizeof(job->id)-1) && naki_id_valid(request_id,sizeof(job->request_id)-1) && path && strlen(path)<sizeof(job->log_path) &&
              config && !config_parse(config,size,&job->config,error,sizeof(error));
    char fingerprint[65];const char *expected=yyjson_get_str(yyjson_obj_get(root,"spec_fingerprint"));
    valid=valid&&naki_json_id(yyjson_obj_get(root,"creation_operation_id"),128)&&naki_json_id(yyjson_obj_get(root,"service_namespace"),128)&&
        expected&&!naki_config_fingerprint(&job->config,fingerprint,error,sizeof(error))&&!strcmp(expected,fingerprint);
    free(config);
    if(!valid)return -1;
    strcpy(job->id,id); strcpy(job->request_id,request_id); strcpy(job->log_path,path); job->lifecycle.state=NAKI_JOB_STATE_STARTING;
    if(!job->config.output_count)strcpy(job->output_id,job->config.single_output_id);
    crash_set_job(job->id);
    job->lifecycle.started_us=yyjson_get_sint(yyjson_obj_get(root,"request_us"));
    naki_overload_init(&job->flow.overload);
    job->progress.assigned_us=yyjson_get_sint(yyjson_obj_get(root,"assigned_us"));
    job->progress.worker_received_us=now_us(); job->progress.window_start_us=job->lifecycle.started_us;
    job->lifecycle.prewarmed=yyjson_get_bool(yyjson_obj_get(root,"prewarmed"));
    job->operations=naki_operations_open(256,2*1024*1024,1800000000);if(!job->operations)return -1;
    NakiOperationIntent create={.kind=NAKI_OP_CREATE,.job_id=job->id,
        .operation_id=yyjson_get_str(yyjson_obj_get(root,"creation_operation_id")),.fingerprint=fingerprint,.origin_request_id=request_id};
    NakiOperationHandle created;
    if(naki_operations_reserve(job->operations,&create,now_us(),&created)!=1||naki_operations_pin(job->operations,created,NAKI_OP_PIN_OWNERSHIP,0))return -1;
    if(job->config.frame_log && !(job->trace=logger_trace_open(job->logger,job->log_path)))return -1;
    job->watermark=naki_watermark_open(&job->config.watermark,job->lifecycle.started_us,&job->lifecycle.cancel);
    if(!job->watermark)return -1;
    if(!job->config.output_count&&naki_watermark_bind(job->watermark,job->operations,job->id,job->output_id,job->request_id,job->logger))return -1;
    for(int i=0;i<job->config.output_count;i++) {
        Job *b=calloc(1,sizeof(*b));if(!b)return -1;
        naki_output_config(&b->config,&job->config,i);b->parent=job;
        strcpy(b->output_id,job->config.outputs[i].id);strcpy(b->id,job->id);strcpy(b->request_id,job->request_id);
        b->lifecycle.state=NAKI_JOB_STATE_WAITING_ASSETS;strcpy(b->log_path,job->log_path);
        b->logger=job->logger;b->trace=job->trace;b->lifecycle.started_us=job->lifecycle.started_us;b->progress.window_start_us=job->lifecycle.started_us;
        b->progress.assigned_us=job->progress.assigned_us;b->progress.worker_received_us=job->progress.worker_received_us;b->progress.worker_spawned_us=job->progress.worker_spawned_us;
        b->progress.worker_ready_us=job->progress.worker_ready_us;b->lifecycle.prewarmed=job->lifecycle.prewarmed;
        b->wake_fd=eventfd(0,EFD_NONBLOCK|EFD_CLOEXEC);
        if(b->wake_fd<0||pthread_mutex_init(&b->mutex,NULL)) {if(b->wake_fd>=0)close(b->wake_fd);free(b);return -1;}
        atomic_init(&b->lifecycle.cancel,0);atomic_init(&b->lifecycle.done,0);atomic_init(&b->lifecycle.branch_failed,0);naki_overload_init(&b->flow.overload);
        job->outputs[i]=b;b->watermark_snapshot=naki_watermark_status;
        b->watermark=naki_watermark_open(&b->config.watermark,b->lifecycle.started_us,&b->lifecycle.cancel);if(!b->watermark)return -1;
        if(naki_watermark_bind(b->watermark,job->operations,job->id,b->output_id,job->request_id,job->logger))return -1;
    }
    return pthread_create(&job->thread,NULL,pipeline_main,job) ? -1 : 0;
}
static int stop_intent(Job *job,const char *operation,const char *origin,NakiOperationHandle *handle) {
    yyjson_mut_doc *d=yyjson_mut_doc_new(NULL);if(!d)return -ENOMEM;
    yyjson_mut_val *root=yyjson_mut_obj(d);yyjson_mut_doc_set_root(d,root);
    yyjson_mut_obj_add_str(d,root,"kind","stop");yyjson_mut_obj_add_strcpy(d,root,"job_id",job->id);
    size_t bytes=0;char *text=yyjson_mut_write(d,0,&bytes);yyjson_mut_doc_free(d);
    yyjson_doc *immutable=text?yyjson_read(text,bytes,0):NULL;free(text);char fingerprint[65],error[128];
    int ret=immutable?naki_intent_fingerprint(yyjson_doc_get_root(immutable),fingerprint,error,sizeof(error)):-ENOMEM;
    if(immutable)yyjson_doc_free(immutable);
    if(ret)return ret;
    NakiOperationIntent in={.kind=NAKI_OP_STOP,.job_id=job->id,.operation_id=operation,.origin_request_id=origin,.fingerprint=fingerprint};
    ret=naki_operations_reserve(job->operations,&in,now_us(),handle);
    if(ret==1)ret=naki_operations_transition(job->operations,*handle,NAKI_OP_STOPPING,0,0,NULL,now_us());
    return ret;
}
static Job *target_output(Job *job,const char *output) {
    if(!job->config.output_count)return output&&!strcmp(output,job->output_id)?job:NULL;
    for(int i=0;i<job->config.output_count;i++)if(output&&!strcmp(output,job->outputs[i]->output_id))return job->outputs[i];
    return NULL;
}
static int targeted(yyjson_val *root,Job *job) {
    const char *id=yyjson_get_str(yyjson_obj_get(root,"job_id")),*instance=yyjson_get_str(yyjson_obj_get(root,"worker_instance_id"));
    return id&&!strcmp(id,job->id)&&instance&&!strcmp(instance,naki_process_id());
}

/* Private executable: fd 3 is the first controller; the private endpoint permits later takeover. */
int main(int argc,char **argv) {
    if(naki_process_init())return 1;
    crash_set_instance(naki_process_id());
    if(argc!=9)return 2;
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
        .rotate_seconds=(unsigned)strtoul(argv[4],NULL,10),.rotate_bytes=strtoull(argv[5],NULL,10)};
    strcpy(options.role,"worker");strcpy(options.instance_id,naki_process_id());
    if(snprintf(options.event_prefix,sizeof(options.event_prefix),"%s/worker-%s.events",argv[8],naki_process_id())>=(int)sizeof(options.event_prefix))return 2;
    unsigned status_interval=(unsigned)strtoul(argv[6],NULL,10);
    unsigned stop_timeout=(unsigned)strtoul(argv[7],NULL,10);
    if(!stop_timeout||stop_timeout>60)return 2;
    if(options.queue_capacity<32 || !options.rotate_seconds || !options.rotate_bytes || !status_interval)return 2;
    Job *job=calloc(1,sizeof(*job)); NetworkStats network={0};
    if(!job)return 1;
    job->logger=logger_start(&options); job->network=&network;
    if(!job->logger) {fprintf(stderr,"Cannot initialize worker process event sink\n");return 1;}
    logger_submit(job->logger,NULL,new_event("worker_prewarmed",NULL));
    job->progress.worker_spawned_us=strtoll(argv[2],NULL,10);
    job->wake_fd=eventfd(0,EFD_NONBLOCK|EFD_CLOEXEC);
    if(!job->logger || job->wake_fd<0 || pthread_mutex_init(&job->mutex,NULL))return 1;
    atomic_init(&job->lifecycle.cancel,0); atomic_init(&job->lifecycle.done,0);atomic_init(&job->lifecycle.branch_failed,0);
    network_stats_reset(&network);
    avformat_set_network_observer(network_observe,&network);
    if(avformat_network_init()<0||curl_global_init(CURL_GLOBAL_DEFAULT)!=CURLE_OK)return 1;
    job->progress.worker_ready_us=now_us();
    int running=0,joined=0,final_ack=0,result=0;
    int64_t next_snapshot=now_us()+250000,next_report=0;
    yyjson_doc *assignment=NULL;
    yyjson_mut_doc *watermark_reply=NULL;
    yyjson_mut_doc *accepted_reply=NULL;
    yyjson_mut_doc *stop_reply=NULL;
    yyjson_mut_doc *operation_reply=NULL,*export_reply=NULL;
    NakiOperationHandle stop_operation=0;
    int owner_info_needed=0,export_pending=0;unsigned export_cursor=0;uint64_t export_id=0;
    int64_t disconnected_us=0,outage_us=0;
    uint64_t reconnects=0;
    uint64_t dropped=0;
    yyjson_mut_doc *ready=ipc_message("ready"); json_int(ready,"ready_us",job->progress.worker_ready_us);
    json_int(ready,"component_updates_api",1);json_int(ready,"operation_query_api",1);
    json_int(ready,"operation_result_version_api",1);
    if(channel_send(&channel,ready))next_snapshot=0;
    yyjson_mut_doc_free(ready);
    for(;;) {
        if(stopping) {
            if(running) {
                pthread_mutex_lock(&job->mutex);
                if(!job->lifecycle.stop_requested_us) {job->lifecycle.stop_requested_us=now_us();job->lifecycle.stop_deadline_us=job->lifecycle.stop_requested_us+(int64_t)stop_timeout*1000000;}
                pthread_mutex_unlock(&job->mutex);request_stop(job);
            } else break;
        }
        if(running&&!joined&&job->lifecycle.stop_deadline_us&&now_us()>=job->lifecycle.stop_deadline_us)_exit(124);
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
                    yyjson_mut_doc *recovered=new_event("worker_control_reconnected",running?job:NULL);
                    logger_submit(job->logger,NULL,recovered);
                    next_snapshot=0;
                    if(running) {owner_info_needed=1;export_pending=1;export_cursor=0;export_id++;if(export_reply)yyjson_mut_doc_free(export_reply);export_reply=NULL;}
                }
            }
        }
        if(fds[0].revents&POLLIN) {
            yyjson_doc *doc=NULL; int received=ipc_receive(channel.fd,&doc);
            if(received<0) {if(doc)yyjson_doc_free(doc);doc=NULL;channel_close(&channel);}
            if(received>0) {
                if(ipc_kind(doc,"start") && !running && !stopping) {
                    if(start_job(job,doc)) {yyjson_doc_free(doc); result=1; break;}
                    assignment=doc; doc=NULL;
                    running=1; next_snapshot=0; next_report=now_us()+(int64_t)status_interval*1000000;
                    accepted_reply=ipc_message("accepted");json_str(accepted_reply,"job_id",job->id);
                    json_int(accepted_reply,"worker_received_us",job->progress.worker_received_us);
                    owner_info_needed=1;export_pending=1;export_cursor=0;export_id++;
                } else if(ipc_kind(doc,"stop")||ipc_kind(doc,"standby_stop")||ipc_kind(doc,"drain")) {
                    yyjson_val *root=yyjson_doc_get_root(doc);
                    const char *instance=yyjson_get_str(yyjson_obj_get(root,"worker_instance_id"));
                    const char *id=yyjson_get_str(yyjson_obj_get(root,"job_id"));
                    const char *operation=yyjson_get_str(yyjson_obj_get(root,"operation_id"));
                    const char *origin=yyjson_get_str(yyjson_obj_get(root,"origin_request_id"));
                    int valid=instance&&!strcmp(instance,naki_process_id())&&
                        (ipc_kind(doc,"drain")||(ipc_kind(doc,"standby_stop")&&!running)||
                         (ipc_kind(doc,"stop")&&running&&!joined&&id&&!strcmp(id,job->id)&&naki_json_id(yyjson_obj_get(root,"operation_id"),128)&&
                          naki_json_id(yyjson_obj_get(root,"origin_request_id"),128)&&(!*job->lifecycle.stop_operation_id||!strcmp(operation,job->lifecycle.stop_operation_id))));
                    if(valid&&ipc_kind(doc,"stop")&&stop_intent(job,operation,origin,&stop_operation)<0)valid=0;
                    if(valid) {
                        if(ipc_kind(doc,"drain")||ipc_kind(doc,"standby_stop"))stopping=1;
                        if(running) {
                            int64_t requested=yyjson_get_sint(yyjson_obj_get(root,"stop_requested_us"));
                            int64_t deadline=yyjson_get_sint(yyjson_obj_get(root,"stop_deadline_us"));
                            int64_t now=now_us();
                            if(requested<=0||requested>now||deadline<requested||deadline-requested>(int64_t)stop_timeout*1000000) {
                                requested=now;deadline=now+(int64_t)stop_timeout*1000000;
                            }
                            pthread_mutex_lock(&job->mutex);
                            if(!job->lifecycle.stop_requested_us) {job->lifecycle.stop_requested_us=requested;job->lifecycle.stop_deadline_us=deadline;}
                            if(ipc_kind(doc,"stop")&&!*job->lifecycle.stop_operation_id) {strcpy(job->lifecycle.stop_operation_id,operation);strcpy(job->lifecycle.stop_origin_request_id,origin);}
                            pthread_mutex_unlock(&job->mutex);request_stop(job);
                            naki_watermark_stop(job->watermark);
                            for(int i=0;i<job->config.output_count;i++)naki_watermark_stop(job->outputs[i]->watermark);
                            if(ipc_kind(doc,"stop")&&!stop_reply) {
                                stop_reply=ipc_message("stop_ack");json_str(stop_reply,"job_id",job->id);json_str(stop_reply,"operation_id",job->lifecycle.stop_operation_id);
                            }
                            next_snapshot=0;
                        }
                    } else {
                        yyjson_mut_doc *answer=ipc_message("control_rejected");json_str(answer,"error_code","target_mismatch");
                        if(running)json_str(answer,"job_id",job->id);
                        channel_send(&channel,answer);yyjson_mut_doc_free(answer);
                    }
                } else if(ipc_kind(doc,"watermark")&&running&&!watermark_reply) {
                    yyjson_val *root=yyjson_doc_get_root(doc);
                    NakiWatermarkConfig config;uint64_t revision=0;char error[256]={0};
                    const char *id=yyjson_get_str(yyjson_obj_get(root,"job_id"));
                    const char *instance=yyjson_get_str(yyjson_obj_get(root,"worker_instance_id"));
                    int ret=!id||strcmp(id,job->id)||!instance||strcmp(instance,naki_process_id());
                    if(ret)snprintf(error,sizeof(error),"job/worker target mismatch");
                    else ret=naki_watermark_update(yyjson_obj_get(root,"update"),&config,&revision,error,sizeof(error));
                    Job *target=job;const char *output_id=yyjson_get_str(yyjson_obj_get(yyjson_obj_get(root,"update"),"output_id"));
                    if(!ret&&job->config.output_count) {
                        target=NULL;for(int i=0;i<job->config.output_count;i++)if(output_id&&!strcmp(output_id,job->outputs[i]->output_id))target=job->outputs[i];
                        if(!target) {snprintf(error,sizeof(error),"output_id must name an existing branch");ret=-1;}
                    } else if(!ret&&output_id&&strcmp(output_id,job->output_id)) {snprintf(error,sizeof(error),"output_id does not name the logical output");ret=-1;}
                    const char *operation=yyjson_get_str(yyjson_obj_get(root,"operation_id")),*origin=yyjson_get_str(yyjson_obj_get(root,"origin_request_id"));
                    NakiUpdateRequest update;yyjson_mut_doc *result_doc=NULL;
                    if(!ret&&(!naki_id_valid(operation,128)||!naki_id_valid(origin,128))) {snprintf(error,sizeof(error),"invalid legacy operation identity");ret=-1;}
                    /* Legacy full configurations commonly carry a default
                     * startup timeout. Import only mutable typed fields; that
                     * timeout never changes the already-fixed startup policy. */
                    if(!ret&&config.required!=target->config.watermark.required) {snprintf(error,sizeof(error),"startup.required is immutable");ret=-1;}
                    if(!ret)ret=naki_update_from_legacy(&config,revision,job->id,target->output_id,operation,&update,error,sizeof(error));
                    if(!ret)ret=naki_watermark_submit(target->watermark,&update,origin,&result_doc,error,sizeof(error));
                    watermark_reply=ipc_message("watermark_ack");
                    json_str(watermark_reply,"job_id",job->id);json_str(watermark_reply,"operation_id",operation);
                    json_int(watermark_reply,"token",yyjson_get_sint(yyjson_obj_get(root,"token")));
                    json_int(watermark_reply,"revision",(int64_t)revision);
                    json_int(watermark_reply,"accepted",!ret);json_str(watermark_reply,"error",error);
                    if(output_id)json_str(watermark_reply,"output_id",output_id);
                    if(result_doc) {yyjson_mut_obj_add_val(watermark_reply,yyjson_mut_doc_get_root(watermark_reply),"operation",yyjson_mut_val_mut_copy(watermark_reply,yyjson_mut_doc_get_root(result_doc)));yyjson_mut_doc_free(result_doc);}
                    next_snapshot=0;
                } else if((ipc_kind(doc,"update")||ipc_kind(doc,"operation_get"))&&running&&!operation_reply) {
                    yyjson_val *root=yyjson_doc_get_root(doc);char error[256]="target_mismatch";yyjson_mut_doc *record=NULL;int code=409;
                    if(targeted(root,job)) {
                        if(ipc_kind(doc,"operation_get")) {
                            const char *operation=yyjson_get_str(yyjson_obj_get(root,"operation_id"));NakiOperationHandle handle;
                            code=naki_id_valid(operation,128)&&!naki_operations_lookup(job->operations,job->id,operation,&handle)?200:404;
                            if(code==200)record=naki_operations_get(job->operations,handle);
                            snprintf(error,sizeof(error),"operation_not_found");
                        } else {
                            NakiUpdateRequest update;const char *origin=yyjson_get_str(yyjson_obj_get(root,"origin_request_id"));
                            if(!naki_id_valid(origin,128)||naki_update_parse(yyjson_obj_get(root,"update"),job->id,&update,error,sizeof(error)))code=400;
                            else {
                                Job *target=target_output(job,update.output_id);
                                if(!target) {code=404;snprintf(error,sizeof(error),"output_not_found");}
                                else {
                                    int ret=naki_watermark_submit(target->watermark,&update,origin,&record,error,sizeof(error));
                                    code=!ret?202:ret==-ENOSPC?429:ret==-ENOMEM?503:409;
                                }
                            }
                        }
                    }
                    operation_reply=ipc_message("operation_reply");json_str(operation_reply,"job_id",job->id);
                    json_str(operation_reply,"requester_instance_id",yyjson_get_str(yyjson_obj_get(root,"instance_id")));
                    json_int(operation_reply,"token",yyjson_get_sint(yyjson_obj_get(root,"token")));json_int(operation_reply,"http_status",code);
                    json_str(operation_reply,"error",error);
                    if(record) {yyjson_mut_obj_add_val(operation_reply,yyjson_mut_doc_get_root(operation_reply),"operation",yyjson_mut_val_mut_copy(operation_reply,yyjson_mut_doc_get_root(record)));yyjson_mut_doc_free(record);}
                    next_snapshot=0;
                } else if(ipc_kind(doc,"operations_sync")&&running&&targeted(yyjson_doc_get_root(doc),job)) {
                    owner_info_needed=1;export_pending=1;export_cursor=0;export_id++;if(export_reply)yyjson_mut_doc_free(export_reply);export_reply=NULL;
                } else if(ipc_kind(doc,"ack") && joined)final_ack=1;
                else channel_close(&channel);
                if(doc)yyjson_doc_free(doc);
            }
        }
        if(fds[0].revents&(POLLERR|POLLHUP|POLLNVAL))channel_close(&channel);
        int64_t current=now_us();
        if(accepted_reply&&channel.fd>=0&&!channel_send(&channel,accepted_reply)) {yyjson_mut_doc_free(accepted_reply);accepted_reply=NULL;}
        if(stop_reply&&channel.fd>=0&&!channel_send(&channel,stop_reply)) {yyjson_mut_doc_free(stop_reply);stop_reply=NULL;}
        if(watermark_reply&&channel.fd>=0&&!channel_send(&channel,watermark_reply)) {
            yyjson_mut_doc_free(watermark_reply);watermark_reply=NULL;
        }
        if(running && !joined) {
            naki_policy_tick(job,current);
            for(int i=0;i<job->config.output_count;i++)if(job->outputs[i]&&!atomic_load(&job->outputs[i]->lifecycle.done))naki_policy_tick(job->outputs[i],current);
        }
        if(channel.fd<0 && !disconnected_us)disconnected_us=current;
        if(running && !joined && atomic_load(&job->lifecycle.done)) {
            pthread_join(job->thread,NULL); joined=1; next_snapshot=0;
            if(stop_operation)naki_operations_transition(job->operations,stop_operation,NAKI_OP_STOPPED,0,0,NULL,current);
            export_pending=1;export_cursor=0;export_id++;if(export_reply)yyjson_mut_doc_free(export_reply);export_reply=NULL;
        }
        if(owner_info_needed&&channel.fd>=0) {
            yyjson_mut_doc *info=ipc_message("owner_info");json_str(info,"job_id",job->id);json_int(info,"component_updates_api",1);
            json_int(info,"operation_query_api",1);json_int(info,"operation_result_version_api",1);json_int(info,"operation_page_limit",8);
            yyjson_mut_obj_add_bool(info,yyjson_mut_doc_get_root(info),"stop_intent",job->lifecycle.stop_requested_us!=0);
            json_int(info,"stop_deadline_us",job->lifecycle.stop_deadline_us);json_str(info,"stop_operation_id",job->lifecycle.stop_operation_id);json_str(info,"stop_origin_request_id",job->lifecycle.stop_origin_request_id);
            yyjson_mut_obj_add_val(info,yyjson_mut_doc_get_root(info),"job_info",yyjson_val_mut_copy(info,yyjson_doc_get_root(assignment)));
            if(!channel_send(&channel,info))owner_info_needed=0;
            else if(errno!=EAGAIN&&errno!=EWOULDBLOCK)channel_close(&channel);
            yyjson_mut_doc_free(info);
        }
        for(int page=0;page<4&&export_pending&&!owner_info_needed&&channel.fd>=0;page++) {
            unsigned next=0;
            if(!export_reply) {
                export_reply=naki_operations_page(job->operations,export_cursor,8,&next);
                if(!export_reply) {result=1;stopping=1;break;}
                json_int(export_reply,"ipc_version",IPC_VERSION);json_str(export_reply,"ipc_kind","operations_page");json_str(export_reply,"instance_id",naki_process_id());
                json_str(export_reply,"job_id",job->id);json_int(export_reply,"export_id",(int64_t)export_id);json_int(export_reply,"cursor",export_cursor);
            }
            next=(unsigned)yyjson_mut_get_uint(yyjson_mut_obj_get(yyjson_mut_doc_get_root(export_reply),"next_cursor"));
            if(channel_send(&channel,export_reply)) {if(errno!=EAGAIN&&errno!=EWOULDBLOCK)channel_close(&channel);break;}
            yyjson_mut_doc_free(export_reply);export_reply=NULL;export_cursor=next;if(!next)export_pending=0;
        }
        if(operation_reply&&channel.fd>=0&&!owner_info_needed&&!channel_send(&channel,operation_reply)) {yyjson_mut_doc_free(operation_reply);operation_reply=NULL;}
        if(final_ack || (stopping && joined && channel.fd<0))break;
        if(running && !joined && current>=next_report) {
            yyjson_mut_doc *report=job_report(job,0);
            if(report)logger_submit(job->logger,NULL,report);
            next_report=current+(int64_t)status_interval*1000000;
        }
        if(channel.fd>=0 && !owner_info_needed&&!export_pending&&current>=next_snapshot) {
            yyjson_mut_doc *doc=running ? job_snapshot(job) : ipc_message("ready");
            if(running) {
                if(!job->config.output_count)naki_watermark_status(job->watermark,doc);
                json_int(doc,"ipc_version",IPC_VERSION);
                json_str(doc,"ipc_kind",joined ? "finished" : "snapshot");
                json_int(doc,"ipc_snapshots_coalesced",(int64_t)dropped);
                json_int(doc,"control_reconnects",(int64_t)reconnects);
                json_num(doc,"control_disconnected_ms",outage_us/1000.0);
            } else {
                json_int(doc,"ready_us",job->progress.worker_ready_us);
                json_int(doc,"component_updates_api",1);json_int(doc,"operation_query_api",1);
                json_int(doc,"operation_result_version_api",1);
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
        for(int k=0;k<2;k++)if(b->progress.first_chain[k])yyjson_mut_doc_free(b->progress.first_chain[k]);
        close(b->wake_fd);pthread_mutex_destroy(&b->mutex);free(b);
    }
    if(watermark_reply)yyjson_mut_doc_free(watermark_reply);
    if(accepted_reply)yyjson_mut_doc_free(accepted_reply);
    if(stop_reply)yyjson_mut_doc_free(stop_reply);
    if(operation_reply)yyjson_mut_doc_free(operation_reply);
    if(export_reply)yyjson_mut_doc_free(export_reply);
    naki_operations_close(&job->operations);
    avformat_set_network_observer(NULL,NULL); avformat_network_deinit();
    curl_global_cleanup();
    logger_trace_release(job->logger,job->trace);
    logger_stop(job->logger);
    if(assignment)yyjson_doc_free(assignment);
    for(int i=0;i<2;i++)if(job->progress.first_chain[i])yyjson_mut_doc_free(job->progress.first_chain[i]);
    close(job->wake_fd); channel_close(&channel); close(listener); unlink(endpoint);
    pthread_mutex_destroy(&channel.mutex); pthread_mutex_destroy(&job->mutex); free(job);
    return result;
}
