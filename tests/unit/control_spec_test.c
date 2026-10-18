#include "worker/job.h"
#include "platform/runtime.h"
#include "worker/frame_policy_runtime.h"
#include "control/intent.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static Config parse(const char *text) {
    Config c;char error[256];int ret=config_parse(text,strlen(text),&c,error,sizeof(error));
    if(ret)fprintf(stderr,"config parse error: %s for %s\n",error,text);
    assert(!ret);return c;
}
static void digest(Config *c,char out[65]) {char error[256];assert(!naki_config_fingerprint(c,out,error,sizeof(error)));}
int main(void) {
    Config legacy=parse("{\"input\":\"input.ts\",\"output\":\"output.ts\"}");
    Config explicit=parse("{\"fps\":25,\"encoder\":\"libx264\",\"bitrate\":1500000,\"input\":\"input.ts\",\"output\":\"output.ts\",\"audio_copy\":true}");
    Config logical=parse("{\"input\":\"input.ts\",\"outputs\":[{\"output\":\"output.ts\"}]}");
    char a[65],b[65];digest(&legacy,a);digest(&explicit,b);assert(!strcmp(a,b));digest(&logical,b);assert(!strcmp(a,b));
    assert(logical.output_count==0&&naki_logical_output_count(&logical)==1&&!strcmp(logical.single_output_id,"main"));
    OutputConfig output;naki_logical_output(&logical,0,&output);assert(!strcmp(output.id,"main")&&output.queue_items==16&&output.audio_packets==256);
    Config quotas=parse("{\"input\":\"input.ts\",\"outputs\":[{\"output\":\"output.ts\",\"video_items\":16,\"audio_packets\":256}]}");
    digest(&quotas,b);assert(!strcmp(a,b));quotas.outputs[0].audio_packets=128;digest(&quotas,b);assert(strcmp(a,b));
    Config named=parse("{\"input\":\"input.ts\",\"outputs\":[{\"id\":\"preview\",\"output\":\"output.ts\"}]}");
    assert(named.output_count==0&&!strcmp(named.single_output_id,"preview"));digest(&named,b);assert(strcmp(a,b));
    explicit.bitrate++;digest(&explicit,b);assert(strcmp(a,b));
    Config sei_alias=parse("{\"input\":\"input.ts\",\"output\":\"output.ts\",\"sei\":{\"types\":[\"uuid\",101,100]}}");
    Config sei_numeric=parse("{\"input\":\"input.ts\",\"outputs\":[{\"output\":\"output.ts\",\"sei\":{\"types\":[100,5,101]}}]}");
    digest(&sei_alias,a);digest(&sei_numeric,b);assert(!strcmp(a,b));
    digest(&legacy,b);assert(strcmp(a,b));
    Config sei_multi=parse("{\"input\":\"input.ts\",\"sei\":{\"types\":[100]},\"outputs\":[{\"id\":\"a\",\"output\":\"a.ts\"},{\"id\":\"b\",\"output\":\"b.ts\",\"sei\":{\"types\":[]}}]}");
    Config branch;naki_output_config(&branch,&sei_multi,0);assert(branch.sei.count==1&&branch.sei.types[0]==100);
    naki_output_config(&branch,&sei_multi,1);assert(!branch.sei.count);
    Config rejected;char error[256];const char *bad="{\"input\":\"i\",\"output\":\"o\",\"watermark\":{\"enabled\":false,\"enabled\":false}}";
    Config cqf=parse("{\"input\":\"i\",\"output\":\"o\",\"encoder\":\"libvvenc\",\"rate_control\":\"cqf\",\"qp\":32,\"maxrate\":6000000,\"output_fps\":30}");
    assert(cqf.cqf&&cqf.bitrate==0&&cqf.qp==32&&cqf.maxrate==6000000&&cqf.output_fps==30);
    assert(cqf.encoder_bit_depth==10);
    Config eight=cqf;eight.encoder_bit_depth=8;digest(&cqf,a);digest(&eight,b);assert(strcmp(a,b));
    Config eight_logical=parse("{\"input\":\"i\",\"outputs\":[{\"output\":\"o\",\"encoder\":\"libvvenc\",\"encoder_bit_depth\":8,\"rate_control\":\"cqf\",\"qp\":32,\"maxrate\":6000000,\"output_fps\":30}]}");
    digest(&eight,a);digest(&eight_logical,b);assert(!strcmp(a,b));
    const char *invalid_depth[]={
        "{\"input\":\"i\",\"output\":\"o\",\"encoder\":\"libvvenc\",\"encoder_bit_depth\":9}",
        "{\"input\":\"i\",\"output\":\"o\",\"encoder_bit_depth\":8}"};
    for(unsigned k=0;k<sizeof(invalid_depth)/sizeof(*invalid_depth);k++)assert(config_parse(invalid_depth[k],strlen(invalid_depth[k]),&rejected,error,sizeof(error))<0);
    Config internal=cqf;internal.codec_trace=1;internal.encoder_max_future_frames=4;digest(&cqf,a);digest(&internal,b);assert(strcmp(a,b));
    Config traced=parse("{\"input\":\"rtmp://127.0.0.1/live/input\",\"output\":\"o\",\"encoder\":\"libvvenc\",\"rate_control\":\"cqf\",\"maxrate\":1100000,\"network_trace\":true,\"encoder_max_future_frames\":4}");
    assert(traced.network_trace&&traced.encoder_max_future_frames==4);
    char n1[65],n2[65];digest(&traced,n1);traced.network_trace=0;digest(&traced,n2);assert(strcmp(n1,n2));
    traced.network_trace=1;traced.encoder_max_future_frames=3;digest(&traced,n2);assert(strcmp(n1,n2));
    traced.encoder_max_future_frames=4;traced.rtmp_tcp_nodelay=1;digest(&traced,n2);assert(strcmp(n1,n2));
    const char *invalid_trace[]={
        "{\"input\":\"i\",\"output\":\"o\",\"network_trace\":true}",
        "{\"input\":\"rtmps://h/live/x\",\"output\":\"o\",\"network_trace\":true}",
        "{\"input\":\"i\",\"output\":\"o\",\"encoder_max_future_frames\":4}",
        "{\"input\":\"i\",\"output\":\"o\",\"encoder\":\"libvvenc\",\"rate_control\":\"cqf\",\"maxrate\":1100000,\"encoder_max_future_frames\":5}"};
    for(unsigned k=0;k<sizeof(invalid_trace)/sizeof(*invalid_trace);k++)assert(config_parse(invalid_trace[k],strlen(invalid_trace[k]),&rejected,error,sizeof(error))<0);
    Config cqf_logical=parse("{\"input\":\"i\",\"outputs\":[{\"output\":\"o\",\"encoder\":\"libvvenc\",\"rate_control\":\"cqf\",\"qp\":32,\"maxrate\":6000000,\"output_fps\":30}]}");
    digest(&cqf,a);digest(&cqf_logical,b);assert(!strcmp(a,b));
    cqf_logical.outputs[0].qp++;digest(&cqf_logical,b);assert(strcmp(a,b));
    Config cqf_multi=parse("{\"input\":\"i\",\"encoder\":\"libvvenc\",\"rate_control\":\"cqf\",\"maxrate\":6000000,\"outputs\":[{\"id\":\"a\",\"output\":\"a\",\"output_fps\":30},{\"id\":\"b\",\"output\":\"b\",\"qp\":28}]}");
    naki_output_config(&branch,&cqf_multi,0);assert(branch.cqf&&branch.output_fps==30&&branch.qp==32&&branch.bitrate==0);
    naki_output_config(&branch,&cqf_multi,1);assert(branch.cqf&&!branch.output_fps&&branch.qp==28);
    const char *crf_ok="{\"input\":\"i\",\"output\":\"o\",\"encoder\":\"libvvenc\",\"rate_control\":\"constrained_crf\",\"crf\":30,\"maxrate\":1100000,\"output_fps\":30,\"encoder_max_future_frames\":4,\"reader_queue_capacity\":1,\"timestamps\":{\"mode\":\"source_all\"},\"intra\":{\"mode\":\"follow_source\"}}";
    Config experimental;
    assert(!config_parse(crf_ok,strlen(crf_ok),&experimental,error,sizeof(error)));
    assert(experimental.constrained_crf&&experimental.cqf&&experimental.qp==30&&experimental.reader_queue_capacity==1);
    const char *byte_ok="{\"input\":\"i\",\"output\":\"o\",\"encoder\":\"libvvenc\",\"encoder_bit_depth\":8,\"encoder_input_bit_depth\":8,\"rate_control\":\"constrained_crf\",\"crf\":32,\"maxrate\":1100000,\"output_fps\":30,\"encoder_max_future_frames\":4,\"timestamps\":{\"mode\":\"source_all\"},\"intra\":{\"mode\":\"follow_source\"}}";
    Config byte=parse(byte_ok);assert(byte.encoder_input_bit_depth==8&&byte.encoder_bit_depth==8);
    Config legacy_byte=byte;legacy_byte.encoder_input_bit_depth=10;digest(&byte,a);digest(&legacy_byte,b);assert(strcmp(a,b));
    Config ref8=byte;ref8.encoder_native8_reference=1;digest(&byte,a);digest(&ref8,b);assert(strcmp(a,b));
    /* Benchmark-resolved guarded configuration legitimately exceeds 32 keys. */
    Config expanded=parse("{\"input\":\"rtmp://127.0.0.1/live/input\",\"encoder\":\"libvvenc\",\"encoder_profile\":\"live_3b_2s_speed\",\"encoder_preset\":\"faster\",\"encoder_threads\":8,\"width\":960,\"height\":540,\"output_fps\":30,\"rate_control\":\"constrained_crf\",\"maxrate\":1100000,\"audio_copy\":true,\"mux_wait_ms\":20,\"timestamps\":{\"mode\":\"source_all\"},\"intra\":{\"mode\":\"follow_source\"},\"frame_log\":true,\"network_trace\":true,\"encoder_max_future_frames\":4,\"rtmp_tcp_nodelay\":true,\"queue_capacity\":1,\"max_threads\":2,\"min_threads\":2,\"budget_slots\":2,\"reader_queue_capacity\":1,\"crf\":32,\"source_rate_limit\":true,\"decoder_pump\":true,\"codec_trace\":false,\"encoder_bit_depth\":8,\"encoder_input_bit_depth\":8,\"encoder_native8_reference\":true,\"output\":\"o\",\"frame_log_every\":1,\"slow_frame_ms\":200}");
    assert(expanded.encoder_native8_reference && expanded.encoder_input_bit_depth==8);
    const char *bad_byte[]={
        "{\"input\":\"i\",\"output\":\"o\",\"encoder_native8_reference\":true}",
        "{\"input\":\"i\",\"output\":\"o\",\"encoder\":\"libvvenc\",\"encoder_input_bit_depth\":8}",
        "{\"input\":\"i\",\"output\":\"o\",\"encoder\":\"libvvenc\",\"encoder_input_bit_depth\":9}"};
    for(unsigned i=0;i<sizeof(bad_byte)/sizeof(*bad_byte);i++)assert(config_parse(bad_byte[i],strlen(bad_byte[i]),&rejected,error,sizeof(error))<0);
    const char *invalid_modes[]={
        "{\"input\":\"i\",\"output\":\"o\",\"encoder\":\"libvvenc\",\"rate_control\":\"crf\"}",
        "{\"input\":\"i\",\"output\":\"o\",\"rate_control\":\"cqf\",\"maxrate\":6000000}",
        "{\"input\":\"i\",\"output\":\"o\",\"encoder\":\"libvvenc\",\"rate_control\":\"cqf\"}",
        "{\"input\":\"i\",\"output\":\"o\",\"encoder\":\"libvvenc\",\"rate_control\":\"cqf\",\"maxrate\":6000000,\"bitrate\":1500000}",
        "{\"input\":\"i\",\"output\":\"o\",\"qp\":32}",
        "{\"input\":\"i\",\"output\":\"o\",\"output_fps\":0}"};
    for(unsigned i=0;i<sizeof(invalid_modes)/sizeof(*invalid_modes);i++)assert(config_parse(invalid_modes[i],strlen(invalid_modes[i]),&rejected,error,sizeof(error))<0);
    assert(config_parse(bad,strlen(bad),&rejected,error,sizeof(error))<0);
    const char *ambiguous="{\"input\":\"i\",\"outputs\":[{\"output\":\"o\",\"queue_items\":16,\"video_items\":16}]}";
    assert(config_parse(ambiguous,strlen(ambiguous),&rejected,error,sizeof(error))<0);
    const char *oversized[]={
        "{\"input\":\"i\",\"output\":\"o\",\"width\":8192,\"height\":8192}",
        "{\"input\":\"i\",\"outputs\":[{\"output\":\"o\",\"width\":8192,\"height\":8192}]}",
        "{\"input\":\"i\",\"outputs\":[{\"id\":\"a\",\"output\":\"a\",\"width\":8192,\"height\":8192},{\"id\":\"b\",\"output\":\"b\"}]}"};
    for(unsigned i=0;i<sizeof(oversized)/sizeof(*oversized);i++)assert(config_parse(oversized[i],strlen(oversized[i]),&rejected,error,sizeof(error))<0);
    yyjson_mut_doc *d=new_event("request",NULL);assert(d);json_str(d,"request_id","first");json_str(d,"request_id","second");
    size_t size=0;char *text=yyjson_mut_write(d,0,&size);yyjson_doc *read=yyjson_read(text,size,0);assert(read);
    assert(!naki_intent_validate(yyjson_doc_get_root(read),error,sizeof(error)));
    assert(!strcmp(yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(read),"request_id")),"second"));
    free(text);yyjson_doc_free(read);yyjson_mut_doc_free(d);
    Job *job=calloc(1,sizeof(*job));assert(job&&!pthread_mutex_init(&job->mutex,NULL));job->config=legacy;job->lifecycle.state=NAKI_JOB_STATE_RUNNING;
    LoggerOptions logger_options={.queue_capacity=32,.rotate_seconds=1,.rotate_bytes=1048576};job->logger=logger_start(&logger_options);assert(job->logger);
    Job *projection=calloc(1,sizeof(*projection));
    assert(projection&&!pthread_mutex_init(&projection->mutex,NULL));
    projection->logger=job->logger;projection->config=job->config;
    /* Internal enum representation must not change the public state strings. */
    const struct {NakiJobState state;const char *wire;} states[]={
        {NAKI_JOB_STATE_NONE,""},{NAKI_JOB_STATE_STARTING,"starting"},
        {NAKI_JOB_STATE_WAITING_ASSETS,"waiting_assets"},{NAKI_JOB_STATE_RUNNING,"running"},
        {NAKI_JOB_STATE_FAILING,"failing"},{NAKI_JOB_STATE_STOPPED,"stopped"},
        {NAKI_JOB_STATE_FAILED,"failed"},{NAKI_JOB_STATE_COMPLETED,"completed"},
        {NAKI_JOB_STATE_COMPLETED_PARTIAL,"completed_partial"}};
    for(size_t i=0;i<sizeof(states)/sizeof(states[0]);i++) {
        projection->lifecycle.state=states[i].state;
        d=job_snapshot(projection);assert(d);
        assert(!strcmp(yyjson_mut_get_str(yyjson_mut_obj_get(yyjson_mut_doc_get_root(d),"state")),states[i].wire));
        yyjson_mut_doc_free(d);
    }
    const struct {NakiMediaPhase phase;const char *wire;} phases[]={
        {NAKI_MEDIA_PHASE_NONE,"starting"},{NAKI_MEDIA_PHASE_OPENING_INPUT,"opening_input"},
        {NAKI_MEDIA_PHASE_WAITING_INPUT,"waiting_input"},{NAKI_MEDIA_PHASE_RUNNING,"running"},
        {NAKI_MEDIA_PHASE_DRAINING,"draining"}};
    projection->lifecycle.state=NAKI_JOB_STATE_RUNNING;
    for(size_t i=0;i<sizeof(phases)/sizeof(phases[0]);i++) {
        projection->progress.media_phase=phases[i].phase;
        d=job_snapshot(projection);assert(d);
        yyjson_mut_val *v=yyjson_mut_obj_get(yyjson_mut_doc_get_root(d),"progress");
        assert(!strcmp(yyjson_mut_get_str(yyjson_mut_obj_get(v,"phase")),phases[i].wire));
        yyjson_mut_doc_free(d);
    }
    projection->progress.media_phase=NAKI_MEDIA_PHASE_NONE;
    pthread_mutex_destroy(&projection->mutex);free(projection);
    job->progress.last_input_us=100;job->progress.last_decoded_us=200;job->progress.last_encoded_us=300;
    assert(!naki_policy_track(job,NAKI_TRACK_PACKET,1,50));assert(!naki_policy_track(job,NAKI_TRACK_FRAME,2,60));
    d=job_snapshot(job);assert(d);text=yyjson_mut_write(d,0,&size);assert(text);read=yyjson_read(text,size,0);assert(read);
    assert(!naki_intent_validate(yyjson_doc_get_root(read),error,sizeof(error)));
    yyjson_val *progress=yyjson_obj_get(yyjson_doc_get_root(read),"progress");
    assert(yyjson_get_sint(yyjson_obj_get(progress,"last_input_us"))==100);
    assert(yyjson_get_sint(yyjson_obj_get(progress,"last_decoded_us"))==200);
    assert(yyjson_get_sint(yyjson_obj_get(progress,"last_encoded_us"))==300);
    assert(yyjson_is_null(yyjson_obj_get(progress,"last_written_us")));
    assert(yyjson_get_sint(yyjson_obj_get(progress,"oldest_video_inflight_us"))==50);
    assert(yyjson_get_uint(yyjson_obj_get(progress,"tracked_video_packets"))==1&&yyjson_get_uint(yyjson_obj_get(progress,"tracked_video_frames"))==1);
    assert(yyjson_is_num(yyjson_obj_get(progress,"last_input_age_ms"))&&yyjson_is_null(yyjson_obj_get(progress,"oldest_mixed_queue_age_ms")));
    assert(yyjson_get_uint(yyjson_obj_get(yyjson_doc_get_root(read),"snapshot_generation"))==1);
    free(text);yyjson_doc_free(read);yyjson_mut_doc_free(d);
    assert(naki_policy_untrack(job,NAKI_TRACK_PACKET,1)&&naki_policy_untrack(job,NAKI_TRACK_FRAME,2));logger_stop(job->logger);pthread_mutex_destroy(&job->mutex);free(job);
    puts("control spec: normalized defaults, one logical direct owner, stable output identity, changed intent and unique JSON replacement passed");return 0;
}
