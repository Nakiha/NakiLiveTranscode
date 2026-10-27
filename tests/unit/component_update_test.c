#include "control/component_update.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static NakiUpdateRequest parse(const char *patch,const char *operation,uint64_t expected,const char *output) {
    char request[8192],error[256];snprintf(request,sizeof(request),"{\"operation_id\":\"%s\",\"target\":{\"component\":\"watermark\",\"output_id\":\"%s\"},\"expected_revision\":%llu,\"patch\":%s}",operation,output,(unsigned long long)expected,patch);
    yyjson_doc *doc=yyjson_read(request,strlen(request),0);assert(doc);NakiUpdateRequest result;
    assert(!naki_update_parse(yyjson_doc_get_root(doc),"job-A",&result,error,sizeof(error)));yyjson_doc_free(doc);return result;
}
static void reject(const char *patch) {
    char request[8192],error[256];snprintf(request,sizeof(request),"{\"operation_id\":\"op\",\"target\":{\"component\":\"watermark\",\"output_id\":\"main\"},\"expected_revision\":1,\"patch\":%s}",patch);
    yyjson_doc *doc=yyjson_read(request,strlen(request),0);assert(doc);NakiUpdateRequest result;
    assert(naki_update_parse(yyjson_doc_get_root(doc),"job-A",&result,error,sizeof(error))<0);yyjson_doc_free(doc);
}
static void topology_requests(void){
    const char *requests[]={
      "{\"operation_id\":\"op\",\"target\":{\"component\":\"rendition\",\"output_id\":\"new\"},\"action\":\"add_rendition\",\"expected_revision\":1,\"spec\":{\"id\":\"new\",\"output\":\"new.mp4\"}}",
      "{\"operation_id\":\"op\",\"target\":{\"component\":\"destination\",\"output_id\":\"new\",\"destination_id\":\"mirror\"},\"action\":\"add_destination\",\"expected_revision\":2,\"expected_generation\":7,\"spec\":{\"output\":\"mirror.mp4\",\"output_format\":\"mp4\"}}",
      "{\"operation_id\":\"op\",\"target\":{\"component\":\"rendition\",\"output_id\":\"new\"},\"action\":\"request_random_access\",\"expected_revision\":3,\"expected_generation\":7}"
    };
    char fingerprints[3][65];char error[256];
    for(unsigned i=0;i<3;i++){yyjson_doc *doc=yyjson_read(requests[i],strlen(requests[i]),0);assert(doc);NakiUpdateRequest request;assert(!naki_update_parse(yyjson_doc_get_root(doc),"job-A",&request,error,sizeof(error)));assert(request.action==(i==0?NAKI_RENDITION_ADD:i==1?NAKI_DESTINATION_ADD:NAKI_RANDOM_ACCESS));strcpy(fingerprints[i],request.fingerprint);yyjson_doc_free(doc);}
    assert(strcmp(fingerprints[0],fingerprints[1])&&strcmp(fingerprints[1],fingerprints[2]));
    const char *invalid="{\"operation_id\":\"op\",\"target\":{\"component\":\"watermark\",\"output_id\":\"new\"},\"action\":\"remove_rendition\",\"expected_revision\":1}";
    yyjson_doc *doc=yyjson_read(invalid,strlen(invalid),0);assert(doc);NakiUpdateRequest request;assert(naki_update_parse(yyjson_doc_get_root(doc),"job-A",&request,error,sizeof(error))<0);yyjson_doc_free(doc);
}
int main(void) {
    topology_requests();
    const char *initial="{\"enabled\":true,\"resource\":{\"uri\":\"original.png\",\"load_timeout_ms\":4000},\"placement\":{\"anchor\":\"top_left\",\"offset\":{\"x\":17,\"y\":23},\"size\":{\"width\":192,\"height\":64},\"opacity\":0.8}}";
    yyjson_doc *doc=yyjson_read(initial,strlen(initial),0);assert(doc);char error[256];NakiWatermarkConfig desired,merged;
    assert(!naki_watermark_config(yyjson_doc_get_root(doc),&desired,error,sizeof(error)));yyjson_doc_free(doc);
    NakiUpdateRequest a=parse("{\"placement\":{\"opacity\":0.65,\"offset\":{\"x\":0}}}","one",1,"main");
    assert(a.patch.present==(NAKI_PATCH_OPACITY|NAKI_PATCH_X));assert(!naki_watermark_patch_apply(&desired,&a.patch,&merged,error,sizeof(error)));
    assert(merged.x==0&&merged.y==23&&merged.opacity==.65&&merged.width==192&&merged.height==64&&merged.enabled&&merged.load_timeout_ms==4000&&!strcmp(merged.uri,"original.png"));
    NakiUpdateRequest b=parse("{\"placement\":{\"offset\":{\"x\":0},\"opacity\":0.65}}","another-id",1,"main");assert(!strcmp(a.fingerprint,b.fingerprint));
    b=parse("{\"placement\":{\"opacity\":0.65,\"offset\":{\"x\":0}}}","one",2,"main");assert(strcmp(a.fingerprint,b.fingerprint));
    b=parse("{\"placement\":{\"opacity\":0.65,\"offset\":{\"x\":0}}}","one",1,"preview");assert(strcmp(a.fingerprint,b.fingerprint));
    a=parse("{\"placement\":{\"opacity\":1}}","one",1,"main");b=parse("{\"placement\":{\"opacity\":1.0}}","two",1,"main");assert(!strcmp(a.fingerprint,b.fingerprint));
    a=parse("{}","one",1,"main");b=parse("{\"resource\":{}}","one",1,"main");assert(strcmp(a.fingerprint,b.fingerprint));
    assert(!naki_watermark_patch_apply(&desired,&a.patch,&merged,error,sizeof(error)));
    assert(merged.enabled==desired.enabled&&merged.x==desired.x&&merged.y==desired.y&&merged.width==desired.width&&merged.opacity==desired.opacity&&!strcmp(merged.uri,desired.uri));
    a=parse("{\"enabled\":false}","one",1,"main");desired.required=1;assert(naki_watermark_patch_apply(&desired,&a.patch,&merged,error,sizeof(error))<0);
    desired.required=0;desired.enabled=0;a=parse("{\"enabled\":true}","one",1,"main");assert(!naki_watermark_patch_apply(&desired,&a.patch,&merged,error,sizeof(error))&&merged.enabled&&!strcmp(merged.uri,"original.png"));
    *desired.uri=0;assert(naki_watermark_patch_apply(&desired,&a.patch,&merged,error,sizeof(error))<0);
    const char *bad[]={"null","{\"enabled\":null}","{\"enabled\":1}","{\"enabled\":true,\"enabled\":true}","{\"startup\":{\"required\":false}}",
        "{\"resource\":null}","{\"resource\":{\"uri\":\"\"}}","{\"resource\":{\"load_timeout_ms\":0}}","{\"resource\":{\"uri\":\"ftp://bad\"}}",
        "{\"placement\":{\"space\":\"output_canvas\"}}","{\"placement\":{\"offset\":{\"unit\":\"px\"}}}","{\"placement\":{\"opacity\":1.1}}",
        "{\"encoder\":\"libx264\"}","{\"placement\":{\"anchor\":\"random\"}}","{\"animation\":{}}"};
    for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++)reject(bad[i]);
    yyjson_mut_doc *encoded=yyjson_mut_doc_new(NULL);yyjson_mut_doc_set_root(encoded,naki_watermark_config_json(encoded,&desired));size_t bytes=0;char *text=yyjson_mut_write(encoded,0,&bytes);assert(text);
    doc=yyjson_read(text,bytes,0);NakiWatermarkConfig roundtrip;assert(!naki_watermark_config(yyjson_doc_get_root(doc),&roundtrip,error,sizeof(error)));
    assert(roundtrip.width==desired.width&&roundtrip.y==desired.y&&!roundtrip.enabled&&!*roundtrip.uri);
    free(text);yyjson_doc_free(doc);yyjson_mut_doc_free(encoded);
    puts("PASS typed sparse patch presence, desired merge, immutable policy, strict bounds, normalized intent and config projection");return 0;
}
