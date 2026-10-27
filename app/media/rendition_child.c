#define _GNU_SOURCE
#include "rendition_runtime.h"
#include "rendition_message.h"
#include "rendition_media.h"
#include "frame_wire.h"
#include "audio_wire.h"
#include "pipeline_internal.h"
#include "platform/ipc.h"
#include "platform/rendition_guard.h"
#include "control/identity.h"
#include "control/intent.h"
#include <curl/curl.h>
#include <sys/eventfd.h>
#include <sys/prctl.h>
#include <signal.h>
#define COMPLETIONS 256U
/* Only local threads share this ring. Credit bounds imply at most two
 * completions (accepted/release) per authorized raw lease, <=96, even when
 * the parent stops reading. No pixel destructor waits on socket or logger. */
typedef struct Child {
    Job *job,*parent;
    Pipeline *media;
    NakiPoolView video,audio;
    char session[33],instance[33];
    uint64_t generation,source_generation,inflight_limit;
    pthread_mutex_t mutex,media_mutex;
    int accepts_updates;
    NakiRenditionMessage completions[COMPLETIONS];unsigned head,count;
    int completion_wake,control_fd,media_fd,completion_fd,is_thread;
    atomic_int prepared,media_done,completion_error;
    int prepare_error;char failure_reason[256];
    AVFrame prototype;
    AVRational tb;
    NakiVideoRate rate;
    int wrap_bits,io_timeout_ms;
} Child;
typedef struct RawOwner {Child *child;NakiRenditionMessage message;} RawOwner;
static void complete(Child *c,NakiRenditionMessage message,unsigned kind) {
    message.kind=kind;pthread_mutex_lock(&c->mutex);
    if(c->count==COMPLETIONS)atomic_store(&c->completion_error,1);
    else {c->completions[(c->head+c->count)%COMPLETIONS]=message;c->count++;}
    pthread_mutex_unlock(&c->mutex);uint64_t one=1;ssize_t n=write(c->completion_wake,&one,sizeof(one));(void)n;
}
static void raw_free(void *opaque,uint8_t *data){(void)data;RawOwner *o=opaque;complete(o->child,o->message,NAKI_RMSG_RELEASE);free(o);}
static AVBufferRef *raw_owner(Child *c,NakiRenditionMessage *m) {
    NakiPoolView *view=m->media==NAKI_RMSG_VIDEO?&c->video:&c->audio;
    const uint8_t *data=naki_pool_view_claim(view,&m->token);if(!data)return NULL;
    RawOwner *o=malloc(sizeof(*o));if(!o)return NULL;*o=(RawOwner){.child=c,.message=*m};
    AVBufferRef *ref=av_buffer_create((uint8_t *)data,m->token.bytes,raw_free,o,AV_BUFFER_FLAG_READONLY);if(!ref)free(o);return ref;
}
static int scoped(Child *c,const NakiRenditionMessage *m){return !strcmp(c->session,m->session)&&!strcmp(c->instance,m->instance)&&c->generation==m->rendition_generation&&m->source_generation>=c->source_generation;}
static void *media_main(void *opaque) {
    Child *c=opaque;Job *j=c->job;(void)crash_thread_init();
    int ret=naki_rendition_media_open(&c->media,j,&c->prototype,c->tb,c->rate,c->wrap_bits,c->io_timeout_ms);
    c->prepare_error=ret;pthread_mutex_lock(&c->media_mutex);c->accepts_updates=ret>=0;pthread_mutex_unlock(&c->media_mutex);atomic_store(&c->prepared,1);
    AVFrame *frame=av_frame_alloc();if(!frame&&ret>=0)ret=AVERROR(ENOMEM);
    while(ret>=0&&!atomic_load(&j->lifecycle.cancel)) {
        if(atomic_load(&c->completion_error)){ret=AVERROR(ENOBUFS);break;}
        NakiRenditionMessage m;ret=naki_rendition_receive(c->media_fd,&m);
        if(ret==-EAGAIN||ret==-EWOULDBLOCK){ret=naki_rendition_media_poll(c->media);if(ret<0)break;struct pollfd fd={.fd=c->media_fd,.events=POLLIN};poll(&fd,1,5);continue;}
        if(ret<0)break;
        if(strcmp(c->session,m.session)||strcmp(c->instance,m.instance)||c->generation!=m.rendition_generation){ret=AVERROR(EPROTO);break;}
        if(m.kind==NAKI_RMSG_EOF){ret=0;break;}
        if(!scoped(c,&m)||(m.kind!=NAKI_RMSG_VIDEO&&m.kind!=NAKI_RMSG_AUDIO)||m.kind!=m.media){ret=AVERROR(EPROTO);break;}
        c->source_generation=m.source_generation;
        AVBufferRef *owner=raw_owner(c,&m);if(!owner){ret=AVERROR_INVALIDDATA;break;}
        complete(c,m,NAKI_RMSG_ACCEPTED);
        if(m.kind==NAKI_RMSG_VIDEO) {
            ret=naki_frame_wire_unpack(frame,owner,NAKI_INPUT_MAX_PIXELS);
            if(ret>=0){Stamp *s=(Stamp *)frame->opaque_ref->data;if((uint64_t)s->frame_id!=m.frame_id||s->ingress_us!=m.ingress_us)ret=AVERROR_INVALIDDATA;}
            if(ret>=0){Stamp *s=(Stamp *)frame->opaque_ref->data;atomic_store(&j->remote_source_rate,s->source_rate_bps);ret=naki_rendition_media_frame(c->media,frame,c->inflight_limit);}
            av_frame_unref(frame);
        } else {
            InputItem item={0};ret=naki_audio_wire_unpack(&item,owner);
            if(ret>=0&&item.demuxed_us!=m.ingress_us)ret=AVERROR_INVALIDDATA;
            if(ret>=0)ret=naki_rendition_media_audio(c->media,&item);
            pipeline_input_free(&item);
        }
        av_buffer_unref(&owner);
        if(ret>=0)ret=naki_rendition_media_poll(c->media);
    }
    pthread_mutex_lock(&c->media_mutex);c->accepts_updates=0;pthread_mutex_unlock(&c->media_mutex);
    av_frame_free(&frame);ret=naki_rendition_media_close(&c->media,ret,atomic_load(&j->lifecycle.cancel));
    if(atomic_load(&j->lifecycle.cancel)){pthread_mutex_lock(&j->mutex);if(!j->lifecycle.branch_failed)j->lifecycle.state=NAKI_JOB_STATE_STOPPED;pthread_mutex_unlock(&j->mutex);}
    logger_trace_finish(j->logger,j->trace);
    (void)ret;atomic_store(&c->media_done,1);uint64_t one=1;ssize_t n=write(c->completion_wake,&one,sizeof(one));(void)n;return NULL;
}
static int bootstrap(Child *c,yyjson_val *v) {
    Job *j=c->job;char error[256];yyjson_val *config=yyjson_obj_get(v,"config");size_t bytes;char *text=yyjson_val_write(config,0,&bytes);
    int ret=text?config_parse(text,bytes,&j->config,error,sizeof(error)):-1;free(text);if(ret){snprintf(c->failure_reason,sizeof(c->failure_reason),"config: %.240s",error);return -EINVAL;}
    const char *id=yyjson_get_str(yyjson_obj_get(v,"job_id")),*request=yyjson_get_str(yyjson_obj_get(v,"request_id")),*output=yyjson_get_str(yyjson_obj_get(v,"output_id"));
    const char *session=yyjson_get_str(yyjson_obj_get(v,"session")),*instance=yyjson_get_str(yyjson_obj_get(v,"rendition_instance")),*log=yyjson_get_str(yyjson_obj_get(v,"log_path"));
    if(!naki_id_valid(id,63)||!naki_id_valid(request,128)||!naki_id_valid(output,32)||!session||strlen(session)!=32||!instance||strcmp(c->instance,instance)||!log||strlen(log)>4000)return -1;
    strcpy(j->id,id);strcpy(j->request_id,request);strcpy(j->output_id,output);strcpy(j->log_path,log);strcpy(c->session,session);
    c->generation=yyjson_get_uint(yyjson_obj_get(v,"rendition_generation"));c->source_generation=yyjson_get_uint(yyjson_obj_get(v,"source_generation"));c->inflight_limit=yyjson_get_uint(yyjson_obj_get(v,"inflight_bytes"));if(!c->generation||!c->source_generation||!c->inflight_limit)return -1;
#define I(k) yyjson_get_sint(yyjson_obj_get(v,k))
    c->prototype.width=I("width");c->prototype.height=I("height");c->prototype.format=I("format");c->prototype.color_range=I("color_range");c->prototype.colorspace=I("colorspace");
    c->prototype.color_primaries=I("color_primaries");c->prototype.color_trc=I("color_trc");c->prototype.chroma_location=I("chroma_location");c->prototype.sample_aspect_ratio=(AVRational){I("sar_num"),I("sar_den")};
    c->tb=(AVRational){I("tb_num"),I("tb_den")};c->wrap_bits=I("wrap_bits");
    if(c->tb.num<=0||c->tb.den<=0||c->prototype.width<1||c->prototype.height<1||(int64_t)c->prototype.width*c->prototype.height>NAKI_INPUT_MAX_PIXELS||
       (c->prototype.format!=AV_PIX_FMT_YUV420P&&c->prototype.format!=AV_PIX_FMT_YUV420P10LE)||naki_video_rate_resolve((AVRational){I("fps_num"),I("fps_den")},j->config.fps,&c->rate)<0)return -1;
    j->lifecycle.started_us=I("request_us");j->progress.window_start_us=j->lifecycle.started_us;
    int64_t prepare_accepted_us=I("prepare_accepted_us");c->io_timeout_ms=I("io_timeout_ms");
    if(prepare_accepted_us<=0||c->io_timeout_ms<100||c->io_timeout_ms>10000)return -EINVAL;
    j->progress.input_open_started_us=I("input_open_started_us");j->progress.input_open_done_us=I("input_open_done_us");j->progress.input_probe_done_us=I("input_probe_done_us");j->progress.decoder_ready_us=I("decoder_ready_us");j->progress.video_rate=c->rate;
    j->progress.assigned_us=I("assigned_us");j->progress.worker_received_us=I("worker_received_us");j->progress.worker_spawned_us=I("worker_spawned_us");j->progress.worker_ready_us=I("worker_ready_us");j->lifecycle.prewarmed=I("prewarmed");
#undef I
    j->progress.rendition_inflight_limit=c->inflight_limit;
    j->operations=naki_operations_open(256,2*1024*1024,1800000000);if(!j->operations)return -1;
    j->watermark=naki_watermark_open(&j->config.watermark,prepare_accepted_us,&j->lifecycle.cancel);
    if(!j->watermark||naki_watermark_bind(j->watermark,j->operations,j->id,j->output_id,j->request_id,j->logger))return -1;
    j->watermark_snapshot=naki_watermark_status;
    j->lifecycle.state=NAKI_JOB_STATE_STARTING;naki_overload_init(&j->flow.overload);if(!c->is_thread){crash_set_job(j->id);if(crash_set_rendition(c->session,j->output_id,c->generation))return -1;}
    j->rendition_generation=c->generation;j->rendition_pid=getpid();return 0;
}
static void desired_snapshot(Child *c,yyjson_mut_doc *message){
    yyjson_mut_doc *snapshot=yyjson_mut_doc_new(NULL);if(!snapshot)return;yyjson_mut_doc_set_root(snapshot,yyjson_mut_obj(snapshot));naki_watermark_status(c->job->watermark,snapshot);
    yyjson_mut_val *wm=yyjson_mut_obj_get(yyjson_mut_obj_get(yyjson_mut_doc_get_root(snapshot),"components"),"watermark");
    yyjson_mut_val *desired=yyjson_mut_obj_get(wm,"desired");if(desired){yyjson_mut_obj_add_val(message,yyjson_mut_doc_get_root(message),"watermark_desired",yyjson_mut_val_mut_copy(message,desired));json_int(message,"watermark_desired_revision",yyjson_mut_get_uint(yyjson_mut_obj_get(wm,"desired_revision")));}
    yyjson_mut_doc_free(snapshot);
}
typedef struct HistoryVersion {char id[129];uint64_t version;} HistoryVersion;
/* Return 1 after a complete pass, 0 after a partial pass, negative on socket
 * pressure/error. Versions advance only after the bounded message is queued. */
static int send_history(Child *c,unsigned *cursor,HistoryVersion *seen) {
    unsigned next=0;yyjson_mut_doc *page=naki_operations_page(c->job->operations,*cursor,8,&next);if(!page)return -ENOMEM;
    yyjson_mut_doc *message=ipc_message("rendition_history");yyjson_mut_val *out=yyjson_mut_arr(message);json_str(message,"rendition_instance",c->instance);
    yyjson_mut_val *records=yyjson_mut_obj_get(yyjson_mut_doc_get_root(page),"operations");unsigned slots[8],count=0;uint64_t versions[8];
    for(size_t i=0;i<yyjson_mut_arr_size(records);i++) {
        yyjson_mut_val *record=yyjson_mut_arr_get(records,i);const char *id=yyjson_mut_get_str(yyjson_mut_obj_get(record,"operation_id"));uint64_t version=yyjson_mut_get_uint(yyjson_mut_obj_get(record,"result_version"));if(!id)continue;
        unsigned index=256;
        for(unsigned k=0;k<256;k++)if(!strcmp(seen[k].id,id)){index=k;break;}
        if(index==256)for(unsigned k=0;k<256;k++){NakiOperationHandle handle;if(!*seen[k].id||naki_operations_lookup(c->job->operations,c->job->id,seen[k].id,&handle)){index=k;snprintf(seen[k].id,sizeof(seen[k].id),"%s",id);seen[k].version=0;break;}}
        if(index==256){yyjson_mut_doc_free(page);yyjson_mut_doc_free(message);return -ENOSPC;}
        if(version<=seen[index].version)continue;
        slots[count]=index;versions[count++]=version;yyjson_mut_arr_add_val(out,yyjson_mut_val_mut_copy(message,record));
    }
    yyjson_mut_obj_add_val(message,yyjson_mut_doc_get_root(message),"operations",out);desired_snapshot(c,message);int ret=count&&ipc_send(c->control_fd,message)?-errno:0;
    if(!ret){for(unsigned i=0;i<count;i++)seen[slots[i]].version=versions[i];*cursor=next;ret=next?0:1;}
    yyjson_mut_doc_free(page);yyjson_mut_doc_free(message);return ret;
}
static int child_run(int fds[5],const char *instance,int is_thread,pid_t parent) {
    Child c={.mutex=PTHREAD_MUTEX_INITIALIZER,.media_mutex=PTHREAD_MUTEX_INITIALIZER,.completion_wake=-1,
        .control_fd=fds[0],.media_fd=fds[1],.completion_fd=fds[2],.is_thread=is_thread};
    strcpy(c.instance,instance);
    int result=1,jmutex=0,pmutex=0,network=0,curl=0,media_started=0;const char *failure_stage="rendition allocation";
    pthread_t media;yyjson_doc *boot=NULL;yyjson_mut_doc *operation_reply=NULL;
    c.completion_wake=eventfd(0,EFD_NONBLOCK|EFD_CLOEXEC);if(c.completion_wake<0)goto cleanup;
    c.job=calloc(1,sizeof(Job));c.parent=calloc(1,sizeof(Job));
    if(c.job)c.job->wake_fd=-1;
    if(!c.job||!c.parent)goto cleanup;
    Job *j=c.job;c.parent->wake_fd=-1;j->parent=c.parent;j->remote_rendition=2;
    if(pthread_mutex_init(&j->mutex,NULL))goto cleanup;
    jmutex=1;
    if(pthread_mutex_init(&c.parent->mutex,NULL))goto cleanup;
    pmutex=1;
    j->wake_fd=eventfd(0,EFD_NONBLOCK|EFD_CLOEXEC);if(j->wake_fd<0)goto cleanup;
    int64_t deadline=now_us()+10000000;
    while(!boot&&now_us()<deadline){int r=ipc_receive(c.control_fd,&boot);if(r<0)goto cleanup;if(!r){struct pollfd fd={.fd=c.control_fd,.events=POLLIN};poll(&fd,1,20);}}
    if(!boot||!ipc_kind(boot,"rendition_init"))goto cleanup;
    yyjson_val *v=yyjson_doc_get_root(boot);const char *events=yyjson_get_str(yyjson_obj_get(v,"event_prefix"));if(!events||strlen(events)>4000)goto cleanup;
    LoggerOptions options={.queue_capacity=512,.rotate_seconds=60,.rotate_bytes=4*1024*1024};strcpy(options.role,"rendition");strcpy(options.instance_id,naki_process_id());strcpy(options.event_prefix,events);
    failure_stage="rendition logger";j->logger=logger_start(&options);if(!j->logger)goto cleanup;
    /* Initialize before bootstrap can start a watermark resource loader.
     * The thread adapter shares its session's initialized process globals. */
    if(!is_thread){if(avformat_network_init()<0)goto cleanup;network=1;if(curl_global_init(CURL_GLOBAL_DEFAULT)!=CURLE_OK)goto cleanup;curl=1;}
    failure_stage="rendition bootstrap";if(bootstrap(&c,v))goto cleanup;
    if(j->config.frame_log||j->config.network_trace||j->config.codec_trace){j->trace=logger_trace_open(j->logger,j->log_path);if(!j->trace)goto cleanup;}
    failure_stage="video pool view";if(naki_pool_view_open(&c.video,fds[3],yyjson_get_uint(yyjson_obj_get(v,"video_pool"))))goto cleanup;
    failure_stage="audio pool view";if(naki_pool_view_open(&c.audio,fds[4],yyjson_get_uint(yyjson_obj_get(v,"audio_pool"))))goto cleanup;
    yyjson_doc_free(boot);boot=NULL;
    close(fds[3]);fds[3]=-1;close(fds[4]);fds[4]=-1;
    failure_stage="rendition media thread";if(pthread_create(&media,NULL,media_main,&c))goto cleanup;
    media_started=1;
    HistoryVersion history[256]={0};unsigned history_cursor=0;int final_history=0;int64_t next_history=0;
    int announced=0,ack=0;int64_t next=0,finished_deadline=0;
    for(;;) {
        if(!c.is_thread&&getppid()!=parent){request_stop(j);break;}
        uint64_t wake;while(read(c.completion_wake,&wake,sizeof(wake))>0){}
        pthread_mutex_lock(&c.mutex);
        while(c.count){int r=naki_rendition_send(c.completion_fd,&c.completions[c.head]);if(r==-EAGAIN||r==-EWOULDBLOCK)break;if(r<0){atomic_store(&j->lifecycle.cancel,1);break;}c.head=(c.head+1)%COMPLETIONS;c.count--;}
        pthread_mutex_unlock(&c.mutex);
        if(atomic_load(&c.prepared)&&!announced){yyjson_mut_doc *d=ipc_message("rendition_ready");json_int(d,"error",c.prepare_error);json_int(d,"pid",getpid());json_str(d,"rendition_instance",c.instance);if(!ipc_send(c.control_fd,d))announced=1;yyjson_mut_doc_free(d);}
        int done=atomic_load(&c.media_done);
        if(done&&!finished_deadline)finished_deadline=now_us()+3000000;
        if(announced&&((done&&!final_history)||(!done&&now_us()>=next_history))){int hr=send_history(&c,&history_cursor,history);if(hr==1){if(done)final_history=1;next_history=now_us()+100000;}else if(hr<0&&hr!=-EAGAIN&&hr!=-EWOULDBLOCK){request_stop(j);ack=1;}}
        if(((done&&final_history)||(!done&&now_us()>=next))&&announced){yyjson_mut_doc *d=ipc_message(done?"rendition_finished":"rendition_status");yyjson_mut_doc *snapshot=job_snapshot(j);if(snapshot){naki_watermark_status(j->watermark,snapshot);yyjson_mut_obj_add_val(d,yyjson_mut_doc_get_root(d),"snapshot",yyjson_mut_val_mut_copy(d,yyjson_mut_doc_get_root(snapshot)));yyjson_mut_doc_free(snapshot);}json_str(d,"rendition_instance",c.instance);if(ipc_send(c.control_fd,d)&&errno!=EAGAIN&&errno!=EWOULDBLOCK){request_stop(j);ack=1;}yyjson_mut_doc_free(d);next=now_us()+50000;}
        if(done&&(ack||now_us()>=finished_deadline))break;
        if(operation_reply&&!ipc_send(c.control_fd,operation_reply)){yyjson_mut_doc_free(operation_reply);operation_reply=NULL;}
        yyjson_doc *command=NULL;int r=operation_reply?0:ipc_receive(c.control_fd,&command);
        if(r<0){request_stop(j);ack=1;}else if(r>0){
            yyjson_val *root=yyjson_doc_get_root(command);const char *instance=yyjson_get_str(yyjson_obj_get(root,"rendition_instance"));
            if(!instance||strcmp(instance,c.instance)){request_stop(j);ack=1;}
            else if(ipc_kind(command,"rendition_stop"))request_stop(j);
            else if(ipc_kind(command,"rendition_ack"))ack=1;
            else if(ipc_kind(command,"rendition_source_rate"))atomic_store(&j->remote_source_rate,yyjson_get_sint(yyjson_obj_get(root,"bps")));
            else if(ipc_kind(command,"rendition_update")&&!operation_reply){NakiUpdateRequest update;char error[256]={0};yyjson_mut_doc *record=NULL;const char *origin=yyjson_get_str(yyjson_obj_get(root,"origin_request_id"));int result=naki_update_parse(yyjson_obj_get(root,"update"),j->id,&update,error,sizeof(error));if(!result){if(update.action){pthread_mutex_lock(&c.media_mutex);result=c.accepts_updates?naki_destinations_submit(c.media,&update,origin,&record,error,sizeof(error)):-ESHUTDOWN;pthread_mutex_unlock(&c.media_mutex);}else result=naki_watermark_submit(j->watermark,&update,origin,&record,error,sizeof(error));}operation_reply=ipc_message("rendition_operation");desired_snapshot(&c,operation_reply);json_str(operation_reply,"rendition_instance",c.instance);json_int(operation_reply,"error",result);json_str(operation_reply,"operation_id",yyjson_get_str(yyjson_obj_get(root,"operation_id")));if(record){yyjson_mut_obj_add_val(operation_reply,yyjson_mut_doc_get_root(operation_reply),"operation",yyjson_mut_val_mut_copy(operation_reply,yyjson_mut_doc_get_root(record)));yyjson_mut_doc_free(record);}}
            yyjson_doc_free(command);
        }
        naki_policy_tick(j,now_us());
        struct pollfd fds[]={{.fd=c.control_fd,.events=POLLIN},{.fd=c.completion_wake,.events=POLLIN}};poll(fds,2,5);
    }
    result=0;
cleanup:
    if(result&&!media_started){yyjson_mut_doc *failed=ipc_message("rendition_ready");if(failed){json_str(failed,"rendition_instance",c.instance);json_int(failed,"error",-EINVAL);json_str(failed,"error_reason",*c.failure_reason?c.failure_reason:failure_stage);(void)ipc_send(c.control_fd,failed);yyjson_mut_doc_free(failed);}}
    /* All codec/filter/logger holders and read-only views are gone before a
     * local backend publishes completion. A hung thread stays quarantined. */
    if(media_started){request_stop(c.job);pthread_join(media,NULL);}
    if(boot)yyjson_doc_free(boot);
    if(operation_reply)yyjson_mut_doc_free(operation_reply);
    if(c.job){
        naki_watermark_close(&c.job->watermark);naki_operations_close(&c.job->operations);
        for(unsigned i=0;i<2;i++)if(c.job->progress.first_chain[i])yyjson_mut_doc_free(c.job->progress.first_chain[i]);
        if(c.job->destination_snapshot)yyjson_mut_doc_free(c.job->destination_snapshot);
        if(c.job->logger){logger_trace_release(c.job->logger,c.job->trace);logger_stop(c.job->logger);}
        if(c.job->wake_fd>=0)close(c.job->wake_fd);
        if(jmutex)pthread_mutex_destroy(&c.job->mutex);
    }
    naki_pool_view_close(&c.video);naki_pool_view_close(&c.audio);
    if(c.completion_wake>=0)close(c.completion_wake);
    if(pmutex)pthread_mutex_destroy(&c.parent->mutex);
    pthread_mutex_destroy(&c.mutex);pthread_mutex_destroy(&c.media_mutex);free(c.parent);free(c.job);
    for(unsigned i=0;i<5;i++)if(fds[i]>=0)close(fds[i]);
    if(curl)curl_global_cleanup();
    if(network)avformat_network_deinit();
    return result;
}
typedef struct ThreadArgs {int fds[5];char instance[33];atomic_int *done;} ThreadArgs;
static void *thread_run(void *opaque){ThreadArgs *a=opaque;(void)child_run(a->fds,a->instance,1,0);atomic_store(a->done,1);free(a);return NULL;}
int naki_rendition_thread_start(int fds[5],const char *instance,pthread_t *thread,atomic_int *done){
    ThreadArgs *a=malloc(sizeof(*a));if(!a)return -ENOMEM;
    memcpy(a->fds,fds,sizeof(a->fds));strcpy(a->instance,instance);a->done=done;atomic_store(done,0);
    int ret=pthread_create(thread,NULL,thread_run,a);if(ret){free(a);return -ret;}return 0;
}
int naki_rendition_child_main(int argc,char **argv) {
    if(argc!=4||naki_process_init())return 2;
    pid_t parent=(pid_t)strtol(argv[2],NULL,10);if(parent<2||prctl(PR_SET_PDEATHSIG,SIGKILL)||getppid()!=parent)return 2;
    /* CLOEXEC before collector creation; the collector receives no pool FD. */
    for(int fd=3;fd<=7;fd++)if(fcntl(fd,F_SETFD,FD_CLOEXEC)<0)return 2;
    if(strlen(argv[3])!=32)return 2;
    crash_set_instance(naki_process_id());CrashOptions crash;if(crash_options_env(&crash)||crash_client_start(&crash,"rendition"))return 1;
    int guard=naki_rendition_guard();if(guard<0){fprintf(stderr,"rendition holder guard: %d\n",guard);return 1;}
    signal(SIGPIPE,SIG_IGN);av_log_set_level(AV_LOG_QUIET);
    int fds[5]={3,4,5,6,7};return child_run(fds,argv[3],0,parent);
}
