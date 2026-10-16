#include "live.h"
#include "crash.h"
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <libavutil/time.h>

int64_t now_us(void) { return av_gettime_relative(); }
void wake_job(void *opaque) {
    Job *job = opaque;
    uint64_t one = 1;
    ssize_t n;
    do { n = write(job->wake_fd, &one, sizeof(one)); } while (n < 0 && errno == EINTR);
    /* EAGAIN means an existing level-triggered wakeup is pending. */
}
void request_stop(Job *job) { atomic_store(&job->cancel, 1); wake_job(job); }
yyjson_mut_doc *new_event(const char *event, const Job *job) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) return NULL;
    yyjson_mut_val *obj = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, obj);
    json_str(doc, "event", event);
    json_int(doc, "unix_us", av_gettime());
    json_int(doc, "monotonic_us", now_us());
    json_int(doc,"process_id",getpid());
    if (job) {json_str(doc, "job_id", job->id); json_str(doc,"request_id",job->request_id);}
    return doc;
}
void json_num(yyjson_mut_doc *doc, const char *name, double value) {
    if (doc) yyjson_mut_obj_add_real(doc, yyjson_mut_doc_get_root(doc), name, value);
}
void json_int(yyjson_mut_doc *doc, const char *name, int64_t value) {
    if (doc) yyjson_mut_obj_add_sint(doc, yyjson_mut_doc_get_root(doc), name, value);
}
void json_str(yyjson_mut_doc *doc, const char *name, const char *value) {
    if (doc) yyjson_mut_obj_add_strcpy(doc, yyjson_mut_doc_get_root(doc), name, value);
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
    ADD(demux_read); ADD(input_queue); ADD(decode); ADD(setup); ADD(filter); ADD(encode); ADD(mux);
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
static yyjson_mut_doc *snapshot(Job *job, int report, int final) {
    yyjson_mut_doc *doc = new_event("status", job);
    if (!doc) return NULL;
    pthread_mutex_lock(&job->mutex);
    if(report && !final && job->ended_us) {pthread_mutex_unlock(&job->mutex); yyjson_mut_doc_free(doc); return NULL;}
    json_str(doc, "state", job->state);
    json_str(doc, "error", job->error);
    json_str(doc, "log_path", job->log_path);
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
    yyjson_mut_obj_add_real(doc,startup,"request_to_dispatch_ms",(job->assigned_us-job->started_us)/1000.0);
    yyjson_mut_obj_add_real(doc,startup,"dispatch_to_worker_ms",(job->worker_received_us-job->assigned_us)/1000.0);
    yyjson_mut_obj_add_real(doc,startup,"worker_boot_ms",(job->worker_ready_us-job->worker_spawned_us)/1000.0);
    if(job->first_video_output_us)yyjson_mut_obj_add_real(doc,startup,"request_to_first_video_mux_ms",(job->first_video_output_us-job->started_us)/1000.0);
    else yyjson_mut_obj_add_null(doc,startup,"request_to_first_video_mux_ms");
    if(job->first_key_output_us)yyjson_mut_obj_add_real(doc,startup,"request_to_first_key_mux_ms",(job->first_key_output_us-job->started_us)/1000.0);
    else yyjson_mut_obj_add_null(doc,startup,"request_to_first_key_mux_ms");
    yyjson_mut_obj_add_val(doc,yyjson_mut_doc_get_root(doc),"startup",startup);
    json_int(doc,"frame_log",job->config.frame_log);
    json_int(doc,"crash_collector_pid",crash_collector_pid());
    json_int(doc,"slow_frame_ms",job->config.slow_frame_ms);
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
    yyjson_mut_val *window=yyjson_mut_obj(doc);
    double seconds=(end-job->window_start_us)/1000000.0;
    yyjson_mut_obj_add_real(doc,window,"duration_s",seconds>0 ? seconds : 0);
    yyjson_mut_obj_add_real(doc,window,"input_fps",seconds>0 ? (job->packets_in-job->window_start_input)/seconds : 0);
    yyjson_mut_obj_add_real(doc,window,"output_fps",seconds>0 ? (job->packets_out-job->window_start_output)/seconds : 0);
    add_latencies(doc,window,&job->window);
    yyjson_mut_obj_add_val(doc,window,"frame_types",group_json(doc,&job->window_groups));
    i_stages=yyjson_mut_obj(doc); add_latencies(doc,i_stages,&job->i_window);
    yyjson_mut_obj_add_val(doc,window,"i_frame_stages",i_stages);
    yyjson_mut_obj_add_val(doc,yyjson_mut_doc_get_root(doc),"window",window);
    if(report) {
        memset(&job->window,0,sizeof(job->window)); memset(&job->window_groups,0,sizeof(job->window_groups));
        memset(&job->i_window,0,sizeof(job->i_window));
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
    LoggerStats stats; logger_get_stats(job->logger,&stats);
    yyjson_mut_val *logging=yyjson_mut_obj(doc);
    yyjson_mut_obj_add_uint(doc,logging,"queued",stats.queued);
    yyjson_mut_obj_add_uint(doc,logging,"capacity",stats.capacity);
    yyjson_mut_obj_add_uint(doc,logging,"high_water",stats.high_water);
    yyjson_mut_obj_add_uint(doc,logging,"dropped_frames",stats.dropped_frames);
    yyjson_mut_obj_add_uint(doc,logging,"dropped_status",stats.dropped_status);
    yyjson_mut_obj_add_uint(doc,logging,"io_errors",stats.io_errors);
    yyjson_mut_obj_add_uint(doc,logging,"rotations",stats.rotations);
    yyjson_mut_obj_add_val(doc,yyjson_mut_doc_get_root(doc),"logging",logging);
    yyjson_mut_obj_add_val(doc,yyjson_mut_doc_get_root(doc),"network",network_stats_json(doc,job->network,report));
    return doc;
}
yyjson_mut_doc *job_snapshot(Job *job) {return snapshot(job,0,0);}
yyjson_mut_doc *job_report(Job *job,int final) {return snapshot(job,1,final);}
static int bad(char *error, size_t cap, const char *message) { snprintf(error, cap, "%s", message); return -1; }
int config_parse(const char *body, size_t size, Config *out, char *error, size_t cap) {
    yyjson_doc *doc = yyjson_read(body, size, 0);
    if (!doc) return bad(error, cap, "Invalid JSON");
    yyjson_val *root = yyjson_doc_get_root(doc);
    Config c = {.bitrate=1500000, .max_threads=12, .min_threads=2, .budget_slots=12,
                .queue_capacity=4, .encoder_threads=2, .fps=25, .frame_log_every=1, .slow_frame_ms=200};
    strcpy(c.output_format, "mpegts");
    int ret = -1;
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
        char *dest = NULL; size_t len = 0;
        if (!strcmp(name,"input")) { dest=c.input; len=sizeof(c.input); }
        else if (!strcmp(name,"output")) { dest=c.output; len=sizeof(c.output); }
        else if (!strcmp(name,"output_format")) { dest=c.output_format; len=sizeof(c.output_format); }
        if (dest) {
            if (!yyjson_is_str(val) || !yyjson_get_len(val) || yyjson_get_len(val)>=len ||
                strlen(yyjson_get_str(val)) != yyjson_get_len(val)) { bad(error,cap,"Invalid string field"); goto end; }
            strcpy(dest, yyjson_get_str(val)); continue;
        }
        if (!strcmp(name,"pace") || !strcmp(name,"frame_log")) {
            if (!yyjson_is_bool(val)) { bad(error,cap,"Expected boolean"); goto end; }
            if(!strcmp(name,"pace"))c.pace=yyjson_get_bool(val); else c.frame_log=yyjson_get_bool(val);
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
        else { bad(error,cap,"Unknown configuration key"); goto end; }
        if (!yyjson_is_int(val) || yyjson_get_sint(val)<min || yyjson_get_sint(val)>max) {bad(error,cap,"Integer out of range"); goto end;}
        *field=(int)yyjson_get_sint(val);
    }
    if (!*c.input || !*c.output) {bad(error,cap,"input and output are required"); goto end;}
    if (c.min_threads>c.max_threads || (!!c.width != !!c.height) || c.width%2 || c.height%2) {bad(error,cap,"Invalid thread limits or dimensions (use two even dimensions)"); goto end;}
    if (strcmp(c.output_format,"mpegts") && strcmp(c.output_format,"mp4") && strcmp(c.output_format,"flv")) {bad(error,cap,"output_format must be mpegts, mp4 or flv"); goto end;}
    if (strchr(c.output,':') && strncmp(c.output,"udp://",6) && strncmp(c.output,"tcp://",6) &&
        strncmp(c.output,"rtmp://",7) && strncmp(c.output,"http://",7)) {
        bad(error,cap,"Use a local output path or udp://, tcp://, rtmp://, http:// URL"); goto end;
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
