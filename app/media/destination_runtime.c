#define _GNU_SOURCE
#include "destination_runtime.h"
#include "worker/job_remote.h"
#include "pipeline_internal.h"
#include "control/identity.h"
#include <libavutil/opt.h>
#define DESTINATIONS 4U
#define PACKETS 16U
/* Four writers: 16 queued + one executing + four live-mux references each.
 * 84 compressed references + 32 raw credits + transient reserve < 128 logical
 * inflight frames. Reuse requires zerolatency AVC without lookahead/B frames. */
#define DEST_BYTES (8U*1024*1024)
typedef struct Delivery {AVPacket *packet;InputItem audio;uint64_t bytes;int64_t ingress;} Delivery;
typedef struct Destination {
    struct NakiDestinations *owner;
    char id[33];Pipeline *mux;Job *private_job;
    pthread_t thread;atomic_int ready,done,stop,failed;
    pthread_mutex_t mutex;
    Delivery queue[PACKETS];unsigned head,count;uint64_t bytes;
    int started,active,removed;atomic_int exit_error;
    uint64_t generation,written,audio_written,bytes_written;
    int64_t first_pts_us,first_written_us,last_written_us,attach_deadline_us,last_video_pts,last_video_dts;int have_video_clock;
    Metric latency;
    NakiOperationHandle add_operation,remove_operation;
    NakiFrameIdentity first_identity;
} Destination;
struct NakiDestinations {
    Pipeline *encoder;
    pthread_mutex_t mutex;
    Destination slots[DESTINATIONS];
    int activated,closing;
    uint64_t next_generation;
    struct {yyjson_doc *request;NakiOperationHandle operation;} commands[8];unsigned head,count;
    NakiOperationHandle rap_operation;
    uint64_t rap_frame;
};
/* Complete NAL headers, never AV_PKT_FLAG_KEY or a generic I classification.
 * v1 reuse is AVC with immutable SPS/PPS from encoder extradata. HEVC and
 * live-frame-intra require an independent rendition until a RAP contract exists. */
static unsigned annex_types(const uint8_t *data,size_t size) {
    unsigned flags=0;
    for(size_t i=0;i+3<size;i++)if(!data[i]&&!data[i+1]&&data[i+2]==1){unsigned type=data[i+3]&31;if(type<32)flags|=1U<<type;i+=2;}
    return flags;
}
static unsigned nal_types(const uint8_t *data,size_t size,int length_size){
    if(!length_size)return annex_types(data,size);
    size_t at=0;unsigned flags=0;
    while(at<size){if(size-at<(unsigned)length_size)return 0;uint32_t n=0;for(int i=0;i<length_size;i++)n=n*256+data[at++];if(!n||n>size-at)return 0;unsigned type=data[at]&31;flags|=1U<<type;at+=n;}
    return flags;
}
static int parameters(const NakiEncodedDescription *encoded){
    NakiEncodedInfo info=naki_encoded_info(encoded);const uint8_t *x=info.extradata;int size=info.extradata_size;
    if(!x||size<4)return 0;
    if(x[0]!=1)return (annex_types(x,size)&((1U<<7)|(1U<<8)))==((1U<<7)|(1U<<8));
    if(size<7)return 0;
    size_t at=6;unsigned sps=x[5]&31,pps=0;
    if(!sps)return 0;
    for(unsigned i=0;i<sps;i++){if(at+2>(size_t)size)return 0;unsigned n=x[at]*256+x[at+1];at+=2;if(!n||n>(size_t)size-at||(x[at]&31)!=7)return 0;at+=n;}
    if(at>=(size_t)size)return 0;
    pps=x[at++];if(!pps)return 0;
    for(unsigned i=0;i<pps;i++){if(at+2>(size_t)size)return 0;unsigned n=x[at]*256+x[at+1];at+=2;if(!n||n>(size_t)size-at||(x[at]&31)!=8)return 0;at+=n;}
    return 1;
}
int naki_destinations_validate(const Config *cfg,const NakiUpdateRequest *u,char *error,size_t cap){
    if(u->action==NAKI_DESTINATION_REMOVE)return 0;
    if(strcmp(cfg->encoder,"libx264")||cfg->policy.intra||cfg->constrained_crf){snprintf(error,cap,"reuse_requires_libx264_without_live_frame_intra_or_constrained_crf; add an independent rendition");return -ENOTSUP;}
    if(u->action==NAKI_RANDOM_ACCESS)return 0;
    if(u->action!=NAKI_DESTINATION_ADD)return -EINVAL;
    if(!yyjson_is_obj(u->spec)||yyjson_obj_size(u->spec)!=2){snprintf(error,cap,"destination spec requires exactly output and output_format");return -EINVAL;}
    yyjson_val *url=yyjson_obj_get(u->spec,"output"),*format=yyjson_obj_get(u->spec,"output_format");
    if(!yyjson_is_str(url)||!yyjson_get_len(url)||yyjson_get_len(url)>=sizeof(cfg->output)||strlen(yyjson_get_str(url))!=yyjson_get_len(url)||
       !yyjson_is_str(format)||(strcmp(yyjson_get_str(format),"mp4")&&strcmp(yyjson_get_str(format),"mpegts")&&strcmp(yyjson_get_str(format),"flv"))){snprintf(error,cap,"invalid destination output or format");return -EINVAL;}
    if(!strncmp(yyjson_get_str(url),"srt://",6)){snprintf(error,cap,"process_holder_v1_sendmsg_unavailable_for_SRT; use independent thread/legacy job");return -ENOTSUP;}
    return naki_output_validate(yyjson_get_str(url),yyjson_get_str(format),error,cap)?-EINVAL:0;
}
static void delivery_free(Delivery *x){av_packet_free(&x->packet);pipeline_input_free(&x->audio);memset(x,0,sizeof(*x));}
static void failed(Destination *d,int error){
    atomic_store(&d->failed,1);atomic_store(&d->stop,1);atomic_store(&d->exit_error,error);
    pipeline_mux_cancel(d->mux);
}
static void *writer(void *opaque){
    Destination *d=opaque;Pipeline *p=d->mux;(void)crash_thread_init();int ret=d->private_job?pipeline_open_mux(p):0;
    if(ret<0){failed(d,ret);atomic_store(&d->ready,-1);}else atomic_store(&d->ready,1);
    while(ret>=0){
        Delivery item={0};pthread_mutex_lock(&d->mutex);
        if(d->count){item=d->queue[d->head];memset(&d->queue[d->head],0,sizeof(item));d->head=(d->head+1)%PACKETS;d->count--;d->bytes-=item.bytes;}
        int stop=atomic_load(&d->stop);pthread_mutex_unlock(&d->mutex);
        if(atomic_load(&d->failed)){delivery_free(&item);break;}
        if(item.packet||item.audio.packet){
            if(now_us()-item.ingress>10000000){delivery_free(&item);ret=AVERROR(ETIMEDOUT);break;}
            if(item.packet){av_packet_rescale_ts(item.packet,d->owner->encoder->video.time_base,p->video.time_base);item.packet->stream_index=p->video.index;
                ret=pipeline_validate_output_timestamps(p,item.packet,p->video.time_base);
                /* Reuse v1 is libx264 with no output B frames. Rounding into a
                 * different mux time base must not collapse its display clock. */
                if(ret>=0&&naki_job_config(p->job)->policy.timestamps==NAKI_TS_SOURCE_ALL&&d->have_video_clock&&(item.packet->pts<=d->last_video_pts||item.packet->dts<=d->last_video_dts))ret=AVERROR_INVALIDDATA;
                if(ret>=0){d->last_video_pts=item.packet->pts;d->last_video_dts=item.packet->dts;d->have_video_clock=1;ret=pipeline_submit_mux(p,item.packet);}}
            else ret=pipeline_handle_audio(p,&item.audio);
            delivery_free(&item);
        }else if(stop)break;
        else poll(NULL,0,2);
        if(ret>=0&&pipeline_mux_live(p))ret=pipeline_flush_deferred_audio(p);
        if(ret>=0&&pipeline_mux_live(p))ret=pipeline_drain_mux(p,0);
    }
    if(ret>=0&&!atomic_load(&d->failed)){ret=pipeline_flush_deferred_audio(p);if(ret>=0)ret=pipeline_drain_mux(p,1);}
    if(ret<0)failed(d,ret);
    ret=pipeline_finalize_output(p,ret,atomic_load(&d->failed));
    if(ret<0&&!atomic_load(&d->failed))failed(d,ret);
    uint64_t final_bytes=naki_job_bytes(p->job);
    pthread_mutex_lock(&d->owner->mutex);d->bytes_written=final_bytes;pthread_mutex_unlock(&d->owner->mutex);
    /* Cancellation can still arrive until join. Retire payload references now,
     * but keep the immutable mux pointer/atomic interrupt storage alive. */
    pipeline_mux_retire_audio(p);
    pthread_mutex_lock(&d->mutex);while(d->count){delivery_free(&d->queue[d->head]);d->head=(d->head+1)%PACKETS;d->count--;}d->bytes=0;pthread_mutex_unlock(&d->mutex);
    atomic_store(&d->done,1);return NULL;
}
static Pipeline *mux_new(NakiDestinations *m,Destination *d,Job *job){
    Pipeline *p=calloc(1,sizeof(*p));if(!p)return NULL;Pipeline *source=m->encoder;
    *p=(Pipeline){.job=job,.encoded=naki_encoded_ref(source->encoded),.input_tb=source->input_tb,.video_rate=source->video_rate,.output_rate=source->output_rate,
        .first_encoder_pts=source->first_encoder_pts,.stage="destination",.io_timeout_us=1000000,
        .destination_owner=d,.destination_copy=!!d->private_job};
    /* Publish output interrupt storage before pthread_create; failed() may run
     * concurrently with output open. The writer never replaces this pointer. */
    if(d->private_job&&pipeline_mux_prepare(p)<0){naki_encoded_unref(&p->encoded);free(p);return NULL;}
    return p;
}
int naki_destinations_open(Pipeline *p){
    NakiDestinations *m=calloc(1,sizeof(*m));if(!m)return -ENOMEM;m->encoder=p;
    if(pthread_mutex_init(&m->mutex,NULL)){free(m);return -ENOMEM;}
    for(unsigned i=0;i<DESTINATIONS;i++){m->slots[i].owner=m;if(pthread_mutex_init(&m->slots[i].mutex,NULL)){for(unsigned k=0;k<i;k++)pthread_mutex_destroy(&m->slots[k].mutex);pthread_mutex_destroy(&m->mutex);free(m);return -ENOMEM;}}
    p->destinations=m;return 0;
}
static int activate(NakiDestinations *m){
    if(m->activated)return 0;
    Pipeline *source=m->encoder;Destination *d=&m->slots[0];strcpy(d->id,"primary");
    d->generation=++m->next_generation;d->active=1;d->mux=mux_new(m,d,source->job);if(!d->mux)return -ENOMEM;
    NakiDestinationBaseline baseline;naki_job_destination_baseline(source->job,&baseline);
    d->written=baseline.packets;d->audio_written=baseline.audio;d->first_written_us=baseline.first_us;d->last_written_us=baseline.last_us;d->latency=baseline.latency;
    d->bytes_written=pipeline_mux_bytes(source);
    Pipeline *p=d->mux;
    int ret=pipeline_mux_move(source,p);
    if(ret>=0){
        atomic_store(&d->ready,1); /* Preserve the existing primary's next packet. */
        ret=pthread_create(&d->thread,NULL,writer,d);if(ret)ret=-ret;
    }
    if(ret<0){/* No writer exists: return the same object, including callbacks. */
        if(p->mux&&pipeline_mux_move(p,source)<0)abort();
        naki_encoded_unref(&p->encoded);free(p);d->mux=NULL;atomic_store(&d->ready,0);return ret;}
    d->started=1;
    m->activated=1;return 0;
}
int naki_destinations_submit(Pipeline *p,const NakiUpdateRequest *u,const char *origin,yyjson_mut_doc **result,char *error,size_t cap){
    NakiDestinations *m=p->destinations;if(!m)return -ENOTSUP;
    int ret=naki_destinations_validate(naki_job_config(p->job),u,error,cap);if(ret<0)return ret;
    if((u->action==NAKI_DESTINATION_ADD||u->action==NAKI_RANDOM_ACCESS)&&!parameters(p->encoded)){snprintf(error,cap,"immutable_SPS_PPS_missing; add an independent rendition");return -ENOTSUP;}
    NakiOperationIntent in={.kind=NAKI_OP_TOPOLOGY,.job_id=naki_job_id(p->job),.output_id=naki_job_output_id(p->job),.operation_id=u->operation_id,
        .fingerprint=u->fingerprint,.origin_request_id=origin,.component=u->action==NAKI_RANDOM_ACCESS?"rendition":"destination",
        .action=u->action==NAKI_DESTINATION_ADD?"add_destination":u->action==NAKI_DESTINATION_REMOVE?"remove_destination":"request_random_access",
        .destination_id=*u->destination_id?u->destination_id:NULL,.expected_revision=u->expected_revision,.revision=u->expected_revision+1};
    ret=naki_operations_replay(naki_job_operations(p->job),&in,result);if(ret!=-ENOENT)return ret;
    pthread_mutex_lock(&m->mutex);
    if(m->closing||m->count==8){pthread_mutex_unlock(&m->mutex);return -EAGAIN;}
    yyjson_mut_doc *doc=yyjson_mut_doc_new(NULL);yyjson_mut_val *root=yyjson_mut_obj(doc);yyjson_mut_doc_set_root(doc,root);
    yyjson_mut_obj_add_int(doc,root,"action",u->action);yyjson_mut_obj_add_strcpy(doc,root,"destination_id",u->destination_id);
    if(u->spec)yyjson_mut_obj_add_val(doc,root,"spec",yyjson_val_mut_copy(doc,u->spec));
    size_t bytes;char *text=yyjson_mut_write(doc,0,&bytes);yyjson_mut_doc_free(doc);yyjson_doc *copy=text?yyjson_read(text,bytes,0):NULL;free(text);
    if(!copy){pthread_mutex_unlock(&m->mutex);return -ENOMEM;}
    NakiOperationHandle operation;ret=naki_operations_reserve(naki_job_operations(p->job),&in,now_us(),&operation);
    if(ret==1){unsigned at=(m->head+m->count++)%8;m->commands[at].request=copy;m->commands[at].operation=operation;*result=naki_operations_get(naki_job_operations(p->job),operation);ret=0;}
    else yyjson_doc_free(copy);
    pthread_mutex_unlock(&m->mutex);return ret;
}
static void command_apply(NakiDestinations *m,yyjson_val *request,NakiOperationHandle operation){
    Pipeline *p=m->encoder;NakiOperations *ops=naki_job_operations(p->job);int action=yyjson_get_int(yyjson_obj_get(request,"action"));const char *id=yyjson_get_str(yyjson_obj_get(request,"destination_id"));
    naki_operations_transition(ops,operation,NAKI_OP_PREPARING,0,0,NULL,now_us());
    int ret=activate(m);if(ret<0)goto fail;
    if(action==NAKI_RANDOM_ACCESS){if(m->rap_operation){ret=-EBUSY;goto fail;}m->rap_operation=operation;m->rap_frame=0;naki_operations_transition(ops,operation,NAKI_OP_PREPARED,0,0,NULL,now_us());return;}
    Destination *d=NULL;unsigned free_slot=DESTINATIONS;
    for(unsigned i=0;i<DESTINATIONS;i++){if(!strcmp(m->slots[i].id,id)&&*id)d=&m->slots[i];if(!*m->slots[i].id&&free_slot==DESTINATIONS)free_slot=i;}
    if(action==NAKI_DESTINATION_REMOVE){
        if(!d||d->removed){ret=-ENOENT;goto fail;}d->active=0;d->removed=1;d->remove_operation=operation;atomic_store(&d->stop,1);return;
    }
    if(d){ret=-EEXIST;goto fail;}if(free_slot==DESTINATIONS){ret=-ENOSPC;goto fail;}
    yyjson_val *spec=yyjson_obj_get(request,"spec");const char *output=yyjson_get_str(yyjson_obj_get(spec,"output")),*format=yyjson_get_str(yyjson_obj_get(spec,"output_format"));
    if(!strcmp(output,naki_job_config(p->job)->output)){ret=-EEXIST;goto fail;}
    for(unsigned i=0;i<DESTINATIONS;i++)if(m->slots[i].mux&&!strcmp(output,naki_job_config(m->slots[i].mux->job)->output)){ret=-EEXIST;goto fail;}
    d=&m->slots[free_slot];d->private_job=naki_job_destination_new(p->job,output,format);if(!d->private_job){ret=-ENOMEM;goto fail;}
    Job *j=d->private_job;
    d->mux=mux_new(m,d,j);if(!d->mux){naki_job_destination_free(&d->private_job);ret=-ENOMEM;goto fail;}
    strcpy(d->id,id);d->generation=++m->next_generation;d->add_operation=operation;d->first_pts_us=INT64_MIN;d->attach_deadline_us=now_us()+10000000;
    ret=pthread_create(&d->thread,NULL,writer,d);if(ret){ret=-ret;pipeline_mux_release(d->mux);naki_encoded_unref(&d->mux->encoded);free(d->mux);d->mux=NULL;naki_job_destination_free(&d->private_job);d->id[0]=0;goto fail;}
    d->started=1;return;
fail:naki_operations_transition(ops,operation,NAKI_OP_FAILED,1,ret,"destination transition rejected",now_us());
}
static void status(NakiDestinations *m){
    yyjson_mut_doc *doc=yyjson_mut_doc_new(NULL);if(!doc)return;yyjson_mut_val *list=yyjson_mut_arr(doc);yyjson_mut_doc_set_root(doc,list);
    if(!m->activated){yyjson_mut_val *v=yyjson_mut_obj(doc);yyjson_mut_obj_add_str(doc,v,"destination_id","primary");yyjson_mut_obj_add_str(doc,v,"state","active");yyjson_mut_arr_add_val(list,v);}
    else for(unsigned i=0;i<DESTINATIONS;i++){Destination *d=&m->slots[i];if(!*d->id)continue;yyjson_mut_val *v=yyjson_mut_obj(doc);
        yyjson_mut_obj_add_strcpy(doc,v,"destination_id",d->id);yyjson_mut_obj_add_uint(doc,v,"destination_generation",d->generation);
        yyjson_mut_obj_add_str(doc,v,"state",atomic_load(&d->failed)?"failed":d->removed?"removed":atomic_load(&d->done)?"completed":d->active?"active":atomic_load(&d->ready)>0?"waiting_for_rap":"preparing");
        yyjson_mut_obj_add_real(doc,v,"ingress_to_mux_mean_ms",d->latency.count?d->latency.sum_ms/d->latency.count:0);yyjson_mut_obj_add_real(doc,v,"ingress_to_mux_max_ms",d->latency.max_ms);
        yyjson_mut_obj_add_uint(doc,v,"packets_written",d->written);yyjson_mut_obj_add_uint(doc,v,"audio_packets_written",d->audio_written);yyjson_mut_obj_add_uint(doc,v,"bytes_written",d->bytes_written);yyjson_mut_obj_add_sint(doc,v,"first_written_us",d->first_written_us);yyjson_mut_obj_add_sint(doc,v,"last_written_us",d->last_written_us);yyjson_mut_obj_add_int(doc,v,"error_code",atomic_load(&d->exit_error));
        pthread_mutex_lock(&d->mutex);yyjson_mut_obj_add_uint(doc,v,"queue_packets",d->count);yyjson_mut_obj_add_uint(doc,v,"queue_bytes",d->bytes);pthread_mutex_unlock(&d->mutex);
        yyjson_mut_arr_add_val(list,v);
    }
    naki_job_destinations_publish(m->encoder->job,doc);
}
int naki_destinations_poll(Pipeline *p){
    NakiDestinations *m=p->destinations;if(!m)return 0;pthread_mutex_lock(&m->mutex);
    for(unsigned i=0;i<8&&m->count;i++){unsigned at=m->head;command_apply(m,yyjson_doc_get_root(m->commands[at].request),m->commands[at].operation);yyjson_doc_free(m->commands[at].request);m->commands[at].request=NULL;m->head=(at+1)%8;m->count--;}
    for(unsigned i=0;i<DESTINATIONS;i++){Destination *d=&m->slots[i];
        if(d->add_operation&&!d->active&&now_us()>=d->attach_deadline_us)failed(d,-ETIMEDOUT);
        if(d->add_operation&&atomic_load(&d->failed)&&!d->active){naki_operations_transition(naki_job_operations(p->job),d->add_operation,NAKI_OP_FAILED,1,atomic_load(&d->exit_error),"destination failed before RAP admission",now_us());d->add_operation=0;}
        if(d->add_operation&&atomic_load(&d->done)){naki_operations_holder_exited(naki_job_operations(p->job),d->add_operation,now_us());d->add_operation=0;}
        if(d->add_operation&&atomic_load(&d->ready)<0){naki_operations_transition(naki_job_operations(p->job),d->add_operation,NAKI_OP_FAILED,1,atomic_load(&d->exit_error),"destination open failed",now_us());d->add_operation=0;}
        else if(d->add_operation&&atomic_load(&d->ready)>0&&!d->active){int ret=naki_operations_transition(naki_job_operations(p->job),d->add_operation,NAKI_OP_PREPARED,0,0,NULL,now_us());if(ret<0)failed(d,ret);}
        if(d->remove_operation&&atomic_load(&d->done)){naki_operations_transition(naki_job_operations(p->job),d->remove_operation,NAKI_OP_PREPARED,0,0,NULL,now_us());naki_operations_transition(naki_job_operations(p->job),d->remove_operation,NAKI_OP_APPLYING,1,0,NULL,now_us());NakiFrameIdentity empty={0};naki_operations_applied(naki_job_operations(p->job),d->remove_operation,&empty,0,now_us());naki_operations_retire(naki_job_operations(p->job),d->remove_operation,now_us());d->remove_operation=0;
            /* Only a joined writer releases its ID/budget for a new generation. */
            pthread_join(d->thread,NULL);pipeline_mux_release(d->mux);naki_encoded_unref(&d->mux->encoded);naki_job_destination_free(&d->private_job);free(d->mux);
            pthread_mutex_destroy(&d->mutex);memset(d,0,sizeof(*d));d->owner=m;pthread_mutex_init(&d->mutex,NULL);
        }
    }
    status(m);int active=m->activated;pthread_mutex_unlock(&m->mutex);
    if(!active){int ret=pipeline_flush_deferred_audio(p);if(ret>=0)ret=pipeline_drain_mux(p,0);return ret;}return 0;
}
static int enqueue(Destination *d,Delivery *item){
    pthread_mutex_lock(&d->mutex);int ret=0;
    if(atomic_load(&d->failed)||atomic_load(&d->stop))ret=-EPIPE;
    else if(d->count==PACKETS||item->bytes>DEST_BYTES-d->bytes)ret=-ENOBUFS;
    else {d->queue[(d->head+d->count++)%PACKETS]=*item;d->bytes+=item->bytes;memset(item,0,sizeof(*item));}
    pthread_mutex_unlock(&d->mutex);if(ret)failed(d,ret);return ret;
}
int naki_destinations_packet(Pipeline *p,AVPacket *packet){
    NakiDestinations *m=p->destinations;if(!m->activated)return pipeline_submit_mux(p,packet);
    MuxTrace *meta=(MuxTrace *)packet->opaque_ref->data;meta->fanout_job=p->job;
    NakiEncodedInfo info=naki_encoded_info(p->encoded);
    int length_size=info.extradata_size>4&&info.extradata[0]==1?(info.extradata[4]&3)+1:0;
    int rap=!!(nal_types(packet->data,packet->size,length_size)&(1U<<5));
    NakiFrameIdentity identity=pipeline_frame_identity(p,&meta->stamp,packet->pts,p->video.time_base);
    pthread_mutex_lock(&m->mutex);
    if(m->rap_operation&&m->rap_frame==(uint64_t)meta->stamp.frame_id){
        if(!rap){naki_operations_transition(naki_job_operations(p->job),m->rap_operation,NAKI_OP_FAILED,1,-EPROTO,"encoder did not return a verified IDR",now_us());m->rap_operation=0;}
        else {naki_operations_applied(naki_job_operations(p->job),m->rap_operation,&identity,1,now_us());}
    }
    for(unsigned i=0;i<DESTINATIONS;i++){Destination *d=&m->slots[i];if(!d->started||d->removed||atomic_load(&d->failed)||atomic_load(&d->done)||atomic_load(&d->ready)<=0)continue;
        if(!d->active&&!rap)continue;
        Delivery x={.packet=av_packet_clone(packet),.bytes=packet->size,.ingress=meta->stamp.ingress_us};if(!x.packet){failed(d,-ENOMEM);continue;}
        if(!d->active){
            /* ready may have changed since the last owner poll. Commit the
             * prepared checkpoint before admitting the first IDR. */
            int ret=naki_operations_transition(naki_job_operations(p->job),d->add_operation,NAKI_OP_PREPARED,0,0,NULL,now_us());
            if(!ret)ret=naki_operations_transition(naki_job_operations(p->job),d->add_operation,NAKI_OP_APPLYING,1,0,NULL,now_us());
            if(!ret)ret=naki_operations_applied(naki_job_operations(p->job),d->add_operation,&identity,1,now_us());
            if(ret<0){failed(d,ret);delivery_free(&x);continue;}
            d->first_identity=identity;d->first_pts_us=meta->pts_us;d->active=1;}
        (void)enqueue(d,&x);delivery_free(&x);
    }
    pthread_mutex_unlock(&m->mutex);return 0;
}
int naki_destinations_audio(Pipeline *p,const InputItem *audio){
    NakiDestinations *m=p->destinations;if(!m->activated)return pipeline_handle_audio(p,audio);
    AVPacket *owned=av_packet_clone(audio->packet);if(!owned)return -ENOMEM;
    int ret=av_packet_make_writable(owned);if(ret<0){av_packet_free(&owned);return ret;}
    pthread_mutex_lock(&m->mutex);
    for(unsigned i=0;i<DESTINATIONS;i++){Destination *d=&m->slots[i];if(!d->active||d->removed||atomic_load(&d->failed)||atomic_load(&d->done)||!naki_job_config(d->mux->job)->audio_copy||!strcmp(naki_job_config(d->mux->job)->output_format,"mp4"))continue;
        int64_t pts=audio->packet->pts!=AV_NOPTS_VALUE?av_rescale_q(audio->packet->pts,audio->tb,AV_TIME_BASE_Q):INT64_MIN;
        if(d->private_job&&pts<d->first_pts_us)continue;
        Delivery x={.bytes=(uint64_t)owned->size+4096,.ingress=audio->demuxed_us};x.audio=*audio;x.audio.packet=av_packet_clone(owned);x.audio.params=avcodec_parameters_alloc();
        if(!x.audio.packet||!x.audio.params||avcodec_parameters_copy(x.audio.params,audio->params)<0){delivery_free(&x);failed(d,-ENOMEM);continue;}
        (void)enqueue(d,&x);delivery_free(&x);
    }
    pthread_mutex_unlock(&m->mutex);av_packet_free(&owned);return 0;
}
int naki_destinations_force(Pipeline *p,AVFrame *frame){
    NakiDestinations *m=p->destinations;if(!m->activated)return 0;pthread_mutex_lock(&m->mutex);int ret=0;
    if(m->rap_operation&&!m->rap_frame){
        ret=pipeline_video_force_idr(p);
        if(ret>=0){frame->pict_type=AV_PICTURE_TYPE_I;m->rap_frame=((Stamp *)frame->opaque_ref->data)->frame_id;naki_operations_transition(naki_job_operations(p->job),m->rap_operation,NAKI_OP_APPLYING,1,0,NULL,now_us());}
    }
    pthread_mutex_unlock(&m->mutex);return ret;
}
void naki_destinations_audio_written(Pipeline *p){
    Destination *d=p->destination_owner;if(!d)return;pthread_mutex_lock(&d->owner->mutex);d->audio_written++;d->bytes_written=pipeline_mux_bytes(p);pthread_mutex_unlock(&d->owner->mutex);
}
int naki_destinations_written(Pipeline *p,const AVPacket *packet,int64_t when){
    Destination *d=p->destination_owner;if(!d)return 0;MuxTrace *meta=(MuxTrace *)packet->opaque_ref->data;NakiDestinations *m=d->owner;
    NakiFrameIdentity identity=pipeline_frame_identity(p,&meta->stamp,packet->pts,p->video.time_base);
    if(meta->stamp.delivery_lease){int ret=naki_watermark_written(meta->stamp.delivery_owner,meta->stamp.delivery_lease,&identity,when);if(ret<0)return ret;}
    pthread_mutex_lock(&m->mutex);d->written++;metric_add(&d->latency,(when-meta->stamp.ingress_us)/1000.0,200);d->bytes_written=pipeline_mux_bytes(p);d->last_written_us=when;if(!d->first_written_us)d->first_written_us=when;
    if(d->add_operation&&d->first_identity.frame_id==(uint64_t)meta->stamp.frame_id){
        int ret=naki_operations_written(naki_job_operations(m->encoder->job),d->add_operation,&identity,when);
        if(!ret)ret=naki_operations_frame_release(naki_job_operations(m->encoder->job),d->add_operation,when);
        if(ret<0){pthread_mutex_unlock(&m->mutex);return ret;}
        d->add_operation=0;}
    if(m->rap_operation&&m->rap_frame==(uint64_t)meta->stamp.frame_id){naki_operations_written(naki_job_operations(m->encoder->job),m->rap_operation,&identity,when);naki_operations_frame_release(naki_job_operations(m->encoder->job),m->rap_operation,when);m->rap_operation=0;m->rap_frame=0;}
    pthread_mutex_unlock(&m->mutex);
    naki_job_destination_written(m->encoder->job,when);
    yyjson_mut_doc *event=new_event("destination_frame",m->encoder->job);json_str(event,"destination_id",d->id);json_int(event,"destination_generation",d->generation);json_int(event,"frame_id",meta->stamp.frame_id);json_int(event,"pts_us",meta->pts_us);json_int(event,"mux_flush_done_monotonic_us",when);json_num(event,"ingress_to_mux_ms",(when-meta->stamp.ingress_us)/1000.0);logger_submit(naki_job_logger(m->encoder->job),naki_job_trace(m->encoder->job),event);return 0;
}
int naki_destinations_close(Pipeline *p,int stopped){
    NakiDestinations *m=p->destinations;if(!m)return 0;
    int inactive_error=0;if(!m->activated&&!stopped){inactive_error=pipeline_flush_deferred_audio(p);if(inactive_error>=0)inactive_error=pipeline_drain_mux(p,1);}
    pthread_mutex_lock(&m->mutex);m->closing=1;
    while(m->count){unsigned at=m->head;naki_operations_transition(naki_job_operations(p->job),m->commands[at].operation,NAKI_OP_CANCELLED,1,0,"rendition closing",now_us());yyjson_doc_free(m->commands[at].request);m->head=(at+1)%8;m->count--;}
    for(unsigned i=0;i<DESTINATIONS;i++){Destination *d=&m->slots[i];if(d->started){atomic_store(&d->stop,1);if(stopped)failed(d,AVERROR_EXIT);}}
    pthread_mutex_unlock(&m->mutex);int success=0,desired=0;
    for(unsigned i=0;i<DESTINATIONS;i++){Destination *d=&m->slots[i];if(d->started){desired++;pthread_join(d->thread,NULL);success+=!atomic_load(&d->failed);if(d->add_operation){if(!d->active)naki_operations_transition(naki_job_operations(p->job),d->add_operation,NAKI_OP_CANCELLED,1,0,"rendition closed before RAP admission",now_us());else naki_operations_holder_exited(naki_job_operations(p->job),d->add_operation,now_us());}
        if(d->remove_operation){NakiFrameIdentity empty={0};naki_operations_transition(naki_job_operations(p->job),d->remove_operation,NAKI_OP_PREPARED,0,0,NULL,now_us());naki_operations_transition(naki_job_operations(p->job),d->remove_operation,NAKI_OP_APPLYING,1,0,NULL,now_us());naki_operations_applied(naki_job_operations(p->job),d->remove_operation,&empty,0,now_us());naki_operations_retire(naki_job_operations(p->job),d->remove_operation,now_us());}}}
    if(m->rap_operation){if(!m->rap_frame)naki_operations_transition(naki_job_operations(p->job),m->rap_operation,NAKI_OP_CANCELLED,1,0,"rendition closed before RAP frame",now_us());else naki_operations_holder_exited(naki_job_operations(p->job),m->rap_operation,now_us());}
    pthread_mutex_lock(&m->mutex);status(m);pthread_mutex_unlock(&m->mutex);
    for(unsigned i=0;i<DESTINATIONS;i++){Destination *d=&m->slots[i];if(d->mux){pipeline_mux_release(d->mux);naki_encoded_unref(&d->mux->encoded);}naki_job_destination_free(&d->private_job);free(d->mux);pthread_mutex_destroy(&d->mutex);}
    int ret=m->activated&&desired&&!success&&!stopped?-EPIPE:inactive_error;pthread_mutex_destroy(&m->mutex);free(m);p->destinations=NULL;return ret;
}
