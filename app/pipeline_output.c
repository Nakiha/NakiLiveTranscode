#include "pipeline_internal.h"

/* Called only by the owning shared/branch pipeline thread. The orchestration
 * layer stops readers/async or aborts branch queues before releasing media.
 * Finalize preserves the first error and closes the hook before references. */
/* The output owner alone runs this sequence. Preserve the first media error and
 * publish every cleanup result before the hook observes the terminal boundary. */
int pipeline_finalize_output(Pipeline *p,int primary,int stopped) {
    Job *job=p->job;int trailer=0,flush=0,closed=0;int64_t began=now_us();
    int64_t deadline=began+(p->io_timeout_us?p->io_timeout_us:IO_TIMEOUT_US);
    atomic_store(&p->write_interrupt.cleanup,1);atomic_store(&p->write_interrupt.deadline,deadline);
    pthread_mutex_lock(&job->mutex);job->progress.finalize_started_us=began;job->progress.finalize_deadline_us=deadline;
    job->progress.write_started_us=began;job->progress.write_phase=NAKI_WRITE_PHASE_OUTPUT_FINALIZE;pthread_mutex_unlock(&job->mutex);
    if(p->header_written)trailer=av_write_trailer(p->output);
    if(p->output&&p->output->pb) {
        avio_flush(p->output->pb);flush=p->output->pb->error;
        pthread_mutex_lock(&job->mutex);job->stats.bytes_out=p->output->pb->bytes_written;pthread_mutex_unlock(&job->mutex);
        if(p->output->flags&AVFMT_FLAG_CUSTOM_IO) {av_freep(&p->output->pb->buffer);avio_context_free(&p->output->pb);}
        else closed=avio_closep(&p->output->pb);
    }
    if(p->custom_output_fd>=0) {
        if(close(p->custom_output_fd)<0&&closed>=0)closed=AVERROR(errno);
        p->custom_output_fd=-1;
    }
    if(primary>=0&&!stopped) {
        if(trailer<0) {primary=trailer;p->stage="output trailer";}
        else if(flush<0) {primary=flush;p->stage="output flush";}
        else if(closed<0) {primary=closed;p->stage="output protocol close";}
    }
    pthread_mutex_lock(&job->mutex);
    job->progress.finalize_trailer_error=trailer<0?trailer:0;job->progress.finalize_flush_error=flush<0?flush:0;
    job->progress.finalize_close_error=closed<0?closed:0;job->progress.finalize_primary_error=primary<0?primary:0;
    job->progress.finalize_finished_us=now_us();job->progress.write_started_us=0;job->progress.write_phase=NAKI_WRITE_PHASE_NONE;
    pthread_mutex_unlock(&job->mutex);
    if(p->hooks.close) {
        NakiMediaEnd end={.cancelled=stopped,.error_code=primary<0?primary:0,.stage=p->stage};
        if(job->parent)end.output_failures=atomic_load(&job->lifecycle.branch_failed)||(primary<0&&!stopped);
        else for(int i=0;i<job->config.output_count;i++)end.output_failures+=atomic_load(&job->outputs[i]->lifecycle.branch_failed);
        p->hooks.close(p->hooks.opaque,&end);p->hooks=(NakiMediaHooks){0};
    }
    if(p->output) {avformat_free_context(p->output);p->output=NULL;}
    atomic_store(&p->write_interrupt.deadline,0);return primary;
}

static int local_write(void *opaque, const uint8_t *buffer, int size) {
    Pipeline *p=opaque; int offset=0;
    while (offset<size) {
        ssize_t n=write(p->custom_output_fd,buffer+offset,(size_t)(size-offset));
        if (n<0 && errno==EINTR) continue;
        if (n<=0) return AVERROR(n<0 ? errno : EIO);
        offset+=(int)n;
    }
    return size;
}

static int64_t local_seek(void *opaque, int64_t offset, int whence) {
    Pipeline *p=opaque;
    if (whence==AVSEEK_SIZE) {struct stat st; return fstat(p->custom_output_fd,&st) ? AVERROR(errno) : st.st_size;}
    off_t pos=lseek(p->custom_output_fd,(off_t)offset,whence&~AVSEEK_FORCE);
    return pos<0 ? AVERROR(errno) : (int64_t)pos;
}

int pipeline_open_output(Pipeline *p, AVFrame *frame) {
    const Config *cfg=&p->job->config;
    int vvc=!strcmp(cfg->encoder,"libvvenc");
    const char *pixel_format=vvc ? "yuv420p10le" : "yuv420p";
    char chain[256];
    if(cfg->width)snprintf(chain,sizeof(chain),"scale=%d:%d:flags=bilinear,format=pix_fmts=%s",cfg->width,cfg->height,pixel_format);
    else snprintf(chain,sizeof(chain),"null,format=pix_fmts=%s",pixel_format);
    int ret;
    p->stage="filter initialization";
    ret=naki_video_filter_prepare(&p->graph,&p->source,&p->sink,frame,p->input_tb,
        cfg->width,cfg->height,vvc?AV_PIX_FMT_YUV420P10LE:AV_PIX_FMT_YUV420P);
    if(ret<0)return ret;
    p->input_width=frame->width; p->input_height=frame->height; p->input_format=frame->format;
    p->input_colorspace=frame->colorspace;p->input_range=frame->color_range;
    pthread_mutex_lock(&p->job->mutex);p->job->stats.filter_generations=1;pthread_mutex_unlock(&p->job->mutex);

    p->stage="encoder initialization";
    pipeline_checkpoint(p,&p->job->progress.encoder_open_started_us);
    const AVCodec *codec=avcodec_find_encoder_by_name(cfg->encoder);
    if (!codec) return AVERROR_ENCODER_NOT_FOUND;
    p->encoder=avcodec_alloc_context3(codec);
    if (!p->encoder) return AVERROR(ENOMEM);
    p->encoder->width=av_buffersink_get_w(p->sink);
    p->encoder->height=av_buffersink_get_h(p->sink);
    pthread_mutex_lock(&p->job->mutex);p->job->progress.fixed_output_width=p->encoder->width;p->job->progress.fixed_output_height=p->encoder->height;pthread_mutex_unlock(&p->job->mutex);
    p->encoder->sample_aspect_ratio=av_buffersink_get_sample_aspect_ratio(p->sink);
    p->encoder->pix_fmt=vvc ? AV_PIX_FMT_YUV420P10LE : AV_PIX_FMT_YUV420P;
    /* The live VVenC fork preserves PTS at 90 kHz and generates DTS from the
       input timestamp FIFO. The reference profile retains upstream timing. */
    int pts_gop=vvc && strcmp(cfg->encoder_profile,"reference");
    p->encoder->time_base=vvc && !pts_gop ? av_inv_q(p->frame_rate) : (AVRational){1,90000};
    p->encoder->framerate=p->frame_rate;
    p->encoder->bit_rate=cfg->bitrate;
    p->encoder->thread_count=cfg->encoder_threads;
    p->encoder->thread_type=FF_THREAD_SLICE;
    p->encoder->gop_size=p->video_rate.nominal_gop_frames; p->encoder->max_b_frames=0;
    p->encoder->color_range=av_buffersink_get_color_range(p->sink);
    p->encoder->color_primaries=frame->color_primaries;
    p->encoder->color_trc=frame->color_trc; p->encoder->colorspace=av_buffersink_get_colorspace(p->sink);
    ret=avformat_alloc_output_context2(&p->output,NULL,cfg->output_format,cfg->output);
    if (ret<0) return ret;
    if(cfg->policy.timestamps==NAKI_TS_SOURCE_ALL)p->output->avoid_negative_ts=AVFMT_AVOID_NEG_TS_DISABLED;
    if (p->output->oformat->flags&AVFMT_GLOBALHEADER) p->encoder->flags|=AV_CODEC_FLAG_GLOBAL_HEADER;
    AVDictionary *options=NULL;
    av_dict_set(&options,"preset",cfg->encoder_preset,0);
    char vvenc_params[2048]="";
    int intra_frames=0;
    if (vvc && strcmp(cfg->encoder_profile,"reference")) {
        int temporal_filter=strcmp(cfg->encoder_profile,"live_3b_2s_no_mctf")!=0;
        intra_frames=p->video_rate.nominal_gop_frames;
        if (intra_frames<4) return AVERROR(EINVAL);
        p->encoder->gop_size=intra_frames; p->encoder->max_b_frames=3;
        /* Display order I B B B P ... ; coding order P4 B2 B1 B3. The
           nominal frame period remains an RC hint; CTS schedules CRA.
           Frame-parallel=0 keeps intra-frame threading and avoids the extra
           stage queue. LookAhead stays enabled for single-pass bitrate RC.
           BIM=1 with this P-anchor layout + MCTF reproducibly aborts v1.14
           on the 320x180 10-bit regression; keep BIM off independently. */
        snprintf(vvenc_params,sizeof(vvenc_params),
            "GOPSize=4:IntraPeriod=%d:RefreshSec=2:POC0IDR=1:STA=0:MaxParallelFrames=0:"
            "MCTF=%d:BIM=0:MCTFFutureReference=0:"
            "Frame1=P 4 0 0 0 0 0 1.0 0 0 0 0 0 0 0 1 1 4 0 0:"
            "Frame2=B 2 1 0 0 0 0 1.0 0 0 0 0 0 0 1 1 1 2 1 1 -2:"
            "Frame3=B 1 2 0 0 0 0 1.0 0 0 0 0 0 0 2 1 1 1 1 1 -1:"
            "Frame4=B 3 2 0 0 0 0 1.0 0 0 0 0 0 0 2 1 1 1 1 1 -1",
            intra_frames,temporal_filter);
        av_dict_set(&options,"vvenc-params",vvenc_params,0);
        av_dict_set(&options,"live-pts-gop","1",0);
    }
    else if (vvc) av_dict_set(&options,"period","2",0);
    else av_dict_set(&options,"tune","zerolatency",0);
    if(cfg->policy.intra)av_dict_set(&options,"live-frame-intra","1",0);
    ret=avcodec_open2(p->encoder,codec,&options);
    if (ret>=0 && av_dict_count(options)) ret=AVERROR_OPTION_NOT_FOUND;
    av_dict_free(&options);
    if (ret<0) return ret;
    pipeline_checkpoint(p,&p->job->progress.encoder_open_done_us);
    p->video=avformat_new_stream(p->output,NULL);
    if (!p->video) return AVERROR(ENOMEM);
    ret=avcodec_parameters_from_context(p->video->codecpar,p->encoder);
    if (ret<0) return ret;
    p->video->time_base=p->encoder->time_base; p->video->avg_frame_rate=p->frame_rate;
    p->output->flags|=AVFMT_FLAG_FLUSH_PACKETS;
    p->output->max_delay=0; p->output->max_interleave_delta=0;
    p->output->interrupt_callback=(AVIOInterruptCB){pipeline_interrupted,&p->write_interrupt};
    p->stage="output open/header";
    atomic_store(&p->write_interrupt.deadline,now_us()+(p->io_timeout_us?p->io_timeout_us:IO_TIMEOUT_US));
    if (!strstr(cfg->output,"://") && !strchr(cfg->output,':')) {
        p->custom_output_fd=open(cfg->output,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0644);
        if (p->custom_output_fd<0) return AVERROR(errno);
        unsigned char *buffer=av_malloc(32768);
        if (!buffer) return AVERROR(ENOMEM);
        p->output->pb=avio_alloc_context(buffer,32768,1,p,NULL,local_write,local_seek);
        if (!p->output->pb) {av_free(buffer); return AVERROR(ENOMEM);}
        p->output->flags|=AVFMT_FLAG_CUSTOM_IO;
    } else {
        ret=avio_open2(&p->output->pb,cfg->output,AVIO_FLAG_WRITE,&p->output->interrupt_callback,NULL);
        if (ret<0) return ret;
    }
    ret=avformat_write_header(p->output,NULL);
    atomic_store(&p->write_interrupt.deadline,0);
    if (ret<0) return ret;
    p->header_written=1;
    if(strcmp(cfg->output_format,"mp4")) {
        ret=avformat_live_mux_init(p->output,(int64_t)cfg->mux_wait_ms*1000,256,4*1024*1024);
        if(ret<0)return ret;
        p->live_mux=1;
    }
    pipeline_checkpoint(p,&p->job->progress.output_header_done_us);
    yyjson_mut_doc *doc=new_event("pipeline_ready",p->job);
    json_int(doc,"input_width",frame->width); json_int(doc,"input_height",frame->height);
    json_int(doc,"output_width",p->encoder->width); json_int(doc,"output_height",p->encoder->height);
    json_str(doc,"encoder",cfg->encoder); json_str(doc,"encoder_preset",cfg->encoder_preset);
    json_str(doc,"encoder_profile",cfg->encoder_profile);
    if (vvc) {
        json_str(doc,"vvenc_params",vvenc_params);
        json_int(doc,"rate_control_intra_period_frames",intra_frames);
        json_str(doc,"keyframe_schedule",pts_gop ? "pts_grid" : "reference_frames");
        if (pts_gop) json_int(doc,"keyframe_period_ms",2000);
    }
    json_str(doc,"pixel_format",pixel_format); json_str(doc,"filter",chain);
    json_int(doc,"encoder_time_base_num",p->encoder->time_base.num);
    json_int(doc,"encoder_time_base_den",p->encoder->time_base.den);
    json_int(doc,"trace_ledger_capacity",LEDGER_CAPACITY);
    json_num(doc,"startup_ms",(now_us()-p->job->lifecycle.started_us)/1000.0);
    logger_submit(p->job->logger,NULL,doc); return 0;
}

int pipeline_drain_output(Pipeline *p) {
    /* Called once at normal EOF by the output owner, never on cancellation.
     * The caller retains its existing media_phase publication boundary. */
    if(!p->graph) {p->stage="empty video";return AVERROR(ENODATA);}
    p->stage="filter drain";int ret=av_buffersrc_add_frame_flags(p->source,NULL,0);
    if(ret<0||(ret=pipeline_filter_output(p))<0)return ret;
    p->stage="encoder drain";ret=pipeline_encoder_send(p,NULL);
    if(ret<0||(ret=pipeline_receive_encoded(p))<0)return ret;
    if((ret=pipeline_flush_deferred_audio(p))<0||(ret=pipeline_drain_mux(p,1))<0)return ret;
    return pipeline_ledger_pending(p)?AVERROR_INVALIDDATA:0;
}

/* Call after reader/async shutdown (shared owner) or queue abort (branch owner),
 * and after finalize_output has closed the mux and hook. A branch has no input,
 * decoder or receive frame. Keep their releases in this order: frame/codec/
 * filter references may still own watermark delivery leases and ledger stamps. */
void pipeline_release_media_resources(Pipeline *p,AVFrame **receive_frame) {
    for(int i=0;i<p->deferred_count;i++)pipeline_input_free(&p->deferred_audio[i]);
    p->deferred_count=0;p->deferred_bytes=0;
    for(int i=0;i<p->audio_count;i++) {
        av_bsf_free(&p->audio[i].bsf);
        naki_audio_signature_clear(&p->audio[i].accepted);
    }
    p->audio_count=0;
    if(receive_frame)av_frame_free(receive_frame);
    avcodec_free_context(&p->decoder);
    avcodec_free_context(&p->encoder);
    avfilter_graph_free(&p->graph);
    naki_watermark_compositor_free(&p->watermark_compositor);
    avformat_close_input(&p->input);
    pipeline_ledger_clear(p);
}

/* Publish only after owned media references are released. Branch failure has
 * priority over parent cancellation; the shared owner keeps its captured stop
 * classification, including the existing overload override. Only the shared
 * owner finishes the common trace, after all branches have joined. */
void pipeline_complete_job(Pipeline *p,int ret,int stopped) {
    Job *j=p->job;
    pthread_mutex_lock(&j->mutex);
    memset(&j->flow.policy_inflight,0,sizeof(j->flow.policy_inflight));
    j->lifecycle.ended_us=now_us();
    if(j->parent) {
        j->flow.policy_closing=1;
        memset(&j->flow.branch_pending,0,sizeof(j->flow.branch_pending));
        j->stats.branch_inflight_bytes=j->stats.branch_inflight_frames=0;
        j->stats.branch_queued=j->stats.branch_queue_bytes=j->stats.branch_queue_media_us=0;
        j->stats.branch_video_queued=j->stats.branch_audio_queued=0;
        j->progress.branch_oldest_ingress_us=0;
        j->lifecycle.state=j->lifecycle.branch_failed?NAKI_JOB_STATE_FAILED:atomic_load(&j->parent->lifecycle.cancel)?NAKI_JOB_STATE_STOPPED:NAKI_JOB_STATE_COMPLETED;
    } else {
        int partial=0;
        for(int i=0;i<j->config.output_count;i++)if(j->outputs[i]->lifecycle.branch_failed)partial=1;
        j->lifecycle.state=stopped?NAKI_JOB_STATE_STOPPED:ret<0?NAKI_JOB_STATE_FAILED:partial?NAKI_JOB_STATE_COMPLETED_PARTIAL:NAKI_JOB_STATE_COMPLETED;
        if(ret<0&&!stopped) {
            char message[AV_ERROR_MAX_STRING_SIZE];av_strerror(ret,message,sizeof(message));
            snprintf(j->lifecycle.error,sizeof(j->lifecycle.error),"%s: %s",p->stage,message);
        }
    }
    pthread_mutex_unlock(&j->mutex);
    logger_submit(j->logger,NULL,job_report(j,1));
    if(!j->parent)logger_trace_finish(j->logger,j->trace);
    atomic_store(&j->lifecycle.done,1);
    if(j->parent)wake_job(j->parent);
}
