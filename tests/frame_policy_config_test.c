#include "frame_policy_config.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static int parse(const char *text, NakiPolicyConfig *c) {
    yyjson_doc *doc=yyjson_read(text,strlen(text),0); char error[256]; assert(doc);
    *c=(NakiPolicyConfig){0};
    int ret=naki_policy_config_parse(yyjson_doc_get_root(doc),c,error,sizeof(error));
    yyjson_doc_free(doc); return ret;
}
#define STRICT "\"timestamps\":{\"mode\":\"source_all\"}"
#define FOLLOW "\"intra\":{\"mode\":\"follow_source\"}"
int main(void) {
    NakiPolicyConfig c;
    assert(!parse("{}",&c) && !c.intra && !c.timestamps && !c.overload_levels);
    assert(!parse("{" STRICT "," FOLLOW "}",&c));
    assert(c.intra==NAKI_INTRA_SOURCE && c.timestamps==NAKI_TS_SOURCE_ALL && !c.overload_levels);
    assert(parse("{" FOLLOW "}",&c));
    assert(parse("{\"timestamps\":{\"mode\":\"source_i_anchors\"}}",&c));
    assert(parse("{\"timestamps\":{\"mode\":\"source_all\",\"missing_pts\":\"synthesize\"}}",&c));
    assert(parse("{\"timestamps\":{\"mode\":\"source_all\",\"mode\":\"legacy\"}}",&c));
    assert(parse("{" STRICT ",\"intra\":{\"mode\":\"periodic_pts_grid\"}}",&c));
    assert(parse("{" STRICT ",\"intra\":{\"mode\":\"follow_source\",\"refresh_type\":\"idr\"}}",&c));
    assert(parse("{" STRICT ",\"intra\":{\"mode\":\"follow_source\",\"protect_from_drop\":false}}",&c));
    assert(parse("{" STRICT "," FOLLOW ",\"frame_selection\":{\"mode\":\"live_drop_only\"}}",&c));
    const char *enabled="{" STRICT "," FOLLOW ",\"frame_selection\":{\"mode\":\"live_drop_only\",\"overload\":{"
        "\"signal\":\"oldest_inflight_ingress_age_ms\",\"levels\":[{\"enter_ms\":100,\"sustain_ms\":20,\"keep_one_in\":2}],"
        "\"recover_below_ms\":50,\"recover_sustain_ms\":30,\"max_continuous_degraded_ms\":1000,\"on_exhausted\":\"fail_task\"}}}";
    assert(!parse(enabled,&c) && c.overload_levels==1 && c.levels[0].keep_one_in==2 && c.max_degraded_ms==1000);
    const char *zero_recovery="{" STRICT "," FOLLOW ",\"frame_selection\":{\"mode\":\"live_drop_only\",\"overload\":{"
        "\"signal\":\"oldest_inflight_ingress_age_ms\",\"levels\":[{\"enter_ms\":100,\"sustain_ms\":20,\"keep_one_in\":2}],"
        "\"recover_below_ms\":0,\"recover_sustain_ms\":30,\"max_continuous_degraded_ms\":1000,\"on_exhausted\":\"fail_task\"}}}";
    assert(parse(zero_recovery,&c));
    puts("PASS default compatibility, explicit overload activation, strict nested keys and unsupported contract rejection");
    return 0;
}
