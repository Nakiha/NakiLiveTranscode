#ifndef NAKI_JOB_STATE_H
#define NAKI_JOB_STATE_H
#include <string.h>

/* Internal states are typed. Wire projections preserve the existing strings,
 * including the empty initial state. No transition or admission policy lives here. */
typedef enum NakiJobState {
    NAKI_JOB_STATE_NONE,
    NAKI_JOB_STATE_STARTING,
    NAKI_JOB_STATE_WAITING_ASSETS,
    NAKI_JOB_STATE_RUNNING,
    NAKI_JOB_STATE_FAILING,
    NAKI_JOB_STATE_STOPPED,
    NAKI_JOB_STATE_FAILED,
    NAKI_JOB_STATE_COMPLETED,
    NAKI_JOB_STATE_COMPLETED_PARTIAL,
    NAKI_JOB_STATE_IDLE,
    NAKI_JOB_STATE_STOPPING,
    NAKI_JOB_STATE_FINISHING
} NakiJobState;
static inline const char *naki_job_state_name(NakiJobState value) {
    switch(value) {
    case NAKI_JOB_STATE_NONE: return "";
    case NAKI_JOB_STATE_STARTING: return "starting";
    case NAKI_JOB_STATE_WAITING_ASSETS: return "waiting_assets";
    case NAKI_JOB_STATE_RUNNING: return "running";
    case NAKI_JOB_STATE_FAILING: return "failing";
    case NAKI_JOB_STATE_STOPPED: return "stopped";
    case NAKI_JOB_STATE_FAILED: return "failed";
    case NAKI_JOB_STATE_COMPLETED: return "completed";
    case NAKI_JOB_STATE_COMPLETED_PARTIAL: return "completed_partial";
    case NAKI_JOB_STATE_IDLE: return "idle";
    case NAKI_JOB_STATE_STOPPING: return "stopping";
    case NAKI_JOB_STATE_FINISHING: return "finishing";
    }
    return "";
}

typedef enum NakiRecordState {
    NAKI_RECORD_STATE_NONE,
    NAKI_RECORD_STATE_RESERVED,
    NAKI_RECORD_STATE_ACCEPTED,
    NAKI_RECORD_STATE_UNKNOWN,
    NAKI_RECORD_STATE_STOPPING,
    NAKI_RECORD_STATE_STOPPED,
    NAKI_RECORD_STATE_FAILED
} NakiRecordState;
static inline const char *naki_record_state_name(NakiRecordState value) {
    switch(value) {
    case NAKI_RECORD_STATE_NONE: return "";
    case NAKI_RECORD_STATE_RESERVED: return "reserved";
    case NAKI_RECORD_STATE_ACCEPTED: return "accepted";
    case NAKI_RECORD_STATE_UNKNOWN: return "unknown";
    case NAKI_RECORD_STATE_STOPPING: return "stopping";
    case NAKI_RECORD_STATE_STOPPED: return "stopped";
    case NAKI_RECORD_STATE_FAILED: return "failed";
    }
    return "";
}

typedef enum NakiMediaPhase {
    NAKI_MEDIA_PHASE_NONE,
    NAKI_MEDIA_PHASE_OPENING_INPUT,
    NAKI_MEDIA_PHASE_WAITING_INPUT,
    NAKI_MEDIA_PHASE_RUNNING,
    NAKI_MEDIA_PHASE_DRAINING
} NakiMediaPhase;
static inline const char *naki_media_phase_name(NakiMediaPhase value) {
    switch(value) {
    case NAKI_MEDIA_PHASE_NONE: return "";
    case NAKI_MEDIA_PHASE_OPENING_INPUT: return "opening_input";
    case NAKI_MEDIA_PHASE_WAITING_INPUT: return "waiting_input";
    case NAKI_MEDIA_PHASE_RUNNING: return "running";
    case NAKI_MEDIA_PHASE_DRAINING: return "draining";
    }
    return "";
}

typedef enum NakiWritePhase {
    NAKI_WRITE_PHASE_NONE,
    NAKI_WRITE_PHASE_MUX_WRITE,
    NAKI_WRITE_PHASE_AVIO_FLUSH,
    NAKI_WRITE_PHASE_OUTPUT_FINALIZE
} NakiWritePhase;
static inline const char *naki_write_phase_name(NakiWritePhase value) {
    switch(value) {
    case NAKI_WRITE_PHASE_NONE: return "";
    case NAKI_WRITE_PHASE_MUX_WRITE: return "mux_write";
    case NAKI_WRITE_PHASE_AVIO_FLUSH: return "avio_flush";
    case NAKI_WRITE_PHASE_OUTPUT_FINALIZE: return "output_finalize";
    }
    return "";
}

static inline NakiJobState naki_job_state_parse(const char *text) {
    if(!text)return NAKI_JOB_STATE_NONE;
    if(!strcmp(text,"starting"))return NAKI_JOB_STATE_STARTING;
    if(!strcmp(text,"waiting_assets"))return NAKI_JOB_STATE_WAITING_ASSETS;
    if(!strcmp(text,"running"))return NAKI_JOB_STATE_RUNNING;
    if(!strcmp(text,"failing"))return NAKI_JOB_STATE_FAILING;
    if(!strcmp(text,"stopped"))return NAKI_JOB_STATE_STOPPED;
    if(!strcmp(text,"failed"))return NAKI_JOB_STATE_FAILED;
    if(!strcmp(text,"completed"))return NAKI_JOB_STATE_COMPLETED;
    if(!strcmp(text,"completed_partial"))return NAKI_JOB_STATE_COMPLETED_PARTIAL;
    if(!strcmp(text,"idle"))return NAKI_JOB_STATE_IDLE;
    if(!strcmp(text,"stopping"))return NAKI_JOB_STATE_STOPPING;
    if(!strcmp(text,"finishing"))return NAKI_JOB_STATE_FINISHING;
    return NAKI_JOB_STATE_NONE;
}
#endif
