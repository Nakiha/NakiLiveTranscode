#include "control/operation_store.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static const char fingerprint[]="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static NakiOperationIntent intent(const char *id) {
    return (NakiOperationIntent){.kind=NAKI_OP_UPDATE,.job_id="job-A",.operation_id=id,.output_id="main",.fingerprint=fingerprint,.origin_request_id="first-attempt",.expected_revision=1,.revision=2};
}
static yyjson_doc *read_record(NakiOperations *s,NakiOperationHandle id) {
    yyjson_mut_doc *m=naki_operations_get(s,id);assert(m);size_t bytes;char *text=yyjson_mut_write(m,0,&bytes);assert(text);
    yyjson_doc *d=yyjson_read(text,bytes,0);assert(d);free(text);yyjson_mut_doc_free(m);return d;
}
static void check(NakiOperations *s,NakiOperationHandle id,const char *state,const char *delivery,uint64_t refs) {
    yyjson_doc *d=read_record(s,id);yyjson_val *root=yyjson_doc_get_root(d),*v=yyjson_obj_get(root,"delivery");
    assert(!strcmp(yyjson_get_str(yyjson_obj_get(root,"state")),state));
    if(delivery) {assert(!strcmp(yyjson_get_str(yyjson_obj_get(v,"state")),delivery));assert(yyjson_get_uint(yyjson_obj_get(v,"inflight_frame_leases"))==refs);}
    yyjson_doc_free(d);
}
static void claim(NakiOperations *s,NakiOperationHandle id,int64_t now) {
    assert(!naki_operations_transition(s,id,NAKI_OP_PREPARING,0,0,NULL,now));
    assert(!naki_operations_transition(s,id,NAKI_OP_PREPARED,0,0,NULL,now+1));
    assert(naki_operations_transition(s,id,NAKI_OP_APPLYING,0,0,NULL,now+2)==-EPERM);
    assert(!naki_operations_transition(s,id,NAKI_OP_APPLYING,1,0,NULL,now+2));
}
typedef struct Thread {NakiOperations *store;NakiOperationHandle id;} Thread;
static void *lease_loop(void *opaque) {
    Thread *t=opaque;for(int i=0;i<1000;i++) {
        assert(!naki_operations_frame_acquire(t->store,t->id));assert(!naki_operations_frame_release(t->store,t->id,100));
    }return NULL;
}
static void topology_results(void){
    NakiOperations *source=naki_operations_open(4,2*1024*1024,1000),*remote=naki_operations_open(4,2*1024*1024,1000);assert(source&&remote);
    NakiOperationIntent in=intent("remove");in.kind=NAKI_OP_TOPOLOGY;in.component="rendition";in.action="remove_rendition";
    NakiOperationHandle a,b;assert(naki_operations_reserve(source,&in,1,&a)==1);assert(naki_operations_reserve(remote,&in,1,&b)==1);
    claim(source,a,2);NakiFrameIdentity empty={0};assert(!naki_operations_applied(source,a,&empty,0,6));assert(!naki_operations_retire(source,a,7));
    yyjson_doc *record=read_record(source,a);assert(!naki_operations_observe(remote,b,yyjson_doc_get_root(record),8));yyjson_doc_free(record);check(remote,b,"applied","not_written",0);
    in=intent("missing-ack");in.kind=NAKI_OP_TOPOLOGY;in.component="rendition";in.action="add_rendition";assert(naki_operations_reserve(source,&in,10,&a)==1);
    claim(source,a,11);NakiFrameIdentity frame={.frame_id=2,.source_pts=200,.source_us=200000,.source_tb_num=1,.source_tb_den=1000,.output_tb_num=1,.output_tb_den=90000};
    assert(!naki_operations_applied(source,a,&frame,1,15));assert(!naki_operations_holder_exited(source,a,16));check(source,a,"applied","uncertain",0);
    in=intent("confirmed");in.kind=NAKI_OP_TOPOLOGY;in.component="destination";in.action="add_destination";in.destination_id="mirror";assert(naki_operations_reserve(source,&in,20,&a)==1);
    claim(source,a,21);assert(!naki_operations_applied(source,a,&frame,1,25));assert(!naki_operations_written(source,a,&frame,26));assert(!naki_operations_holder_exited(source,a,27));check(source,a,"applied","first_written",0);
    naki_operations_close(&source);naki_operations_close(&remote);
}
int main(void) {
    topology_results();
    NakiOperations *s=naki_operations_open(2,2*1024*1024,1000);assert(s);NakiOperationHandle a,b,c,duplicate,create,stop;
    NakiOperationIntent in=intent("A");assert(naki_operations_reserve(s,&in,1,&a)==1);
    in.origin_request_id="retry-attempt";assert(!naki_operations_reserve(s,&in,2,&duplicate)&&duplicate==a);
    yyjson_doc *d=read_record(s,a);
    assert(!strcmp(yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(d),"origin_request_id")),"first-attempt"));yyjson_doc_free(d);
    in.fingerprint="bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    assert(naki_operations_reserve(s,&in,3,&duplicate)==-EEXIST);
    in=intent("B");assert(naki_operations_reserve(s,&in,3,&b)==1);
    in=intent("C");assert(naki_operations_reserve(s,&in,4,&c)==-ENOSPC);
    in=intent("create");in.kind=NAKI_OP_CREATE;assert(naki_operations_reserve(s,&in,4,&create)==1);
    in=intent("stop");in.kind=NAKI_OP_STOP;assert(naki_operations_reserve(s,&in,4,&stop)==1);
    in=intent("A");in.kind=NAKI_OP_STOP;assert(naki_operations_reserve(s,&in,5,&duplicate)==-EEXIST);
    assert(!naki_operations_pin(s,b,NAKI_OP_PIN_DESIRED,0));claim(s,a,10);
    assert(!naki_operations_transition(s,b,NAKI_OP_CANCELLED,0,0,"job stopping before owner claim",20));
    assert(!naki_operations_retire(s,b,20));assert(!naki_operations_retire(s,a,20));
    check(s,b,"cancelled","not_ready",0);check(s,a,"applying","not_ready",0);
    assert(naki_operations_transition(s,a,NAKI_OP_SUPERSEDED,0,0,"later desired",21)==-EPERM);
    NakiFrameIdentity first={.frame_id=1,.source_pts=100,.source_us=100000,.source_tb_num=1,.source_tb_den=1000,.output_tb_num=1,.output_tb_den=90000};
    assert(!naki_operations_applied(s,a,&first,1,30));check(s,a,"applied","pending",1);
    assert(!naki_operations_retire(s,a,31));check(s,a,"applied","pending",1);
    assert(!naki_operations_frame_release(s,a,32));check(s,a,"applied","not_written",0);
    assert(naki_operations_frame_release(s,a,33)==-EINVAL);
    in=intent("C");assert(naki_operations_reserve(s,&in,500,&c)==-ENOSPC);
    assert(naki_operations_reserve(s,&in,2000,&c)==1&&c!=a);assert(!naki_operations_get(s,a));
    in=intent("D");assert(naki_operations_reserve(s,&in,2000,&a)==-ENOSPC);
    assert(!naki_operations_pin(s,b,0,NAKI_OP_PIN_DESIRED));assert(naki_operations_reserve(s,&in,2001,&a)==1);
    claim(s,a,2010);assert(!naki_operations_applied(s,a,&first,1,2020));assert(!naki_operations_frame_acquire(s,a));
    NakiFrameIdentity later=first;later.frame_id=3;later.source_pts=300;later.output_pts=700;
    assert(!naki_operations_retire(s,a,2021));assert(!naki_operations_written(s,a,&later,2022));
    assert(!naki_operations_written(s,a,&first,2023));check(s,a,"applied","first_written",2);
    d=read_record(s,a);yyjson_val *delivery=yyjson_obj_get(yyjson_doc_get_root(d),"delivery");
    assert(yyjson_get_uint(yyjson_obj_get(yyjson_obj_get(delivery,"first_written"),"frame_id"))==3);yyjson_doc_free(d);
    assert(!naki_operations_frame_release(s,a,2024));assert(!naki_operations_frame_release(s,a,2025));check(s,a,"applied","first_written",0);
    assert(!naki_operations_transition(s,stop,NAKI_OP_STOPPING,0,0,NULL,2030));assert(!naki_operations_transition(s,stop,NAKI_OP_STOPPED,0,0,NULL,2031));
    unsigned next;yyjson_mut_doc *page=naki_operations_page(s,0,8,&next);assert(page&&!next);size_t bytes;
    char *text=yyjson_mut_write(page,0,&bytes);assert(text&&bytes<65536);free(text);yyjson_mut_doc_free(page);naki_operations_close(&s);assert(!s);
    s=naki_operations_open(1,1024*1024,1000);assert(s);in=intent("threaded");assert(naki_operations_reserve(s,&in,1,&a)==1);
    claim(s,a,2);assert(!naki_operations_applied(s,a,&first,0,5));Thread t={s,a};pthread_t threads[4];
    for(int i=0;i<4;i++)assert(!pthread_create(&threads[i],NULL,lease_loop,&t));
    for(int i=0;i<4;i++)pthread_join(threads[i],NULL);
    check(s,a,"applied","pending",0);assert(!naki_operations_retire(s,a,100));check(s,a,"applied","not_written",0);naki_operations_close(&s);
    puts("PASS bounded operations, replay/conflict, reserved stop, retention pins, owner claim, leases and frame delivery");return 0;
}
