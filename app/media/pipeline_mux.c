#include "mux_internal.h"

/* Sole output owner: bounded live mux -> av_write_frame -> flush -> accounting.
 * Video latency ends at actual successful flush, including mux queue residence.
 * Late audio behind the output watermark is dropped and counted here. */
static int write_packet_now(Pipeline *p, AVPacket *packet, int64_t *interleaved, int64_t *written) {
    Job *job=p->job;
    int64_t dts_us=av_rescale_q(packet->dts,p->mux->output->streams[packet->stream_index]->time_base,AV_TIME_BASE_Q);
    p->stage="mux/write";
    naki_job_write_begin(job,now_us());
    atomic_store(&p->mux->interrupt.deadline,now_us()+(p->io_timeout_us?p->io_timeout_us:IO_TIMEOUT_US));
    int ret=av_write_frame(p->mux->output,packet);
    /* Flush TS audio PES too: AVIO flushing alone cannot release those bytes. */
    if(ret>=0 && !strcmp(naki_job_config(job)->output_format,"mpegts")) ret=av_write_frame(p->mux->output,NULL);
    *interleaved=now_us();
    naki_job_write_flushing(job);
    avio_flush(p->mux->output->pb); *written=now_us();
    atomic_store(&p->mux->interrupt.deadline,0);
    naki_job_write_end(job);
    if(ret>=0 && p->mux->output->pb->error<0)ret=p->mux->output->pb->error;
    if(ret>=0 && dts_us>p->mux->mux_max_dts_us)p->mux->mux_max_dts_us=dts_us;
    return ret<0 ? ret : 0;
}

static int write_video_packet(Pipeline *p, AVPacket *packet) {
    if(!packet->opaque_ref || packet->opaque_ref->size!=sizeof(MuxTrace)) return AVERROR_INVALIDDATA;
    MuxTrace *meta=(MuxTrace *)packet->opaque_ref->data;
    Stamp stamp=meta->stamp;
    if(p->destination_copy){int64_t interleaved=0,written=0;int ret=write_packet_now(p,packet,&interleaved,&written);if(!ret){p->mux->video_started=1;ret=naki_destinations_written(p,packet,written);}return ret;}

    int bytes=meta->bytes;
    int64_t encoded=meta->encoded_us;
    int output_key=!!(packet->flags&AV_PKT_FLAG_KEY);
    int64_t pts_us=meta->pts_us;
    int actual_picture_type=meta->picture_type;
    int64_t hook_us=meta->hook_us;
    int pts_gop=!naki_job_config(p->job)->policy.intra && !strcmp(naki_job_config(p->job)->encoder,"libvvenc") && strcmp(naki_job_config(p->job)->encoder_profile,"reference");
    int64_t key_target=0, key_interval=0;
    int had_output_key=p->mux->seen_output_key;
    if (output_key && pts_gop) {
        int64_t origin=av_rescale_q(p->first_encoder_pts,naki_encoded_info(p->encoded).time_base,AV_TIME_BASE_Q);
        key_target=origin+((pts_us-origin)/2000000)*2000000;
        key_interval=pts_us-p->mux->previous_key_pts;
        p->mux->previous_key_pts=pts_us; p->mux->seen_output_key=1;
    }
    int64_t mux_start=encoded, write_begin=now_us(), interleaved=0, written=0;
    NakiFrameIdentity identity=pipeline_frame_identity(p,&stamp,packet->pts,p->video.time_base);
    int ret=write_packet_now(p,packet,&interleaved,&written);
    if(ret<0)return ret;
    if(naki_job_config(p->job)->constrained_crf) {
        int64_t bits=8*((int64_t)bytes+(pts_us==av_rescale_q(p->first_encoder_pts,naki_encoded_info(p->encoded).time_base,AV_TIME_BASE_Q)?naki_encoded_info(p->encoded).extradata_size:0));
        if(pipeline_video_written(p,av_rescale_q(packet->dts,p->video.time_base,AV_TIME_BASE_Q),written,bits)<0)return AVERROR_INVALIDDATA;
    }
    naki_job_count(p->job,NAKI_COUNT_SEI_WRITTEN,naki_sei_count(pipeline_stamp_sei(meta->owner),&naki_job_config(p->job)->sei));
    if(!p->destination_owner&&stamp.delivery_lease&&(ret=naki_watermark_written(stamp.delivery_owner,stamp.delivery_lease,&identity,written))<0)return ret;
    if(!meta->fanout_job){
        if(!naki_policy_untrack(p->job,NAKI_TRACK_FRAME,(uint64_t)stamp.frame_id))return AVERROR_INVALIDDATA;
        naki_branch_release(p->job,(uint64_t)stamp.frame_id,stamp.branch_charge);
    }
    if((ret=naki_destinations_written(p,packet,written))<0)return ret;
    p->mux->video_started=1;
    double decode=(stamp.decoded_us-stamp.submitted_us)/1000.0;
    double filter=(stamp.filtered_us-stamp.filter_started_us)/1000.0;
    double encode=(encoded-stamp.encoder_submit_us)/1000.0;
    double mux=(written-mux_start)/1000.0;
    double total=(written-stamp.ingress_us)/1000.0,slow=naki_job_config(p->job)->slow_frame_ms;
    NakiWrittenFrame completed={.stamp=&stamp,.encoded_us=encoded,.mux_begin_us=mux_start,
        .interleaved_us=interleaved,.written_us=written,.bytes=(uint64_t)p->mux->output->pb->bytes_written,.output_key=output_key};
    NakiWrittenResult result=naki_job_frame_written(p->job,&completed);
    int first_output=result.first_output,first_output_key=result.first_key;
    int first_i=stamp.first_i;
    uint64_t ordinal=result.ordinal,submitted=result.submitted;
    int trace=naki_job_config(p->job)->frame_log && (!((ordinal-1)%naki_job_config(p->job)->frame_log_every) ||
       stamp.picture_type==AV_PICTURE_TYPE_I || output_key || total>=slow);
    if(naki_job_config(p->job)->network_trace||naki_job_config(p->job)->codec_trace)trace=1;
    if(!trace && !first_output && !first_output_key)return 0;
    yyjson_mut_doc *doc=new_event("frame",p->job);
    json_int(doc,"frame_id",stamp.frame_id); json_int(doc,"generation",stamp.generation);
    json_int(doc,"mux_flush_done_monotonic_us",written);json_int(doc,"packet_ingress_monotonic_us",stamp.ingress_us);
    json_int(doc,"source_time_base_num",p->input_tb.num); json_int(doc,"source_time_base_den",p->input_tb.den);
    pipeline_trace_pts(doc,"source_packet_pts",stamp.packet_pts); pipeline_trace_pts(doc,"source_packet_dts",stamp.packet_dts);
    pipeline_trace_pts(doc,"decoded_raw_pts",stamp.raw_pts); pipeline_trace_pts(doc,"source_pts",stamp.source_pts);
    json_str(doc,"timestamp_provenance",stamp.timestamp_source==1 ? "source_all" : stamp.timestamp_source==3 ? "legacy_synthetic" : "legacy_best_effort");
    json_int(doc,"intra_reason",stamp.intra_reason);
    char output_type[2]={av_get_picture_type_char(actual_picture_type),0}; json_str(doc,"output_picture_type",output_type);
    json_int(doc,"input_sequence",stamp.sequence); json_int(doc,"pts_us",pts_us);
    json_int(doc,"decoder_async",naki_job_config(p->job)->decoder_async);
    json_int(doc,"decoder_auto_threads",naki_job_config(p->job)->decoder_async&&naki_job_config(p->job)->decoder_auto_threads);
    if(stamp.decoder_receive_us&&stamp.submit_first_us) {
        json_int(doc,"decoder_first_submit_monotonic_us",stamp.submit_first_us);
        json_int(doc,"decoder_receive_monotonic_us",stamp.decoder_receive_us);
        json_num(doc,"decoder_submit_retry_ms",(stamp.submitted_us-stamp.submit_first_us)/1000.0);
        json_num(doc,"decoder_submit_to_receive_ms",(stamp.decoder_receive_us-stamp.submitted_us)/1000.0);
        json_num(doc,"decoder_receive_to_prepare_ms",(stamp.decoded_us-stamp.decoder_receive_us)/1000.0);
    }
    json_int(doc,"output_dts_us",av_rescale_q(packet->dts,p->video.time_base,AV_TIME_BASE_Q));
    json_int(doc,"encoded_bytes",bytes);
    if(naki_job_config(p->job)->network_trace) {
        json_int(doc,"network_first_read_monotonic_us",stamp.network_first_read_us);
        json_int(doc,"rtmp_complete_monotonic_us",stamp.network_complete_us);
        json_int(doc,"demux_finished_monotonic_us",stamp.demux_finished_us);
        json_int(doc,"network_tag_position",stamp.network_tag_position);
        json_int(doc,"decoder_submit_monotonic_us",stamp.submitted_us);
        json_int(doc,"decoded_monotonic_us",stamp.decoded_us);
        json_int(doc,"filter_submit_monotonic_us",stamp.filter_started_us);
        json_int(doc,"filtered_monotonic_us",stamp.filtered_us);
        json_int(doc,"encoder_submit_monotonic_us",stamp.encoder_submit_us);
        json_int(doc,"encoder_accepted_monotonic_us",stamp.encoder_accepted_us);
        json_int(doc,"encoded_monotonic_us",encoded);
        json_int(doc,"mux_write_begin_monotonic_us",write_begin);
        json_num(doc,"network_to_mux_ms",(written-stamp.network_first_read_us)/1000.0);
        json_int(doc,"deadline_300ms_exceeded",written-stamp.network_first_read_us>300000);
        json_int(doc,"video_config_bytes",first_output?naki_encoded_info(p->encoded).extradata_size:0);
        json_num(doc,"network_assembly_ms",(stamp.network_complete_us-stamp.network_first_read_us)/1000.0);
        json_num(doc,"demux_buffer_ms",(stamp.demux_finished_us-stamp.network_complete_us)/1000.0);
        json_num(doc,"demux_to_ingress_ms",(stamp.ingress_us-stamp.demux_finished_us)/1000.0);
    }
    if(naki_job_config(p->job)->encoder_max_future_frames)json_int(doc,"encoder_future_frames",stamp.encoder_future_frames);
    if(naki_job_config(p->job)->codec_trace) {
        if(!naki_job_config(p->job)->network_trace) {
            json_int(doc,"decoder_submit_monotonic_us",stamp.submitted_us);
            json_int(doc,"decoded_monotonic_us",stamp.decoded_us);
            json_int(doc,"encoder_submit_monotonic_us",stamp.encoder_submit_us);
            json_int(doc,"encoded_monotonic_us",encoded);
        }
        naki_decode_trace_json(doc,&stamp.decode_trace);naki_encoder_trace_json(doc,&stamp.encode_trace);
    }
    if(stamp.async_ready_us) {
        json_int(doc,"async_ready_monotonic_us",stamp.async_ready_us);
        json_num(doc,"async_ready_ms",(stamp.async_ready_us-stamp.submitted_us)/1000.0);
        json_num(doc,"decoded_dispatch_queue_ms",(stamp.decoder_owner_us-stamp.async_ready_us)/1000.0);
    }
    if(naki_job_config(p->job)->constrained_crf) {
        json_int(doc,"rc_qp",stamp.rate.qp);json_int(doc,"rc_actual_slice_qp",stamp.actual_slice_qp);
        json_int(doc,"rc_source_bps",stamp.rate.source_bps);json_int(doc,"rc_effective_bps",stamp.rate.effective_bps);
        json_int(doc,"rc_decision_monotonic_us",stamp.rate.decision_us);json_int(doc,"rc_predicted_bits",stamp.rate.predicted_bits);
        json_int(doc,"rc_saturated",stamp.rate.saturated);json_num(doc,"rc_complexity",stamp.rate.complexity);
        json_num(doc,"rc_quality_qp",stamp.rate.quality_qp);json_num(doc,"rc_allowance_bits",stamp.rate.allowance_bits);
        json_int(doc,"rc_model_kind",stamp.rate.kind);json_num(doc,"rc_model_coefficient",stamp.rate.model_coefficient);
        json_num(doc,"rc_window_room_bits",stamp.rate.window_room_bits);
        json_int(doc,"rc_media_clock_us",stamp.rate.media_clock_us);
        json_num(doc,"rc_followup_reserve_bits",stamp.rate.followup_reserve_bits);
    }
    char picture_type[2]={av_get_picture_type_char(stamp.picture_type),0};
    json_str(doc,"decoded_picture_type",picture_type);
    json_int(doc,"input_packet_key",stamp.input_key); json_int(doc,"decoded_key",stamp.decoded_key);
    json_int(doc,"output_key",output_key); json_int(doc,"first_i",first_i && stamp.picture_type==AV_PICTURE_TYPE_I);
    json_int(doc,"watermark_revision",(int64_t)stamp.watermark_revision);
    json_num(doc,"branch_queue_ms",(stamp.branch_started_us-stamp.branch_enqueued_us)/1000.0);
    if (output_key && pts_gop) {
        json_int(doc,"keyframe_target_pts_us",key_target);
        json_num(doc,"keyframe_lateness_ms",(pts_us-key_target)/1000.0);
        if(had_output_key)json_num(doc,"previous_key_interval_ms",key_interval/1000.0);
        else yyjson_mut_obj_add_null(doc,yyjson_mut_doc_get_root(doc),"previous_key_interval_ms");
    }
    json_num(doc,"demux_read_ms",stamp.read_us/1000.0);
    json_num(doc,"input_queue_ms",(stamp.submitted_us-stamp.ingress_us)/1000.0);
    json_num(doc,"decode_ms",decode); json_num(doc,"filter_ms",filter);
    json_num(doc,"setup_or_dispatch_ms",(stamp.filter_started_us-stamp.decoded_us)/1000.0);
    json_num(doc,"encode_ms",encode); json_num(doc,"mux_ms",mux);
    json_num(doc,"encoder_prepare_ms",(stamp.encoder_submit_us-stamp.filtered_us)/1000.0);
    json_num(doc,"media_hook_ms",hook_us/1000.0);
    json_num(doc,"encode_submit_ms",(stamp.encoder_accepted_us-stamp.encoder_submit_us)/1000.0);
    json_num(doc,"encode_wait_ms",(encoded-stamp.encoder_accepted_us)/1000.0);
    json_num(doc,"interleave_call_ms",(interleaved-mux_start)/1000.0);
    json_num(doc,"interleave_queue_ms",(write_begin-encoded)/1000.0);
    json_num(doc,"avio_flush_ms",(written-interleaved)/1000.0);
    json_num(doc,"ingress_to_mux_ms",total);
    if (stamp.scheduled_us) json_num(doc,"scheduled_to_mux_ms",(written-stamp.scheduled_us)/1000.0);
    if(first_output || first_output_key) {
        NakiJobStartup startup=naki_job_startup(p->job);
        yyjson_mut_doc *first=yyjson_mut_doc_mut_copy(doc,NULL);
        yyjson_mut_obj_remove_key(yyjson_mut_doc_get_root(first),"event");
        json_str(first,"event","first_frame");
        json_str(first,"boundary","complete_http_request_handler_to_mux_flush_not_peer_display");
        json_int(first,"first_video",first_output); json_int(first,"first_output_key",first_output_key);
        json_int(first,"prewarmed",startup.prewarmed);
        json_int(first,"request_monotonic_us",naki_job_started_us(p->job));
        json_int(first,"dispatch_monotonic_us",startup.assigned_us);
        json_int(first,"worker_received_monotonic_us",startup.worker_received_us);
        yyjson_mut_obj_remove_key(yyjson_mut_doc_get_root(first),"packet_ingress_monotonic_us");json_int(first,"packet_ingress_monotonic_us",stamp.ingress_us);
        json_int(first,"decoder_submit_monotonic_us",stamp.submitted_us);
        json_int(first,"decoded_monotonic_us",stamp.decoded_us);
        json_int(first,"filter_submit_monotonic_us",stamp.filter_started_us);
        json_int(first,"filtered_monotonic_us",stamp.filtered_us);
        json_int(first,"encoder_submit_monotonic_us",stamp.encoder_submit_us);
        json_int(first,"encoded_monotonic_us",encoded);
        json_int(first,"mux_begin_monotonic_us",mux_start);
        yyjson_mut_obj_remove_key(yyjson_mut_doc_get_root(first),"mux_flush_done_monotonic_us");json_int(first,"mux_flush_done_monotonic_us",written);
        json_num(first,"request_to_dispatch_ms",(startup.assigned_us-naki_job_started_us(p->job))/1000.0);
        json_num(first,"dispatch_to_worker_ms",(startup.worker_received_us-startup.assigned_us)/1000.0);
        json_num(first,"worker_to_packet_ingress_ms",(stamp.ingress_us-startup.worker_received_us)/1000.0);
        json_num(first,"request_to_mux_ms",(written-naki_job_started_us(p->job))/1000.0);
        if(p->destination_owner)yyjson_mut_obj_add_null(first,yyjson_mut_doc_get_root(first),"encoder_api_wall_to_output_ms");else json_num(first,"encoder_api_wall_to_output_ms",pipeline_video_api_us(p)/1000.0);
        json_int(first,"encoder_submitted_frames_at_output",submitted);
        yyjson_mut_doc *service_status=job_snapshot(p->job);
        yyjson_mut_val *startup_json=yyjson_mut_obj_get(yyjson_mut_doc_get_root(service_status),"startup");
        if(startup_json) yyjson_mut_obj_add_val(first,yyjson_mut_doc_get_root(first),"startup",yyjson_mut_val_mut_copy(first,startup_json));
        yyjson_mut_doc_free(service_status);
        naki_job_first_frame(p->job,first,first_output);
        logger_submit(naki_job_logger(p->job),NULL,first);
    }
    if(trace)logger_submit(naki_job_logger(p->job),naki_job_trace(p->job),doc);
    else yyjson_mut_doc_free(doc);
    return 0;
}

int pipeline_drain_mux(Pipeline *p, int force) {
    if(!pipeline_mux_live(p))return 0;
    AVPacket *packet=av_packet_alloc();
    if(!packet)return AVERROR(ENOMEM);
    int ret;
    while((ret=avformat_live_mux_receive(p->mux->output,packet,force))>0) {
        if(packet->stream_index==p->video.index)ret=write_video_packet(p,packet);
        else {
            int64_t dts=av_rescale_q(packet->dts,p->mux->output->streams[packet->stream_index]->time_base,AV_TIME_BASE_Q);
            /* A stalled audio track may return behind already-emitted media.
               Drop its stale packets explicitly rather than rewinding output. */
            if(dts<p->mux->mux_max_dts_us) {
                naki_job_count(p->job,NAKI_COUNT_AUDIO_DROPPED,1);
                ret=0;
            } else {
                int64_t interleaved,written;
                ret=write_packet_now(p,packet,&interleaved,&written);
                if(ret>=0) {naki_job_count(p->job,NAKI_COUNT_AUDIO_WRITTEN,1);naki_destinations_audio_written(p);}
            }
        }
        av_packet_unref(packet);
        if(ret<0)break;
    }
    av_packet_free(&packet);
    AVLiveMuxStats stats;
    if(!avformat_live_mux_stats(p->mux->output,&stats)) {
        naki_job_mux_observation(p->job,&stats);
    }
    return ret<0 ? ret : 0;
}

int pipeline_submit_mux(Pipeline *p, AVPacket *packet) {
    if(!p->mux->live_mux)return write_video_packet(p,packet);
    int ret;
    while((ret=avformat_live_mux_send(p->mux->output,packet))==AVERROR(EAGAIN))
        if((ret=pipeline_drain_mux(p,1))<0)return ret;
    if(ret<0)return ret;
    return pipeline_drain_mux(p,0);
}
