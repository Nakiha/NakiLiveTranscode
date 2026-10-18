#include "live.h"
#include "crash.h"
#include "frame_policy_config.h"
#include "intent.h"
#include "identity.h"
#include "resource_limits.h"
#include "branch_queue.h"
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <libavutil/time.h>
_Static_assert(sizeof(((Job *)0)->branch_rejected)/sizeof(uint64_t)==NAKI_BRANCH_REASONS,"Branch reason counters mismatch");

int64_t now_us(void) { return av_gettime_relative(); }
void wake_job(void *opaque) {
    Job *job = opaque;
    uint64_t one = 1;
    ssize_t n;
    do { n = write(job->wake_fd, &one, sizeof(one)); } while (n < 0 && errno == EINTR);
    /* EAGAIN means an existing level-triggered wakeup is pending. */
}
void request_stop(Job *job) {
    atomic_store(&job->cancel,1);wake_job(job);
    for(int i=0;i<job->config.output_count;i++)if(job->outputs[i])request_stop(job->outputs[i]);
}
void naki_branch_release(Job *job,uint64_t id,uint64_t bytes) {
    if(!job->parent)return;
    pthread_mutex_lock(&job->mutex);
    if(naki_inflight_remove(&job->branch_pending,NAKI_TRACK_FRAME,id)) {
        if(job->branch_inflight_bytes>=bytes)job->branch_inflight_bytes-=bytes;
        if(job->branch_inflight_frames)job->branch_inflight_frames--;
    }
    pthread_mutex_unlock(&job->mutex);
}
yyjson_mut_doc *new_event(const char *event, const Job *job) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) return NULL;
    yyjson_mut_val *obj = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, obj);
    json_str(doc, "event", event);
    json_int(doc, "unix_us", av_gettime());
    json_int(doc, "monotonic_us", now_us());
    json_int(doc,"process_id",getpid());
    if(*naki_process_id())json_str(doc,"instance_id",naki_process_id());
    if (job) {json_str(doc, "job_id", job->id); json_str(doc,"request_id",job->request_id);json_str(doc,"create_request_id",job->request_id);if(*job->output_id)json_str(doc,"output_id",job->output_id);}
    return doc;
}
void json_num(yyjson_mut_doc *doc, const char *name, double value) {
    if (doc) {yyjson_mut_obj_remove_key(yyjson_mut_doc_get_root(doc),name);yyjson_mut_obj_add_real(doc, yyjson_mut_doc_get_root(doc), name, value);}
}
void json_int(yyjson_mut_doc *doc, const char *name, int64_t value) {
    if (doc) {yyjson_mut_obj_remove_key(yyjson_mut_doc_get_root(doc),name);yyjson_mut_obj_add_sint(doc, yyjson_mut_doc_get_root(doc), name, value);}
}
void json_str(yyjson_mut_doc *doc, const char *name, const char *value) {
    if (doc) {yyjson_mut_obj_remove_key(yyjson_mut_doc_get_root(doc),name);yyjson_mut_obj_add_strcpy(doc, yyjson_mut_doc_get_root(doc), name, value);}
}
void metric_add(Metric *metric, double ms, double slow_ms) {
    metric->count++; metric->sum_ms += ms;
    if (ms > metric->max_ms) metric->max_ms = ms;
    if(ms>=slow_ms) metric->slow_count++;
    int bin=ms<200 ? (int)ms : ms<1000 ? 200+(int)((ms-200)/20) : ms<16000 ? 240+(int)((ms-1000)/1000) : 255;
    if(bin<0)bin=0;
    metric->bins[bin]++;
}
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
        const NakiInFlightEntry *entry=&job->policy_inflight.entries[i];
        packets+=entry->kind==NAKI_TRACK_PACKET;frames+=entry->kind==NAKI_TRACK_FRAME;
        if(entry->kind&&entry->ingress_us>0&&(!oldest||entry->ingress_us<oldest))oldest=entry->ingress_us;
        entry=&job->branch_pending.entries[i];retained+=entry->kind!=0;
        if(entry->kind&&entry->ingress_us>0&&(!oldest||entry->ingress_us<oldest))oldest=entry->ingress_us;
    }
    const char *phase=job->ended_us?job->state:job->stop_requested_us?"draining":*job->media_phase?job->media_phase:!strcmp(job->state,"waiting_assets")?"waiting_assets":"starting";
    yyjson_mut_obj_add_strcpy(d,v,"phase",phase);yyjson_mut_obj_add_sint(d,v,"sampled_monotonic_us",current);
    yyjson_mut_obj_add_str(d,v,"input_scope",job->parent?"branch_owner_admission":"reader_ring_admission");yyjson_mut_obj_add_str(d,v,"frame_scope","video");
    yyjson_mut_obj_add_str(d,v,"decoded_scope","shared_decoder_output");
    progress_time(d,v,"last_input",job->last_input_us,current);progress_time(d,v,"last_decoded",job->last_decoded_us,current);
    progress_time(d,v,"last_encoded",job->last_encoded_us,current);progress_time(d,v,"last_written",job->last_output_us,current);
    progress_time(d,v,"oldest_video_inflight",oldest,current);progress_time(d,v,"oldest_mixed_queue",job->branch_oldest_ingress_us,current);
    yyjson_mut_obj_add_uint(d,v,"tracked_video_packets",packets);yyjson_mut_obj_add_uint(d,v,"tracked_video_frames",frames);yyjson_mut_obj_add_uint(d,v,"retained_branch_frames",retained);
    yyjson_mut_obj_add_uint(d,v,"video_tracker_capacity",NAKI_INFLIGHT_CAPACITY);return v;
}
static yyjson_mut_val *video_rate_json(yyjson_mut_doc *doc,const Job *job) {
    const NakiVideoRate *rate=&job->video_rate;if(rate->value.num<=0)return yyjson_mut_null(doc);
    yyjson_mut_val *value=yyjson_mut_obj(doc),*rc=yyjson_mut_obj(doc);
    yyjson_mut_obj_add_int(doc,value,"num",rate->value.num);yyjson_mut_obj_add_int(doc,value,"den",rate->value.den);
    yyjson_mut_obj_add_str(doc,value,"source",rate->from_fallback?"config_fallback":"av_guess_frame_rate");
    yyjson_mut_obj_add_int(doc,value,"fallback_fps",job->config.fps);
    yyjson_mut_obj_add_sint(doc,value,"nominal_frame_work_us",rate->nominal_frame_work_us);
    yyjson_mut_obj_add_int(doc,value,"nominal_gop_frames",rate->nominal_gop_frames);
    const char *schedule=job->config.policy.intra?"frame_policy":!strcmp(job->config.encoder,"libvvenc")&&strcmp(job->config.encoder_profile,"reference")?"source_pts_2s_grid":"nominal_frame_count_max";
    yyjson_mut_obj_add_str(doc,value,"keyframe_schedule",schedule);
    yyjson_mut_obj_add_int(doc,rc,"num",rate->value.num);yyjson_mut_obj_add_int(doc,rc,"den",rate->value.den);
    yyjson_mut_obj_add_val(doc,value,"rc_nominal_rate",rc);return value;
}
static yyjson_mut_val *finalize_json(yyjson_mut_doc *doc,const Job *job) {
    yyjson_mut_val *value=yyjson_mut_obj(doc);
    yyjson_mut_obj_add_sint(doc,value,"started_us",job->finalize_started_us);
    yyjson_mut_obj_add_sint(doc,value,"finished_us",job->finalize_finished_us);
    yyjson_mut_obj_add_sint(doc,value,"deadline_us",job->finalize_deadline_us);
    yyjson_mut_obj_add_int(doc,value,"primary_error",job->finalize_primary_error);
    yyjson_mut_obj_add_int(doc,value,"trailer_error",job->finalize_trailer_error);
    yyjson_mut_obj_add_int(doc,value,"flush_error",job->finalize_flush_error);
    yyjson_mut_obj_add_int(doc,value,"close_error",job->finalize_close_error);return value;
}
static yyjson_mut_val *first_frame_json(yyjson_mut_doc *doc,const Job *job) {
    yyjson_mut_val *items=yyjson_mut_arr(doc);
    for(int i=0;i<2;i++)if(job->first_chain[i])yyjson_mut_arr_add_val(items,yyjson_mut_val_mut_copy(doc,yyjson_mut_doc_get_root(job->first_chain[i])));
    return items;
}
static yyjson_mut_doc *snapshot(Job *job, int report, int final) {
    yyjson_mut_doc *doc = new_event("status", job);
    if (!doc) return NULL;
    pthread_mutex_lock(&job->mutex);
    if(report && !final && job->ended_us) {pthread_mutex_unlock(&job->mutex); yyjson_mut_doc_free(doc); return NULL;}
    json_str(doc, "state", job->state);
    json_str(doc, "error", job->error);
    json_str(doc, "log_path", job->log_path);
    json_int(doc,"snapshot_generated_us",now_us());
    json_int(doc,"snapshot_generation",(int64_t)++job->snapshot_generation);
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
    json_int(doc,"stop_requested_us",job->stop_requested_us);json_int(doc,"stop_deadline_us",job->stop_deadline_us);
    json_str(doc,"stop_operation_id",job->stop_operation_id);json_str(doc,"stop_origin_request_id",job->stop_origin_request_id);
    yyjson_mut_obj_add_bool(doc,yyjson_mut_doc_get_root(doc),"stop_intent",job->stop_requested_us!=0);
    yyjson_mut_obj_add_val(doc,yyjson_mut_doc_get_root(doc),"output_finalize",finalize_json(doc,job));
    json_int(doc, "packets_in", job->packets_in);
    json_int(doc, "frames_decoded", job->frames_decoded);
    json_int(doc, "frames_filtered", job->frames_filtered);
    json_int(doc, "packets_out", job->packets_out);
    json_int(doc, "bytes_out", job->bytes_out);
    json_num(doc, "elapsed_ms", ((job->ended_us ? job->ended_us : now_us()) - job->started_us) / 1000.0);
    yyjson_mut_val *startup=yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc,startup,"boundary","complete_http_request_handler_to_mux_flush");
    yyjson_mut_obj_add_bool(doc,startup,"prewarmed",job->prewarmed);
    yyjson_mut_obj_add_sint(doc,startup,"request_monotonic_us",job->started_us);
#define CHECKPOINT(name) do { if(job->name##_us) yyjson_mut_obj_add_real(doc,startup,#name "_ms",(job->name##_us-job->started_us)/1000.0); else yyjson_mut_obj_add_null(doc,startup,#name "_ms"); } while(0)
    CHECKPOINT(input_open_started); CHECKPOINT(input_open_done); CHECKPOINT(input_probe_done); CHECKPOINT(decoder_ready);
    CHECKPOINT(first_packet); CHECKPOINT(first_decoded); CHECKPOINT(encoder_open_started); CHECKPOINT(encoder_open_done);
    CHECKPOINT(output_header_done); CHECKPOINT(first_encoder_submit);
#undef CHECKPOINT
    yyjson_mut_obj_add_real(doc,startup,"request_to_dispatch_ms",(job->assigned_us-job->started_us)/1000.0);
    yyjson_mut_obj_add_real(doc,startup,"dispatch_to_worker_ms",(job->worker_received_us-job->assigned_us)/1000.0);
    yyjson_mut_obj_add_real(doc,startup,"worker_boot_ms",(job->worker_ready_us-job->worker_spawned_us)/1000.0);
    if(job->first_video_output_us)yyjson_mut_obj_add_real(doc,startup,"request_to_first_video_mux_ms",(job->first_video_output_us-job->started_us)/1000.0);
    else yyjson_mut_obj_add_null(doc,startup,"request_to_first_video_mux_ms");
    if(job->first_key_output_us)yyjson_mut_obj_add_real(doc,startup,"request_to_first_key_mux_ms",(job->first_key_output_us-job->started_us)/1000.0);
    else yyjson_mut_obj_add_null(doc,startup,"request_to_first_key_mux_ms");
    yyjson_mut_obj_add_val(doc,yyjson_mut_doc_get_root(doc),"startup",startup);
    json_int(doc,"frame_log",job->config.frame_log);
    json_str(doc,"timestamps_mode",job->config.policy.timestamps ? "source_all" : "legacy");
    static const char *const intra_names[]={"legacy","follow_source","periodic_pts_grid","follow_source_with_max_gap"};
    json_str(doc,"intra_mode",intra_names[job->config.policy.intra]);
    json_int(doc,"intra_interval_ms",job->config.policy.interval_ms);
    json_int(doc,"frame_id_last",job->frame_id_last); json_int(doc,"frames_selected",job->frames_selected);
    json_int(doc,"frames_dropped_overload",job->frames_dropped_overload); json_int(doc,"frames_protected",job->frames_protected);
    json_int(doc,"intra_requested",job->intra_requested); json_int(doc,"intra_confirmed",job->intra_confirmed);
    json_int(doc,"timestamp_wraps",job->timestamp_wraps);
    json_int(doc,"sei_extracted",job->sei_extracted);json_int(doc,"sei_written",job->sei_written);
    json_int(doc,"sei_protected_frames",job->sei_protected);
    yyjson_mut_val *sei_types=yyjson_mut_arr(doc);
    for(unsigned i=0;i<job->config.sei.count;i++)yyjson_mut_arr_add_uint(doc,sei_types,job->config.sei.types[i]);
    yyjson_mut_obj_add_val(doc,yyjson_mut_doc_get_root(doc),"sei_types",sei_types);
    json_int(doc,"overload_enabled",job->config.policy.overload_levels!=0); json_int(doc,"overload_level",job->overload.level);
    json_str(doc,"overload_state",job->overload.failed ? "failed" : job->overload.recovering ? "recovering" : job->overload.level ? "degraded" : "normal");
    json_num(doc,"overload_oldest_ingress_age_ms",job->oldest_ingress_age_us/1000.0);
    json_int(doc,"overload_escalations",job->overload.escalations); json_int(doc,"overload_recoveries",job->overload.recoveries);
    json_num(doc,"overload_fail_write_in_progress_ms",job->overload_fail_write_us/1000.0);
    json_str(doc,"overload_fail_write_phase",job->overload_fail_write_phase);
    json_str(doc,"encoder",job->config.encoder);
    json_str(doc,"encoder_preset",job->config.encoder_preset);
    json_str(doc,"encoder_profile",job->config.encoder_profile);
    json_int(doc,"crash_collector_pid",crash_collector_pid());
    json_int(doc,"slow_frame_ms",job->config.slow_frame_ms);
    json_int(doc,"audio_tracks",job->audio_tracks); json_int(doc,"audio_packets_out",job->audio_packets_out);
    json_int(doc,"audio_dropped",job->audio_dropped); json_int(doc,"audio_unsupported",job->audio_unsupported);
    json_int(doc,"mux_wait_ms",job->config.mux_wait_ms);
    json_int(doc,"mux_queued_packets",job->mux_queued_packets); json_int(doc,"mux_queued_bytes",job->mux_queued_bytes);
    json_int(doc,"mux_high_water_packets",job->mux_high_water); json_int(doc,"mux_timeout_releases",job->mux_timeouts);
    json_int(doc,"mux_pressure_releases",job->mux_pressure);
    json_int(doc,"mux_forced_releases",job->mux_forced);
    int64_t current=now_us(), end=job->ended_us ? job->ended_us : current;
    if(job->last_output_us) json_num(doc,"last_output_age_ms",(current-job->last_output_us)/1000.0);
    else yyjson_mut_obj_add_null(doc,yyjson_mut_doc_get_root(doc),"last_output_age_ms");
    if(job->last_i_output_us) json_num(doc,"last_i_output_age_ms",(current-job->last_i_output_us)/1000.0);
    else yyjson_mut_obj_add_null(doc,yyjson_mut_doc_get_root(doc),"last_i_output_age_ms");
    json_num(doc,"write_in_progress_ms",job->write_started_us ? (current-job->write_started_us)/1000.0 : 0);
    json_str(doc,"write_phase",job->write_phase);
    json_num(doc,"auto_stats_age_ms",job->auto_updated_us ? (current-job->auto_updated_us)/1000.0 : 0);
    add_latencies(doc,yyjson_mut_doc_get_root(doc),&job->lifetime);
    yyjson_mut_obj_add_val(doc,yyjson_mut_doc_get_root(doc),"frame_types",group_json(doc,&job->groups));
    yyjson_mut_val *i_stages=yyjson_mut_obj(doc);
    add_latencies(doc,i_stages,&job->i_lifetime);
    yyjson_mut_obj_add_val(doc,yyjson_mut_doc_get_root(doc),"i_frame_stages",i_stages);
    yyjson_mut_val *key_stages=yyjson_mut_obj(doc); add_latencies(doc,key_stages,&job->key_lifetime);
    yyjson_mut_obj_add_val(doc,yyjson_mut_doc_get_root(doc),"output_key_stages",key_stages);
    yyjson_mut_val *window=yyjson_mut_obj(doc);
    double seconds=(end-job->window_start_us)/1000000.0;
    yyjson_mut_obj_add_real(doc,window,"duration_s",seconds>0 ? seconds : 0);
    yyjson_mut_obj_add_real(doc,window,"input_fps",seconds>0 ? (job->packets_in-job->window_start_input)/seconds : 0);
    yyjson_mut_obj_add_real(doc,window,"output_fps",seconds>0 ? (job->packets_out-job->window_start_output)/seconds : 0);
    add_latencies(doc,window,&job->window);
    yyjson_mut_obj_add_val(doc,window,"frame_types",group_json(doc,&job->window_groups));
    i_stages=yyjson_mut_obj(doc); add_latencies(doc,i_stages,&job->i_window);
    yyjson_mut_obj_add_val(doc,window,"i_frame_stages",i_stages);
    key_stages=yyjson_mut_obj(doc); add_latencies(doc,key_stages,&job->key_window);
    yyjson_mut_obj_add_val(doc,window,"output_key_stages",key_stages);
    yyjson_mut_obj_add_val(doc,yyjson_mut_doc_get_root(doc),"window",window);
    if(report) {
        memset(&job->window,0,sizeof(job->window)); memset(&job->window_groups,0,sizeof(job->window_groups));
        memset(&job->i_window,0,sizeof(job->i_window));
        memset(&job->key_window,0,sizeof(job->key_window));
        job->window_start_us=end; job->window_start_input=job->packets_in; job->window_start_output=job->packets_out;
    }
    yyjson_mut_val *a = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_int(doc, a, "target", job->auto_stats.target);
    yyjson_mut_obj_add_int(doc, a, "peak_target", job->auto_stats.peak_target);
    yyjson_mut_obj_add_int(doc, a, "minimum", job->auto_stats.minimum);
    yyjson_mut_obj_add_int(doc, a, "maximum", job->auto_stats.maximum);
    yyjson_mut_obj_add_int(doc, a, "budget_slots", job->auto_stats.budget_slots);
    yyjson_mut_obj_add_int(doc, a, "inflight", job->auto_stats.inflight);
    yyjson_mut_obj_add_int(doc, a, "input_queue", job->auto_stats.input_queue);
    yyjson_mut_obj_add_int(doc, a, "output_queue", job->auto_stats.output_queue);
    yyjson_mut_obj_add_uint(doc, a, "increases", job->auto_stats.increases);
    yyjson_mut_obj_add_uint(doc, a, "decreases", job->auto_stats.decreases);
    yyjson_mut_obj_add_val(doc, yyjson_mut_doc_get_root(doc), "auto", a);
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
            yyjson_mut_doc *bd=new_event("output_status",b);pthread_mutex_lock(&b->mutex);
            json_str(bd,"state",b->state);json_str(bd,"error",b->error);
            yyjson_mut_obj_add_val(bd,yyjson_mut_doc_get_root(bd),"progress",progress_json(bd,b));
            yyjson_mut_obj_add_val(bd,yyjson_mut_doc_get_root(bd),"video_rate",video_rate_json(bd,b));
            yyjson_mut_obj_add_val(bd,yyjson_mut_doc_get_root(bd),"first_frame_checkpoints",first_frame_json(bd,b));
            yyjson_mut_obj_add_val(bd,yyjson_mut_doc_get_root(bd),"output_finalize",finalize_json(bd,b));
            json_int(bd,"frames_decoded",b->frames_decoded);json_int(bd,"frames_filtered",b->frames_filtered);
            json_int(bd,"packets_out",b->packets_out);json_int(bd,"bytes_out",b->bytes_out);
            json_int(bd,"audio_tracks",b->audio_tracks);json_int(bd,"audio_packets_out",b->audio_packets_out);json_int(bd,"audio_dropped",b->audio_dropped);
            json_int(bd,"audio_unsupported",b->audio_unsupported);
            json_int(bd,"sei_extracted",b->sei_extracted);json_int(bd,"sei_written",b->sei_written);json_int(bd,"sei_protected_frames",b->sei_protected);
            yyjson_mut_val *types=yyjson_mut_arr(bd);
            for(unsigned k=0;k<b->config.sei.count;k++)yyjson_mut_arr_add_uint(bd,types,b->config.sei.types[k]);
            yyjson_mut_obj_add_val(bd,yyjson_mut_doc_get_root(bd),"sei_types",types);
            sei_written+=b->sei_written;sei_protected+=b->sei_protected;
            json_int(bd,"queue_items",b->branch_queued);json_int(bd,"queue_bytes",b->branch_queue_bytes);json_int(bd,"queue_media_us",b->branch_queue_media_us);
            json_int(bd,"queued_work_us",b->branch_queue_media_us);json_int(bd,"queue_video_items",b->branch_video_queued);json_int(bd,"queue_audio_packets",b->branch_audio_queued);
            json_int(bd,"queue_video_high",b->branch_video_high);json_int(bd,"queue_audio_high",b->branch_audio_high);
            json_str(bd,"queue_rejection_reason",naki_branch_reason_name(b->branch_rejection_reason));
            yyjson_mut_val *limits=yyjson_mut_obj(bd),*rejections=yyjson_mut_obj(bd);const OutputConfig *limit=&job->config.outputs[i];
            yyjson_mut_obj_add_int(bd,limits,"video_items",limit->queue_items);yyjson_mut_obj_add_int(bd,limits,"audio_packets",limit->audio_packets);
            yyjson_mut_obj_add_int(bd,limits,"total_items",limit->queue_items+limit->audio_packets);yyjson_mut_obj_add_uint(bd,limits,"bytes",limit->queue_bytes);
            yyjson_mut_obj_add_sint(bd,limits,"nominal_work_us",(int64_t)limit->queue_media_ms*1000);yyjson_mut_obj_add_sint(bd,limits,"age_us",(int64_t)limit->max_lag_ms*1000);
            for(unsigned reason=1;reason<NAKI_BRANCH_REASONS;reason++)yyjson_mut_obj_add_uint(bd,rejections,naki_branch_reason_name(reason),b->branch_rejected[reason]);
            yyjson_mut_obj_add_val(bd,yyjson_mut_doc_get_root(bd),"queue_limits",limits);yyjson_mut_obj_add_val(bd,yyjson_mut_doc_get_root(bd),"queue_rejected_attempts",rejections);
            json_int(bd,"queue_high_items",b->branch_queue_high);json_int(bd,"inflight_bytes",b->branch_inflight_bytes);json_int(bd,"inflight_frames",b->branch_inflight_frames);
            json_int(bd,"inflight_high_frames",b->branch_inflight_high);json_int(bd,"filter_generations",b->filter_generations);
            json_int(bd,"first_decoded_us",b->first_decoded_us);json_int(bd,"first_video_mux_us",b->first_video_output_us);json_int(bd,"last_video_mux_us",b->last_output_us);
            json_int(bd,"encoder_open_started_us",b->encoder_open_started_us);json_int(bd,"encoder_open_done_us",b->encoder_open_done_us);
            json_int(bd,"output_width",b->fixed_output_width);json_int(bd,"output_height",b->fixed_output_height);json_int(bd,"output_header_done_us",b->output_header_done_us);
            json_num(bd,"write_in_progress_ms",b->write_started_us?(now_us()-b->write_started_us)/1000.0:0);
            yyjson_mut_obj_add_val(bd,yyjson_mut_doc_get_root(bd),"ingress_to_mux",metric_json(bd,&b->lifetime.ingress_to_mux));
            yyjson_mut_obj_add_val(bd,yyjson_mut_doc_get_root(bd),"encode",metric_json(bd,&b->lifetime.encode));
            yyjson_mut_obj_add_val(bd,yyjson_mut_doc_get_root(bd),"filter",metric_json(bd,&b->lifetime.filter));
            yyjson_mut_obj_add_val(bd,yyjson_mut_doc_get_root(bd),"branch_queue",metric_json(bd,&b->lifetime.branch_queue));
            yyjson_mut_obj_add_val(bd,yyjson_mut_doc_get_root(bd),"frame_types",group_json(bd,&b->groups));
            packets+=b->packets_out;bytes+=b->bytes_out;filtered+=b->frames_filtered;failed+=b->branch_failed||!strcmp(b->state,"failed");completed+=!strcmp(b->state,"completed");
            pthread_mutex_unlock(&b->mutex);if(b->watermark_snapshot)b->watermark_snapshot(b->watermark,bd);
            yyjson_mut_arr_add_val(outputs,yyjson_mut_val_mut_copy(doc,yyjson_mut_doc_get_root(bd)));yyjson_mut_doc_free(bd);
        }
        yyjson_mut_obj_add_val(doc,yyjson_mut_doc_get_root(doc),"outputs",outputs);
        json_int(doc,"sei_written",sei_written);json_int(doc,"sei_protected_frames",sei_protected);
        json_int(doc,"outputs_failed",failed);json_int(doc,"outputs_completed",completed);
        yyjson_mut_obj_remove_key(yyjson_mut_doc_get_root(doc),"packets_out");json_int(doc,"packets_out",packets);
        yyjson_mut_obj_remove_key(yyjson_mut_doc_get_root(doc),"bytes_out");json_int(doc,"bytes_out",bytes);
        yyjson_mut_obj_remove_key(yyjson_mut_doc_get_root(doc),"frames_filtered");json_int(doc,"frames_filtered",filtered);
    }
    return doc;
}
yyjson_mut_doc *job_snapshot(Job *job) {return snapshot(job,0,0);}
yyjson_mut_doc *job_report(Job *job,int final) {
    yyjson_mut_doc *doc=snapshot(job,1,final);
    if(doc&&!final)for(int i=0;i<job->config.output_count;i++)if(job->outputs[i])logger_submit(job->logger,NULL,snapshot(job->outputs[i],1,0));
    return doc;
}
static int bad(char *error, size_t cap, const char *message) { snprintf(error, cap, "%s", message); return -1; }
int config_parse(const char *body, size_t size, Config *out, char *error, size_t cap) {
    yyjson_doc *doc = yyjson_read(body, size, 0);
    if (!doc) return bad(error, cap, "Invalid JSON");
    yyjson_val *root = yyjson_doc_get_root(doc);
    Config c = {.bitrate=1500000, .max_threads=12, .min_threads=2, .budget_slots=12,
                .queue_capacity=4, .encoder_threads=2, .fps=25, .frame_log_every=1, .slow_frame_ms=200,
                .audio_copy=1, .mux_wait_ms=50};
    strcpy(c.output_format, "mpegts");
    strcpy(c.encoder, "libx264");
    strcpy(c.single_output_id,"main");
    int ret = -1;
    if(naki_intent_validate(root,error,cap))goto end;
    if (!yyjson_is_obj(root)) { bad(error, cap, "Expected a JSON object"); goto end; }
    const char *seen[32]; size_t count = 0;
    yyjson_obj_iter iter = yyjson_obj_iter_with(root);
    yyjson_val *key;
    while ((key = yyjson_obj_iter_next(&iter))) {
        const char *name = yyjson_get_str(key);
        yyjson_val *val = yyjson_obj_iter_get_val(key);
        if (strlen(name) != yyjson_get_len(key)) { bad(error, cap, "NUL in key"); goto end; }
        for (size_t i=0; i<count; i++) if (!strcmp(seen[i], name)) { bad(error, cap, "Duplicate key"); goto end; }
        if (count == 32) { bad(error, cap, "Too many keys"); goto end; }
        seen[count++] = name;
        if(!strcmp(name,"timestamps") || !strcmp(name,"intra") || !strcmp(name,"frame_selection") || !strcmp(name,"watermark") || !strcmp(name,"outputs") || !strcmp(name,"sei"))continue;
        char *dest = NULL; size_t len = 0;
        if (!strcmp(name,"input")) { dest=c.input; len=sizeof(c.input); }
        else if (!strcmp(name,"output")) { dest=c.output; len=sizeof(c.output); }
        else if (!strcmp(name,"output_format")) { dest=c.output_format; len=sizeof(c.output_format); }
        else if (!strcmp(name,"encoder")) { dest=c.encoder; len=sizeof(c.encoder); }
        else if (!strcmp(name,"encoder_preset")) { dest=c.encoder_preset; len=sizeof(c.encoder_preset); }
        else if (!strcmp(name,"encoder_profile")) { dest=c.encoder_profile; len=sizeof(c.encoder_profile); }
        if (dest) {
            if (!yyjson_is_str(val) || !yyjson_get_len(val) || yyjson_get_len(val)>=len ||
                strlen(yyjson_get_str(val)) != yyjson_get_len(val)) { bad(error,cap,"Invalid string field"); goto end; }
            strcpy(dest, yyjson_get_str(val)); continue;
        }
        if (!strcmp(name,"pace") || !strcmp(name,"frame_log") || !strcmp(name,"audio_copy")) {
            if (!yyjson_is_bool(val)) { bad(error,cap,"Expected boolean"); goto end; }
            if(!strcmp(name,"pace"))c.pace=yyjson_get_bool(val);
            else if(!strcmp(name,"audio_copy"))c.audio_copy=yyjson_get_bool(val);
            else c.frame_log=yyjson_get_bool(val);
            continue;
        }
        int *field = NULL, min=0, max=0;
        if (!strcmp(name,"width")) {field=&c.width; max=8192;}
        else if (!strcmp(name,"height")) {field=&c.height; max=8192;}
        else if (!strcmp(name,"bitrate")) {field=&c.bitrate; min=10000; max=100000000;}
        else if (!strcmp(name,"max_threads")) {field=&c.max_threads; min=2; max=64;}
        else if (!strcmp(name,"min_threads")) {field=&c.min_threads; min=1; max=64;}
        else if (!strcmp(name,"budget_slots")) {field=&c.budget_slots; min=1; max=256;}
        else if (!strcmp(name,"queue_capacity")) {field=&c.queue_capacity; min=1; max=64;}
        else if (!strcmp(name,"encoder_threads")) {field=&c.encoder_threads; min=1; max=16;}
        else if (!strcmp(name,"fps")) {field=&c.fps; min=1; max=240;}
        else if (!strcmp(name,"frame_log_every")) {field=&c.frame_log_every; min=1; max=1000000;}
        else if (!strcmp(name,"slow_frame_ms")) {field=&c.slow_frame_ms; min=1; max=600000;}
        else if (!strcmp(name,"mux_wait_ms")) {field=&c.mux_wait_ms; min=1; max=1000;}
        else { bad(error,cap,"Unknown configuration key"); goto end; }
        if (!yyjson_is_int(val) || yyjson_get_sint(val)<min || yyjson_get_sint(val)>max) {bad(error,cap,"Integer out of range"); goto end;}
        *field=(int)yyjson_get_sint(val);
    }
    int multi=naki_outputs_parse(root,&c,error,cap);
    if(multi<0)goto end;
    if(multi) {*out=c;ret=0;goto end;}
    if (!*c.input || !*c.output) {bad(error,cap,"input and output are required"); goto end;}
    if(naki_policy_config_parse(root,&c.policy,error,cap))goto end;
    if(naki_watermark_config(yyjson_obj_get(root,"watermark"),&c.watermark,error,cap))goto end;
    if(naki_sei_config(yyjson_obj_get(root,"sei"),&c.sei,error,cap))goto end;
    int vvc=!strcmp(c.encoder,"libvvenc");
    if (!vvc && strcmp(c.encoder,"libx264")) {bad(error,cap,"encoder must be libx264 or libvvenc"); goto end;}
    if(!*c.encoder_profile) strcpy(c.encoder_profile,vvc ? "live_3b_2s" : "default");
    if(vvc && c.policy.timestamps==NAKI_TS_SOURCE_ALL && !strcmp(c.encoder_profile,"reference")) {
        bad(error,cap,"source_all requires a live VVenC profile; reference uses a nominal frame time base"); goto end;
    }
    if ((vvc && strcmp(c.encoder_profile,"reference") && strcmp(c.encoder_profile,"live_3b_2s") && strcmp(c.encoder_profile,"live_3b_2s_no_mctf")) ||
        (!vvc && strcmp(c.encoder_profile,"default"))) {bad(error,cap,"Invalid encoder_profile for selected encoder"); goto end;}
    if (!*c.encoder_preset) strcpy(c.encoder_preset,vvc ? "faster" : "veryfast");
    const char *presets=vvc ? "|faster|fast|medium|slow|slower|" : "|ultrafast|superfast|veryfast|faster|fast|medium|slow|slower|veryslow|";
    char preset_key[20]; snprintf(preset_key,sizeof(preset_key),"|%s|",c.encoder_preset);
    if (strchr(c.encoder_preset,'|') || !strstr(presets,preset_key)) {bad(error,cap,"Invalid preset for selected encoder"); goto end;}
    if (c.min_threads>c.max_threads || (!!c.width != !!c.height) || c.width%2 || c.height%2) {bad(error,cap,"Invalid thread limits or dimensions (use two even dimensions)"); goto end;}
    if((uint64_t)c.width*c.height>NAKI_INPUT_MAX_PIXELS) {bad(error,cap,"live-4k-v1 output canvas exceeds 4096x2160 pixels");goto end;}
    if (strcmp(c.output_format,"mpegts") && strcmp(c.output_format,"mp4") && strcmp(c.output_format,"flv")) {bad(error,cap,"output_format must be mpegts, mp4 or flv"); goto end;}
    if (strchr(c.output,':') && strncmp(c.output,"udp://",6) && strncmp(c.output,"tcp://",6) &&
        strncmp(c.output,"rtmp://",7) && strncmp(c.output,"srt://",6) && strncmp(c.output,"http://",7)) {
        bad(error,cap,"Use a local output path or udp://, tcp://, rtmp://, srt://, http:// URL"); goto end;
    }
    if ((!strncmp(c.output,"rtmp://",7) && strcmp(c.output_format,"flv")) ||
        (!strncmp(c.output,"srt://",6) && strcmp(c.output_format,"mpegts"))) {
        bad(error,cap,"rtmp output requires flv; srt output requires mpegts"); goto end;
    }
    if (c.pace && strstr(c.input, "://")) {bad(error,cap,"pace is for local file replay only"); goto end;}
    *out=c; ret=0;
end:
    yyjson_doc_free(doc); return ret;
}
int mkdir_tree(const char *path) {
    char *copy = strdup(path);
    if (!copy) return -1;
    for (char *p=copy+1; *p; p++) if (*p=='/') {
        *p=0;
        if (mkdir(copy,0755)<0 && errno!=EEXIST) {free(copy); return -1;}
        *p='/';
    }
    int ret=mkdir(copy,0755);
    if (ret<0 && errno==EEXIST) {struct stat st; ret=stat(copy,&st)==0 && S_ISDIR(st.st_mode) ? 0 : -1;}
    free(copy); return ret;
}
