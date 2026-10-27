#include "config.h"
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
static int bad(char *e,size_t n,const char *s) {snprintf(e,n,"outputs: %s",s);return -1;}
void naki_output_config(Config *out,const Config *common,int index) {
    *out=*common;out->output_count=0;memset(out->outputs,0,sizeof(out->outputs));
    const OutputConfig *o=&common->outputs[index];
    strcpy(out->single_output_id,o->id);out->outputs[0]=*o;
    strcpy(out->output,o->output);strcpy(out->output_format,o->output_format);strcpy(out->encoder,o->encoder);
    strcpy(out->encoder_preset,o->encoder_preset);strcpy(out->encoder_profile,o->encoder_profile);
    out->width=o->width;out->height=o->height;out->bitrate=o->bitrate;out->encoder_threads=o->encoder_threads;
    out->audio_copy=o->audio_copy;out->mux_wait_ms=o->mux_wait_ms;out->watermark=o->watermark;out->sei=o->sei;
    out->output_fps=o->output_fps;out->cqf=o->cqf;out->qp=o->qp;out->maxrate=o->maxrate;out->encoder_bit_depth=o->encoder_bit_depth;out->encoder_input_bit_depth=o->encoder_input_bit_depth;out->encoder_native8_reference=o->encoder_native8_reference;
}
int naki_outputs_parse(yyjson_val *root,Config *out,char *e,size_t n) {
    yyjson_val *list=yyjson_obj_get(root,"outputs");if(!list)return 0;
    int isolated=yyjson_obj_get(root,"rendition_backend")!=NULL;
    if(!isolated&&yyjson_is_true(yyjson_obj_get(root,"codec_trace")))return bad(e,n,"codec_trace requires a direct output");
    if(!isolated&&yyjson_is_true(yyjson_obj_get(root,"decoder_pump")))return bad(e,n,"decoder_pump requires a direct output");
    if(!isolated&&(yyjson_is_true(yyjson_obj_get(root,"source_rate_limit"))||yyjson_obj_get(root,"crf")||(yyjson_is_str(yyjson_obj_get(root,"rate_control"))&&!strcmp(yyjson_get_str(yyjson_obj_get(root,"rate_control")),"constrained_crf"))))return bad(e,n,"constrained_crf currently requires a direct output");
    if(!yyjson_is_arr(list)||yyjson_arr_size(list)<1||yyjson_arr_size(list)>4)return bad(e,n,"must contain 1..4 fixed outputs");
    if(yyjson_obj_get(root,"output")||yyjson_obj_get(root,"watermark"))return bad(e,n,"output and watermark belong inside each branch");
    OutputConfig outputs[4]={0};Config first={0};unsigned count=0;int encoder_threads=0;
    size_t ai,am;yyjson_val *v;
    yyjson_arr_foreach(list,ai,am,v) {
        OutputConfig *o=&outputs[count];
        if(yyjson_arr_size(list)==1)strcpy(o->id,"main");
        o->queue_items=16;o->audio_packets=256;o->queue_bytes=128U*1024*1024;o->queue_media_ms=5000;o->max_lag_ms=10000;o->io_timeout_ms=5000;o->inflight_bytes=512U*1024*1024;
        if(!yyjson_is_obj(v))return bad(e,n,"branch must be an object");
        if(yyjson_obj_get(v,"queue_items")&&yyjson_obj_get(v,"video_items"))return bad(e,n,"queue_items and video_items are alternative spellings");
        yyjson_mut_doc *d=yyjson_mut_doc_new(NULL);if(!d)return bad(e,n,"allocation failed");
        yyjson_mut_val *merged=yyjson_val_mut_copy(d,root);yyjson_mut_doc_set_root(d,merged);yyjson_mut_obj_remove_key(merged,"outputs");
        const char *seen[32];size_t seen_count=0,ki,km;yyjson_val *key,*value;int fail=0;
        yyjson_obj_foreach(v,ki,km,key,value) {
            const char *name=yyjson_get_str(key);
            if(strlen(name)!=yyjson_get_len(key)||seen_count==32) {fail=1;break;}
            for(size_t k=0;k<seen_count;k++)if(!strcmp(seen[k],name))fail=1;
            if(fail)break;
            seen[seen_count++]=name;
            if(!strcmp(name,"id")) {
                if(!yyjson_is_str(value)||!yyjson_get_len(value)||yyjson_get_len(value)>32||strlen(yyjson_get_str(value))!=yyjson_get_len(value)) {fail=1;break;}
                strcpy(o->id,yyjson_get_str(value));
                for(const char *p=o->id;*p;p++)if(!isalnum((unsigned char)*p)&&*p!='_'&&*p!='-')fail=1;
                continue;
            }
            uint64_t *wide=NULL;int *integer=NULL;uint64_t low=1,high=0;
            if(!strcmp(name,"queue_items")||!strcmp(name,"video_items")) {integer=&o->queue_items;high=16;}
            else if(!strcmp(name,"audio_packets")) {integer=&o->audio_packets;high=256;}
            else if(!strcmp(name,"queue_bytes")) {wide=&o->queue_bytes;low=4096;high=256U*1024*1024;}
            else if(!strcmp(name,"inflight_bytes")) {wide=&o->inflight_bytes;low=1048576;high=1024U*1024*1024;}
            else if(!strcmp(name,"queue_media_ms")) {integer=&o->queue_media_ms;high=30000;}
            else if(!strcmp(name,"max_lag_ms")) {integer=&o->max_lag_ms;low=100;high=30000;}
            else if(!strcmp(name,"io_timeout_ms")) {integer=&o->io_timeout_ms;low=100;high=10000;}
            if(integer||wide) {
                if(!yyjson_is_uint(value)||yyjson_get_uint(value)<low||yyjson_get_uint(value)>high) {fail=1;break;}
                if(integer)*integer=(int)yyjson_get_uint(value);else *wide=yyjson_get_uint(value);continue;
            }
            const char *allowed[]={"output","output_format","encoder","encoder_preset","encoder_profile","encoder_bit_depth","encoder_input_bit_depth","encoder_native8_reference","width","height","bitrate","encoder_threads","audio_copy","mux_wait_ms","watermark","sei","output_fps","rate_control","qp","maxrate"};
            int allowed_key=0;for(unsigned k=0;k<sizeof(allowed)/sizeof(*allowed);k++)if(!strcmp(allowed[k],name))allowed_key=1;
            if(!allowed_key) {fail=1;break;}
            yyjson_mut_obj_remove_key(merged,name);
            yyjson_mut_obj_add_val(d,merged,name,yyjson_val_mut_copy(d,value));
        }
        size_t bytes=0;char *json=fail?NULL:yyjson_mut_write(d,0,&bytes);Config sub;
        int ret=json?config_parse(json,bytes,&sub,e,n):-1;int allocation_failed=!fail&&!json;free(json);yyjson_mut_doc_free(d);
        if(fail||!*o->id)return bad(e,n,"invalid/duplicate branch field or missing id");
        if(allocation_failed)return bad(e,n,"allocation failed");
        if(ret)return -1;
        for(unsigned k=0;k<count;k++)if(!strcmp(o->id,outputs[k].id)||!strcmp(sub.output,outputs[k].output))return bad(e,n,"duplicate id or destination");
        if((uint64_t)sub.width*sub.height>4096U*2160)return bad(e,n,"output canvas exceeds 4096x2160 pixels");
        if(!count)first=sub;
        strcpy(o->output,sub.output);strcpy(o->output_format,sub.output_format);strcpy(o->encoder,sub.encoder);
        strcpy(o->encoder_profile,sub.encoder_profile);strcpy(o->encoder_preset,sub.encoder_preset);
        o->width=sub.width;o->height=sub.height;o->bitrate=sub.bitrate;o->encoder_threads=sub.encoder_threads;
        o->audio_copy=sub.audio_copy;o->mux_wait_ms=sub.mux_wait_ms;o->watermark=sub.watermark;o->sei=sub.sei;
        o->output_fps=sub.output_fps;o->cqf=sub.cqf;o->qp=sub.qp;o->maxrate=sub.maxrate;o->encoder_bit_depth=sub.encoder_bit_depth;o->encoder_input_bit_depth=sub.encoder_input_bit_depth;o->encoder_native8_reference=sub.encoder_native8_reference;
        encoder_threads+=o->encoder_threads;count++;
    }
    if(encoder_threads>16)return bad(e,n,"sum of encoder_threads must not exceed 16");
    *out=first;out->output_count=count==1&&!isolated?0:(int)count;memcpy(out->outputs,outputs,sizeof(outputs));
    if(count==1) {strcpy(out->single_output_id,outputs[0].id);return 1;}
    memset(&out->watermark,0,sizeof(out->watermark));
    out->sei=(NakiSeiConfig){0};
    for(unsigned k=0;k<count;k++)if(naki_sei_union(&out->sei,&outputs[k].sei))return bad(e,n,"SEI union budget exceeded");
    /* Root's reader must deliver audio if any branch needs it. */
    out->audio_copy=0;for(unsigned k=0;k<count;k++)if(outputs[k].audio_copy&&strcmp(outputs[k].output_format,"mp4"))out->audio_copy=1;
    return 1;
}
