#include "job.h"
#include "platform/runtime.h"
#include "media/branch_queue.h"
#include "crash/crash.h"
#include "control/identity.h"
#include "control/resource_limits.h"
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <unistd.h>
_Static_assert(sizeof(((Job *)0)->stats.branch_rejected)/sizeof(uint64_t)==NAKI_BRANCH_REASONS,"Branch reason counters mismatch");
static double percentile(const Metric *metric, unsigned percent) {
    if(!metric->count)return 0;
    uint64_t rank=(metric->count*percent+99)/100, count=0;
    for(int i=0;i<256;i++) {
        count+=metric->bins[i];
        if(count>=rank) return i<200 ? i+1 : i<240 ? 200+(i-199)*20 : i<255 ? 1000+(i-239)*1000 : NAN;
    }
    return NAN;
}
static yyjson_mut_val *metric_json(yyjson_mut_doc *doc, const Metric *metric) {
    yyjson_mut_val *obj = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_uint(doc, obj, "count", metric->count);
    yyjson_mut_obj_add_real(doc, obj, "mean_ms", metric->count ? metric->sum_ms / metric->count : 0);
    yyjson_mut_obj_add_real(doc, obj, "max_ms", metric->max_ms);
    const char *names[]={"p50_upper_ms","p95_upper_ms","p99_upper_ms"};
    const unsigned values[]={50,95,99};
    for(int i=0;i<3;i++) {
        double value=percentile(metric,values[i]);
        if(isfinite(value)) yyjson_mut_obj_add_real(doc,obj,names[i],value);
        else yyjson_mut_obj_add_null(doc,obj,names[i]);
    }
    yyjson_mut_obj_add_uint(doc,obj,"overflow_16s",metric->bins[255]);
    yyjson_mut_obj_add_uint(doc,obj,"slow_count",metric->slow_count);
    return obj;
}
static void add_latencies(yyjson_mut_doc *doc, yyjson_mut_val *obj, const Latencies *m) {
#define ADD(name) yyjson_mut_obj_add_val(doc,obj,#name,metric_json(doc,&m->name))
    ADD(demux_read); ADD(input_queue); ADD(decode); ADD(branch_queue); ADD(setup); ADD(filter); ADD(encode); ADD(encode_submit); ADD(encode_wait); ADD(mux);
    ADD(ingress_to_mux); ADD(scheduled_to_mux); ADD(interleave_call); ADD(avio_flush);
    ADD(network_to_mux);
#undef ADD
}
static yyjson_mut_val *group_json(yyjson_mut_doc *doc,const FrameGroups *g) {
    yyjson_mut_val *obj=yyjson_mut_obj(doc);
#define ADD(name) yyjson_mut_obj_add_val(doc,obj,#name,metric_json(doc,&g->name))
    ADD(i_frame); ADD(first_i); ADD(recurring_i); ADD(p_frame); ADD(b_frame); ADD(decoded_key); ADD(output_key);
#undef ADD
    return obj;
}
static void progress_time(yyjson_mut_doc *d,yyjson_mut_val *value,const char *name,int64_t time,int64_t current) {
    char key[64];snprintf(key,sizeof(key),"%s_us",name);
    yyjson_mut_obj_add(value,yyjson_mut_strcpy(d,key),time?yyjson_mut_sint(d,time):yyjson_mut_null(d));
    snprintf(key,sizeof(key),"%s_age_ms",name);
    yyjson_mut_obj_add(value,yyjson_mut_strcpy(d,key),time?yyjson_mut_real(d,(current>time?current-time:0)/1000.0):yyjson_mut_null(d));
}
static yyjson_mut_val *progress_json(yyjson_mut_doc *d,const Job *job) {
    yyjson_mut_val *v=yyjson_mut_obj(d);int64_t current=now_us(),oldest=0;unsigned packets=0,frames=0,retained=0;
    for(unsigned i=0;i<NAKI_INFLIGHT_CAPACITY;i++) {
        const NakiInFlightEntry *entry=&job->flow.policy_inflight.entries[i];
        packets+=entry->kind==NAKI_TRACK_PACKET;frames+=entry->kind==NAKI_TRACK_FRAME;
        if(entry->kind&&entry->ingress_us>0&&(!oldest||entry->ingress_us<oldest))oldest=entry->ingress_us;
        entry=&job->flow.branch_pending.entries[i];retained+=entry->kind!=0;
        if(entry->kind&&entry->ingress_us>0&&(!oldest||entry->ingress_us<oldest))oldest=entry->ingress_us;
    }
    const char *phase=job->lifecycle.ended_us?naki_job_state_name(job->lifecycle.state):job->lifecycle.stop_requested_us?"draining":job->progress.media_phase?naki_media_phase_name(job->progress.media_phase):(job->lifecycle.state==NAKI_JOB_STATE_WAITING_ASSETS)?"waiting_assets":"starting";
    yyjson_mut_obj_add_strcpy(d,v,"phase",phase);yyjson_mut_obj_add_sint(d,v,"sampled_monotonic_us",current);
    yyjson_mut_obj_add_str(d,v,"input_scope",job->parent?"branch_owner_admission":"reader_ring_admission");yyjson_mut_obj_add_str(d,v,"frame_scope","video");
    yyjson_mut_obj_add_str(d,v,"decoded_scope","shared_decoder_output");
    progress_time(d,v,"last_input",job->progress.last_input_us,current);progress_time(d,v,"last_decoded",job->progress.last_decoded_us,current);
    progress_time(d,v,"last_encoded",job->progress.last_encoded_us,current);progress_time(d,v,"last_written",job->progress.last_output_us,current);
    progress_time(d,v,"oldest_video_inflight",oldest,current);progress_time(d,v,"oldest_mixed_queue",job->progress.branch_oldest_ingress_us,current);
    yyjson_mut_obj_add_uint(d,v,"tracked_video_packets",packets);yyjson_mut_obj_add_uint(d,v,"tracked_video_frames",frames);yyjson_mut_obj_add_uint(d,v,"retained_branch_frames",retained);
    yyjson_mut_obj_add_uint(d,v,"video_tracker_capacity",NAKI_INFLIGHT_CAPACITY);return v;
}
static yyjson_mut_val *video_rate_json(yyjson_mut_doc *doc,const Job *job) {
    const NakiVideoRate *rate=&job->progress.video_rate;if(rate->value.num<=0)return yyjson_mut_null(doc);
    yyjson_mut_val *value=yyjson_mut_obj(doc),*rc=yyjson_mut_obj(doc);
    yyjson_mut_obj_add_int(doc,value,"num",rate->value.num);yyjson_mut_obj_add_int(doc,value,"den",rate->value.den);
    yyjson_mut_obj_add_str(doc,value,"source",rate->from_fallback?"config_fallback":"av_guess_frame_rate");
    yyjson_mut_obj_add_int(doc,value,"fallback_fps",job->config.fps);
    yyjson_mut_obj_add_sint(doc,value,"nominal_frame_work_us",rate->nominal_frame_work_us);
    yyjson_mut_obj_add_int(doc,value,"nominal_gop_frames",rate->nominal_gop_frames);
    const char *schedule=job->config.policy.intra?"frame_policy":!strcmp(job->config.encoder,"libvvenc")&&strcmp(job->config.encoder_profile,"reference")?"source_pts_2s_grid":"nominal_frame_count_max";
    yyjson_mut_obj_add_str(doc,value,"keyframe_schedule",schedule);
    NakiVideoRate output;int divisor;
    if(naki_video_rate_output(rate,job->config.output_fps,&output,&divisor)<0)return value;
    yyjson_mut_obj_add_int(doc,value,"output_num",output.value.num);yyjson_mut_obj_add_int(doc,value,"output_den",output.value.den);
    yyjson_mut_obj_add_int(doc,value,"decimation_divisor",divisor);
    yyjson_mut_obj_remove_key(value,"nominal_gop_frames");
    yyjson_mut_obj_add_int(doc,value,"nominal_gop_frames",output.nominal_gop_frames);
    yyjson_mut_obj_add_sint(doc,value,"output_nominal_frame_work_us",output.nominal_frame_work_us);
    yyjson_mut_obj_add_int(doc,rc,"num",output.value.num);yyjson_mut_obj_add_int(doc,rc,"den",output.value.den);
    yyjson_mut_obj_add_val(doc,value,"rc_nominal_rate",rc);return value;
}
static yyjson_mut_val *finalize_json(yyjson_mut_doc *doc,const Job *job) {
    yyjson_mut_val *value=yyjson_mut_obj(doc);
    yyjson_mut_obj_add_sint(doc,value,"started_us",job->progress.finalize_started_us);
    yyjson_mut_obj_add_sint(doc,value,"finished_us",job->progress.finalize_finished_us);
    yyjson_mut_obj_add_sint(doc,value,"deadline_us",job->progress.finalize_deadline_us);
    yyjson_mut_obj_add_int(doc,value,"primary_error",job->progress.finalize_primary_error);
    yyjson_mut_obj_add_int(doc,value,"trailer_error",job->progress.finalize_trailer_error);
    yyjson_mut_obj_add_int(doc,value,"flush_error",job->progress.finalize_flush_error);
    yyjson_mut_obj_add_int(doc,value,"close_error",job->progress.finalize_close_error);return value;
}
static yyjson_mut_val *first_frame_json(yyjson_mut_doc *doc,const Job *job) {
    yyjson_mut_val *items=yyjson_mut_arr(doc);
    for(int i=0;i<2;i++)if(job->progress.first_chain[i])yyjson_mut_arr_add_val(items,yyjson_mut_val_mut_copy(doc,yyjson_mut_doc_get_root(job->progress.first_chain[i])));
    return items;
}
/* Keep the session snapshot bounded independently of rendition count. Full
 * per-rendition state remains in its private cached snapshot and event log. */
static yyjson_mut_doc *remote_brief(const yyjson_doc *source) {
    yyjson_mut_doc *d=yyjson_mut_doc_new(NULL);if(!d)return NULL;
    yyjson_mut_val *out=yyjson_mut_obj(d);yyjson_mut_doc_set_root(d,out);
    yyjson_val *root=yyjson_doc_get_root((yyjson_doc *)source);
    const char *keys[]={"event","job_id","request_id","output_id","log_path","process_id","instance_id","state","error",
        "progress","video_rate","output_finalize","destinations","actual_output","frames_decoded","frames_filtered","frames_dropped_rate",
        "delivery_statistics_scope","frames_selected","frames_dropped_overload","packets_out","bytes_out","audio_tracks","audio_packets_out","audio_dropped","audio_unsupported",
        "sei_extracted","sei_written","sei_protected_frames","sei_types","delivery_inflight","ingress_to_mux","encode","filter","branch_queue","frame_types","watermark"};
    for(unsigned i=0;i<sizeof(keys)/sizeof(*keys);i++){yyjson_val *v=yyjson_obj_get(root,keys[i]);if(v){if((!strcmp(keys[i],"log_path")||!strcmp(keys[i],"actual_output"))&&yyjson_is_str(v)&&yyjson_get_len(v)>512)yyjson_mut_obj_add_null(d,out,keys[i]);else yyjson_mut_obj_add_val(d,out,keys[i],yyjson_val_mut_copy(d,v));}}
    yyjson_mut_val *first=yyjson_mut_arr(d);yyjson_val *record;size_t index,count;
    yyjson_arr_foreach(yyjson_obj_get(root,"first_frame_checkpoints"),index,count,record){
        yyjson_mut_val *item=yyjson_mut_obj(d);
        const char *fields[]={"frame_id","generation","pts_us","first_video","first_output_key","packet_ingress_monotonic_us","mux_flush_done_monotonic_us","request_to_mux_ms","boundary"};
        for(unsigned i=0;i<sizeof(fields)/sizeof(*fields);i++){yyjson_val *v=yyjson_obj_get(record,fields[i]);if(v)yyjson_mut_obj_add_val(d,item,fields[i],yyjson_val_mut_copy(d,v));}
        yyjson_mut_arr_add_val(first,item);
    }
    yyjson_mut_obj_add_val(d,out,"first_frame_checkpoints",first);
    yyjson_mut_obj_add_str(d,out,"detail_scope","summary");return d;
}
static yyjson_mut_doc *snapshot(Job *job, int report, int final) {
    yyjson_mut_doc *doc = new_event("status", job);
    if (!doc) return NULL;
    pthread_mutex_lock(&job->mutex);
    if(report && !final && job->lifecycle.ended_us) {pthread_mutex_unlock(&job->mutex); yyjson_mut_doc_free(doc); return NULL;}
    json_str(doc, "state", naki_job_state_name(job->lifecycle.state));
    json_str(doc, "error", job->lifecycle.error);
    json_str(doc, "log_path", job->log_path);
    json_str(doc,"actual_output",job->config.output);
    if(job->remote_rendition==2)json_str(doc,"delivery_statistics_scope","primary_destination");
    if(job->destination_snapshot)yyjson_mut_obj_add_val(doc,yyjson_mut_doc_get_root(doc),"destinations",yyjson_mut_val_mut_copy(doc,yyjson_mut_doc_get_root(job->destination_snapshot)));
    json_int(doc,"snapshot_generated_us",now_us());
    json_int(doc,"snapshot_generation",(int64_t)++job->progress.snapshot_generation);
    if(job->renditions){json_int(doc,"topology_revision",job->progress.topology_revision);json_int(doc,"effective_topology_revision",job->progress.effective_topology_revision);
        json_int(doc,"source_format_generation",job->progress.source_format_generation);json_int(doc,"idle_timeout_ms",job->config.idle_timeout_ms);json_int(doc,"idle_deadline_us",job->progress.idle_deadline_us);json_int(doc,"idle_state",job->progress.idle_state);
        json_int(doc,"shared_pool_reserved_bytes",job->progress.shared_pool_bytes);json_int(doc,"shared_pool_occupied_slots",job->progress.shared_pool_occupied);}

    yyjson_mut_obj_add_val(doc,yyjson_mut_doc_get_root(doc),"progress",progress_json(doc,job));
    yyjson_mut_obj_add_val(doc,yyjson_mut_doc_get_root(doc),"video_rate",video_rate_json(doc,job));
    yyjson_mut_obj_add_val(doc,yyjson_mut_doc_get_root(doc),"first_frame_checkpoints",first_frame_json(doc,job));
    yyjson_mut_val *limits=yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc,limits,"profile",NAKI_LIVE_PROFILE);
    yyjson_mut_obj_add_uint(doc,limits,"input_max_pixels",NAKI_INPUT_MAX_PIXELS);
    yyjson_mut_obj_add_uint(doc,limits,"output_max_pixels",NAKI_INPUT_MAX_PIXELS);
    yyjson_mut_obj_add_uint(doc,limits,"input_packet_bytes",NAKI_INPUT_PACKET_BYTES);
    yyjson_mut_obj_add_uint(doc,limits,"reader_retained_bytes",NAKI_READER_RETAINED_BYTES);
    yyjson_mut_obj_add_uint(doc,limits,"reader_pending_item_bytes",NAKI_INPUT_RETAINED_ITEM_BYTES);
    yyjson_mut_obj_add_uint(doc,limits,"consumer_pending_item_bytes",NAKI_INPUT_RETAINED_ITEM_BYTES);
    yyjson_mut_obj_add_val(doc,yyjson_mut_doc_get_root(doc),"effective_resource_limits",limits);
    json_int(doc,"stop_requested_us",job->lifecycle.stop_requested_us);json_int(doc,"stop_deadline_us",job->lifecycle.stop_deadline_us);
    json_str(doc,"stop_operation_id",job->lifecycle.stop_operation_id);json_str(doc,"stop_origin_request_id",job->lifecycle.stop_origin_request_id);
    yyjson_mut_obj_add_bool(doc,yyjson_mut_doc_get_root(doc),"stop_intent",job->lifecycle.stop_requested_us!=0);
    yyjson_mut_obj_add_val(doc,yyjson_mut_doc_get_root(doc),"output_finalize",finalize_json(doc,job));
    json_int(doc, "packets_in", job->stats.packets_in);
    json_int(doc, "frames_decoded", job->stats.frames_decoded);
    json_int(doc, "frames_filtered", job->stats.frames_filtered);
    json_int(doc, "packets_out", job->stats.packets_out);
    json_int(doc, "bytes_out", job->stats.bytes_out);
    json_num(doc, "elapsed_ms", ((job->lifecycle.ended_us ? job->lifecycle.ended_us : now_us()) - job->lifecycle.started_us) / 1000.0);
    yyjson_mut_val *startup=yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc,startup,"boundary","complete_http_request_handler_to_mux_flush");
    yyjson_mut_obj_add_bool(doc,startup,"prewarmed",job->lifecycle.prewarmed);
    yyjson_mut_obj_add_sint(doc,startup,"request_monotonic_us",job->lifecycle.started_us);
#define CHECKPOINT(name) do { if(job->progress.name##_us) yyjson_mut_obj_add_real(doc,startup,#name "_ms",(job->progress.name##_us-job->lifecycle.started_us)/1000.0); else yyjson_mut_obj_add_null(doc,startup,#name "_ms"); } while(0)
    CHECKPOINT(input_open_started); CHECKPOINT(input_open_done); CHECKPOINT(input_probe_done); CHECKPOINT(decoder_ready);
    CHECKPOINT(first_packet); CHECKPOINT(first_decoded); CHECKPOINT(encoder_open_started); CHECKPOINT(encoder_open_done);
    CHECKPOINT(output_header_done); CHECKPOINT(first_encoder_submit);
#undef CHECKPOINT
    yyjson_mut_obj_add_real(doc,startup,"request_to_dispatch_ms",(job->progress.assigned_us-job->lifecycle.started_us)/1000.0);
    yyjson_mut_obj_add_real(doc,startup,"dispatch_to_worker_ms",(job->progress.worker_received_us-job->progress.assigned_us)/1000.0);
    yyjson_mut_obj_add_real(doc,startup,"worker_boot_ms",(job->progress.worker_ready_us-job->progress.worker_spawned_us)/1000.0);
    if(job->progress.first_video_output_us)yyjson_mut_obj_add_real(doc,startup,"request_to_first_video_mux_ms",(job->progress.first_video_output_us-job->lifecycle.started_us)/1000.0);
    else yyjson_mut_obj_add_null(doc,startup,"request_to_first_video_mux_ms");
    if(job->progress.first_key_output_us)yyjson_mut_obj_add_real(doc,startup,"request_to_first_key_mux_ms",(job->progress.first_key_output_us-job->lifecycle.started_us)/1000.0);
    else yyjson_mut_obj_add_null(doc,startup,"request_to_first_key_mux_ms");
    yyjson_mut_obj_add_val(doc,yyjson_mut_doc_get_root(doc),"startup",startup);
    json_int(doc,"frame_log",job->config.frame_log);
    json_str(doc,"timestamps_mode",job->config.policy.timestamps ? "source_all" : "legacy");
    static const char *const intra_names[]={"legacy","follow_source","periodic_pts_grid","follow_source_with_max_gap"};
    json_str(doc,"intra_mode",intra_names[job->config.policy.intra]);
    json_int(doc,"intra_interval_ms",job->config.policy.interval_ms);
    json_int(doc,"frame_id_last",job->stats.frame_id_last); json_int(doc,"frames_selected",job->stats.frames_selected);
    json_int(doc,"frames_dropped_overload",job->stats.frames_dropped_overload); json_int(doc,"frames_protected",job->stats.frames_protected);
    json_int(doc,"frames_dropped_rate",job->stats.frames_dropped_rate);
    json_str(doc,"rate_control",job->config.constrained_crf?"constrained_crf":(job->config.cqf?"cqf":"bitrate"));
    json_int(doc,"decoder_pump",job->config.decoder_pump);
    json_int(doc,"decoder_async",job->config.decoder_async);
    json_int(doc,"decoder_auto_threads",job->config.decoder_async&&job->config.decoder_auto_threads);
    json_int(doc,"decoder_early_output_requested",job->config.decoder_early_output);
    json_int(doc,"codec_trace",job->config.codec_trace);
    if(job->parent){yyjson_mut_val *inflight=yyjson_mut_obj(doc);yyjson_mut_obj_add_uint(doc,inflight,"frames",job->stats.branch_inflight_frames);yyjson_mut_obj_add_uint(doc,inflight,"bytes",job->stats.branch_inflight_bytes);yyjson_mut_obj_add_uint(doc,inflight,"frame_limit",128);yyjson_mut_obj_add_uint(doc,inflight,"byte_limit",job->progress.rendition_inflight_limit);yyjson_mut_obj_add_val(doc,yyjson_mut_doc_get_root(doc),"delivery_inflight",inflight);}

    json_int(doc,"reader_queue_capacity",job->config.reader_queue_capacity);
    if(job->config.constrained_crf) {json_int(doc,"crf",job->config.qp);json_int(doc,"source_rate_limit",job->config.source_rate_limit);json_int(doc,"rate_window_ms",1000);}
    if(job->config.cqf) {json_int(doc,"qp",job->config.qp);json_int(doc,"qpa",1);json_int(doc,"maxrate",job->config.maxrate);}
    json_int(doc,"intra_requested",job->stats.intra_requested); json_int(doc,"intra_confirmed",job->stats.intra_confirmed);
    json_int(doc,"timestamp_wraps",job->stats.timestamp_wraps);
    json_int(doc,"sei_extracted",job->stats.sei_extracted);json_int(doc,"sei_written",job->stats.sei_written);
    json_int(doc,"sei_protected_frames",job->stats.sei_protected);
    yyjson_mut_val *sei_types=yyjson_mut_arr(doc);
    for(unsigned i=0;i<job->config.sei.count;i++)yyjson_mut_arr_add_uint(doc,sei_types,job->config.sei.types[i]);
    yyjson_mut_obj_add_val(doc,yyjson_mut_doc_get_root(doc),"sei_types",sei_types);
    json_int(doc,"overload_enabled",job->config.policy.overload_levels!=0); json_int(doc,"overload_level",job->flow.overload.level);
    json_str(doc,"overload_state",job->flow.overload.failed ? "failed" : job->flow.overload.recovering ? "recovering" : job->flow.overload.level ? "degraded" : "normal");
    json_num(doc,"overload_oldest_ingress_age_ms",job->flow.oldest_ingress_age_us/1000.0);
    json_int(doc,"overload_escalations",job->flow.overload.escalations); json_int(doc,"overload_recoveries",job->flow.overload.recoveries);
    json_num(doc,"overload_fail_write_in_progress_ms",job->flow.overload_fail_write_us/1000.0);
    json_str(doc,"overload_fail_write_phase",naki_write_phase_name(job->flow.overload_fail_write_phase));
    json_str(doc,"encoder",job->config.encoder);
    json_str(doc,"encoder_preset",job->config.encoder_preset);
    json_str(doc,"encoder_profile",job->config.encoder_profile);
    if(!strcmp(job->config.encoder,"libvvenc")) {json_int(doc,"encoder_bit_depth",job->config.encoder_bit_depth);json_int(doc,"encoder_input_bit_depth",job->config.encoder_input_bit_depth);yyjson_mut_obj_add_bool(doc,yyjson_mut_doc_get_root(doc),"encoder_native8_reference",job->config.encoder_native8_reference);}
    json_int(doc,"crash_collector_pid",crash_collector_pid());
    json_int(doc,"slow_frame_ms",job->config.slow_frame_ms);
    json_int(doc,"audio_tracks",job->stats.audio_tracks); json_int(doc,"audio_packets_out",job->stats.audio_packets_out);
    json_int(doc,"audio_dropped",job->stats.audio_dropped); json_int(doc,"audio_unsupported",job->stats.audio_unsupported);
    json_int(doc,"mux_wait_ms",job->config.mux_wait_ms);
    json_int(doc,"mux_queued_packets",job->stats.mux_queued_packets); json_int(doc,"mux_queued_bytes",job->stats.mux_queued_bytes);
    json_int(doc,"mux_high_water_packets",job->stats.mux_high_water); json_int(doc,"mux_timeout_releases",job->stats.mux_timeouts);
    json_int(doc,"mux_pressure_releases",job->stats.mux_pressure);
    json_int(doc,"mux_forced_releases",job->stats.mux_forced);
    int64_t current=now_us(), end=job->lifecycle.ended_us ? job->lifecycle.ended_us : current;
    if(job->progress.last_output_us) json_num(doc,"last_output_age_ms",(current-job->progress.last_output_us)/1000.0);
    else yyjson_mut_obj_add_null(doc,yyjson_mut_doc_get_root(doc),"last_output_age_ms");
    if(job->progress.last_i_output_us) json_num(doc,"last_i_output_age_ms",(current-job->progress.last_i_output_us)/1000.0);
    else yyjson_mut_obj_add_null(doc,yyjson_mut_doc_get_root(doc),"last_i_output_age_ms");
    json_num(doc,"write_in_progress_ms",job->progress.write_started_us ? (current-job->progress.write_started_us)/1000.0 : 0);
    json_str(doc,"write_phase",naki_write_phase_name(job->progress.write_phase));
    json_num(doc,"auto_stats_age_ms",job->progress.auto_updated_us ? (current-job->progress.auto_updated_us)/1000.0 : 0);
    add_latencies(doc,yyjson_mut_doc_get_root(doc),&job->stats.lifetime);
    yyjson_mut_obj_add_val(doc,yyjson_mut_doc_get_root(doc),"frame_types",group_json(doc,&job->stats.groups));
    yyjson_mut_val *network=yyjson_mut_obj(doc);
    yyjson_mut_obj_add_bool(doc,network,"enabled",job->config.network_trace);
    yyjson_mut_obj_add_str(doc,network,"boundary","plain_rtmp_first_chunk_recv_complete_to_actual_mux_flush");
    yyjson_mut_obj_add_uint(doc,network,"deadline_us",300000);
    yyjson_mut_obj_add_uint(doc,network,"exceeded",job->stats.network_deadline_exceeded);
    yyjson_mut_obj_add_uint(doc,network,"encoder_future_max",job->stats.encoder_future_max);
    yyjson_mut_obj_add_val(doc,network,"frame_types",group_json(doc,&job->stats.network_groups));
    yyjson_mut_obj_add_val(doc,yyjson_mut_doc_get_root(doc),"network_read",network);
    yyjson_mut_val *i_stages=yyjson_mut_obj(doc);
    add_latencies(doc,i_stages,&job->stats.i_lifetime);
    yyjson_mut_obj_add_val(doc,yyjson_mut_doc_get_root(doc),"i_frame_stages",i_stages);
    yyjson_mut_val *key_stages=yyjson_mut_obj(doc); add_latencies(doc,key_stages,&job->stats.key_lifetime);
    yyjson_mut_obj_add_val(doc,yyjson_mut_doc_get_root(doc),"output_key_stages",key_stages);
    yyjson_mut_val *window=yyjson_mut_obj(doc);
    double seconds=(end-job->progress.window_start_us)/1000000.0;
    yyjson_mut_obj_add_real(doc,window,"duration_s",seconds>0 ? seconds : 0);
    yyjson_mut_obj_add_real(doc,window,"input_fps",seconds>0 ? (job->stats.packets_in-job->stats.window_start_input)/seconds : 0);
    yyjson_mut_obj_add_real(doc,window,"output_fps",seconds>0 ? (job->stats.packets_out-job->stats.window_start_output)/seconds : 0);
    add_latencies(doc,window,&job->stats.window);
    yyjson_mut_obj_add_val(doc,window,"frame_types",group_json(doc,&job->stats.window_groups));
    yyjson_mut_obj_add_uint(doc,window,"network_deadline_exceeded",job->stats.network_window_exceeded);
    yyjson_mut_obj_add_val(doc,window,"network_frame_types",group_json(doc,&job->stats.window_network_groups));
    i_stages=yyjson_mut_obj(doc); add_latencies(doc,i_stages,&job->stats.i_window);
    yyjson_mut_obj_add_val(doc,window,"i_frame_stages",i_stages);
    key_stages=yyjson_mut_obj(doc); add_latencies(doc,key_stages,&job->stats.key_window);
    yyjson_mut_obj_add_val(doc,window,"output_key_stages",key_stages);
    yyjson_mut_obj_add_val(doc,yyjson_mut_doc_get_root(doc),"window",window);
    if(report) {
        memset(&job->stats.window,0,sizeof(job->stats.window)); memset(&job->stats.window_groups,0,sizeof(job->stats.window_groups));
        memset(&job->stats.window_network_groups,0,sizeof(job->stats.window_network_groups));job->stats.network_window_exceeded=0;
        memset(&job->stats.i_window,0,sizeof(job->stats.i_window));
        memset(&job->stats.key_window,0,sizeof(job->stats.key_window));
        job->progress.window_start_us=end; job->stats.window_start_input=job->stats.packets_in; job->stats.window_start_output=job->stats.packets_out;
    }
    yyjson_mut_val *a = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_int(doc, a, "target", job->progress.auto_stats.target);
    yyjson_mut_obj_add_int(doc, a, "peak_target", job->progress.auto_stats.peak_target);
    yyjson_mut_obj_add_int(doc, a, "minimum", job->progress.auto_stats.minimum);
    yyjson_mut_obj_add_int(doc, a, "maximum", job->progress.auto_stats.maximum);
    yyjson_mut_obj_add_int(doc, a, "budget_slots", job->progress.auto_stats.budget_slots);
    yyjson_mut_obj_add_int(doc, a, "inflight", job->progress.auto_stats.inflight);
    yyjson_mut_obj_add_int(doc, a, "input_queue", job->progress.auto_stats.input_queue);
    yyjson_mut_obj_add_int(doc, a, "output_queue", job->progress.auto_stats.output_queue);
    yyjson_mut_obj_add_uint(doc, a, "increases", job->progress.auto_stats.increases);
    yyjson_mut_obj_add_uint(doc, a, "decreases", job->progress.auto_stats.decreases);
    yyjson_mut_obj_add_val(doc, yyjson_mut_doc_get_root(doc), "auto", a);
    const AVCodecEarlyOutputStats *early=&job->progress.early_stats;
    yyjson_mut_val *observation=yyjson_mut_obj(doc),*modes=yyjson_mut_obj(doc),*decisions=yyjson_mut_obj(doc);
    yyjson_mut_obj_add_bool(doc,observation,"available",job->progress.early_stats_available);
    yyjson_mut_obj_add_str(doc,observation,"scope","codec_selected_lifetime_not_muxed;flush_cancel_may_discard");
    yyjson_mut_obj_add_int(doc,observation,"api_version",early->version);
    yyjson_mut_obj_add_int(doc,observation,"requested_poc",early->requested_poc);
    yyjson_mut_obj_add_int(doc,observation,"requested_idr",early->requested_idr);
    yyjson_mut_obj_add_uint(doc,observation,"pictures",early->pictures);
    yyjson_mut_obj_add_uint(doc,observation,"candidate_pictures",early->candidate_pictures);
    yyjson_mut_obj_add_uint(doc,observation,"suppressed_duplicates",early->suppressed_frames);
    yyjson_mut_obj_add_str(doc,observation,"last_worker_decision",job->progress.early_stats_available?
        avcodec_early_output_reason_name(early->last_reason):"unavailable");
    const char *mode_names[]={"ordinary","h264_hrd","h264_verified","hevc_idr","hevc_prefix"};
    uint64_t total=0,fast=0;
    for(int i=0;i<AV_EARLY_MODE_COUNT;i++) {
        yyjson_mut_obj_add_uint(doc,modes,mode_names[i],early->frames[i]);
        total+=early->frames[i];if(i)fast+=early->frames[i];
    }
    yyjson_mut_obj_add_uint(doc,observation,"selected_frames",total);
    yyjson_mut_obj_add_uint(doc,observation,"early_selected_frames",fast);
    for(int i=0;i<AV_EARLY_REASON_COUNT;i++)
        yyjson_mut_obj_add_uint(doc,decisions,avcodec_early_output_reason_name(i),early->decisions[i]);
    yyjson_mut_obj_add_val(doc,observation,"selected_by_mode",modes);
    yyjson_mut_obj_add_val(doc,observation,"picture_decisions",decisions);
    yyjson_mut_obj_add_val(doc,yyjson_mut_doc_get_root(doc),"decoder_early_output",observation);

    pthread_mutex_unlock(&job->mutex);
    if(!job->parent) {
    LoggerStats stats; logger_get_stats(job->logger,&stats);yyjson_mut_val *logging=yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc,logging,"scope","process");yyjson_mut_obj_add_strcpy(doc,logging,"instance_id",naki_process_id());
    yyjson_mut_obj_add_uint(doc,logging,"event_sequence",stats.event_sequence);
    yyjson_mut_obj_add_uint(doc,logging,"queued",stats.queued);
    yyjson_mut_obj_add_uint(doc,logging,"capacity",stats.capacity);
    yyjson_mut_obj_add_uint(doc,logging,"high_water",stats.high_water);
    yyjson_mut_obj_add_uint(doc,logging,"dropped_frames",stats.dropped_frames);
    yyjson_mut_obj_add_uint(doc,logging,"dropped_status",stats.dropped_status);
    yyjson_mut_obj_add_uint(doc,logging,"io_errors",stats.io_errors);
    yyjson_mut_obj_add_uint(doc,logging,"rotations",stats.rotations);
    yyjson_mut_obj_add_val(doc,yyjson_mut_doc_get_root(doc),"logging",logging);
    }
    if(job->network)yyjson_mut_obj_add_val(doc,yyjson_mut_doc_get_root(doc),"network",network_stats_json(doc,job->network,report));
    if(job->config.output_count) {
        yyjson_mut_val *outputs=yyjson_mut_arr(doc);unsigned failed=0,completed=0;uint64_t packets=0,bytes=0,filtered=0,sei_written=0,sei_protected=0;
        for(int i=0;i<job->config.output_count;i++) {
            Job *b=job->outputs[i];if(!b)continue;
            pthread_mutex_lock(&b->mutex);
            if(!*b->output_id){pthread_mutex_unlock(&b->mutex);continue;}
            if(b->remote_snapshot) {
                yyjson_mut_doc *remote=remote_brief(b->remote_snapshot);
                if(remote){yyjson_mut_obj_remove_key(yyjson_mut_doc_get_root(remote),"state");if(*b->lifecycle.error)yyjson_mut_obj_remove_key(yyjson_mut_doc_get_root(remote),"error");json_str(remote,"state",naki_job_state_name(b->lifecycle.state));if(*b->lifecycle.error)json_str(remote,"error",b->lifecycle.error);json_int(remote,"rendition_pid",b->rendition_pid);json_int(remote,"rendition_generation",b->rendition_generation);json_int(remote,"exit_status_known",b->progress.rendition_exit_known);json_int(remote,"exit_code",b->progress.rendition_exit_code);json_int(remote,"exit_signal",b->progress.rendition_exit_signal);
                    json_int(remote,"raw_video_retained",b->progress.raw_video_retained);json_int(remote,"raw_audio_retained",b->progress.raw_audio_retained);
                    json_int(remote,"queue_video_items",b->stats.branch_video_queued);json_int(remote,"queue_audio_packets",b->stats.branch_audio_queued);json_int(remote,"queue_bytes",b->stats.branch_queue_bytes);json_int(remote,"queue_work_us",b->stats.branch_queue_media_us);
                    json_int(remote,"automatic_restart_attempts",b->progress.automatic_restart_attempts);json_int(remote,"automatic_restart_at_us",b->progress.automatic_restart_at_us);
                    packets+=b->stats.packets_out;bytes+=b->stats.bytes_out;filtered+=b->stats.frames_filtered;sei_written+=b->stats.sei_written;sei_protected+=b->stats.sei_protected;failed+=b->lifecycle.branch_failed;completed+=b->lifecycle.state==NAKI_JOB_STATE_COMPLETED;
                    yyjson_mut_arr_add_val(outputs,yyjson_mut_val_mut_copy(doc,yyjson_mut_doc_get_root(remote)));yyjson_mut_doc_free(remote);}
                pthread_mutex_unlock(&b->mutex);continue;
            }
            pthread_mutex_unlock(&b->mutex);
            yyjson_mut_doc *bd=new_event("output_status",b);pthread_mutex_lock(&b->mutex);
            if(b->remote_rendition){json_int(bd,"rendition_pid",b->rendition_pid);json_int(bd,"rendition_generation",b->rendition_generation);json_int(bd,"exit_status_known",b->progress.rendition_exit_known);json_int(bd,"exit_code",b->progress.rendition_exit_code);json_int(bd,"exit_signal",b->progress.rendition_exit_signal);}
            json_str(bd,"state",naki_job_state_name(b->lifecycle.state));json_str(bd,"error",b->lifecycle.error);
            yyjson_mut_obj_add_val(bd,yyjson_mut_doc_get_root(bd),"progress",progress_json(bd,b));
            yyjson_mut_obj_add_val(bd,yyjson_mut_doc_get_root(bd),"video_rate",video_rate_json(bd,b));
            yyjson_mut_obj_add_val(bd,yyjson_mut_doc_get_root(bd),"first_frame_checkpoints",first_frame_json(bd,b));
            yyjson_mut_obj_add_val(bd,yyjson_mut_doc_get_root(bd),"output_finalize",finalize_json(bd,b));
            json_int(bd,"frames_decoded",b->stats.frames_decoded);json_int(bd,"frames_filtered",b->stats.frames_filtered);
            json_int(bd,"frames_dropped_rate",b->stats.frames_dropped_rate);
            json_int(bd,"packets_out",b->stats.packets_out);json_int(bd,"bytes_out",b->stats.bytes_out);
            json_int(bd,"audio_tracks",b->stats.audio_tracks);json_int(bd,"audio_packets_out",b->stats.audio_packets_out);json_int(bd,"audio_dropped",b->stats.audio_dropped);
            json_int(bd,"audio_unsupported",b->stats.audio_unsupported);
            json_int(bd,"sei_extracted",b->stats.sei_extracted);json_int(bd,"sei_written",b->stats.sei_written);json_int(bd,"sei_protected_frames",b->stats.sei_protected);
            yyjson_mut_val *types=yyjson_mut_arr(bd);
            for(unsigned k=0;k<b->config.sei.count;k++)yyjson_mut_arr_add_uint(bd,types,b->config.sei.types[k]);
            yyjson_mut_obj_add_val(bd,yyjson_mut_doc_get_root(bd),"sei_types",types);
            sei_written+=b->stats.sei_written;sei_protected+=b->stats.sei_protected;
            json_int(bd,"queue_items",b->stats.branch_queued);json_int(bd,"queue_bytes",b->stats.branch_queue_bytes);json_int(bd,"queue_media_us",b->stats.branch_queue_media_us);
            json_int(bd,"queued_work_us",b->stats.branch_queue_media_us);json_int(bd,"queue_video_items",b->stats.branch_video_queued);json_int(bd,"queue_audio_packets",b->stats.branch_audio_queued);
            json_int(bd,"queue_video_high",b->stats.branch_video_high);json_int(bd,"queue_audio_high",b->stats.branch_audio_high);
            json_str(bd,"queue_rejection_reason",naki_branch_reason_name(b->stats.branch_rejection_reason));
            yyjson_mut_val *limits=yyjson_mut_obj(bd),*rejections=yyjson_mut_obj(bd);const OutputConfig *limit=b->remote_rendition?&b->config.outputs[0]:&job->config.outputs[i];
            yyjson_mut_obj_add_int(bd,limits,"video_items",limit->queue_items);yyjson_mut_obj_add_int(bd,limits,"audio_packets",limit->audio_packets);
            yyjson_mut_obj_add_int(bd,limits,"total_items",limit->queue_items+limit->audio_packets);yyjson_mut_obj_add_uint(bd,limits,"bytes",limit->queue_bytes);
            yyjson_mut_obj_add_sint(bd,limits,"nominal_work_us",(int64_t)limit->queue_media_ms*1000);yyjson_mut_obj_add_sint(bd,limits,"age_us",(int64_t)limit->max_lag_ms*1000);
            for(unsigned reason=1;reason<NAKI_BRANCH_REASONS;reason++)yyjson_mut_obj_add_uint(bd,rejections,naki_branch_reason_name(reason),b->stats.branch_rejected[reason]);
            yyjson_mut_obj_add_val(bd,yyjson_mut_doc_get_root(bd),"queue_limits",limits);yyjson_mut_obj_add_val(bd,yyjson_mut_doc_get_root(bd),"queue_rejected_attempts",rejections);
            json_int(bd,"queue_high_items",b->stats.branch_queue_high);json_int(bd,"inflight_bytes",b->stats.branch_inflight_bytes);json_int(bd,"inflight_frames",b->stats.branch_inflight_frames);
            json_int(bd,"inflight_high_frames",b->stats.branch_inflight_high);json_int(bd,"filter_generations",b->stats.filter_generations);
            json_int(bd,"first_decoded_us",b->progress.first_decoded_us);json_int(bd,"first_video_mux_us",b->progress.first_video_output_us);json_int(bd,"last_video_mux_us",b->progress.last_output_us);
            json_int(bd,"encoder_open_started_us",b->progress.encoder_open_started_us);json_int(bd,"encoder_open_done_us",b->progress.encoder_open_done_us);
            json_int(bd,"output_width",b->progress.fixed_output_width);json_int(bd,"output_height",b->progress.fixed_output_height);json_int(bd,"output_header_done_us",b->progress.output_header_done_us);
            json_num(bd,"write_in_progress_ms",b->progress.write_started_us?(now_us()-b->progress.write_started_us)/1000.0:0);
            yyjson_mut_obj_add_val(bd,yyjson_mut_doc_get_root(bd),"ingress_to_mux",metric_json(bd,&b->stats.lifetime.ingress_to_mux));
            yyjson_mut_obj_add_val(bd,yyjson_mut_doc_get_root(bd),"encode",metric_json(bd,&b->stats.lifetime.encode));
            yyjson_mut_obj_add_val(bd,yyjson_mut_doc_get_root(bd),"filter",metric_json(bd,&b->stats.lifetime.filter));
            yyjson_mut_obj_add_val(bd,yyjson_mut_doc_get_root(bd),"branch_queue",metric_json(bd,&b->stats.lifetime.branch_queue));
            yyjson_mut_obj_add_val(bd,yyjson_mut_doc_get_root(bd),"frame_types",group_json(bd,&b->stats.groups));
            packets+=b->stats.packets_out;bytes+=b->stats.bytes_out;filtered+=b->stats.frames_filtered;failed+=b->lifecycle.branch_failed||(b->lifecycle.state==NAKI_JOB_STATE_FAILED);completed+=(b->lifecycle.state==NAKI_JOB_STATE_COMPLETED);
            pthread_mutex_unlock(&b->mutex);if(b->watermark_snapshot)b->watermark_snapshot(b->watermark,bd);
            yyjson_mut_arr_add_val(outputs,yyjson_mut_val_mut_copy(doc,yyjson_mut_doc_get_root(bd)));yyjson_mut_doc_free(bd);
        }
        yyjson_mut_obj_add_val(doc,yyjson_mut_doc_get_root(doc),"outputs",outputs);
        json_int(doc,"sei_written",sei_written);json_int(doc,"sei_protected_frames",sei_protected);
        json_int(doc,"outputs_failed",failed);json_int(doc,"outputs_completed",completed);
        if(!job->renditions){yyjson_mut_obj_remove_key(yyjson_mut_doc_get_root(doc),"packets_out");json_int(doc,"packets_out",packets);
        yyjson_mut_obj_remove_key(yyjson_mut_doc_get_root(doc),"bytes_out");json_int(doc,"bytes_out",bytes);
        yyjson_mut_obj_remove_key(yyjson_mut_doc_get_root(doc),"frames_filtered");json_int(doc,"frames_filtered",filtered);}
    }
    return doc;
}
yyjson_mut_doc *job_snapshot(Job *job) {return snapshot(job,0,0);}
yyjson_mut_doc *job_report(Job *job,int final) {
    yyjson_mut_doc *doc=snapshot(job,1,final);
    if(doc&&!final)for(int i=0;i<job->config.output_count;i++)if(job->outputs[i]&&!job->outputs[i]->remote_rendition)logger_submit(job->logger,NULL,snapshot(job->outputs[i],1,0));
    return doc;
}
