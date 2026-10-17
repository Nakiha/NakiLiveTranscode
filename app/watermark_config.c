#include "watermark_config.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int fail(char *e,size_t n,const char *message) {snprintf(e,n,"watermark: %s",message);return -1;}
static int keys(yyjson_val *v,const char *allowed,char *e,size_t n) {
    if(!yyjson_is_obj(v))return fail(e,n,"expected object");
    const char *seen[16];size_t count=0;
    yyjson_obj_iter it=yyjson_obj_iter_with(v);yyjson_val *k;
    while((k=yyjson_obj_iter_next(&it))) {
        const char *s=yyjson_get_str(k);char token[128];
        if(strlen(s)!=yyjson_get_len(k) || strlen(s)>100 || strchr(s,'|'))return fail(e,n,"invalid key");
        snprintf(token,sizeof(token),"|%s|",s);
        if(!strstr(allowed,token))return fail(e,n,"unknown key");
        for(size_t i=0;i<count;i++)if(!strcmp(seen[i],s))return fail(e,n,"duplicate key");
        if(count==16)return fail(e,n,"too many keys");
        seen[count++]=s;
    }
    return 0;
}
static int integer(yyjson_val *v,const char *key,int *out,int min,int max,char *e,size_t n) {
    yyjson_val *x=yyjson_obj_get(v,key);if(!x)return 0;
    if(!yyjson_is_int(x)||yyjson_get_sint(x)<min||yyjson_get_sint(x)>max)return fail(e,n,"integer out of range");
    *out=(int)yyjson_get_sint(x);return 0;
}
static int boolean(yyjson_val *v,const char *key,int *out,char *e,size_t n) {
    yyjson_val *x=yyjson_obj_get(v,key);if(!x)return 0;
    if(!yyjson_is_bool(x))return fail(e,n,"expected boolean");
    *out=yyjson_get_bool(x);return 0;
}
static int fixed(yyjson_val *v,const char *key,const char *expected,char *e,size_t n) {
    yyjson_val *x=yyjson_obj_get(v,key);if(!x)return 0;
    if(!yyjson_is_str(x)||yyjson_get_len(x)!=strlen(expected)||strcmp(yyjson_get_str(x),expected))return fail(e,n,"unsupported policy or unit");
    return 0;
}
int naki_watermark_config(yyjson_val *v,NakiWatermarkConfig *out,char *e,size_t n) {
    NakiWatermarkConfig c={.load_timeout_ms=3000,.startup_timeout_ms=5000,.width=160,.height=80,.anchor=8,.opacity=1};
    if(!v) {*out=c;return 0;}
    if(keys(v,"|enabled||resource||startup||update||placement||animation|",e,n)||boolean(v,"enabled",&c.enabled,e,n))return -1;
    yyjson_val *r=yyjson_obj_get(v,"resource");
    if(r) {
        if(keys(r,"|uri||load_timeout_ms|",e,n)||integer(r,"load_timeout_ms",&c.load_timeout_ms,1,30000,e,n))return -1;
        yyjson_val *u=yyjson_obj_get(r,"uri");
        if(u) {
            if(!yyjson_is_str(u)||!yyjson_get_len(u)||yyjson_get_len(u)>=sizeof(c.uri)||strlen(yyjson_get_str(u))!=yyjson_get_len(u))return fail(e,n,"invalid resource URI");
            strcpy(c.uri,yyjson_get_str(u));
            if(strstr(c.uri,"://")&&strncmp(c.uri,"http://",7)&&strncmp(c.uri,"https://",8))return fail(e,n,"only local paths and HTTP/HTTPS are supported");
        }
    }
    r=yyjson_obj_get(v,"startup");
    if(r&&(keys(r,"|required||timeout_ms||on_timeout|",e,n)||boolean(r,"required",&c.required,e,n)||integer(r,"timeout_ms",&c.startup_timeout_ms,1,30000,e,n)||fixed(r,"on_timeout","fail",e,n)))return -1;
    r=yyjson_obj_get(v,"update");
    if(r&&(keys(r,"|on_error||apply_at|",e,n)||fixed(r,"on_error","keep_previous",e,n)||fixed(r,"apply_at","next_frame",e,n)))return -1;
    r=yyjson_obj_get(v,"animation");
    if(r) {
        int restart=1;
        if(keys(r,"|clock||loop||restart_on_replace|",e,n)||fixed(r,"clock","media_pts",e,n)||fixed(r,"loop","forever",e,n)||boolean(r,"restart_on_replace",&restart,e,n))return -1;
        if(!restart)return fail(e,n,"replacement must restart animation");
    }
    r=yyjson_obj_get(v,"placement");
    if(r) {
        if(keys(r,"|space||anchor||offset||size||opacity|",e,n)||fixed(r,"space","output_canvas",e,n))return -1;
        yyjson_val *a=yyjson_obj_get(r,"anchor");
        if(a) {
            const char *names[]={"top_left","top_center","top_right","center_left","center","center_right","bottom_left","bottom_center","bottom_right"};
            c.anchor=-1;
            for(int i=0;i<9;i++)if(yyjson_is_str(a)&&yyjson_get_len(a)==strlen(names[i])&&!strcmp(yyjson_get_str(a),names[i]))c.anchor=i;
            if(c.anchor<0)return fail(e,n,"invalid anchor");
        }
        a=yyjson_obj_get(r,"offset");
        if(a&&(keys(a,"|x||y||unit|",e,n)||integer(a,"x",&c.x,-8192,8192,e,n)||integer(a,"y",&c.y,-8192,8192,e,n)||fixed(a,"unit","px",e,n)))return -1;
        a=yyjson_obj_get(r,"size");
        if(a) {
            if(keys(a,"|width||height||unit||fit|",e,n)||integer(a,"width",&c.width,1,2048,e,n)||integer(a,"height",&c.height,1,2048,e,n)||fixed(a,"unit","px",e,n))return -1;
            yyjson_val *f=yyjson_obj_get(a,"fit");
            if(f) {
                if(!yyjson_is_str(f)||strlen(yyjson_get_str(f))!=yyjson_get_len(f))return fail(e,n,"invalid fit");
                if(!strcmp(yyjson_get_str(f),"stretch"))c.stretch=1;
                else if(strcmp(yyjson_get_str(f),"contain"))return fail(e,n,"fit must be contain or stretch");
            }
        }
        a=yyjson_obj_get(r,"opacity");
        if(a) {
            if(!yyjson_is_num(a)||!isfinite(yyjson_get_num(a))||yyjson_get_num(a)<0||yyjson_get_num(a)>1)return fail(e,n,"opacity must be 0..1");
            c.opacity=yyjson_get_num(a);
        }
    }
    if(c.enabled&&!*c.uri)return fail(e,n,"enabled requires resource.uri");
    if(c.required&&!c.enabled)return fail(e,n,"required watermark must be enabled");
    *out=c;return 0;
}
int naki_watermark_update(yyjson_val *v,NakiWatermarkConfig *out,uint64_t *revision,char *e,size_t n) {
    if(keys(v,"|revision||watermark|",e,n))return -1;
    yyjson_val *r=yyjson_obj_get(v,"revision"),*w=yyjson_obj_get(v,"watermark");
    if(!yyjson_is_uint(r)||!yyjson_get_uint(r)||yyjson_get_uint(r)>INT64_MAX||!w)return fail(e,n,"revision and watermark are required");
    if(naki_watermark_config(w,out,e,n))return -1;
    *revision=yyjson_get_uint(r);return 0;
}
