#include "frame_policy_config.h"
#include <stdio.h>
#include <string.h>

static int object(yyjson_val *value, const char *const *names, size_t count) {
    if(!yyjson_is_obj(value))return -1;
    unsigned seen=0; yyjson_obj_iter it=yyjson_obj_iter_with(value); yyjson_val *key;
    while((key=yyjson_obj_iter_next(&it))) {
        const char *name=yyjson_get_str(key); size_t i;
        if(strlen(name)!=yyjson_get_len(key))return -1;
        for(i=0;i<count;i++)if(!strcmp(name,names[i]))break;
        if(i==count || (seen&(1u<<i)))return -1;
        seen|=1u<<i;
    }
    return 0;
}
static int string_is(yyjson_val *v, const char *value) {
    return yyjson_is_str(v) && yyjson_get_len(v)==strlen(value) && !strcmp(yyjson_get_str(v),value);
}
static int option(yyjson_val *v, const char *key, const char *value) {
    yyjson_val *item=yyjson_obj_get(v,key); return !item || string_is(item,value);
}
static int integer(yyjson_val *v, const char *key, int min, int max, int *out) {
    yyjson_val *item=yyjson_obj_get(v,key);
    if(!yyjson_is_int(item))return -1;
    int64_t value=yyjson_get_sint(item);
    if(value<min || value>max)return -1;
    *out=(int)value; return 0;
}
int naki_policy_config_parse(yyjson_val *root, NakiPolicyConfig *c, char *error, size_t cap) {
    yyjson_val *v=yyjson_obj_get(root,"timestamps");
    if(v) {
        const char *const keys[]={"mode","missing_pts","discontinuity","unwrap","output_precision"};
        if(object(v,keys,5))goto invalid;
        if(string_is(yyjson_obj_get(v,"mode"),"legacy")) {
            if(yyjson_obj_size(v)!=1)goto invalid;
        } else {
            if(!string_is(yyjson_obj_get(v,"mode"),"source_all") || !option(v,"missing_pts","fail") ||
               !option(v,"discontinuity","fail") || !option(v,"unwrap","source_program_clock") ||
               !option(v,"output_precision","container_quantized"))goto invalid;
            c->timestamps=NAKI_TS_SOURCE_ALL;
        }
    }
    v=yyjson_obj_get(root,"intra");
    if(v) {
        const char *const keys[]={"mode","period_ms","max_gap_ms","source_match","source_pts_holes","refresh_type","protect_from_drop"};
        if(object(v,keys,7))goto invalid;
        yyjson_val *mode=yyjson_obj_get(v,"mode");
        if(string_is(mode,"follow_source"))c->intra=NAKI_INTRA_SOURCE;
        else if(string_is(mode,"periodic_pts_grid"))c->intra=NAKI_INTRA_GRID;
        else if(string_is(mode,"follow_source_with_max_gap"))c->intra=NAKI_INTRA_SOURCE_MAX;
        else goto invalid;
        if(!option(v,"source_match","decoded_intra_picture") || !option(v,"source_pts_holes","allow") ||
           !option(v,"refresh_type","encoder_intra"))goto invalid;
        yyjson_val *protect=yyjson_obj_get(v,"protect_from_drop");
        if(protect && (!yyjson_is_bool(protect) || !yyjson_get_bool(protect)))goto invalid;
        if(c->intra==NAKI_INTRA_GRID) {
            if(integer(v,"period_ms",1,3600000,&c->interval_ms) || yyjson_obj_get(v,"max_gap_ms"))goto invalid;
        } else if(c->intra==NAKI_INTRA_SOURCE_MAX) {
            if(integer(v,"max_gap_ms",1,3600000,&c->interval_ms) || yyjson_obj_get(v,"period_ms"))goto invalid;
        } else if(yyjson_obj_get(v,"period_ms") || yyjson_obj_get(v,"max_gap_ms"))goto invalid;
        if(c->timestamps!=NAKI_TS_SOURCE_ALL)goto invalid;
    }
    v=yyjson_obj_get(root,"frame_selection");
    if(v) {
        const char *const keys[]={"mode","overload"};
        if(object(v,keys,2))goto invalid;
        if(string_is(yyjson_obj_get(v,"mode"),"all")) {
            if(yyjson_obj_get(v,"overload"))goto invalid;
            return 0;
        }
        if(!string_is(yyjson_obj_get(v,"mode"),"live_drop_only") || !c->intra)goto invalid;
        yyjson_val *o=yyjson_obj_get(v,"overload");
        const char *const names[]={"signal","levels","recover_below_ms","recover_sustain_ms","max_continuous_degraded_ms","on_exhausted"};
        if(object(o,names,6) || !string_is(yyjson_obj_get(o,"signal"),"oldest_inflight_ingress_age_ms") ||
           !string_is(yyjson_obj_get(o,"on_exhausted"),"fail_task"))goto invalid;
        if(integer(o,"recover_below_ms",1,3600000,&c->recover_below_ms) ||
           integer(o,"recover_sustain_ms",0,3600000,&c->recover_sustain_ms) ||
           integer(o,"max_continuous_degraded_ms",1,86400000,&c->max_degraded_ms))goto invalid;
        if(c->max_degraded_ms<=c->recover_sustain_ms)goto invalid;
        yyjson_val *levels=yyjson_obj_get(o,"levels");
        if(!yyjson_is_arr(levels) || !yyjson_arr_size(levels) || yyjson_arr_size(levels)>NAKI_OVERLOAD_LEVELS)goto invalid;
        c->overload_levels=(int)yyjson_arr_size(levels);
        for(int i=0;i<c->overload_levels;i++) {
            yyjson_val *level=yyjson_arr_get(levels,(size_t)i);
            const char *const fields[]={"enter_ms","sustain_ms","keep_one_in"};
            NakiOverloadLevel *l=&c->levels[i];
            if(object(level,fields,3) || integer(level,"enter_ms",1,3600000,&l->enter_ms) ||
               integer(level,"sustain_ms",0,3600000,&l->sustain_ms) || integer(level,"keep_one_in",2,256,&l->keep_one_in))goto invalid;
            if((i && (l->enter_ms<=c->levels[i-1].enter_ms || l->keep_one_in<=c->levels[i-1].keep_one_in)) ||
               (!i && c->recover_below_ms>=l->enter_ms))goto invalid;
        }
    }
    return 0;
invalid:
    snprintf(error,cap,"Invalid frame policy: explicit source_all, I policy and complete overload thresholds required; unsupported timestamp repair/refresh modes are rejected");
    return -1;
}
