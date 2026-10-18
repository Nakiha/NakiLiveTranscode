#include "netstats.h"
#include <errno.h>
#include <libavutil/error.h>
#include <libavutil/time.h>

static void clear_counter(NetworkCounter *c) {
#define CLEAR(name) atomic_store_explicit(&c->name,0,memory_order_relaxed)
    CLEAR(calls); CLEAR(bytes); CLEAR(errors); CLEAR(would_block); CLEAR(eof); CLEAR(background_calls);
    CLEAR(sum_us); CLEAR(max_us); CLEAR(syscall_us); CLEAR(last_success_us);
#undef CLEAR
}
void network_stats_reset(NetworkStats *s) {
    for(int i=0;i<4;i++) {clear_counter(&s->total[i]); clear_counter(&s->window[i]);}
}
static void observe_counter(NetworkCounter *c,const AVNetworkObservation *e) {
    uint64_t duration=(uint64_t)(e->completed_us-e->started_us);
    atomic_fetch_add_explicit(&c->calls,1,memory_order_relaxed);
    atomic_fetch_add_explicit(&c->sum_us,duration,memory_order_relaxed);
    if(e->syscall_start_us)atomic_fetch_add_explicit(&c->syscall_us,(uint64_t)(e->completed_us-e->syscall_start_us),memory_order_relaxed);
    uint64_t max=atomic_load_explicit(&c->max_us,memory_order_relaxed);
    while(max<duration && !atomic_compare_exchange_weak_explicit(&c->max_us,&max,duration,memory_order_relaxed,memory_order_relaxed)) {}
    if(e->background)atomic_fetch_add_explicit(&c->background_calls,1,memory_order_relaxed);
    if(e->result>0) {
        atomic_fetch_add_explicit(&c->bytes,(uint64_t)e->result,memory_order_relaxed);
        uint64_t last=atomic_load_explicit(&c->last_success_us,memory_order_relaxed);
        while(last<(uint64_t)e->completed_us && !atomic_compare_exchange_weak_explicit(&c->last_success_us,&last,(uint64_t)e->completed_us,memory_order_relaxed,memory_order_relaxed)) {}
    } else if(e->result==AVERROR(EAGAIN) || e->result==AVERROR(EINTR)) atomic_fetch_add_explicit(&c->would_block,1,memory_order_relaxed);
    else if(e->result<0)atomic_fetch_add_explicit(&c->errors,1,memory_order_relaxed);
    else if(e->protocol==AV_NETWORK_TCP)atomic_fetch_add_explicit(&c->eof,1,memory_order_relaxed);
}
void network_observe(void *opaque,const AVNetworkObservation *e) {
    NetworkStats *s=opaque;
    if(e->version!=AVFORMAT_NETWORK_OBSERVER_VERSION || e->protocol<1 || e->protocol>2 || e->direction<0 || e->direction>1)return;
    int i=(e->protocol-1)*2+e->direction;
    observe_counter(&s->total[i],e); observe_counter(&s->window[i],e);
}
static yyjson_mut_val *counter_json(yyjson_mut_doc *doc,NetworkCounter *c,int reset) {
    yyjson_mut_val *obj=yyjson_mut_obj(doc);
#define READ(name) uint64_t name=reset ? atomic_exchange_explicit(&c->name,0,memory_order_relaxed) : atomic_load_explicit(&c->name,memory_order_relaxed)
    READ(calls); READ(bytes); READ(errors); READ(would_block); READ(eof); READ(background_calls);
    READ(sum_us); READ(max_us); READ(syscall_us); READ(last_success_us);
#undef READ
#define ADD(name) yyjson_mut_obj_add_uint(doc,obj,#name,name)
    ADD(calls); ADD(bytes); ADD(errors); ADD(would_block); ADD(eof); ADD(background_calls);
#undef ADD
    yyjson_mut_obj_add_real(doc,obj,"mean_call_ms",calls ? sum_us/(calls*1000.0) : 0);
    yyjson_mut_obj_add_real(doc,obj,"max_call_ms",max_us/1000.0);
    yyjson_mut_obj_add_real(doc,obj,"syscall_total_ms",syscall_us/1000.0);
    yyjson_mut_obj_add_uint(doc,obj,"last_success_monotonic_us",last_success_us);
    if(last_success_us)yyjson_mut_obj_add_real(doc,obj,"last_success_age_ms",(av_gettime_relative()-(int64_t)last_success_us)/1000.0);
    else yyjson_mut_obj_add_null(doc,obj,"last_success_age_ms");
    return obj;
}
yyjson_mut_val *network_stats_json(yyjson_mut_doc *doc,NetworkStats *s,int reset) {
    static const char *names[]={"tcp_receive","tcp_send","udp_receive","udp_send"};
    yyjson_mut_val *obj=yyjson_mut_obj(doc), *window=yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc,obj,"boundary","socket_api_not_wire_or_frame");
    for(int i=0;i<4;i++) {
        yyjson_mut_obj_add_val(doc,obj,names[i],counter_json(doc,&s->total[i],0));
        yyjson_mut_obj_add_val(doc,window,names[i],counter_json(doc,&s->window[i],reset));
    }
    yyjson_mut_obj_add_val(doc,obj,"window",window);
    return obj;
}
