#include "identity.h"
#include "intent.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static int fingerprint(const char *text,char out[65]) {
    yyjson_doc *doc=yyjson_read(text,strlen(text),0);assert(doc);char error[128];
    int ret=naki_intent_fingerprint(yyjson_doc_get_root(doc),out,error,sizeof(error));yyjson_doc_free(doc);return ret;
}
int main(void) {
    char a[65],b[65],id[33],other[33];
    assert(!naki_id_generate(id)&&!naki_id_generate(other)&&strcmp(id,other)&&strlen(id)==32);
    assert(naki_id_valid(id,32)&&!naki_id_valid(id,31));
    assert(!naki_id_valid("",64)&&!naki_id_valid("../job",64)&&!naki_id_valid("job/path",64)&&!naki_id_valid("a b",64));
    assert(!fingerprint("{\"a\":1,\"b\":[null,\"x\"]}",a));
    assert(!fingerprint("{\"b\":[null,\"x\"],\"a\":1.0}",b)&&!strcmp(a,b));
    assert(!fingerprint("{\"a\":1,\"b\":[\"x\",null]}",b)&&strcmp(a,b));
    assert(!fingerprint("{\"a\":\"1\",\"b\":[null,\"x\"]}",b)&&strcmp(a,b));
    assert(fingerprint("{\"a\":1,\"a\":1}",b)<0&&fingerprint("{\"x\":\"a\\u0000b\"}",b)<0);
    yyjson_doc *nul=yyjson_read("\"a\\u0000b\"",10,0);assert(nul&&!naki_json_id(yyjson_doc_get_root(nul),64));yyjson_doc_free(nul);
    char nested[256];memset(nested,'[',40);memcpy(nested+40,"0",1);memset(nested+41,']',40);nested[81]=0;
    assert(fingerprint(nested,b)<0);
    assert(!fingerprint("{\"opacity\":0.5}",a)&&!fingerprint("{}",b)&&strcmp(a,b));
    puts("identity/intent: opaque IDs, bounds, canonical numeric/key order, typed presence, duplicate/NUL/depth rejection passed");return 0;
}
