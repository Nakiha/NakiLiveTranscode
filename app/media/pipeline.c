#include "pipeline_internal.h"
#include "rendition_runtime.h"

int pipeline_stamp_valid(const AVBufferRef *ref) {
    if(!ref||ref->size<sizeof(Stamp))return 0;
    const Stamp *s=(const Stamp *)ref->data;
    if(ref->size!=sizeof(*s)+s->sei_size)return 0;
    if(!s->sei_size)return 1;
    if(s->sei_size<sizeof(NakiSeiBundle))return 0;
    const NakiSeiBundle *b=(const NakiSeiBundle *)(ref->data+sizeof(*s));
    return b->count>0&&b->count<=NAKI_SEI_MESSAGES&&b->bytes<=NAKI_SEI_PAYLOAD_BYTES&&s->sei_size==naki_sei_bundle_size(b);
}
const NakiSeiBundle *pipeline_stamp_sei(const AVBufferRef *ref) {
    return ((const Stamp *)ref->data)->sei_size?(const NakiSeiBundle *)(ref->data+sizeof(Stamp)):NULL;
}
void pipeline_trace_pts(yyjson_mut_doc *doc, const char *key, int64_t pts) {
    if(pts==AV_NOPTS_VALUE)yyjson_mut_obj_add_null(doc,yyjson_mut_doc_get_root(doc),key);
    else json_int(doc,key,pts);
}
static void media_phase(Pipeline *p,NakiMediaPhase phase) {
    naki_job_media_phase(p->job,phase);
}
NakiFrameIdentity pipeline_frame_identity(Pipeline *p,const Stamp *stamp,int64_t output_pts,AVRational output_tb) {
    return (NakiFrameIdentity){.frame_id=(uint64_t)stamp->frame_id,.source_pts=stamp->source_pts,.source_us=stamp->source_us,
        .source_tb_num=p->input_tb.num,.source_tb_den=p->input_tb.den,.output_pts=output_pts,.output_tb_num=output_tb.num,.output_tb_den=output_tb.den};
}

int pipeline_interrupted(void *opaque) {
    Interrupt *io=opaque;
    int64_t deadline=atomic_load(&io->deadline);
    return (!atomic_load(&io->cleanup)&&naki_job_cancelled(io->job)) || (deadline && now_us()>deadline);
}

void *pipeline_main(void *opaque) {
    (void)crash_thread_init();
    Job *job=opaque;
    Pipeline p={.job=job,.stage="initialization"};
    if(!naki_job_config(job)->output_count) {OutputConfig output;naki_logical_output(naki_job_config(job),0,&output);p.io_timeout_us=(int64_t)output.io_timeout_ms*1000;}
    int decode_init=pipeline_input_init(&p);
    InputItem pending={0}; AVFrame *frame=av_frame_alloc();int branches_joined=0,idle_finished=0;
    NakiMediaJobInfo info={.job_id=naki_job_id(job),.request_id=naki_job_request_id(job),.config=naki_job_config(job),.request_us=naki_job_started_us(job),
        .output_id=naki_job_config(job)->output_count?NULL:naki_job_output_id(job)};
    p.stage="media hook initialization";
    int ret=decode_init<0?decode_init:frame ? naki_media_hooks_open(&info,&p.hooks) : AVERROR(ENOMEM);
    if(ret>0 || (ret>=0 && p.hooks.api_version!=NAKI_MEDIA_HOOK_API))ret=AVERROR(EINVAL);
    if(ret>=0) {
        p.stage="required watermark startup gate";
        if(naki_job_config(job)->watermark.required)naki_job_waiting_assets(job);
        ret=naki_watermark_wait(naki_job_watermark(job));
        if(ret>=0&&naki_job_config(job)->output_count) {
            int ready=0;naki_job_waiting_assets(job);
            for(int i=0;i<naki_job_config(job)->output_count;i++) {
                Job *b=naki_job_output(job,(unsigned)i);if(!b||!*naki_job_output_id(b))continue;int gate=naki_watermark_wait(naki_job_watermark(b));
                if(gate<0) {
                    naki_job_startup_fail(b,gate);
                } else ready++;
            }
            if(!ready) {p.stage="all output startup gates failed";ret=AVERROR(EPIPE);}
        }
    }
    if(ret>=0)ret=pipeline_input_open(&p);
    if (ret<0) goto cleanup;
    if(naki_job_config(job)->rendition_backend) {if((ret=naki_renditions_open(&p))<0)goto cleanup;}
    else if(naki_job_config(job)->output_count&&(ret=pipeline_branches_start(&p))<0)goto cleanup;
    ret=pipeline_reader_start(&p);
    if (ret) {ret=AVERROR(ret); goto cleanup;}
    if(naki_job_config(job)->decoder_pump&&(ret=pipeline_decode_start(&p))<0)goto cleanup;
    int eof_sent=0;
    while (!naki_job_cancelled(job)) {
        if(naki_job_renditions(job)){ret=naki_renditions_poll(&p,0);if(ret==AVERROR_EOF){ret=0;idle_finished=1;break;}if(ret<0)goto cleanup;if(!naki_renditions_capacity(&p)){poll(NULL,0,5);continue;}}
        if(p.branches&&(ret=pipeline_branches_check(&p,0))<0) {p.stage="all outputs failed";goto cleanup;}
        uint64_t wake;
        while (read(naki_job_wake_fd(job),&wake,sizeof(wake))<0 && errno==EINTR) {}
        int progress=0;
        p.stage=naki_job_config(job)->decoder_async?"async decode receive":"synchronous decode receive";
        ret=pipeline_input_receive(&p,frame,&pending);
        if (ret==AVERROR_EOF) {ret=0; break;}
        if (ret==AVERROR(EAGAIN)) ret=0;
        else {
            if (ret<0) goto cleanup;
            ret=naki_job_renditions(job)?naki_renditions_frame(&p,frame):p.branches?pipeline_branches_frame(&p,frame):pipeline_process_frame(&p,frame); av_frame_unref(frame);
            if (ret<0) goto cleanup;
            progress=1;
        }
        int input_done=0, input_error=0;
        if (!naki_job_config(job)->decoder_pump && !pending.packet && !eof_sent) pending=pipeline_reader_pop(&p,&input_done,&input_error);
        if(pending.params) {
            if(p.hooks.audio_packet&&!pending.hook_observed) {
                pending.hook_observed=1;
                NakiAudioPacketEvent event={.packet=pending.packet,.parameters=pending.params,.time_base=pending.tb,
                    .input_stream_index=pending.packet->stream_index,.demuxed_us=pending.demuxed_us,.dispatched_us=now_us()};
                p.stage="audio media hook";
                ret=p.hooks.audio_packet(p.hooks.opaque,&event);
                if(ret>0)ret=AVERROR(EINVAL);
            }
            if(ret>=0&&naki_job_renditions(job))ret=naki_renditions_audio(&p,&pending);
            else if(ret>=0&&p.branches)ret=pipeline_branches_audio(&p,&pending);
            else if(ret>=0 && naki_job_config(job)->audio_copy && strcmp(naki_job_config(job)->output_format,"mp4"))ret=pipeline_handle_audio(&p,&pending);
            if(ret==AVERROR(EAGAIN)&&naki_job_renditions(job))ret=0;
            else {pipeline_input_free(&pending);if(ret<0)goto cleanup;progress=1;}
        }
        if (input_done && input_error<0) {p.stage=pipeline_input_sei_error(&p)?"input SEI parsing / complete-AU contract":"input read"; ret=input_error; goto cleanup;}
        if (!naki_job_config(job)->decoder_pump && !pending.params && (pending.packet || (input_done && !eof_sent))) {
            p.stage=naki_job_config(job)->decoder_async?"async decode submit":"synchronous decode submit";
            ret=pipeline_input_send(&p,pending.packet);
            if (!ret) {
                if (pending.packet) pipeline_input_free(&pending); else eof_sent=1;
                progress=1;
            } else if (ret==AVERROR(EAGAIN)) ret=0;
            else goto cleanup;
        }
        if(pipeline_mux_live(&p)) {
            ret=pipeline_flush_deferred_audio(&p);
            if(ret<0 || (ret=pipeline_drain_mux(&p,0))<0)goto cleanup;
        }
        pipeline_input_tick(&p);
        if(!progress) {ret=pipeline_video_poll(&p,&progress);if(ret<0)goto cleanup;}
        if (!progress) {
            media_phase(&p,NAKI_MEDIA_PHASE_WAITING_INPUT);
            struct pollfd fd={.fd=naki_job_wake_fd(job),.events=POLLIN};
            int n;
            do {n=poll(&fd,1,naki_job_renditions(job)?5:p.branches?20:pipeline_mux_live(&p) ? 10 : 1000);} while(n<0 && errno==EINTR && !naki_job_cancelled(job));
            if (n<0) {ret=AVERROR(errno); goto cleanup;}
        }
    }
    if(!naki_job_cancelled(job)&&!idle_finished) {
        if(pipeline_reader_pending_sei(&p))ret=AVERROR_INVALIDDATA;
        if(ret<0) {p.stage="selected SEI has no decoded picture at EOF";goto cleanup;}
    }
    if(naki_job_renditions(job)) {ret=naki_renditions_finish(&p,naki_job_cancelled(job));branches_joined=1;if(ret<0)p.stage="no rendition completed successfully";}
    else if(p.branches) {
        ret=pipeline_branches_finish(&p,naki_job_cancelled(job));branches_joined=1;
        if(ret<0) {p.stage="all outputs failed";goto cleanup;}
    } else if (!naki_job_cancelled(job))ret=pipeline_drain_output(&p);
cleanup:
    media_phase(&p,NAKI_MEDIA_PHASE_DRAINING);
    {
        int stopped=naki_job_cancelled(job);
        if(ret<0||idle_finished)naki_job_cancel(job);
        if(stopped||ret<0||idle_finished) {
            /* Release a full shared reader ring before any private output
             * teardown/join can wait for its bounded I/O cleanup. */
            pipeline_reader_wake(&p);
        }
        if(naki_job_renditions(job)&&!branches_joined)naki_renditions_finish(&p,1);
        if(p.branches&&!branches_joined)pipeline_branches_finish(&p,1);
        for(int i=0;i<naki_job_config(job)->output_count;i++) {
            Job *b=naki_job_output(job,(unsigned)i);
            if(!naki_job_done(b)&&!naki_job_remote(b))naki_watermark_owner_finish(naki_job_watermark(b),ret,stopped);
            naki_watermark_stop(naki_job_watermark(b));
            if(!naki_job_done(b)) {
                naki_job_shared_terminated(b,stopped,p.stage);
            }
        }
        naki_watermark_stop(naki_job_watermark(job));
        int policy_failed=naki_job_begin_shutdown(job);
        if(policy_failed) {stopped=0; ret=AVERROR(ETIMEDOUT); p.stage="continuous overload budget exhausted";}
        if(ret<0 && !stopped) {
            char message[AV_ERROR_MAX_STRING_SIZE]; av_strerror(ret,message,sizeof(message));
            yyjson_mut_doc *error_event=new_event("pipeline_error",job);
            json_str(error_event,"stage",p.stage); json_str(error_event,"error",message);
            logger_submit(naki_job_logger(job),NULL,error_event);
        }
        /* Wake a blocked producer and interrupt network reads before joining. */
        if (ret<0) naki_job_cancel(job);
        pipeline_reader_wake(&p);
        pipeline_input_quiesce(&p);
        naki_watermark_owner_finish(naki_job_watermark(job),ret,stopped);
        ret=pipeline_finalize_output(&p,ret,stopped);
        pipeline_input_free(&pending);
        pipeline_input_clear(&p);
        pipeline_release_media_resources(&p,&frame);
        pipeline_input_destroy(&p);
        pipeline_branches_release(&p);
        naki_job_complete(p.job,ret,stopped,p.stage);
    }
    return NULL;
}

void pipeline_release_media_resources(Pipeline *p,AVFrame **receive_frame) {
    pipeline_mux_release(p);
    if(receive_frame)av_frame_free(receive_frame);
    pipeline_input_release_decoder(p);
    pipeline_video_close(p);
    naki_encoded_unref(&p->encoded);
    pipeline_input_close_demux(p);
    pipeline_video_release(p);
}
