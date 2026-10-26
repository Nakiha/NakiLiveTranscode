#ifndef NAKI_RENDITION_RUNTIME_INTERNAL_H
#define NAKI_RENDITION_RUNTIME_INTERNAL_H
#include "rendition_runtime.h"
#include "pipeline_internal.h"
#include "rendition_message.h"
#include "session_idle.h"
#include "platform/ipc.h"
#include "control/identity.h"
#define LEASES (NAKI_RENDITION_VIDEO_CREDIT+NAKI_RENDITION_AUDIO_CREDIT)
typedef struct Lease {int used,queued;uint64_t work_us;NakiRenditionMessage message;} Lease;
typedef struct Rendition {
    Job *job;
    OutputConfig spec;
    char *config_json;
    char instance[33];
    uint64_t generation;
    int control,media,completion,pidfd;
    pid_t pid;
    int local_thread;
    pthread_t thread;
    atomic_int thread_done;
    int prepared,finished,failed,stopping,eof_sent,removed;
    int member,attached,restarting,remove_draining;
    unsigned automatic_attempts,incarnations;
    int64_t restart_at_us;
    uint64_t prepare_source_generation;
    int64_t required_subscription_generation;
    NakiOperationHandle attach_operation,remove_operation;
    NakiFrameIdentity attach_identity,detach_identity;
    NakiOperationHandle remote_operations[256];
    uint64_t written_generation,watermark_cached_revision;
    int64_t stop_deadline_us,prepared_deadline_us;
    unsigned queued_video,queued_audio;
    uint64_t queued_bytes,queued_work_us;
    Lease leases[LEASES];
} Rendition;
struct NakiRenditions {
    Job *job;
    pthread_mutex_t mutex;
    unsigned initial_count;
    Rendition slots[NAKI_OUTPUT_MAX];
    NakiFramePool *video,*audio;
    AVFrame prototype;
    uint64_t source_generation,next_instance_generation;
    NakiSessionIdle idle;
    int finishing,ready,idle_expired;
    uint64_t desired_revision,effective_revision;
    NakiFrameIdentity last_frame;
    int64_t next_source_rate_us;
    struct {yyjson_doc *request;NakiOperationHandle operation;unsigned slot;uint64_t generation;char origin[129];} commands[8];
    unsigned command_head,command_count;
    struct {int action,started,automatic;int64_t deadline_us;unsigned slot;uint64_t revision,source_generation;NakiOperationHandle operation;Config config;OutputConfig spec;char *config_json;} transition;
};

int naki_rendition_spawn(Pipeline *p,unsigned index);
void naki_rendition_fail(Rendition *r,int error,const char *why);
int naki_rendition_control_tick(Pipeline *p);
int naki_rendition_attach(Pipeline *p,AVFrame *frame);
void naki_rendition_publish(Pipeline *p);
void naki_rendition_reaped(Pipeline *p,unsigned slot);
void naki_rendition_cache_desired(Rendition *r,yyjson_val *desired,uint64_t revision);
void naki_rendition_observe(Pipeline *p,Rendition *r,yyjson_val *record);
void naki_rendition_written(Pipeline *p,Rendition *r,yyjson_val *snapshot);
#endif
