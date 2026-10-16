#include "live.h"
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
    if (job) json_str(doc, "job_id", job->id);
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
int log_json(FILE *file, yyjson_mut_doc *doc) {
    if (!doc) return -1;
    size_t len;
    char *text = yyjson_mut_write(doc, 0, &len);
    yyjson_mut_doc_free(doc);
    if (!text) return -1;
    flockfile(file);
    int ret = fwrite(text, 1, len, file) == len && fputc('\n', file) != EOF && fflush(file) == 0 ? 0 : -1;
    funlockfile(file);
    free(text);
    return ret;
}
void metric_add(Metric *metric, double ms) {
    metric->count++; metric->sum_ms += ms;
    if (ms > metric->max_ms) metric->max_ms = ms;
}
static void add_metric(yyjson_mut_doc *doc, const char *key, const Metric *metric) {
    yyjson_mut_val *obj = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_uint(doc, obj, "count", metric->count);
    yyjson_mut_obj_add_real(doc, obj, "mean_ms", metric->count ? metric->sum_ms / metric->count : 0);
    yyjson_mut_obj_add_real(doc, obj, "max_ms", metric->max_ms);
    yyjson_mut_obj_add_val(doc, yyjson_mut_doc_get_root(doc), key, obj);
}
yyjson_mut_doc *job_snapshot(Job *job) {
    yyjson_mut_doc *doc = new_event("status", job);
    if (!doc) return NULL;
    pthread_mutex_lock(&job->mutex);
    json_str(doc, "state", job->state);
    json_str(doc, "error", job->error);
    json_str(doc, "log_path", job->log_path);
    json_int(doc, "packets_in", job->packets_in);
    json_int(doc, "frames_decoded", job->frames_decoded);
    json_int(doc, "frames_filtered", job->frames_filtered);
    json_int(doc, "packets_out", job->packets_out);
    json_int(doc, "bytes_out", job->bytes_out);
    json_num(doc, "elapsed_ms", ((job->ended_us ? job->ended_us : now_us()) - job->started_us) / 1000.0);
    add_metric(doc, "decode", &job->decode);
    add_metric(doc, "filter", &job->filter);
    add_metric(doc, "encode", &job->encode);
    add_metric(doc, "mux", &job->mux);
    add_metric(doc, "ingress_to_mux", &job->ingress_to_mux);
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
    return doc;
}
static int bad(char *error, size_t cap, const char *message) { snprintf(error, cap, "%s", message); return -1; }
int config_parse(const char *body, size_t size, Config *out, char *error, size_t cap) {
    yyjson_doc *doc = yyjson_read(body, size, 0);
    if (!doc) return bad(error, cap, "Invalid JSON");
    yyjson_val *root = yyjson_doc_get_root(doc);
    Config c = {.bitrate=1500000, .max_threads=12, .min_threads=2, .budget_slots=12,
                .queue_capacity=4, .encoder_threads=2, .fps=25};
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
        if (!strcmp(name,"pace")) {
            if (!yyjson_is_bool(val)) { bad(error,cap,"pace must be boolean"); goto end; }
            c.pace=yyjson_get_bool(val); continue;
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
