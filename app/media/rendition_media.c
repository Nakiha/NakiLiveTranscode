#include "rendition_media.h"
#include "pipeline_internal.h"

int naki_rendition_media_open(Pipeline **out,Job *job,const AVFrame *prototype,AVRational tb,NakiVideoRate rate,int wrap_bits,int io_timeout_ms) {
    Pipeline *p=calloc(1,sizeof(*p));if(!p)return AVERROR(ENOMEM);*out=p;
    *p=(Pipeline){.job=job,.input_tb=tb,.frame_rate=rate.value,.video_rate=rate,.input_wrap_bits=wrap_bits,
        .last_pts=AV_NOPTS_VALUE,.stage="rendition prepare",.io_timeout_us=(int64_t)io_timeout_ms*1000};
    NakiMediaJobInfo info={.job_id=job->id,.request_id=job->request_id,.config=&job->config,.request_us=job->lifecycle.started_us,.output_id=job->output_id};
    int ret=naki_media_hooks_open(&info,&p->hooks);if(ret>0||(ret>=0&&p->hooks.api_version!=NAKI_MEDIA_HOOK_API))return AVERROR(EINVAL);
    if(ret>=0)ret=naki_watermark_wait(job->watermark);
    if(ret>=0)ret=naki_video_rate_output(&rate,job->config.output_fps,&p->output_rate,&p->rate_divisor);
    if(ret>=0)ret=naki_watermark_video(job->watermark,prototype,job->config.width?job->config.width:prototype->width,job->config.height?job->config.height:prototype->height);
    if(ret>=0)ret=pipeline_open_output(p,(AVFrame *)prototype);
    if(ret>=0)ret=naki_destinations_open(p);
    return ret;
}
int naki_rendition_media_frame(Pipeline *p,AVFrame *frame,uint64_t limit) {
    if(!pipeline_stamp_valid(frame->opaque_ref))return AVERROR_INVALIDDATA;
    Stamp *s=(Stamp *)frame->opaque_ref->data;
    int bytes=av_image_get_buffer_size(p->encoder->pix_fmt,p->encoder->width,p->encoder->height,32);
    if(bytes<0)return bytes;
    uint64_t charge=(uint64_t)bytes+av_image_get_buffer_size(frame->format,frame->width,frame->height,32);
    Job *j=p->job;pthread_mutex_lock(&j->mutex);int ret=0;
    if(j->stats.branch_inflight_frames>=128||j->stats.branch_inflight_bytes>limit||charge>limit-j->stats.branch_inflight_bytes)ret=AVERROR(ENOBUFS);
    else if(!(ret=naki_inflight_add(&j->flow.branch_pending,NAKI_TRACK_FRAME,(uint64_t)s->frame_id,s->ingress_us))) {
        j->stats.branch_inflight_frames++;j->stats.branch_inflight_bytes+=charge;
        if(j->stats.branch_inflight_frames>j->stats.branch_inflight_high)j->stats.branch_inflight_high=j->stats.branch_inflight_frames;
    }
    pthread_mutex_unlock(&j->mutex);if(ret<0)return ret;
    s->branch_charge=charge;s->branch_enqueued_us=now_us();
    return pipeline_process_frame(p,frame);
}
int naki_rendition_media_audio(Pipeline *p,const InputItem *audio){return p->destinations?naki_destinations_audio(p,audio):pipeline_handle_audio(p,audio);}
int naki_rendition_media_poll(Pipeline *p) {
    int ret=p->destinations?naki_destinations_poll(p):0;if(ret<0)return ret;
    if(p->encoder_poll_pending)ret=pipeline_receive_encoded(p);
    if(ret>=0&&!p->destinations&&pipeline_mux_live(p))ret=pipeline_flush_deferred_audio(p);
    if(ret>=0&&!p->destinations&&pipeline_mux_live(p))ret=pipeline_drain_mux(p,0);
    return ret;
}
int naki_rendition_media_close(Pipeline **out,int error,int stopped) {
    Pipeline *p=*out;if(!p)return error;Job *j=p->job;
    if(error>=0&&!stopped)error=pipeline_drain_output(p);
    if(p->destinations){int close=naki_destinations_close(p,stopped||error<0);if(error>=0&&close<0)error=close;}
    naki_watermark_owner_finish(j->watermark,error,stopped);naki_watermark_stop(j->watermark);
    error=pipeline_finalize_output(p,error,stopped);pipeline_release_media_resources(p,NULL);
    pthread_mutex_lock(&j->mutex);
    if(error<0&&!stopped){j->lifecycle.branch_failed=1;snprintf(j->lifecycle.error,sizeof(j->lifecycle.error),"%s: %d",p->stage,error);}
    pthread_mutex_unlock(&j->mutex);pipeline_complete_job(p,error,stopped);free(p);*out=NULL;return error;
}
