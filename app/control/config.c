#include "config.h"
#include "frame_policy_config.h"
#include "intent.h"
#include "resource_limits.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
