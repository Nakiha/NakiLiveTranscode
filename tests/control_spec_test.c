#include "live.h"
#include "intent.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static Config parse(const char *text) {
    Config c;char error[256];assert(!config_parse(text,strlen(text),&c,error,sizeof(error)));return c;
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
