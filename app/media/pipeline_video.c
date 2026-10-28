#include "video_internal.h"

static int video_init(Pipeline *p) {
    if(p->v)return 0;
    p->v=calloc(1,sizeof(*p->v));if(!p->v)return AVERROR(ENOMEM);
    p->v->last_pts=AV_NOPTS_VALUE;return 0;
}
int pipeline_video_poll(Pipeline *p,int *progress) {
    if(!p->v||!p->v->encoder_poll_pending)return 0;
    p->stage="encoder pending packet";int ret=pipeline_receive_encoded(p);
    if(ret>=0)*progress=p->v->encoder_polled_packet;
    return ret;
}
int pipeline_video_buffer_size(const Pipeline *p) {
    if(!p->v||!p->v->encoder)return AVERROR(EINVAL);
    const AVCodecContext *e=p->v->encoder;return av_image_get_buffer_size(e->pix_fmt,e->width,e->height,32);
}
int pipeline_video_force_idr(Pipeline *p) {
    return p->v&&p->v->encoder?av_opt_set_int(p->v->encoder->priv_data,"forced-idr",1,0):AVERROR(EINVAL);
}
int pipeline_video_written(Pipeline *p,int64_t media_us,int64_t written_us,int64_t bits) {
    return p->v?naki_rate_written(&p->v->rate,media_us,written_us,bits):AVERROR_INVALIDDATA;
}
int64_t pipeline_video_api_us(const Pipeline *p){return p->v?p->v->encoder_api_us:0;}
void pipeline_video_close(Pipeline *p) {
    if(!p->v)return;
    avcodec_free_context(&p->v->encoder);avfilter_graph_free(&p->v->graph);
    naki_watermark_compositor_free(&p->v->watermark_compositor);
}
void pipeline_video_release(Pipeline *p) {
    if(!p->v)return;
    if(p->v->encoder||p->v->graph||p->v->watermark_compositor)abort();
    pipeline_ledger_clear(p);free(p->v);p->v=NULL;
}

static void delivery_stamp_free(void *opaque,uint8_t *data) {
    (void)opaque;Stamp *stamp=(Stamp *)data;
    if(stamp->delivery_lease&&naki_watermark_release_frame(stamp->delivery_owner,stamp->delivery_lease))abort();
    av_free(data);
}
int pipeline_filter_output(Pipeline *p) {
    AVFrame *frame=av_frame_alloc();
    if (!frame) return AVERROR(ENOMEM);
    int ret;
    while ((ret=av_buffersink_get_frame(p->v->sink,frame))>=0) {
        frame->sample_aspect_ratio=p->v->encoder->sample_aspect_ratio;
        if(p->v->filter_generation>1) {frame->colorspace=p->v->encoder->colorspace;frame->color_range=p->v->encoder->color_range;}
        if(!pipeline_stamp_valid(frame->opaque_ref)) {ret=AVERROR_INVALIDDATA;break;}
        uint64_t watermark_revision=0;
        Stamp stamp=*(Stamp *)frame->opaque_ref->data;NakiOperationHandle lease=0;
        AVRational time_base=av_buffersink_get_time_base(p->v->sink);
        NakiFrameIdentity identity=pipeline_frame_identity(p,&stamp,frame->pts,time_base);
        p->stage="watermark composition";
        ret=naki_watermark_frame_tracked(naki_job_watermark(p->job),&p->v->watermark_compositor,frame,
            time_base,&identity,&watermark_revision,&lease);
        if(ret>=0) {
            stamp.watermark_revision=watermark_revision;stamp.delivery_owner=naki_job_watermark(p->job);stamp.delivery_lease=lease;
            size_t owned_size=frame->opaque_ref->size;
            Stamp *owned=av_memdup(frame->opaque_ref->data,owned_size);
            if(owned)*owned=stamp;
            AVBufferRef *reference=owned?av_buffer_create((uint8_t *)owned,owned_size,delivery_stamp_free,NULL,0):NULL;
            if(!reference) {
                av_free(owned);if(naki_watermark_release_frame(naki_job_watermark(p->job),lease))abort();ret=AVERROR(ENOMEM);
            } else {av_buffer_unref(&frame->opaque_ref);frame->opaque_ref=reference;}
        }
        if(ret>=0) {
            if(p->v->watermark_revision!=watermark_revision) {
                yyjson_mut_doc *event=new_event("watermark_applied",p->job);
                naki_watermark_status(naki_job_watermark(p->job),event);logger_submit(naki_job_logger(p->job),NULL,event);
                p->v->watermark_revision=watermark_revision;
            }
        }
        if(ret>=0)ret=pipeline_encode_frame(p,frame);
        av_frame_unref(frame);
        /* Only EAGAIN/EOF from the main sink below are normal draining.
         * An inner compositor/encoder error must never silently drop a frame. */
        if(ret<0) {av_frame_free(&frame);return ret;}
    }
    av_frame_free(&frame);
    return ret==AVERROR(EAGAIN) || ret==AVERROR_EOF ? 0 : ret;
}
static int rebuild_input_filter(Pipeline *p,const AVFrame *frame) {
    if(frame->width==p->v->input_width&&frame->height==p->v->input_height&&frame->format==p->v->input_format&&
       frame->colorspace==p->v->input_colorspace&&frame->color_range==p->v->input_range&&
       frame->color_primaries==p->v->encoder->color_primaries&&frame->color_trc==p->v->encoder->color_trc)return 0;
    p->stage="dynamic input validation";
    int ret=naki_watermark_video(naki_job_watermark(p->job),frame,p->v->encoder->width,p->v->encoder->height);if(ret<0)return ret;
    if(frame->color_primaries!=p->v->encoder->color_primaries||frame->color_trc!=p->v->encoder->color_trc) {
        p->stage="input primaries/transfer changed; tone mapping unsupported";return AVERROR(ENOSYS);
    }
    if(frame->format!=AV_PIX_FMT_YUV420P&&frame->format!=AV_PIX_FMT_YUV420P10LE)return AVERROR(ENOSYS);
    AVFilterGraph *graph=NULL;AVFilterContext *source=NULL,*sink=NULL;
    p->stage="dynamic filter prepare";
    ret=naki_video_filter_prepare_fixed(&graph,&source,&sink,frame,p->input_tb,p->v->encoder->width,p->v->encoder->height,
        p->v->encoder->pix_fmt,p->v->encoder->colorspace,p->v->encoder->color_range);
    if(ret<0)return ret;
    p->stage="old filter generation drain";
    ret=av_buffersrc_add_frame_flags(p->v->source,NULL,0);if(ret>=0)ret=pipeline_filter_output(p);
    if(ret<0) {avfilter_graph_free(&graph);return ret;}
    avfilter_graph_free(&p->v->graph);p->v->graph=graph;p->v->source=source;p->v->sink=sink;
    p->v->input_width=frame->width;p->v->input_height=frame->height;p->v->input_format=frame->format;
    p->v->input_colorspace=frame->colorspace;p->v->input_range=frame->color_range;
    uint64_t generation=p->v->filter_generation=naki_job_filter_generation(p->job,0);
    yyjson_mut_doc *event=new_event("input_filter_rebuilt",p->job);json_int(event,"generation",generation);
    json_int(event,"input_width",frame->width);json_int(event,"input_height",frame->height);json_int(event,"input_format",frame->format);
    json_int(event,"output_width",p->v->encoder->width);json_int(event,"output_height",p->v->encoder->height);
    json_str(event,"encoder_action","unchanged");logger_submit(naki_job_logger(p->job),NULL,event);return 0;
}
int pipeline_process_frame(Pipeline *p,AVFrame *frame) {
    int initialized=video_init(p);if(initialized<0)return initialized;
    int ret=naki_job_parent(p->job)?av_buffer_make_writable(&frame->opaque_ref):naki_job_config(p->job)->decoder_pump?0:pipeline_prepare_decoded_frame(p,frame);
    if(ret<0)return ret;
    Stamp *stamp=(Stamp *)frame->opaque_ref->data;
    if(naki_job_config(p->job)->decoder_pump&&!naki_job_parent(p->job))stamp->decoder_owner_us=now_us();
    if(naki_job_parent(p->job)) {
        stamp->branch_started_us=now_us();
        naki_job_decoded(p->job,stamp,1);
    }
    if(!p->rate_divisor) {
        p->stage="explicit output rate requires a known integer input/output ratio";
        ret=naki_video_rate_output(&p->video_rate,naki_job_config(p->job)->output_fps,&p->output_rate,&p->rate_divisor);
        if(ret<0)return ret;
    }
    if(!stamp->decoder_tracked&&(ret=naki_policy_track(p->job,NAKI_TRACK_FRAME,(uint64_t)stamp->frame_id,stamp->ingress_us))<0)return ret;
    if(p->v->graph&&(ret=rebuild_input_filter(p,frame))<0)return ret;
    stamp->intra_reason=naki_intra_decide(&naki_job_config(p->job)->policy,&p->v->intra,stamp->source_us,stamp->picture_type==AV_PICTURE_TYPE_I);
    int sei_messages=naki_sei_count(pipeline_stamp_sei(frame->opaque_ref),&naki_job_config(p->job)->sei);
    int sei_protected=sei_messages>0;
    int first=naki_job_parent(p->job)?p->v->rendition_frames++==0:stamp->frame_id==1;
    int protect=stamp->intra_reason || stamp->picture_type==AV_PICTURE_TYPE_I || first || sei_protected;
    int level=naki_job_frame_deciding(p->job,stamp,(unsigned)sei_messages);
    int keep=naki_frame_keep(&naki_job_config(p->job)->policy,level,(uint64_t)stamp->frame_id,protect);
    if(keep<0)return keep;
    int rate_drop=keep && !naki_video_rate_keep((uint64_t)stamp->frame_id,p->rate_divisor,protect);
    if(rate_drop)keep=0;
    naki_job_frame_selected(p->job,protect,sei_protected,keep,rate_drop);
    if(!keep) {
        if(!naki_policy_untrack(p->job,NAKI_TRACK_FRAME,(uint64_t)stamp->frame_id))return AVERROR_INVALIDDATA;
        naki_branch_release(p->job,(uint64_t)stamp->frame_id,stamp->branch_charge);
        if(naki_job_config(p->job)->frame_log && (stamp->frame_id-1)%naki_job_config(p->job)->frame_log_every==0) {
            yyjson_mut_doc *event=new_event("frame_drop",p->job);
            json_int(event,"frame_id",stamp->frame_id); json_int(event,"input_sequence",stamp->sequence);
            json_int(event,"source_pts",stamp->source_pts); json_int(event,"overload_level",level);
            json_str(event,"reason",rate_drop?"output_rate":"overload"); logger_submit(naki_job_logger(p->job),naki_job_trace(p->job),event);
        }
        return 0;
    }
    if(!p->v->graph) {
        p->stage="watermark video validation";
        if((ret=naki_watermark_video(naki_job_watermark(p->job),frame,naki_job_config(p->job)->width?naki_job_config(p->job)->width:frame->width,
              naki_job_config(p->job)->height?naki_job_config(p->job)->height:frame->height))<0)return ret;
        if((ret=pipeline_open_output(p,frame))<0)return ret;
    }
    stamp->generation=(unsigned)p->v->filter_generation;
    p->stage="filter";
    stamp->filter_started_us=now_us();
    ret=av_buffersrc_add_frame_flags(p->v->source,frame,AV_BUFFERSRC_FLAG_KEEP_REF);
    if (ret<0) return ret;
    return pipeline_filter_output(p);
}

int pipeline_open_output(Pipeline *p, AVFrame *frame) {
    int initialized=video_init(p);if(initialized<0)return initialized;
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
    ret=naki_video_filter_prepare(&p->v->graph,&p->v->source,&p->v->sink,frame,p->input_tb,
        cfg->width,cfg->height,output_pixels);
    if(ret<0)return ret;
    p->v->input_width=frame->width; p->v->input_height=frame->height; p->v->input_format=frame->format;
    p->v->input_colorspace=frame->colorspace;p->v->input_range=frame->color_range;
    p->v->filter_generation=naki_job_filter_generation(p->job,1);

    p->stage="encoder initialization";
    naki_job_checkpoint(p->job,NAKI_CP_ENCODER_OPEN_STARTED,0);
    const AVCodec *codec=avcodec_find_encoder_by_name(cfg->encoder);
    if (!codec) return AVERROR_ENCODER_NOT_FOUND;
    p->v->encoder=avcodec_alloc_context3(codec);
    if (!p->v->encoder) return AVERROR(ENOMEM);
    p->v->encoder->width=av_buffersink_get_w(p->v->sink);
    p->v->encoder->height=av_buffersink_get_h(p->v->sink);
    naki_job_output_size_set(p->job,p->v->encoder->width,p->v->encoder->height);
    p->v->encoder->sample_aspect_ratio=av_buffersink_get_sample_aspect_ratio(p->v->sink);
    p->v->encoder->pix_fmt=output_pixels;
    /* The live VVenC fork preserves PTS at 90 kHz and generates DTS from the
       input timestamp FIFO. The reference profile retains upstream timing. */
    int pts_gop=vvc && strcmp(cfg->encoder_profile,"reference");
    p->v->encoder->time_base=vvc && !pts_gop ? av_inv_q(p->output_rate.value) : (AVRational){1,90000};
    p->v->encoder->framerate=p->output_rate.value;
    p->v->encoder->bit_rate=cfg->bitrate;
    if(cfg->cqf&&!cfg->constrained_crf)p->v->encoder->rc_max_rate=cfg->maxrate;
    p->v->encoder->thread_count=cfg->encoder_threads;
    p->v->encoder->thread_type=FF_THREAD_SLICE;
    p->v->encoder->gop_size=p->output_rate.nominal_gop_frames; p->v->encoder->max_b_frames=0;
    p->v->encoder->color_range=av_buffersink_get_color_range(p->v->sink);
    p->v->encoder->color_primaries=frame->color_primaries;
    p->v->encoder->color_trc=frame->color_trc; p->v->encoder->colorspace=av_buffersink_get_colorspace(p->v->sink);
    int global_header=0;ret=pipeline_mux_format(p,&global_header);if(ret<0)return ret;
    if(global_header)p->v->encoder->flags|=AV_CODEC_FLAG_GLOBAL_HEADER;
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
        p->v->encoder->gop_size=intra_frames; p->v->encoder->max_b_frames=3;
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
    ret=avcodec_open2(p->v->encoder,codec,&options);
    if (ret>=0 && av_dict_count(options)) ret=AVERROR_OPTION_NOT_FOUND;
    av_dict_free(&options);
    if (ret<0) return ret;
    naki_job_checkpoint(p->job,NAKI_CP_ENCODER_OPEN_DONE,0);
    p->encoded=naki_encoded_seal(p->v->encoder);if(!p->encoded)return AVERROR(ENOMEM);
    ret=pipeline_open_mux(p);if(ret<0)return ret;
    naki_job_checkpoint(p->job,NAKI_CP_OUTPUT_HEADER_DONE,0);
    yyjson_mut_doc *doc=new_event("pipeline_ready",p->job);
    json_int(doc,"input_width",frame->width); json_int(doc,"input_height",frame->height);
    json_int(doc,"output_width",p->v->encoder->width); json_int(doc,"output_height",p->v->encoder->height);
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
    json_int(doc,"encoder_time_base_num",p->v->encoder->time_base.num);
    json_int(doc,"encoder_time_base_den",p->v->encoder->time_base.den);
    json_int(doc,"trace_ledger_capacity",LEDGER_CAPACITY);
    json_num(doc,"startup_ms",(now_us()-naki_job_started_us(p->job))/1000.0);
    logger_submit(naki_job_logger(p->job),NULL,doc); return 0;
}
int pipeline_drain_output(Pipeline *p) {
    /* Called once at normal EOF by the output owner, never on cancellation.
     * The caller retains its existing media_phase publication boundary. */
    if(!p->v||!p->v->graph) {p->stage="empty video";return AVERROR(ENODATA);}
    p->stage="filter drain";int ret=av_buffersrc_add_frame_flags(p->v->source,NULL,0);
    if(ret<0||(ret=pipeline_filter_output(p))<0)return ret;
    p->stage="encoder drain";p->v->encoder_draining=1;
    while((ret=pipeline_encoder_send(p,NULL))==AVERROR(EAGAIN)) {
        if((ret=pipeline_receive_encoded(p))<0)return ret;
    }
    if(ret<0||(ret=pipeline_receive_encoded(p))<0)return ret;
    if(!p->destinations&&((ret=pipeline_flush_deferred_audio(p))<0||(ret=pipeline_drain_mux(p,1))<0))return ret;
    return (pipeline_ledger_pending(p)||(naki_job_config(p->job)->constrained_crf&&p->v->rate.pending_bits))?AVERROR_INVALIDDATA:0;
}
