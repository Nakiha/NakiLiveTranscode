#include "control/config.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
static Config parse(const char *text) {
    Config c;char error[256];
    assert(!config_parse(text,strlen(text),&c,error,sizeof(error)));return c;
}
static void digest(const Config *c,char out[65]) {
    char error[256];assert(!naki_config_fingerprint(c,out,error,sizeof(error)));
}
int main(void) {
    Config defaults=parse("{\"input\":\"i\",\"output\":\"o\"}");
    Config explicit=parse("{\"input\":\"i\",\"output\":\"o\",\"idle_timeout_ms\":60000}");
    Config none=parse("{\"input\":\"i\",\"output\":\"o\",\"idle_timeout_ms\":0}");
    Config custom=parse("{\"input\":\"i\",\"output\":\"o\",\"idle_timeout_ms\":1234}");
    assert(defaults.idle_timeout_ms==60000&&explicit.idle_timeout_ms==60000);
    assert(none.idle_timeout_ms==0&&custom.idle_timeout_ms==1234);
    char a[65],b[65],c[65];digest(&defaults,a);digest(&explicit,b);assert(!strcmp(a,b));
    digest(&none,b);digest(&custom,c);assert(strcmp(a,b)&&strcmp(a,c)&&strcmp(b,c));
    char text[512];snprintf(text,sizeof(text),"{\"input\":\"i\",\"idle_timeout_ms\":%d,\"outputs\":[{\"id\":\"a\",\"output\":\"a\"},{\"id\":\"b\",\"output\":\"b\"}]}",INT_MAX);
    Config multi=parse(text);assert(multi.idle_timeout_ms==INT_MAX&&multi.output_count==2);
    for(int i=0;i<2;i++){Config branch;naki_output_config(&branch,&multi,i);assert(branch.idle_timeout_ms==INT_MAX);}
    Config logical=parse("{\"input\":\"i\",\"idle_timeout_ms\":0,\"outputs\":[{\"output\":\"o\"}]}");
    digest(&logical,c);assert(!strcmp(b,c));
    /* Keepalive is a session property, never an individual output override. */
    const char *invalid[]={"-1","2147483648","1.5","true","null","\"60000\""};
    for(unsigned i=0;i<sizeof(invalid)/sizeof(*invalid);i++) {
        snprintf(text,sizeof(text),"{\"input\":\"i\",\"output\":\"o\",\"idle_timeout_ms\":%s}",invalid[i]);
        Config rejected;char error[256];assert(config_parse(text,strlen(text),&rejected,error,sizeof(error))<0);
    }
    const char *bad="{\"input\":\"i\",\"outputs\":[{\"output\":\"o\",\"idle_timeout_ms\":0}]}";
    Config rejected;char error[256];assert(config_parse(bad,strlen(bad),&rejected,error,sizeof(error))<0);
    puts("PASS session keepalive defaults, zero/custom/bounds, intent identity and session-only scope");
    return 0;
}
