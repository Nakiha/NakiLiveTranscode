#include "job.h"
#include "job_media.h"
#include "platform/runtime.h"
#include "media/watermark.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <libavutil/error.h>
const Config *naki_job_config(const Job *job){return &job->config;}
const char *naki_job_id(const Job *job){return job->id;}
const char *naki_job_request_id(const Job *job){return job->request_id;}
const char *naki_job_output_id(const Job *job){return job->output_id;}
const char *naki_job_log_path(const Job *job){return job->log_path;}
const char *naki_job_public_config(const Job *job){return job->public_config;}
Job *naki_job_parent(const Job *job){return job->parent;}
Job *naki_job_output(const Job *job,unsigned index){return index<NAKI_OUTPUT_MAX?job->outputs[index]:NULL;}
Logger *naki_job_logger(const Job *job){return job->logger;}
TraceSink *naki_job_trace(const Job *job){return job->trace;}
NakiWatermark *naki_job_watermark(const Job *job){return job->watermark;}
NakiOperations *naki_job_operations(const Job *job){return job->operations;}
NakiRenditions *naki_job_renditions(const Job *job){return job->renditions;}
int naki_job_wake_fd(const Job *job){return job->wake_fd;}
int naki_job_remote(const Job *job){return job->remote_rendition;}
int64_t naki_job_started_us(const Job *job){return job->lifecycle.started_us;}
int naki_job_cancelled(const Job *job){return atomic_load(&job->lifecycle.cancel);}
int naki_job_done(const Job *job){return atomic_load(&job->lifecycle.done);}
int naki_job_failed(const Job *job){return atomic_load(&job->lifecycle.branch_failed);}
void naki_job_cancel(Job *job){atomic_store(&job->lifecycle.cancel,1);}
int64_t naki_job_source_rate(const Job *job){return atomic_load(&job->remote_source_rate);}
void naki_job_source_rate_set(Job *job,int64_t rate){atomic_store(&job->remote_source_rate,rate);}

void naki_job_count(Job *j,NakiJobCounter counter,uint64_t amount) {
    pthread_mutex_lock(&j->mutex);
    switch(counter) {
    case NAKI_COUNT_AUDIO_UNSUPPORTED:j->stats.audio_unsupported+=amount;break;
    case NAKI_COUNT_AUDIO_DROPPED:j->stats.audio_dropped+=amount;break;
    case NAKI_COUNT_AUDIO_TRACKS:j->stats.audio_tracks+=amount;break;
    case NAKI_COUNT_AUDIO_WRITTEN:j->stats.audio_packets_out+=amount;break;
    case NAKI_COUNT_SEI_EXTRACTED:j->stats.sei_extracted+=amount;break;
    case NAKI_COUNT_SEI_WRITTEN:j->stats.sei_written+=amount;break;
    case NAKI_COUNT_INTRA_REQUESTED:j->stats.intra_requested+=amount;break;
    case NAKI_COUNT_INTRA_CONFIRMED:j->stats.intra_confirmed+=amount;break;
    case NAKI_COUNT_FILTERED:j->stats.frames_filtered+=amount;break;
    default:abort();
    }
    pthread_mutex_unlock(&j->mutex);
}
void naki_job_checkpoint(Job *j,NakiJobCheckpoint point,int64_t when) {
    int64_t *field=NULL;
    switch(point) {
    case NAKI_CP_INPUT_OPEN_STARTED:field=&j->progress.input_open_started_us;break;
    case NAKI_CP_INPUT_OPEN_DONE:field=&j->progress.input_open_done_us;break;
    case NAKI_CP_INPUT_PROBE_DONE:field=&j->progress.input_probe_done_us;break;
    case NAKI_CP_DECODER_READY:field=&j->progress.decoder_ready_us;break;
    case NAKI_CP_FIRST_PACKET:field=&j->progress.first_packet_us;break;
    case NAKI_CP_FIRST_DECODED:field=&j->progress.first_decoded_us;break;
    case NAKI_CP_ENCODER_OPEN_STARTED:field=&j->progress.encoder_open_started_us;break;
    case NAKI_CP_ENCODER_OPEN_DONE:field=&j->progress.encoder_open_done_us;break;
    case NAKI_CP_OUTPUT_HEADER_DONE:field=&j->progress.output_header_done_us;break;
    case NAKI_CP_FIRST_ENCODER_SUBMIT:field=&j->progress.first_encoder_submit_us;break;
    default:abort();
    }
    pthread_mutex_lock(&j->mutex);if(!*field)*field=when?when:now_us();pthread_mutex_unlock(&j->mutex);
}
void naki_job_media_phase(Job *j,NakiMediaPhase phase){pthread_mutex_lock(&j->mutex);j->progress.media_phase=phase;pthread_mutex_unlock(&j->mutex);}
void naki_job_input_received(Job *j,int video,int running,int64_t when) {
    pthread_mutex_lock(&j->mutex);j->progress.last_input_us=when;if(video)j->stats.packets_in++;
    if(running)j->progress.media_phase=NAKI_MEDIA_PHASE_RUNNING;
    pthread_mutex_unlock(&j->mutex);
}
void naki_job_decoded(Job *j,const Stamp *s,int branch) {
    pthread_mutex_lock(&j->mutex);
    j->progress.last_decoded_us=s->decoded_us;j->progress.media_phase=NAKI_MEDIA_PHASE_RUNNING;
    if(branch){if(!j->progress.first_decoded_us)j->progress.first_decoded_us=s->decoded_us;if(!j->progress.first_packet_us)j->progress.first_packet_us=s->ingress_us;}
    pthread_mutex_unlock(&j->mutex);
}
void naki_job_encoded(Job *j,int64_t when) {
    pthread_mutex_lock(&j->mutex);j->progress.last_encoded_us=when;pthread_mutex_unlock(&j->mutex);
    if(j->parent){pthread_mutex_lock(&j->parent->mutex);j->parent->progress.last_encoded_us=when;pthread_mutex_unlock(&j->parent->mutex);}
}
void naki_job_video_rate(Job *j,const NakiVideoRate *rate){pthread_mutex_lock(&j->mutex);j->progress.video_rate=*rate;pthread_mutex_unlock(&j->mutex);}
void naki_job_decoder_observation(Job *j,const AVCodecAsyncAutoStats *stats,const AVCodecEarlyOutputStats *early,int available,int64_t when) {
    pthread_mutex_lock(&j->mutex);j->progress.auto_stats=*stats;j->progress.early_stats=*early;j->progress.early_stats_available=available;j->progress.auto_updated_us=when;pthread_mutex_unlock(&j->mutex);
}
uint64_t naki_job_filter_generation(Job *j,int initialize){pthread_mutex_lock(&j->mutex);uint64_t v=initialize?(j->stats.filter_generations=1):++j->stats.filter_generations;pthread_mutex_unlock(&j->mutex);return v;}
void naki_job_output_size_set(Job *j,int width,int height){pthread_mutex_lock(&j->mutex);j->progress.fixed_output_width=width;j->progress.fixed_output_height=height;pthread_mutex_unlock(&j->mutex);}
NakiOutputSize naki_job_output_size_freeze(Job *j,int width,int height) {
    pthread_mutex_lock(&j->mutex);
    if(!j->progress.fixed_output_width){j->progress.fixed_output_width=j->config.width?j->config.width:width;j->progress.fixed_output_height=j->config.height?j->config.height:height;}
    NakiOutputSize result={j->progress.fixed_output_width,j->progress.fixed_output_height};pthread_mutex_unlock(&j->mutex);return result;
}
void naki_job_rate_dropped(Job *j,const Stamp *s) {
    pthread_mutex_lock(&j->mutex);j->stats.frames_decoded++;j->stats.frames_dropped_rate++;
    if(j->stats.frame_id_last<(uint64_t)s->frame_id)j->stats.frame_id_last=(uint64_t)s->frame_id;
    j->stats.timestamp_wraps=s->timestamp_wraps;pthread_mutex_unlock(&j->mutex);
}
int naki_job_frame_deciding(Job *j,const Stamp *s,unsigned sei) {
    pthread_mutex_lock(&j->mutex);j->stats.frames_decoded++;
    if(j->stats.frame_id_last<(uint64_t)s->frame_id)j->stats.frame_id_last=(uint64_t)s->frame_id;
    j->stats.timestamp_wraps=s->timestamp_wraps;if(j->parent)j->stats.sei_extracted+=sei;
    int level=j->flow.overload.level;pthread_mutex_unlock(&j->mutex);return level;
}
void naki_job_frame_selected(Job *j,int protect,int sei,int keep,int rate_drop) {
    pthread_mutex_lock(&j->mutex);if(protect)j->stats.frames_protected++;if(sei)j->stats.sei_protected++;
    if(keep)j->stats.frames_selected++;else if(rate_drop)j->stats.frames_dropped_rate++;else j->stats.frames_dropped_overload++;
    pthread_mutex_unlock(&j->mutex);
}
void naki_job_shared_decoded(Job *j,const Stamp *s,int wraps) {
    pthread_mutex_lock(&j->mutex);j->stats.frames_decoded++;j->stats.frame_id_last=s->frame_id;
    if(wraps)j->stats.timestamp_wraps=s->timestamp_wraps;else j->progress.last_decoded_us=s->decoded_us;
    metric_add(&j->stats.lifetime.decode,(s->decoded_us-s->submitted_us)/1000.0,j->config.slow_frame_ms);
    metric_add(&j->stats.window.decode,(s->decoded_us-s->submitted_us)/1000.0,j->config.slow_frame_ms);pthread_mutex_unlock(&j->mutex);
}
void naki_job_write_begin(Job *j,int64_t when){pthread_mutex_lock(&j->mutex);j->progress.write_started_us=when;j->progress.write_phase=NAKI_WRITE_PHASE_MUX_WRITE;pthread_mutex_unlock(&j->mutex);}
void naki_job_write_flushing(Job *j){pthread_mutex_lock(&j->mutex);j->progress.write_phase=NAKI_WRITE_PHASE_AVIO_FLUSH;pthread_mutex_unlock(&j->mutex);}
void naki_job_write_end(Job *j){pthread_mutex_lock(&j->mutex);j->progress.write_started_us=0;j->progress.write_phase=NAKI_WRITE_PHASE_NONE;pthread_mutex_unlock(&j->mutex);}
void naki_job_bytes_written(Job *j,uint64_t bytes){pthread_mutex_lock(&j->mutex);j->stats.bytes_out=bytes;pthread_mutex_unlock(&j->mutex);}
uint64_t naki_job_bytes(const Job *job){Job *j=(Job *)job;pthread_mutex_lock(&j->mutex);uint64_t bytes=j->stats.bytes_out;pthread_mutex_unlock(&j->mutex);return bytes;}
void naki_job_finalize_begin(Job *j,int64_t began,int64_t deadline){pthread_mutex_lock(&j->mutex);j->progress.finalize_started_us=began;j->progress.finalize_deadline_us=deadline;j->progress.write_started_us=began;j->progress.write_phase=NAKI_WRITE_PHASE_OUTPUT_FINALIZE;pthread_mutex_unlock(&j->mutex);}
void naki_job_finalize_result(Job *j,int trailer,int flush,int closed,int primary,int64_t when) {
    pthread_mutex_lock(&j->mutex);j->progress.finalize_trailer_error=trailer<0?trailer:0;j->progress.finalize_flush_error=flush<0?flush:0;
    j->progress.finalize_close_error=closed<0?closed:0;j->progress.finalize_primary_error=primary<0?primary:0;
    j->progress.finalize_finished_us=when;j->progress.write_started_us=0;j->progress.write_phase=NAKI_WRITE_PHASE_NONE;pthread_mutex_unlock(&j->mutex);
}
void naki_job_mux_observation(Job *j,const AVLiveMuxStats *s) {
    pthread_mutex_lock(&j->mutex);j->stats.mux_queued_packets=s->queued_packets;j->stats.mux_queued_bytes=s->queued_bytes;
    j->stats.mux_high_water=s->high_water_packets;j->stats.mux_timeouts=s->timeout_releases;j->stats.mux_pressure=s->pressure_releases;j->stats.mux_forced=s->forced_releases;pthread_mutex_unlock(&j->mutex);
}
NakiJobStartup naki_job_startup(Job *j) {
    pthread_mutex_lock(&j->mutex);NakiJobStartup s={.prewarmed=j->lifecycle.prewarmed,.started_us=j->lifecycle.started_us};
#define COPY(k) s.k=j->progress.k
    COPY(assigned_us);COPY(worker_received_us);COPY(worker_spawned_us);COPY(worker_ready_us);COPY(input_open_started_us);COPY(input_open_done_us);COPY(input_probe_done_us);COPY(decoder_ready_us);
#undef COPY
    pthread_mutex_unlock(&j->mutex);return s;
}
void naki_job_first_frame(Job *j,const yyjson_mut_doc *record,int first_video) {
    yyjson_mut_doc *copy=yyjson_mut_doc_mut_copy(record,NULL);pthread_mutex_lock(&j->mutex);
    unsigned slot=first_video?0:1;if(j->progress.first_chain[slot])yyjson_mut_doc_free(j->progress.first_chain[slot]);j->progress.first_chain[slot]=copy;pthread_mutex_unlock(&j->mutex);
}
int naki_job_branch_admit(Job *j,uint64_t id,int64_t ingress,uint64_t bytes,uint64_t limit) {
    pthread_mutex_lock(&j->mutex);int ret=0;
    if(j->stats.branch_inflight_frames>=128||j->stats.branch_inflight_bytes>limit||bytes>limit-j->stats.branch_inflight_bytes)ret=AVERROR(ENOBUFS);
    else if(!(ret=naki_inflight_add(&j->flow.branch_pending,NAKI_TRACK_FRAME,id,ingress))){j->stats.branch_inflight_frames++;j->stats.branch_inflight_bytes+=bytes;if(j->stats.branch_inflight_frames>j->stats.branch_inflight_high)j->stats.branch_inflight_high=j->stats.branch_inflight_frames;}
    pthread_mutex_unlock(&j->mutex);return ret;
}
NakiBranchAges naki_job_branch_ages(Job *j,int64_t when) {
    pthread_mutex_lock(&j->mutex);NakiBranchAges result={.failed=j->lifecycle.branch_failed,.retained_us=naki_inflight_age(&j->flow.branch_pending,when),.write_us=j->progress.write_started_us?when-j->progress.write_started_us:0};pthread_mutex_unlock(&j->mutex);return result;
}
void naki_job_branch_queue(Job *j,const NakiBranchQueueReport *s) {
    pthread_mutex_lock(&j->mutex);j->stats.branch_queued=s->count;j->stats.branch_queue_bytes=s->bytes;j->stats.branch_queue_media_us=s->work_us;j->stats.branch_queue_high=s->high;
    j->stats.branch_video_queued=s->video;j->stats.branch_audio_queued=s->audio;j->stats.branch_video_high=s->video_high;j->stats.branch_audio_high=s->audio_high;j->stats.branch_rejection_reason=s->reason;
    j->progress.branch_oldest_ingress_us=s->oldest_us;memcpy(j->stats.branch_rejected,s->rejected,sizeof(s->rejected));pthread_mutex_unlock(&j->mutex);
}
void naki_job_inherit_input(Job *j,Job *parent,const NakiVideoRate *rate) {
    NakiJobStartup s=naki_job_startup(parent);pthread_mutex_lock(&j->mutex);
    j->progress.input_open_started_us=s.input_open_started_us;j->progress.input_open_done_us=s.input_open_done_us;j->progress.input_probe_done_us=s.input_probe_done_us;j->progress.decoder_ready_us=s.decoder_ready_us;j->progress.video_rate=*rate;pthread_mutex_unlock(&j->mutex);
}
void naki_job_running(Job *j){pthread_mutex_lock(&j->mutex);j->lifecycle.state=NAKI_JOB_STATE_RUNNING;pthread_mutex_unlock(&j->mutex);}
void naki_job_waiting_assets(Job *j){pthread_mutex_lock(&j->mutex);j->lifecycle.state=NAKI_JOB_STATE_WAITING_ASSETS;pthread_mutex_unlock(&j->mutex);}
void naki_job_branch_fail(Job *j,int error,const char *stage) {
    if(naki_job_done(j))return;
    pthread_mutex_lock(&j->mutex);if(!j->lifecycle.branch_failed){char text[AV_ERROR_MAX_STRING_SIZE];av_strerror(error,text,sizeof(text));j->lifecycle.branch_failed=1;snprintf(j->lifecycle.error,sizeof(j->lifecycle.error),"%s: %s",stage,text);j->lifecycle.state=NAKI_JOB_STATE_FAILING;}pthread_mutex_unlock(&j->mutex);
}
void naki_job_branch_unstarted(Job *j){pthread_mutex_lock(&j->mutex);j->lifecycle.state=NAKI_JOB_STATE_FAILED;j->flow.policy_closing=1;j->lifecycle.ended_us=now_us();pthread_mutex_unlock(&j->mutex);atomic_store(&j->lifecycle.done,1);}
void naki_job_startup_fail(Job *j,int error){pthread_mutex_lock(&j->mutex);j->lifecycle.branch_failed=1;j->lifecycle.state=NAKI_JOB_STATE_FAILED;snprintf(j->lifecycle.error,sizeof(j->lifecycle.error),"required watermark startup failed: %d",error);j->lifecycle.ended_us=now_us();pthread_mutex_unlock(&j->mutex);naki_watermark_stop(j->watermark);atomic_store(&j->lifecycle.done,1);}
void naki_job_media_error(Job *j,int error,const char *stage){pthread_mutex_lock(&j->mutex);j->lifecycle.branch_failed=1;snprintf(j->lifecycle.error,sizeof(j->lifecycle.error),"%s: %d",stage,error);pthread_mutex_unlock(&j->mutex);}
void naki_job_shared_terminated(Job *j,int stopped,const char *stage){pthread_mutex_lock(&j->mutex);j->lifecycle.state=stopped?NAKI_JOB_STATE_STOPPED:NAKI_JOB_STATE_FAILED;j->lifecycle.branch_failed=!stopped;j->lifecycle.ended_us=now_us();snprintf(j->lifecycle.error,sizeof(j->lifecycle.error),"shared pipeline terminated at %s",stage);pthread_mutex_unlock(&j->mutex);atomic_store(&j->lifecycle.done,1);}
int naki_job_begin_shutdown(Job *j){pthread_mutex_lock(&j->mutex);j->flow.policy_closing=1;int failed=j->flow.overload.failed;pthread_mutex_unlock(&j->mutex);return failed;}
NakiJobState naki_job_state(Job *j){pthread_mutex_lock(&j->mutex);NakiJobState s=j->lifecycle.state;pthread_mutex_unlock(&j->mutex);return s;}
unsigned naki_job_output_failures(Job *j){unsigned failures=0;for(int i=0;i<j->config.output_count;i++)failures+=atomic_load(&j->outputs[i]->lifecycle.branch_failed);return failures;}

static void sample_latencies(Latencies *m, const Stamp *s, int64_t encoded, int64_t begin,
                             int64_t interleaved, int64_t written, double slow) {
#define SAMPLE(name,us) metric_add(&m->name,(us)/1000.0,slow)
    SAMPLE(demux_read,s->read_us); SAMPLE(input_queue,s->submitted_us-s->ingress_us);
    SAMPLE(decode,s->decoded_us-s->submitted_us); SAMPLE(setup,s->filter_started_us-s->decoded_us);
    SAMPLE(branch_queue,s->branch_started_us-s->branch_enqueued_us);
    SAMPLE(filter,s->filtered_us-s->filter_started_us); SAMPLE(encode,encoded-s->encoder_submit_us);
    SAMPLE(encode_submit,s->encoder_accepted_us-s->encoder_submit_us); SAMPLE(encode_wait,encoded-s->encoder_accepted_us);
    SAMPLE(mux,written-begin); SAMPLE(ingress_to_mux,written-s->ingress_us);
    SAMPLE(interleave_call,interleaved-begin); SAMPLE(avio_flush,written-interleaved);
    if(s->network_first_read_us) {SAMPLE(network_to_mux,written-s->network_first_read_us);}
    if(s->scheduled_us) {SAMPLE(scheduled_to_mux,written-s->scheduled_us);}
#undef SAMPLE
}

static void sample_groups(FrameGroups *g, const Stamp *s, int output_key, int first_i, double total, double slow) {
    if(s->picture_type==AV_PICTURE_TYPE_I) {
        metric_add(&g->i_frame,total,slow);
        metric_add(first_i ? &g->first_i : &g->recurring_i,total,slow);
    } else if(s->picture_type==AV_PICTURE_TYPE_P) metric_add(&g->p_frame,total,slow);
    else if(s->picture_type==AV_PICTURE_TYPE_B) metric_add(&g->b_frame,total,slow);
    if(s->decoded_key)metric_add(&g->decoded_key,total,slow);
    if(output_key)metric_add(&g->output_key,total,slow);
}

NakiWrittenResult naki_job_frame_written(Job *j,const NakiWrittenFrame *frame) {
    Stamp stamp=*frame->stamp;
    int output_key=frame->output_key;
    int64_t written=frame->written_us,encoded=frame->encoded_us,mux_start=frame->mux_begin_us,interleaved=frame->interleaved_us;
    if(j->parent) {
        Job *parent=j->parent;pthread_mutex_lock(&parent->mutex);
        if(!parent->progress.first_video_output_us)parent->progress.first_video_output_us=written;
        parent->progress.last_output_us=written;
        if(output_key&&!parent->progress.first_key_output_us)parent->progress.first_key_output_us=written;
        pthread_mutex_unlock(&parent->mutex);
    }
    double total=(written-stamp.ingress_us)/1000.0;
    pthread_mutex_lock(&j->mutex);
    j->stats.packets_out++; j->stats.bytes_out=frame->bytes;
    int first_output=!j->progress.first_video_output_us;
    int first_output_key=output_key && !j->progress.first_key_output_us;
    if(first_output)j->progress.first_video_output_us=written;
    if(first_output_key)j->progress.first_key_output_us=written;
    int first_i=stamp.first_i;
    double slow=j->config.slow_frame_ms;
    sample_latencies(&j->stats.lifetime,&stamp,encoded,mux_start,interleaved,written,slow);
    sample_latencies(&j->stats.window,&stamp,encoded,mux_start,interleaved,written,slow);
    sample_groups(&j->stats.groups,&stamp,output_key,first_i,total,slow);
    sample_groups(&j->stats.window_groups,&stamp,output_key,first_i,total,slow);
    if(stamp.network_first_read_us) {
        double network_ms=(written-stamp.network_first_read_us)/1000.0;
        sample_groups(&j->stats.network_groups,&stamp,output_key,first_i,network_ms,300);
        sample_groups(&j->stats.window_network_groups,&stamp,output_key,first_i,network_ms,300);
        if(network_ms>300) {j->stats.network_deadline_exceeded++;j->stats.network_window_exceeded++;}
    }
    if((uint64_t)stamp.encoder_future_frames>j->stats.encoder_future_max)j->stats.encoder_future_max=(uint64_t)stamp.encoder_future_frames;
    if(output_key) {
        sample_latencies(&j->stats.key_lifetime,&stamp,encoded,mux_start,interleaved,written,slow);
        sample_latencies(&j->stats.key_window,&stamp,encoded,mux_start,interleaved,written,slow);
    }
    if(stamp.picture_type==AV_PICTURE_TYPE_I) {
        sample_latencies(&j->stats.i_lifetime,&stamp,encoded,mux_start,interleaved,written,slow);
        sample_latencies(&j->stats.i_window,&stamp,encoded,mux_start,interleaved,written,slow);
        j->stats.i_frames++; j->progress.last_i_output_us=written;
    }
    j->progress.last_output_us=written;
    uint64_t ordinal=j->stats.packets_out;
    uint64_t submitted=j->stats.frames_filtered;
    pthread_mutex_unlock(&j->mutex);
    return (NakiWrittenResult){first_output,first_output_key,ordinal,submitted};
}

void naki_job_complete(Job *j,int ret,int stopped,const char *stage) {
    pthread_mutex_lock(&j->mutex);
    memset(&j->flow.policy_inflight,0,sizeof(j->flow.policy_inflight));
    j->lifecycle.ended_us=now_us();
    if(j->parent) {
        j->flow.policy_closing=1;
        memset(&j->flow.branch_pending,0,sizeof(j->flow.branch_pending));
        j->stats.branch_inflight_bytes=j->stats.branch_inflight_frames=0;
        j->stats.branch_queued=j->stats.branch_queue_bytes=j->stats.branch_queue_media_us=0;
        j->stats.branch_video_queued=j->stats.branch_audio_queued=0;
        j->progress.branch_oldest_ingress_us=0;
        j->lifecycle.state=j->lifecycle.branch_failed?NAKI_JOB_STATE_FAILED:atomic_load(&j->parent->lifecycle.cancel)?NAKI_JOB_STATE_STOPPED:NAKI_JOB_STATE_COMPLETED;
    } else {
        int partial=0;
        for(int i=0;i<j->config.output_count;i++)if(*j->outputs[i]->output_id&&j->outputs[i]->lifecycle.branch_failed)partial=1;
        j->lifecycle.state=stopped?NAKI_JOB_STATE_STOPPED:ret<0?NAKI_JOB_STATE_FAILED:partial?NAKI_JOB_STATE_COMPLETED_PARTIAL:NAKI_JOB_STATE_COMPLETED;
        if(ret<0&&!stopped) {
            char message[AV_ERROR_MAX_STRING_SIZE];av_strerror(ret,message,sizeof(message));
            snprintf(j->lifecycle.error,sizeof(j->lifecycle.error),"%s: %s",stage,message);
        }
    }
    pthread_mutex_unlock(&j->mutex);
    logger_submit(j->logger,NULL,job_report(j,1));
    if(!j->parent)logger_trace_finish(j->logger,j->trace);
    atomic_store(&j->lifecycle.done,1);
    if(j->parent)wake_job(j->parent);
}

