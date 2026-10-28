#include "mux_internal.h"

int pipeline_mux_prepare(Pipeline *p) {
    if(p->mux)return 0;
    NakiMux *m=calloc(1,sizeof(*m));if(!m)return AVERROR(ENOMEM);
    m->custom_output_fd=-1;m->mux_max_dts_us=INT64_MIN;
    m->interrupt.job=p->job;atomic_init(&m->interrupt.deadline,0);atomic_init(&m->interrupt.cleanup,0);
    p->mux=m;return 0;
}
int pipeline_mux_move(Pipeline *from,Pipeline *to) {
    if(from==to||!from->mux||to->mux||from->job!=to->job)return AVERROR(EINVAL);
    to->mux=from->mux;to->video=from->video;from->mux=NULL;
    return 0;
}
int pipeline_mux_live(const Pipeline *p){return p->mux&&p->mux->live_mux;}
uint64_t pipeline_mux_bytes(const Pipeline *p){return p->mux&&p->mux->output&&p->mux->output->pb?(uint64_t)p->mux->output->pb->bytes_written:0;}
void pipeline_mux_cancel(Pipeline *p){if(p->mux)atomic_store(&p->mux->interrupt.deadline,now_us());}
void pipeline_mux_retire_audio(Pipeline *p) {
    if(!p->mux)return;
    /* Finalize must close protocol callbacks before their storage is freed. */
    if(p->mux->output||p->mux->custom_output_fd>=0)abort();
    for(int i=0;i<p->mux->deferred_count;i++)pipeline_input_free(&p->mux->deferred_audio[i]);
    for(int i=0;i<p->mux->audio_count;i++) {
        av_bsf_free(&p->mux->audio[i].bsf);naki_audio_signature_clear(&p->mux->audio[i].accepted);
    }
    p->mux->deferred_count=0;p->mux->deferred_bytes=0;p->mux->audio_count=0;
}
void pipeline_mux_release(Pipeline *p) {
    if(!p->mux)return;
    pipeline_mux_retire_audio(p);
    free(p->mux);p->mux=NULL;p->video=NULL;
}

/* Called only by the owning shared/branch pipeline thread. The orchestration
 * layer stops readers/async or aborts branch queues before releasing media.
 * Finalize preserves the first error and closes the hook before references. */
/* The output owner alone runs this sequence. Preserve the first media error and
 * publish every cleanup result before the hook observes the terminal boundary. */
int pipeline_finalize_output(Pipeline *p,int primary,int stopped) {
    Job *job=p->job;int trailer=0,flush=0,closed=0;int64_t began=now_us();
    int64_t deadline=began+(p->io_timeout_us?p->io_timeout_us:IO_TIMEOUT_US);
    if(p->mux){atomic_store(&p->mux->interrupt.cleanup,1);atomic_store(&p->mux->interrupt.deadline,deadline);}
    naki_job_finalize_begin(job,began,deadline);
    if(p->mux&&p->mux->header_written)trailer=av_write_trailer(p->mux->output);
    if(p->mux&&p->mux->output&&p->mux->output->pb) {
        avio_flush(p->mux->output->pb);flush=p->mux->output->pb->error;
        naki_job_bytes_written(job,p->mux->output->pb->bytes_written);
        if(p->mux->output->flags&AVFMT_FLAG_CUSTOM_IO) {av_freep(&p->mux->output->pb->buffer);avio_context_free(&p->mux->output->pb);}
        else closed=avio_closep(&p->mux->output->pb);
    }
    if(p->mux&&p->mux->custom_output_fd>=0) {
        if(close(p->mux->custom_output_fd)<0&&closed>=0)closed=AVERROR(errno);
        p->mux->custom_output_fd=-1;
    }
    if(primary>=0&&!stopped) {
        if(trailer<0) {primary=trailer;p->stage="output trailer";}
        else if(flush<0) {primary=flush;p->stage="output flush";}
        else if(closed<0) {primary=closed;p->stage="output protocol close";}
    }
    naki_job_finalize_result(job,trailer,flush,closed,primary,now_us());
    if(p->hooks.close) {
        NakiMediaEnd end={.cancelled=stopped,.error_code=primary<0?primary:0,.stage=p->stage};
        if(naki_job_parent(job))end.output_failures=naki_job_failed(job)||(primary<0&&!stopped);
        else end.output_failures=naki_job_output_failures(job);
        p->hooks.close(p->hooks.opaque,&end);p->hooks=(NakiMediaHooks){0};
    }
    if(p->mux) {
        if(p->mux->output) {avformat_free_context(p->mux->output);p->mux->output=NULL;}
        p->mux->header_written=p->mux->live_mux=0;p->video=NULL;
        atomic_store(&p->mux->interrupt.deadline,0);
    }
    return primary;
}

static int local_write(void *opaque, const uint8_t *buffer, int size) {
    NakiMux *m=opaque; int offset=0;
    while (offset<size) {
        ssize_t n=write(m->custom_output_fd,buffer+offset,(size_t)(size-offset));
        if (n<0 && errno==EINTR) continue;
        if (n<=0) return AVERROR(n<0 ? errno : EIO);
        offset+=(int)n;
    }
    return size;
}

static int64_t local_seek(void *opaque, int64_t offset, int whence) {
    NakiMux *m=opaque;
    if (whence==AVSEEK_SIZE) {struct stat st; return fstat(m->custom_output_fd,&st) ? AVERROR(errno) : st.st_size;}
    off_t pos=lseek(m->custom_output_fd,(off_t)offset,whence&~AVSEEK_FORCE);
    return pos<0 ? AVERROR(errno) : (int64_t)pos;
}

int pipeline_open_output(Pipeline *p, AVFrame *frame) {
    int prepared=pipeline_mux_prepare(p);if(prepared<0)return prepared;
    const Config *cfg=naki_job_config(p->job);
    int vvc=!strcmp(cfg->encoder,"libvvenc");
    int byte_input=vvc&&cfg->encoder_input_bit_depth==8;
    enum AVPixelFormat output_pixels=vvc&&!byte_input?AV_PIX_FMT_YUV420P10LE:AV_PIX_FMT_YUV420P;
    const char *pixel_format=output_pixels==AV_PIX_FMT_YUV420P10LE?"yuv420p10le":"yuv420p";
    char chain[256];
    if(cfg->width)snprintf(chain,sizeof(chain),"scale=%d:%d:flags=bilinear,format=pix_fmts=%s",cfg->width,cfg->height,pixel_format);
    else snprintf(chain,sizeof(chain),"null,format=pix_fmts=%s",pixel_format);
    int ret;
    p->stage="filter initialization";
    ret=naki_video_filter_prepare(&p->graph,&p->source,&p->sink,frame,p->input_tb,
        cfg->width,cfg->height,output_pixels);
    if(ret<0)return ret;
    p->input_width=frame->width; p->input_height=frame->height; p->input_format=frame->format;
    p->input_colorspace=frame->colorspace;p->input_range=frame->color_range;
    p->filter_generation=naki_job_filter_generation(p->job,1);

    p->stage="encoder initialization";
    naki_job_checkpoint(p->job,NAKI_CP_ENCODER_OPEN_STARTED,0);
    const AVCodec *codec=avcodec_find_encoder_by_name(cfg->encoder);
    if (!codec) return AVERROR_ENCODER_NOT_FOUND;
    p->encoder=avcodec_alloc_context3(codec);
    if (!p->encoder) return AVERROR(ENOMEM);
    p->encoder->width=av_buffersink_get_w(p->sink);
    p->encoder->height=av_buffersink_get_h(p->sink);
    naki_job_output_size_set(p->job,p->encoder->width,p->encoder->height);
    p->encoder->sample_aspect_ratio=av_buffersink_get_sample_aspect_ratio(p->sink);
    p->encoder->pix_fmt=output_pixels;
    /* The live VVenC fork preserves PTS at 90 kHz and generates DTS from the
       input timestamp FIFO. The reference profile retains upstream timing. */
    int pts_gop=vvc && strcmp(cfg->encoder_profile,"reference");
    p->encoder->time_base=vvc && !pts_gop ? av_inv_q(p->output_rate.value) : (AVRational){1,90000};
    p->encoder->framerate=p->output_rate.value;
    p->encoder->bit_rate=cfg->bitrate;
    if(cfg->cqf&&!cfg->constrained_crf)p->encoder->rc_max_rate=cfg->maxrate;
    p->encoder->thread_count=cfg->encoder_threads;
    p->encoder->thread_type=FF_THREAD_SLICE;
    p->encoder->gop_size=p->output_rate.nominal_gop_frames; p->encoder->max_b_frames=0;
    p->encoder->color_range=av_buffersink_get_color_range(p->sink);
    p->encoder->color_primaries=frame->color_primaries;
    p->encoder->color_trc=frame->color_trc; p->encoder->colorspace=av_buffersink_get_colorspace(p->sink);
    ret=avformat_alloc_output_context2(&p->mux->output,NULL,cfg->output_format,cfg->output);
    if (ret<0) return ret;
    if(cfg->policy.timestamps==NAKI_TS_SOURCE_ALL)p->mux->output->avoid_negative_ts=AVFMT_AVOID_NEG_TS_DISABLED;
    if (p->mux->output->oformat->flags&AVFMT_GLOBALHEADER) p->encoder->flags|=AV_CODEC_FLAG_GLOBAL_HEADER;
    AVDictionary *options=NULL;
    av_dict_set(&options,"preset",cfg->encoder_preset,0);
    if(cfg->cqf) {av_dict_set_int(&options,"qp",cfg->qp,0);av_dict_set(&options,"qpa","1",0);}
    if(cfg->constrained_crf)av_dict_set(&options,"live-qp-feedback","1",0);
    if(cfg->codec_trace)av_dict_set(&options,"live-frame-trace","1",0);
    if(cfg->encoder_max_future_frames)av_dict_set_int(&options,"live-max-future",cfg->encoder_max_future_frames,0);
    char vvenc_params[2048]="";
    int intra_frames=0;
    if (vvc && strcmp(cfg->encoder_profile,"reference")) {
        int temporal_filter=strcmp(cfg->encoder_profile,"live_3b_2s_no_mctf")!=0;
        intra_frames=p->output_rate.nominal_gop_frames;
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
        int minimum=!strcmp(cfg->encoder_profile,"live_3b_2s_qt8")?8:(!strcmp(cfg->encoder_profile,"live_3b_2s_qt16")||!strcmp(cfg->encoder_profile,"live_3b_2s_speed")||!strcmp(cfg->encoder_profile,"live_3b_2s_rdoq0"))?16:0;
        if(minimum) {
            size_t used=strlen(vvenc_params);
            int added=snprintf(vvenc_params+used,sizeof(vvenc_params)-used,
                ":MinQTISlice=%d:MinQTNonISlice=%d:MinQTChromaISliceInChromaSamples=%d:SearchRange=64:MinSearchWindow=32:FastIntraTools=2",
                minimum,minimum,minimum/2);
            if(added<0||(size_t)added>=sizeof(vvenc_params)-used)return AVERROR(ENOBUFS);
        }
        if(!strcmp(cfg->encoder_profile,"live_3b_2s_speed")||!strcmp(cfg->encoder_profile,"live_3b_2s_rdoq0")) {
            size_t used=strlen(vvenc_params);const char *extra=!strcmp(cfg->encoder_profile,"live_3b_2s_speed")?":RDOQ=0:SelectiveRDOQ=0:ALF=0:CCALF=0":":RDOQ=0:SelectiveRDOQ=0";
            if(used+strlen(extra)>=sizeof(vvenc_params))return AVERROR(ENOBUFS);
            strcat(vvenc_params,extra);
        }
        av_dict_set(&options,"vvenc-params",vvenc_params,0);
        av_dict_set(&options,"live-pts-gop","1",0);
    }
    else if (vvc) av_dict_set(&options,"period","2",0);
    else av_dict_set(&options,"tune","zerolatency",0);
    /* Input precision is selected explicitly; 10-bit input preserves the
       precision-only baseline, byte input uses the matching fork bridge. */
    if(vvc) {
        size_t used=strlen(vvenc_params);
        int added=snprintf(vvenc_params+used,sizeof(vvenc_params)-used,"%sInternalBitDepth=%d:LiveNative8Ref=%d",used?":":"",cfg->encoder_bit_depth,cfg->encoder_native8_reference);
        if(added<0||(size_t)added>=sizeof(vvenc_params)-used)return AVERROR(ENOBUFS);
        av_dict_set(&options,"vvenc-params",vvenc_params,0);
    }
    if(cfg->policy.intra)av_dict_set(&options,"live-frame-intra","1",0);
    ret=avcodec_open2(p->encoder,codec,&options);
    if (ret>=0 && av_dict_count(options)) ret=AVERROR_OPTION_NOT_FOUND;
    av_dict_free(&options);
    if (ret<0) return ret;
    naki_job_checkpoint(p->job,NAKI_CP_ENCODER_OPEN_DONE,0);
    ret=pipeline_open_mux(p);if(ret<0)return ret;
    naki_job_checkpoint(p->job,NAKI_CP_OUTPUT_HEADER_DONE,0);
    yyjson_mut_doc *doc=new_event("pipeline_ready",p->job);
    json_int(doc,"input_width",frame->width); json_int(doc,"input_height",frame->height);
    json_int(doc,"output_width",p->encoder->width); json_int(doc,"output_height",p->encoder->height);
    json_str(doc,"encoder",cfg->encoder); json_str(doc,"encoder_preset",cfg->encoder_preset);
    json_str(doc,"encoder_profile",cfg->encoder_profile);
    json_str(doc,"rate_control",cfg->constrained_crf?"constrained_crf":(cfg->cqf?"cqf":"bitrate"));json_int(doc,"target_bitrate",cfg->bitrate);
    if(cfg->constrained_crf) {json_int(doc,"crf",cfg->qp);json_int(doc,"source_rate_limit",cfg->source_rate_limit);json_int(doc,"rate_window_ms",1000);}
    if(cfg->cqf) {json_int(doc,"qp",cfg->qp);json_int(doc,"qpa",1);json_int(doc,"maxrate",cfg->maxrate);}
    json_int(doc,"output_rate_num",p->output_rate.value.num);json_int(doc,"output_rate_den",p->output_rate.value.den);
    json_int(doc,"rate_decimation_divisor",p->rate_divisor);
    if (vvc) {
        json_str(doc,"vvenc_params",vvenc_params);
        json_int(doc,"rate_control_intra_period_frames",intra_frames);
        json_str(doc,"keyframe_schedule",cfg->policy.intra?"frame_policy":pts_gop?"pts_grid":"reference_frames");
        if (pts_gop && !cfg->policy.intra) json_int(doc,"keyframe_period_ms",2000);
    }
    json_str(doc,"pixel_format",pixel_format); json_str(doc,"filter",chain);
    json_int(doc,"encoder_time_base_num",p->encoder->time_base.num);
    json_int(doc,"encoder_time_base_den",p->encoder->time_base.den);
    json_int(doc,"trace_ledger_capacity",LEDGER_CAPACITY);
    json_num(doc,"startup_ms",(now_us()-naki_job_started_us(p->job))/1000.0);
    logger_submit(naki_job_logger(p->job),NULL,doc); return 0;
}

int pipeline_drain_output(Pipeline *p) {
    /* Called once at normal EOF by the output owner, never on cancellation.
     * The caller retains its existing media_phase publication boundary. */
    if(!p->graph) {p->stage="empty video";return AVERROR(ENODATA);}
    p->stage="filter drain";int ret=av_buffersrc_add_frame_flags(p->source,NULL,0);
    if(ret<0||(ret=pipeline_filter_output(p))<0)return ret;
    p->stage="encoder drain";p->encoder_draining=1;
    while((ret=pipeline_encoder_send(p,NULL))==AVERROR(EAGAIN)) {
        if((ret=pipeline_receive_encoded(p))<0)return ret;
    }
    if(ret<0||(ret=pipeline_receive_encoded(p))<0)return ret;
    if(!p->destinations&&((ret=pipeline_flush_deferred_audio(p))<0||(ret=pipeline_drain_mux(p,1))<0))return ret;
    return (pipeline_ledger_pending(p)||(naki_job_config(p->job)->constrained_crf&&p->rate.pending_bits))?AVERROR_INVALIDDATA:0;
}

/* Call after reader/async shutdown (shared owner) or queue abort (branch owner),
 * and after finalize_output has closed the mux and hook. A branch has no input,
 * decoder or receive frame. Keep their releases in this order: frame/codec/
 * filter references may still own watermark delivery leases and ledger stamps. */
void pipeline_release_media_resources(Pipeline *p,AVFrame **receive_frame) {
    pipeline_mux_release(p);
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
void pipeline_complete_job(Pipeline *p,int ret,int stopped){naki_job_complete(p->job,ret,stopped,p->stage);}

/* One mux owner; encoder parameters are immutable after codec open. This also
 * prepares an independent destination without opening a second encoder. */
int pipeline_open_mux(Pipeline *p) {
    const Config *cfg=naki_job_config(p->job);int ret;
    if((ret=pipeline_mux_prepare(p))<0)return ret;
    if(!p->mux->output){ret=avformat_alloc_output_context2(&p->mux->output,NULL,cfg->output_format,cfg->output);if(ret<0)return ret;
        if(cfg->policy.timestamps==NAKI_TS_SOURCE_ALL)p->mux->output->avoid_negative_ts=AVFMT_AVOID_NEG_TS_DISABLED;}
    p->video=avformat_new_stream(p->mux->output,NULL);
    if (!p->video) return AVERROR(ENOMEM);
    ret=avcodec_parameters_from_context(p->video->codecpar,p->encoder);
    if (ret<0) return ret;
    p->video->time_base=p->encoder->time_base; p->video->avg_frame_rate=p->output_rate.value;
    p->mux->output->flags|=AVFMT_FLAG_FLUSH_PACKETS;
    p->mux->output->max_delay=0; p->mux->output->max_interleave_delta=0;
    p->mux->output->interrupt_callback=(AVIOInterruptCB){pipeline_interrupted,&p->mux->interrupt};
    p->stage="output open/header";
    atomic_store(&p->mux->interrupt.deadline,now_us()+(p->io_timeout_us?p->io_timeout_us:IO_TIMEOUT_US));
    if (!strstr(cfg->output,"://") && !strchr(cfg->output,':')) {
        p->mux->custom_output_fd=open(cfg->output,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0644);
        if (p->mux->custom_output_fd<0) return AVERROR(errno);
        unsigned char *buffer=av_malloc(32768);
        if (!buffer) return AVERROR(ENOMEM);
        p->mux->output->pb=avio_alloc_context(buffer,32768,1,p->mux,NULL,local_write,local_seek);
        if (!p->mux->output->pb) {av_free(buffer); return AVERROR(ENOMEM);}
        p->mux->output->flags|=AVFMT_FLAG_CUSTOM_IO;
    } else {
        AVDictionary *io_options=NULL;
        if(cfg->rtmp_tcp_nodelay&&!strncmp(cfg->output,"rtmp://",7))av_dict_set(&io_options,"tcp_nodelay","1",0);
        ret=avio_open2(&p->mux->output->pb,cfg->output,AVIO_FLAG_WRITE,&p->mux->output->interrupt_callback,&io_options);
        if(ret>=0&&av_dict_count(io_options))ret=AVERROR_OPTION_NOT_FOUND;
        av_dict_free(&io_options);
        if (ret<0) return ret;
    }
    ret=avformat_write_header(p->mux->output,NULL);
    atomic_store(&p->mux->interrupt.deadline,0);
    if (ret<0) return ret;
    p->mux->header_written=1;
    if(strcmp(cfg->output_format,"mp4")) {
        ret=avformat_live_mux_init(p->mux->output,(int64_t)cfg->mux_wait_ms*1000,(p->destination_owner||naki_job_remote(p->job)==2)?4:256,4*1024*1024);
        if(ret<0)return ret;
        p->mux->live_mux=1;
    }
    return 0;
}
