#define _GNU_SOURCE
#include "worker/job.h"
#include "worker/job_media.h"
#include "worker/job_remote.h"
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/eventfd.h>

void *__real_calloc(size_t,size_t);
static unsigned fail_call,calls;
void *__wrap_calloc(size_t count,size_t size){if(fail_call&&++calls==fail_call)return NULL;return __real_calloc(count,size);}
static Job *local_job(void){Job *j=calloc(1,sizeof(*j));assert(j&&!pthread_mutex_init(&j->mutex,NULL));j->wake_fd=eventfd(0,EFD_NONBLOCK|EFD_CLOEXEC);assert(j->wake_fd>=0);j->config.slow_frame_ms=200;return j;}
static void local_free(Job *j){close(j->wake_fd);pthread_mutex_destroy(&j->mutex);free(j);}
int main(void){
    Job *root=local_job(),*child=local_job();child->parent=root;root->outputs[0]=child;root->config.output_count=1;
    naki_job_checkpoint(child,NAKI_CP_FIRST_PACKET,100);naki_job_checkpoint(child,NAKI_CP_FIRST_PACKET,200);assert(child->progress.first_packet_us==100);
    Stamp stamp={.frame_id=1,.picture_type=AV_PICTURE_TYPE_I,.first_i=1,.ingress_us=1000,.submitted_us=2000,.decoded_us=3000,
        .filter_started_us=4000,.filtered_us=5000,.encoder_submit_us=6000,.encoder_accepted_us=7000};
    NakiWrittenFrame frame={.stamp=&stamp,.encoded_us=8000,.mux_begin_us=8000,.interleaved_us=9000,.written_us=10000,.bytes=100};
    NakiWrittenResult result=naki_job_frame_written(child,&frame);assert(result.first_output&&!result.first_key&&result.ordinal==1);
    assert(root->progress.first_video_output_us==10000&&!root->progress.first_key_output_us);
    assert(child->stats.lifetime.ingress_to_mux.count==1&&child->stats.window.ingress_to_mux.count==1);
    assert(child->stats.groups.first_i.count==1&&!child->stats.groups.output_key.count);
    stamp.picture_type=AV_PICTURE_TYPE_P;stamp.first_i=0;frame.output_key=1;frame.written_us=11000;
    result=naki_job_frame_written(child,&frame);assert(!result.first_output&&result.first_key&&result.ordinal==2);
    assert(root->progress.first_video_output_us==10000&&root->progress.first_key_output_us==11000);
    stamp.picture_type=AV_PICTURE_TYPE_I;frame.output_key=0;frame.written_us=12000;naki_job_frame_written(child,&frame);
    assert(child->stats.lifetime.ingress_to_mux.count==3&&child->stats.window.ingress_to_mux.count==3);
    assert(child->stats.groups.i_frame.count==2&&child->stats.groups.first_i.count==1&&child->stats.groups.recurring_i.count==1&&child->stats.groups.output_key.count==1);
    assert(!naki_job_branch_admit(child,123,1000,10,10));assert(naki_job_branch_admit(child,124,1000,1,10)==-ENOBUFS);
    naki_branch_release(child,123,10);naki_branch_release(child,123,10);assert(!child->stats.branch_inflight_bytes&&!child->stats.branch_inflight_frames);
    naki_job_cancel(root);assert(naki_job_cancelled(root)&&!naki_job_cancelled(child));atomic_store(&root->lifecycle.cancel,0);request_stop(root);assert(naki_job_cancelled(child));
    calls=0;fail_call=2;assert(naki_job_remote_slots(root)==-ENOMEM);fail_call=0;
    assert(root->config.output_count==1&&root->outputs[0]==child&&!root->outputs[1]&&!root->outputs[2]&&!root->outputs[3]);
    assert(!naki_job_remote_slots(root)&&root->config.output_count==NAKI_OUTPUT_MAX);
    for(unsigned i=1;i<NAKI_OUTPUT_MAX;i++){assert(naki_job_parent(root->outputs[i])==root);local_free(root->outputs[i]);root->outputs[i]=NULL;}
    Job *destination=naki_job_destination_new(child,"/tmp/private-output.ts","mpegts");assert(destination);
    assert(!strcmp(naki_job_config(destination)->output,"/tmp/private-output.ts")&&!strcmp(naki_job_config(child)->output,""));
    naki_job_destination_free(&destination);assert(!destination);naki_job_destination_free(&destination);
    Job *holder=naki_job_child_new();assert(holder&&naki_job_parent(holder)&&naki_job_remote(holder)==2);
    naki_job_child_free(&holder);assert(!holder);
    local_free(child);local_free(root);
    puts("PASS opaque Job publication, all-frame/group sampling, first checkpoints, admission retirement and factory rollback");return 0;
}
