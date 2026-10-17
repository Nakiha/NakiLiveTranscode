#include "live.h"
#include "intent.h"
#include "resource_limits.h"
#include <stdlib.h>
#include <string.h>
int naki_logical_output_count(const Config *c) {return c->output_count?c->output_count:1;}
void naki_logical_output(const Config *c,int index,OutputConfig *o) {
    if(c->output_count) {*o=c->outputs[index];return;}
    if(*c->outputs[0].id) {*o=c->outputs[0];return;}
    *o=(OutputConfig){.width=c->width,.height=c->height,.bitrate=c->bitrate,.encoder_threads=c->encoder_threads,
        .audio_copy=c->audio_copy,.mux_wait_ms=c->mux_wait_ms,.watermark=c->watermark,
        .queue_items=16,.audio_packets=256,.queue_bytes=128U*1024*1024,.queue_media_ms=5000,.max_lag_ms=10000,
        .io_timeout_ms=5000,.inflight_bytes=512U*1024*1024};
    strcpy(o->id,*c->single_output_id?c->single_output_id:"main");
    strcpy(o->output,c->output);strcpy(o->output_format,c->output_format);strcpy(o->encoder,c->encoder);
    strcpy(o->encoder_preset,c->encoder_preset);strcpy(o->encoder_profile,c->encoder_profile);
}
static yyjson_mut_val *watermark(yyjson_mut_doc *d,const NakiWatermarkConfig *w) {
    yyjson_mut_val *v=yyjson_mut_obj(d);
#define I(k) yyjson_mut_obj_add_sint(d,v,#k,w->k)
    I(enabled);I(required);I(load_timeout_ms);I(startup_timeout_ms);I(width);I(height);I(x);I(y);I(anchor);I(stretch);
#undef I
    yyjson_mut_obj_add_real(d,v,"opacity",w->opacity);yyjson_mut_obj_add_strcpy(d,v,"uri",w->uri);return v;
}
int naki_config_fingerprint(const Config *c,char out[65],char *error,size_t cap) {
    yyjson_mut_doc *d=yyjson_mut_doc_new(NULL);if(!d)return -1;
    yyjson_mut_val *root=yyjson_mut_obj(d);yyjson_mut_doc_set_root(d,root);
    yyjson_mut_obj_add_str(d,root,"normalization","naki-config-v1");yyjson_mut_obj_add_strcpy(d,root,"input",c->input);
    yyjson_mut_obj_add_str(d,root,"resource_profile",NAKI_LIVE_PROFILE);
#define I(k) yyjson_mut_obj_add_sint(d,root,#k,c->k)
    I(max_threads);I(min_threads);I(budget_slots);I(queue_capacity);I(fps);I(pace);I(frame_log);I(frame_log_every);I(slow_frame_ms);
#undef I
    yyjson_mut_val *policy=yyjson_mut_obj(d),*levels=yyjson_mut_arr(d);
#define I(k) yyjson_mut_obj_add_sint(d,policy,#k,c->policy.k)
    I(timestamps);I(intra);I(interval_ms);I(overload_levels);I(recover_below_ms);I(recover_sustain_ms);I(max_degraded_ms);
#undef I
    for(int i=0;i<c->policy.overload_levels;i++) {
        yyjson_mut_val *level=yyjson_mut_arr(d);yyjson_mut_arr_add_sint(d,level,c->policy.levels[i].enter_ms);
        yyjson_mut_arr_add_sint(d,level,c->policy.levels[i].sustain_ms);yyjson_mut_arr_add_sint(d,level,c->policy.levels[i].keep_one_in);
        yyjson_mut_arr_add_val(levels,level);
    }
    yyjson_mut_obj_add_val(d,policy,"levels",levels);yyjson_mut_obj_add_val(d,root,"policy",policy);
    yyjson_mut_val *outputs=yyjson_mut_arr(d);
    for(int i=0;i<naki_logical_output_count(c);i++) {
        OutputConfig o;naki_logical_output(c,i,&o);yyjson_mut_val *v=yyjson_mut_obj(d);
#define S(k) yyjson_mut_obj_add_strcpy(d,v,#k,o.k)
        S(id);S(output);S(output_format);S(encoder);S(encoder_preset);S(encoder_profile);
#undef S
#define I(k) yyjson_mut_obj_add_sint(d,v,#k,o.k)
        I(width);I(height);I(bitrate);I(encoder_threads);I(audio_copy);I(mux_wait_ms);
        I(queue_items);I(audio_packets);I(queue_media_ms);I(max_lag_ms);I(io_timeout_ms);
#undef I
        yyjson_mut_obj_add_uint(d,v,"queue_bytes",o.queue_bytes);yyjson_mut_obj_add_uint(d,v,"inflight_bytes",o.inflight_bytes);
        yyjson_mut_obj_add_val(d,v,"watermark",watermark(d,&o.watermark));yyjson_mut_arr_add_val(outputs,v);
    }
    yyjson_mut_obj_add_val(d,root,"outputs",outputs);
    size_t size=0;char *text=yyjson_mut_write(d,0,&size);yyjson_mut_doc_free(d);
    yyjson_doc *immutable=text&&size<=65536?yyjson_read(text,size,0):NULL;free(text);
    int ret=immutable?naki_intent_fingerprint(yyjson_doc_get_root(immutable),out,error,cap):-1;
    if(immutable)yyjson_doc_free(immutable);
    return ret;
}
