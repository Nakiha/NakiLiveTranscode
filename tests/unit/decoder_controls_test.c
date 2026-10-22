#include "control/config.h"
#include <assert.h>
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
    Config old=parse("{\"input\":\"i\",\"output\":\"o\"}");
    Config defaults=parse("{\"input\":\"i\",\"output\":\"o\",\"decoder_async\":true,\"decoder_auto_threads\":true}");
    assert(old.decoder_async==1&&old.decoder_auto_threads==1&&old.decoder_early_output==-1);
    char original[65],explicit[65];digest(&old,original);digest(&defaults,explicit);assert(!strcmp(original,explicit));
    Config sync=parse("{\"input\":\"i\",\"output\":\"o\",\"decoder_async\":false}");
    assert(!sync.decoder_async&&!sync.decoder_pump);digest(&sync,explicit);assert(strcmp(original,explicit));
    Config fixed=parse("{\"input\":\"i\",\"output\":\"o\",\"decoder_auto_threads\":false}");
    assert(fixed.decoder_async&&!fixed.decoder_auto_threads);digest(&fixed,explicit);assert(strcmp(original,explicit));
    const char *invalid[]={
        "{\"input\":\"i\",\"output\":\"o\",\"decoder_async\":0}",
        "{\"input\":\"i\",\"output\":\"o\",\"decoder_async\":null}",
        "{\"input\":\"i\",\"output\":\"o\",\"decoder_auto_threads\":1}",
        "{\"input\":\"i\",\"output\":\"o\",\"decoder_auto_threads\":null}",
        "{\"input\":\"i\",\"output\":\"o\",\"decoder_pump\":true,\"decoder_async\":false}",
        "{\"input\":\"i\",\"output\":\"o\",\"decoder_async\":false,\"decoder_pump\":true}"};
    for(unsigned i=0;i<sizeof(invalid)/sizeof(*invalid);i++) {
        Config c;char error[256];assert(config_parse(invalid[i],strlen(invalid[i]),&c,error,sizeof(error))<0);
    }
    char identities[4][65];
    for(int a=0;a<2;a++)for(int e=0;e<2;e++) {
        char text[256];snprintf(text,sizeof(text),"{\"input\":\"i\",\"output\":\"o\",\"decoder_async\":%s,\"decoder_early_output\":%s,\"decoder_auto_threads\":false}",a?"true":"false",e?"true":"false");
        Config c=parse(text);assert(c.decoder_async==a&&c.decoder_early_output==e&&!c.decoder_auto_threads);
        digest(&c,identities[2*a+e]);
    }
    for(int i=0;i<4;i++)for(int j=i+1;j<4;j++)assert(strcmp(identities[i],identities[j]));
    Config multi=parse("{\"input\":\"i\",\"decoder_async\":false,\"decoder_auto_threads\":false,\"outputs\":[{\"id\":\"a\",\"output\":\"a\"},{\"id\":\"b\",\"output\":\"b\"}]}");
    for(int i=0;i<2;i++) {Config branch;naki_output_config(&branch,&multi,i);assert(!branch.decoder_async&&!branch.decoder_auto_threads);}
    puts("PASS decoder backend defaults, types, pump constraints, fingerprints and branch inheritance");
    return 0;
}
