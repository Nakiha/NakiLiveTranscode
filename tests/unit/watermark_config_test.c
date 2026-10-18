#include "control/watermark_config.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static int parse(const char *text,NakiWatermarkConfig *c) {
    yyjson_doc *doc=yyjson_read(text,strlen(text),0);char error[256];
    assert(doc);int ret=naki_watermark_config(yyjson_doc_get_root(doc),c,error,sizeof(error));yyjson_doc_free(doc);return ret;
}
int main(void) {
    NakiWatermarkConfig c;char error[256];
    assert(!naki_watermark_config(NULL,&c,error,sizeof(error))&&!c.enabled);
    assert(!parse("{\"enabled\":true,\"resource\":{\"uri\":\"/tmp/a.png\"},\"startup\":{\"required\":true}}",&c));
    assert(c.enabled&&c.required&&c.width==160&&c.anchor==8);
    const char *bad[]={
        "null","[]","{\"required\":true}","{\"enabled\":true}",
        "{\"enabled\":false,\"startup\":{\"required\":true}}",
        "{\"enabled\":false,\"enabled\":true}",
        "{\"resource\":{\"uri\":\"ftp://example/image\"}}",
        "{\"resource\":{\"uri\":\"a\\u0000b\"}}",
        "{\"resource\":{\"load_timeout_ms\":30001}}",
        "{\"placement\":{\"size\":{\"width\":0}}}",
        "{\"placement\":{\"size\":{\"fit\":\"cover\"}}}",
        "{\"placement\":{\"offset\":{\"unit\":\"percent\"}}}",
        "{\"placement\":{\"anchor\":\"bad\"}}",
        "{\"placement\":{\"opacity\":1.01}}",
        "{\"placement\":{\"size\":{\"width\":1,\"width\":2}}}",
        "{\"startup\":{\"on_timeout\":\"start_without_watermark\"}}",
        "{\"update\":{\"on_error\":\"clear\"}}",
        "{\"animation\":{\"clock\":\"wall\"}}",
        "{\"animation\":{\"restart_on_replace\":false}}"
    };
    for(size_t i=0;i<sizeof(bad)/sizeof(*bad);i++)assert(parse(bad[i],&c));
    const char *update="{\"revision\":2,\"watermark\":{\"enabled\":false}}";
    yyjson_doc *doc=yyjson_read(update,strlen(update),0);uint64_t revision=0;
    assert(doc&&!naki_watermark_update(yyjson_doc_get_root(doc),&c,&revision,error,sizeof(error))&&revision==2);
    yyjson_doc_free(doc);puts("watermark configuration checks passed");return 0;
}
