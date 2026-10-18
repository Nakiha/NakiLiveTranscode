#include "control/operation_cache.h"
#include "control/operation_store.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static const char *fingerprint="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static yyjson_doc *record(const char *id,const char *state,const char *delivery,int pins) {
    static unsigned version;
    char text[2048];snprintf(text,sizeof(text),"{\"job_id\":\"job-A\",\"operation_id\":\"%s\",\"kind\":\"update\",\"intent_fingerprint\":\"%s\",\"result_version\":%u,\"outcome_known\":true,\"state\":\"%s\",\"retention_pins\":%d,\"terminal_us\":10,\"applied_checkpoint\":{\"frame_id\":7},\"delivery\":{\"state\":\"%s\",\"inflight_frame_leases\":0,\"first_written\":null}}",id,fingerprint,++version,state,pins,delivery);
    yyjson_doc *d=yyjson_read(text,strlen(text),0);assert(d);return d;
}
static void put(NakiOperationCache *cache,const char *id,const char *state,const char *delivery,int pins) {
    yyjson_doc *d=record(id,state,delivery,pins);assert(!naki_operation_cache_put(cache,yyjson_doc_get_root(d),100));yyjson_doc_free(d);
}
static void query(NakiOperationCache *cache,const char *id,int known,const char *state) {
    yyjson_mut_doc *d=naki_operation_cache_get(cache,id);assert(d);yyjson_mut_val *root=yyjson_mut_doc_get_root(d);
    assert(yyjson_mut_get_bool(yyjson_mut_obj_get(root,"outcome_known"))==known);
    assert(!strcmp(yyjson_mut_get_str(yyjson_mut_obj_get(root,"state")),state));
    if(!known&&yyjson_mut_obj_get(root,"last_observed")&&!yyjson_mut_is_null(yyjson_mut_obj_get(root,"last_observed")))assert(yyjson_mut_obj_get(root,"applied_checkpoint"));
    yyjson_mut_doc_free(d);
}
static uint64_t cache_snapshot(NakiOperationCache *cache,yyjson_mut_doc *snapshot,int64_t received) {
    size_t bytes;char *text=yyjson_mut_write(snapshot,0,&bytes);assert(text);
    yyjson_doc *d=yyjson_read(text,bytes,0);assert(d);free(text);
    uint64_t version=yyjson_get_uint(yyjson_obj_get(yyjson_doc_get_root(d),"result_version"));assert(version);
    assert(!naki_operation_cache_put(cache,yyjson_doc_get_root(d),received));yyjson_doc_free(d);return version;
}
static void ordered_results(int written) {
    NakiOperations *store=naki_operations_open(1,1024*1024,1000);assert(store);
    NakiOperationCache *cache=naki_operation_cache_open("job-A");assert(cache);
    NakiOperationIntent intent={.kind=NAKI_OP_UPDATE,.job_id="job-A",.operation_id="ordered",.output_id="main",.fingerprint=fingerprint,.origin_request_id="attempt",.expected_revision=1,.revision=2};
    NakiOperationHandle id;assert(naki_operations_reserve(store,&intent,1,&id)==1);
    assert(!naki_operations_transition(store,id,NAKI_OP_PREPARING,0,0,NULL,2));
    yyjson_mut_doc *preparing=naki_operations_get(store,id);
    assert(!naki_operations_transition(store,id,NAKI_OP_PREPARED,0,0,NULL,3));
    yyjson_mut_doc *prepared=naki_operations_get(store,id);
    uint64_t v2=cache_snapshot(cache,prepared,20),v1=cache_snapshot(cache,preparing,21);assert(v2>v1);
    yyjson_mut_doc *got=naki_operation_cache_get(cache,"ordered");
    yyjson_mut_val *root=yyjson_mut_doc_get_root(got),*last=yyjson_mut_obj_get(root,"last_observed");
    assert(!strcmp(yyjson_mut_get_str(yyjson_mut_obj_get(last,"state")),"prepared"));
    assert(yyjson_mut_get_sint(yyjson_mut_obj_get(root,"last_observed_received_monotonic_us"))==20);yyjson_mut_doc_free(got);
    assert(!naki_operations_transition(store,id,NAKI_OP_APPLYING,1,0,NULL,4));
    NakiFrameIdentity frame={.frame_id=7,.source_pts=7,.source_us=7000,.source_tb_num=1,.source_tb_den=1000,.output_tb_num=1,.output_tb_den=90000};
    assert(!naki_operations_applied(store,id,&frame,1,5));yyjson_mut_doc *pending=naki_operations_get(store,id);
    if(written)assert(!naki_operations_written(store,id,&frame,6));
    assert(!naki_operations_retire(store,id,7));assert(!naki_operations_frame_release(store,id,8));
    assert(!naki_operations_pin(store,id,NAKI_OP_PIN_DESIRED,0));yyjson_mut_doc *pinned=naki_operations_get(store,id);
    assert(!naki_operations_pin(store,id,0,NAKI_OP_PIN_DESIRED));yyjson_mut_doc *final=naki_operations_get(store,id);
    uint64_t newest=cache_snapshot(cache,final,30);assert(newest>cache_snapshot(cache,pinned,31));
    assert(newest>cache_snapshot(cache,pending,32));assert(cache_snapshot(cache,final,33)==newest);
    NakiOperationCache *staging=naki_operation_cache_open("job-A");assert(staging);
    assert(cache_snapshot(staging,pending,29)<newest);
    assert(!naki_operation_cache_reconcile(staging,cache));
    naki_operation_cache_close(&cache);cache=staging;
    got=naki_operation_cache_get(cache,"ordered");root=yyjson_mut_doc_get_root(got);
    assert(yyjson_mut_get_bool(yyjson_mut_obj_get(root,"outcome_known")));
    assert(yyjson_mut_get_uint(yyjson_mut_obj_get(root,"result_version"))==newest);
    assert(!yyjson_mut_get_uint(yyjson_mut_obj_get(root,"retention_pins")));
    assert(yyjson_mut_get_sint(yyjson_mut_obj_get(root,"last_observed_received_monotonic_us"))==30);
    yyjson_mut_val *delivery=yyjson_mut_obj_get(root,"delivery");
    assert(!strcmp(yyjson_mut_get_str(yyjson_mut_obj_get(delivery,"state")),written?"first_written":"not_written"));
    assert(!yyjson_mut_get_uint(yyjson_mut_obj_get(delivery,"inflight_frame_leases")));
    yyjson_mut_doc_free(got);yyjson_mut_doc_free(final);yyjson_mut_doc_free(pinned);yyjson_mut_doc_free(pending);
    yyjson_mut_doc_free(prepared);yyjson_mut_doc_free(preparing);naki_operation_cache_close(&cache);naki_operations_close(&store);
}
int main(void) {
    ordered_results(0);ordered_results(1);
    NakiOperationCache *cache=naki_operation_cache_open("job-A");assert(cache);
    assert(naki_operation_cache_reserve(cache,"A",fingerprint,1)==1);query(cache,"A",0,"unknown");
    assert(!naki_operation_cache_reserve(cache,"A",fingerprint,2));
    assert(naki_operation_cache_reserve(cache,"A","bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",2)==-EEXIST);
    put(cache,"A","applied","pending",2);query(cache,"A",0,"unknown");
    put(cache,"A","applied","first_written",0);query(cache,"A",1,"applied");
    put(cache,"B","failed","not_ready",1);query(cache,"B",1,"failed");
    naki_operation_cache_unreserve(cache,"A");assert(naki_operation_cache_contains(cache,"A"));
    for(int i=2;i<256;i++) {char id[32];snprintf(id,sizeof(id),"reserved-%d",i);assert(naki_operation_cache_reserve(cache,id,fingerprint,100)==1);}
    assert(naki_operation_cache_reserve(cache,"overflow",fingerprint,100)==-ENOSPC);
    assert(naki_operation_cache_reserve(cache,"replacement",fingerprint,1800000011)==1);assert(!naki_operation_cache_contains(cache,"A"));
    assert(naki_operation_cache_contains(cache,"B")); /* Pinned terminal is retained. */
    naki_operation_cache_unreserve(cache,"reserved-2");assert(!naki_operation_cache_contains(cache,"reserved-2"));
    assert(naki_operation_cache_reserve(cache,"after-unreserve",fingerprint,100)==1);
    naki_operation_cache_close(&cache);assert(!cache);puts("PASS bounded reservations, mutation-version ordering across live/sync caches, nonterminal/delivery/lease/retention results");return 0;
}
