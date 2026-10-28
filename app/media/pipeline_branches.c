#include "pipeline_internal.h"
#include "branch_queue.h"
#define BRANCH_ADMISSION_GRACE_US 100000
typedef struct Branch {
    Pipeline pipeline;
    NakiBranchQueue queue;
    pthread_t thread;
    int initialized,started;
    const OutputConfig *spec;
} Branch;
struct NakiBranches {Branch slots[NAKI_OUTPUT_MAX];int count;};
static void media_phase(Pipeline *p,NakiMediaPhase phase){naki_job_media_phase(p->job,phase);}
static void branch_queue_stats(Branch *b) {
    pthread_mutex_lock(&b->queue.mutex);
    unsigned count=b->queue.count,high=b->queue.high_water;uint64_t bytes=b->queue.bytes,media=b->queue.media_us;
    unsigned video=b->queue.video_count,audio=b->queue.audio_count,video_high=b->queue.video_high,audio_high=b->queue.audio_high,reason=b->queue.rejection_reason;
    int64_t oldest=b->queue.count?b->queue.items[b->queue.head].ingress_us:0;
    uint64_t rejected[NAKI_BRANCH_REASONS];memcpy(rejected,b->queue.rejected,sizeof(rejected));
    pthread_mutex_unlock(&b->queue.mutex);Job *j=b->pipeline.job;
    NakiBranchQueueReport report={.bytes=bytes,.work_us=media,.oldest_us=oldest,.count=count,.high=high,
        .video=video,.audio=audio,.video_high=video_high,.audio_high=audio_high,.reason=reason};
    memcpy(report.rejected,rejected,sizeof(rejected));naki_job_branch_queue(j,&report);
}
static void fail_branch(Branch *b,int ret,const char *stage) {
    Job *j=b->pipeline.job;if(naki_job_done(j))return;
    naki_job_branch_fail(j,ret,stage);request_stop(j);naki_branch_queue_abort(&b->queue);branch_queue_stats(b);
}
int pipeline_branches_check(Pipeline *p,int finishing) {
    int active=0;int64_t now=now_us();
    for(int i=0;i<p->branches->count;i++) {
        Branch *b=&p->branches->slots[i];Job *j=b->pipeline.job;
        if(!b->started||naki_job_done(j))continue;
        if(naki_job_cancelled(p->job)) {request_stop(j);naki_branch_queue_abort(&b->queue);continue;}
        NakiBranchAges ages=naki_job_branch_ages(j,now);
        int64_t age=ages.retained_us,write=ages.write_us;int failed=ages.failed;
        if(!failed) {
            if(age>(int64_t)b->spec->max_lag_ms*1000)fail_branch(b,AVERROR(ETIMEDOUT),"branch retained-frame ingress deadline");
            else if(write>(int64_t)b->spec->io_timeout_ms*1000)fail_branch(b,AVERROR(ETIMEDOUT),"branch output I/O deadline");
            else if(naki_branch_queue_check(&b->queue,now)==AVERROR(ETIMEDOUT)) {
                pthread_mutex_lock(&b->queue.mutex);int pressure=b->queue.admission_deadline_us&&now>=b->queue.admission_deadline_us;unsigned reason=b->queue.rejection_reason;pthread_mutex_unlock(&b->queue.mutex);
                char stage[96];snprintf(stage,sizeof(stage),pressure?"branch bounded admission window (%s)":"branch queued ingress deadline (%s)",naki_branch_reason_name(reason));
                fail_branch(b,AVERROR(ETIMEDOUT),stage);
            }
        }
        if(!naki_job_cancelled(j))active++;
        branch_queue_stats(b);
    }
    return !finishing&&!active?AVERROR(EPIPE):0;
}

static void *branch_main(void *opaque) {
    (void)crash_thread_init();Branch *b=opaque;Pipeline *p=&b->pipeline;Job *j=p->job;
    NakiMediaJobInfo info={.job_id=naki_job_id(j),.request_id=naki_job_request_id(j),.config=naki_job_config(j),.request_us=naki_job_started_us(j),.output_id=naki_job_output_id(j)};
    p->stage="branch media hook initialization";int ret=naki_media_hooks_open(&info,&p->hooks);
    if(ret>0||(ret>=0&&p->hooks.api_version!=NAKI_MEDIA_HOOK_API))ret=AVERROR(EINVAL);
    if(ret>=0) {naki_job_running(j);}
    while(ret>=0&&!naki_job_cancelled(j)) {
        media_phase(p,NAKI_MEDIA_PHASE_WAITING_INPUT);
        NakiBranchItem item={0};ret=naki_branch_queue_pop(&b->queue,&item,now_us()+20000);branch_queue_stats(b);
        if(ret==AVERROR_EOF) {media_phase(p,NAKI_MEDIA_PHASE_DRAINING);ret=pipeline_drain_output(p);break;}
        if(ret==AVERROR(ETIMEDOUT)||ret==AVERROR(EAGAIN)) {ret=pipeline_mux_live(p)?pipeline_drain_mux(p,0):0;continue;}
        if(ret<0)break;
        naki_job_input_received(j,0,1,now_us());
        if(item.frame)ret=pipeline_process_frame(p,item.frame);
        else if(naki_job_config(j)->audio_copy&&strcmp(naki_job_config(j)->output_format,"mp4")) {
            InputItem audio={.packet=item.packet,.params=item.parameters,.tb=item.time_base,.demuxed_us=item.ingress_us};
            ret=pipeline_handle_audio(p,&audio);
        }
        naki_branch_item_free(&item);
        if(ret>=0&&pipeline_mux_live(p)) {ret=pipeline_flush_deferred_audio(p);if(ret>=0)ret=pipeline_drain_mux(p,0);}
    }
    if(!naki_job_cancelled(naki_job_parent(j))&&naki_job_cancelled(j)&&!naki_job_failed(j))fail_branch(b,AVERROR(ETIMEDOUT),"branch overload cancellation");
    if(ret<0&&!naki_job_cancelled(naki_job_parent(j)))fail_branch(b,ret,p->stage);
    naki_branch_queue_abort(&b->queue);naki_watermark_owner_finish(naki_job_watermark(j),ret,naki_job_cancelled(naki_job_parent(j)));naki_watermark_stop(naki_job_watermark(j));
    ret=pipeline_finalize_output(p,ret,naki_job_cancelled(naki_job_parent(j)));
    if(ret<0&&!naki_job_cancelled(naki_job_parent(j)))fail_branch(b,ret,p->stage);
    pipeline_release_media_resources(p,NULL);
    naki_job_complete(p->job,ret,0,p->stage);
    return NULL;
}
int pipeline_branches_start(Pipeline *p) {
    p->branches=calloc(1,sizeof(*p->branches));if(!p->branches)return AVERROR(ENOMEM);p->branches->count=naki_job_config(p->job)->output_count;
    for(int i=0;i<p->branches->count;i++)p->branches->slots[i].pipeline.job=naki_job_output(p->job,(unsigned)i);
    for(int i=0;i<p->branches->count;i++) {
        Branch *b=&p->branches->slots[i];Job *j=naki_job_output(p->job,(unsigned)i);b->spec=&naki_job_config(p->job)->outputs[i];
        b->pipeline=(Pipeline){.job=j,.input_tb=p->input_tb,.frame_rate=p->frame_rate,.video_rate=p->video_rate,.input_wrap_bits=p->input_wrap_bits,
            .stage="branch initialization",.io_timeout_us=(int64_t)b->spec->io_timeout_ms*1000};
        naki_job_inherit_input(j,p->job,&p->video_rate);
        int ret=naki_branch_queue_init_limits(&b->queue,b->spec->queue_items,b->spec->audio_packets,b->spec->queue_bytes,(int64_t)b->spec->queue_media_ms*1000,(int64_t)b->spec->max_lag_ms*1000);
        if(ret<0)return ret;
        b->initialized=1;
        if(naki_job_done(j))continue;
        ret=pthread_create(&b->thread,NULL,branch_main,b);
        if(ret) {
            fail_branch(b,AVERROR(ret),"branch thread creation");naki_watermark_stop(naki_job_watermark(j));
            naki_job_branch_unstarted(j);continue;
        }
        b->started=1;
    }
    return pipeline_branches_check(p,0);
}
static int admission_check(void *opaque) {
    Pipeline *p=opaque;
    return naki_job_cancelled(p->job)?AVERROR_EXIT:pipeline_branches_check(p,0);
}
static int admit_branch_items(Pipeline *p,NakiBranchItem *items) {
    NakiBranchOffer offers[4]={0};
    for(int i=0;i<p->branches->count;i++)if(items[i].frame||items[i].packet)
        offers[i]=(NakiBranchOffer){.queue=&p->branches->slots[i].queue,.item=&items[i],.result=AVERROR(EAGAIN)};
    naki_branch_admit_many(offers,(unsigned)p->branches->count,BRANCH_ADMISSION_GRACE_US,admission_check,p);
    for(int i=0;i<p->branches->count;i++) {
        Branch *b=&p->branches->slots[i];Job *j=b->pipeline.job;
        if(items[i].frame) {
            Stamp *s=(Stamp *)items[i].frame->opaque_ref->data;
            naki_branch_release(j,(uint64_t)s->frame_id,s->branch_charge);
        }
        if(offers[i].queue&&offers[i].result<0&&!naki_job_cancelled(p->job)&&!naki_job_done(j)) {
            pthread_mutex_lock(&b->queue.mutex);unsigned reason=b->queue.rejection_reason;pthread_mutex_unlock(&b->queue.mutex);
            char stage[96];snprintf(stage,sizeof(stage),"branch bounded admission (%s)",naki_branch_reason_name(reason));fail_branch(b,offers[i].result,stage);
        }
        naki_branch_item_free(&items[i]);branch_queue_stats(b);
    }
    int result=pipeline_branches_check(p,0);if(result<0)p->stage="all outputs failed after bounded admission";return result;
}
int pipeline_branches_frame(Pipeline *p,AVFrame *frame) {
    NakiBranchItem items[4]={0};
    int ret=pipeline_prepare_decoded_frame(p,frame);if(ret<0)return ret;Stamp *source=(Stamp *)frame->opaque_ref->data;
    naki_job_shared_decoded(p->job,source,1);
    for(int i=0;i<p->branches->count;i++) {
        Branch *b=&p->branches->slots[i];Job *j=b->pipeline.job;if(!b->started||naki_job_done(j)||naki_job_cancelled(j))continue;
        NakiBranchItem item={.frame=av_frame_clone(frame),.source_us=source->source_us,.ingress_us=source->ingress_us,.sequence=source->sequence,.time_base=p->input_tb};
        /* Queue work-duration budget never turns legal source PTS holes or
         * long display durations into an admission failure. No PTS is retimed. */
        int64_t duration=p->video_rate.nominal_frame_work_us;
        item.media_us=duration>0?(uint64_t)duration:1;
        ret=item.frame?av_buffer_make_writable(&item.frame->opaque_ref):AVERROR(ENOMEM);
        if(ret>=0)ret=naki_branch_item_charge(&item);
        NakiOutputSize size=naki_job_output_size_freeze(j,frame->width,frame->height);int width=size.width,height=size.height;
        int output_bytes=av_image_get_buffer_size(!strcmp(naki_job_config(j)->encoder,"libvvenc")?AV_PIX_FMT_YUV420P10LE:AV_PIX_FMT_YUV420P,width,height,32);
        uint64_t reservation=item.charged_bytes+(output_bytes>0?(uint64_t)output_bytes:0);
        int reserved=0;
        if(ret>=0&&output_bytes>0) {
            ret=naki_job_branch_admit(j,(uint64_t)source->frame_id,source->ingress_us,reservation,b->spec->inflight_bytes);
            reserved=ret>=0;
        } else if(ret>=0)ret=output_bytes<0?output_bytes:AVERROR(EINVAL);
        if(ret>=0) {
            Stamp *stamp=(Stamp *)item.frame->opaque_ref->data;stamp->branch_charge=reservation;stamp->branch_enqueued_us=now_us();
            items[i]=item;item=(NakiBranchItem){0};
        }
        if(ret<0) {if(reserved)naki_branch_release(j,(uint64_t)source->frame_id,reservation);fail_branch(b,ret,"branch frame admission budget");}
        naki_branch_item_free(&item);branch_queue_stats(b);
    }
    return admit_branch_items(p,items);
}
int pipeline_branches_audio(Pipeline *p,const InputItem *audio) {
    NakiBranchItem items[4]={0};
    for(int i=0;i<p->branches->count;i++) {
        Branch *b=&p->branches->slots[i];Job *j=b->pipeline.job;
        if(!b->started||naki_job_done(j)||naki_job_cancelled(j)||!naki_job_config(j)->audio_copy||!strcmp(naki_job_config(j)->output_format,"mp4"))continue;
        NakiBranchItem item={.packet=av_packet_clone(audio->packet),.parameters=avcodec_parameters_alloc(),.time_base=audio->tb,
            .source_us=audio->packet->pts==AV_NOPTS_VALUE?AV_NOPTS_VALUE:av_rescale_q(audio->packet->pts,audio->tb,AV_TIME_BASE_Q),.ingress_us=audio->demuxed_us};
        int ret=item.packet&&item.parameters?avcodec_parameters_copy(item.parameters,audio->params):AVERROR(ENOMEM);
        int64_t duration=audio->packet->duration>0?av_rescale_q(audio->packet->duration,audio->tb,AV_TIME_BASE_Q):0;
        if(duration<=0)duration=audio->params->sample_rate>0?av_rescale(1152,1000000,audio->params->sample_rate):40000;
        item.media_us=duration>0?(uint64_t)duration:1;
        if(ret>=0) {items[i]=item;item=(NakiBranchItem){0};}
        if(ret<0)fail_branch(b,ret,"branch audio admission budget");
        naki_branch_item_free(&item);branch_queue_stats(b);
    }
    return admit_branch_items(p,items);
}
int pipeline_branches_finish(Pipeline *p,int aborting) {
    for(int i=0;i<p->branches->count;i++)if(p->branches->slots[i].initialized) {
        if(aborting) {request_stop(p->branches->slots[i].pipeline.job);naki_branch_queue_abort(&p->branches->slots[i].queue);}
        else naki_branch_queue_finish(&p->branches->slots[i].queue);
    }
    for(;;) {
        int waiting=0;for(int i=0;i<p->branches->count;i++)if(p->branches->slots[i].started&&!naki_job_done(p->branches->slots[i].pipeline.job))waiting=1;
        if(!waiting)break;
        pipeline_branches_check(p,1);struct pollfd fd={.fd=naki_job_wake_fd(p->job),.events=POLLIN};poll(&fd,1,20);
        uint64_t value;while(read(naki_job_wake_fd(p->job),&value,sizeof(value))>0) {}
    }
    int completed=0;
    for(int i=0;i<p->branches->count;i++) {
        Branch *b=&p->branches->slots[i];if(b->started) {if(pthread_join(b->thread,NULL)){abort();}b->started=0;}
        if((naki_job_state(b->pipeline.job)==NAKI_JOB_STATE_COMPLETED))completed++;
    }
    return completed?0:AVERROR(EPIPE);
}

void pipeline_branches_release(Pipeline *p) {
    if(!p->branches)return;
    for(int i=0;i<p->branches->count;i++) {
        Branch *b=&p->branches->slots[i];if(b->started)abort();
        if(b->initialized)naki_branch_queue_destroy(&b->queue);
    }
    free(p->branches);p->branches=NULL;
}
