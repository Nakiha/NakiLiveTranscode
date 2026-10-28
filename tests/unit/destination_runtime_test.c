#include "worker/job.h"
/* Inspect private slot state to deterministically exercise ready becoming true
 * after the owner poll, before its next IDR. No writer thread or I/O is needed. */
#include "media/destination_runtime.c"
#include <assert.h>
void pipeline_input_free(InputItem *item){assert(!item->packet&&!item->params);}
/* No protocol is opened by this admission-state test. Mux lifetime/cancel is
 * exercised against the real implementation by mux-lifetime. */
void pipeline_mux_cancel(Pipeline *p){(void)p;}
int pipeline_submit_mux(Pipeline *p,AVPacket *packet){(void)p;(void)packet;abort();}
NakiFrameIdentity pipeline_frame_identity(Pipeline *p,const Stamp *s,int64_t pts,AVRational tb){
    return (NakiFrameIdentity){.frame_id=s->frame_id,.source_pts=s->source_pts,.source_us=s->source_us,
        .source_tb_num=p->input_tb.num,.source_tb_den=p->input_tb.den,.output_pts=pts,.output_tb_num=tb.num,.output_tb_den=tb.den};
}
int main(void){
    Job *job=calloc(1,sizeof(*job));Pipeline *p=calloc(1,sizeof(*p));assert(job&&p);
    job->operations=naki_operations_open(4,2*1024*1024,1000000);assert(job->operations);
    p->job=job;p->input_tb=(AVRational){1,25};p->encoder=avcodec_alloc_context3(NULL);assert(p->encoder);
    AVStream video={.time_base={1,25}};p->video=&video;
    assert(!naki_destinations_open(p));NakiDestinations *m=p->destinations;m->activated=1;
    Destination *d=&m->slots[1];d->started=1;atomic_store(&d->ready,1);d->mux=calloc(1,sizeof(Pipeline));assert(d->mux);
    NakiOperationIntent in={.kind=NAKI_OP_TOPOLOGY,.job_id="job",.output_id="main",.operation_id="ready-race",.origin_request_id="test",
        .fingerprint="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",.component="destination",.action="add_destination",.destination_id="mirror",.expected_revision=1,.revision=2};
    assert(naki_operations_reserve(job->operations,&in,1,&d->add_operation)==1);
    assert(!naki_operations_transition(job->operations,d->add_operation,NAKI_OP_PREPARING,0,0,NULL,2));
    AVPacket *packet=av_packet_alloc();assert(packet&&!av_new_packet(packet,5));memcpy(packet->data,"\0\0\0\1\x65",5);
    packet->pts=packet->dts=50;packet->opaque_ref=av_buffer_allocz(sizeof(MuxTrace));assert(packet->opaque_ref);
    MuxTrace *meta=(MuxTrace *)packet->opaque_ref->data;meta->stamp=(Stamp){.frame_id=50,.source_pts=50,.source_us=2000000,.ingress_us=now_us()};meta->pts_us=2000000;
    assert(!naki_destinations_packet(p,packet));assert(d->active&&d->count==1&&!atomic_load(&d->failed));
    yyjson_mut_doc *record=naki_operations_get(job->operations,d->add_operation);assert(record);
    assert(!strcmp(yyjson_mut_get_str(yyjson_mut_obj_get(yyjson_mut_doc_get_root(record),"state")),"applied"));yyjson_mut_doc_free(record);
    NakiFrameIdentity identity=pipeline_frame_identity(p,&meta->stamp,50,video.time_base);
    assert(!naki_operations_written(job->operations,d->add_operation,&identity,now_us()));
    assert(!naki_operations_frame_release(job->operations,d->add_operation,now_us()));
    /* The same URI validator is shared by initial and hot destinations. */
    char error[256];assert(naki_output_validate("file:/tmp/existing.mp4","mp4",error,sizeof(error))<0);
    assert(naki_output_validate("rtmp://host/live","mp4",error,sizeof(error))<0);
    assert(!naki_output_validate("/tmp/new.mp4","mp4",error,sizeof(error)));
    for(unsigned i=0;i<DESTINATIONS;i++){while(m->slots[i].count){delivery_free(&m->slots[i].queue[m->slots[i].head]);m->slots[i].head=(m->slots[i].head+1)%PACKETS;m->slots[i].count--;}free(m->slots[i].mux);pthread_mutex_destroy(&m->slots[i].mutex);}
    pthread_mutex_destroy(&m->mutex);free(m);av_packet_free(&packet);avcodec_free_context(&p->encoder);naki_operations_close(&job->operations);free(p);free(job);
    puts("PASS IDR admission commits prepared after asynchronous ready; shared output URI validation");return 0;
}
