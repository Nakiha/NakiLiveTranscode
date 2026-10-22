#include "control/config.h"
#include <libavcodec/async.h>
#include "media/decode_output_policy.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
static Config parse(const char *s){Config c;char e[256];assert(!config_parse(s,strlen(s),&c,e,sizeof(e)));return c;}
static void fingerprint(Config *c,char h[65]){char e[256];assert(!naki_config_fingerprint(c,h,e,sizeof(e)));}
int main(void){
 Config a=parse("{\"input\":\"i\",\"output\":\"o\"}");
 Config b=parse("{\"input\":\"i\",\"output\":\"o\",\"decoder_early_output\":true}");
 Config c=parse("{\"input\":\"i\",\"output\":\"o\",\"decoder_early_output\":false}");
 assert(a.decoder_early_output==-1&&b.decoder_early_output==1&&!c.decoder_early_output);
 char x[65],y[65],z[65];fingerprint(&a,x);fingerprint(&b,y);fingerprint(&c,z);assert(strcmp(x,y)&&strcmp(x,z)&&strcmp(y,z));
 const char *bad[]={"{\"input\":\"i\",\"output\":\"o\",\"decoder_early_output\":1}","{\"input\":\"i\",\"output\":\"o\",\"decoder_early_output\":null}"};
 for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++){char e[256];assert(config_parse(bad[i],strlen(bad[i]),&a,e,sizeof(e))<0);}
 for(int id=0;id<2;id++){
  const AVCodec *codec=avcodec_find_decoder(id?AV_CODEC_ID_HEVC:AV_CODEC_ID_H264);assert(codec);
  for(int async=0;async<=1;async++)for(int early=0;early<=1;early++){
   AVCodecContext *ctx=avcodec_alloc_context3(codec);assert(ctx);ctx->thread_count=2;ctx->thread_type=FF_THREAD_FRAME;
   assert(!naki_decode_output_policy(ctx,early));int64_t got=-1;
   int ret=av_opt_get_int(ctx->priv_data,"live_poc_early_output",0,&got);
   if(id || ret>=0)assert(!ret&&got==early);
   if(!id && av_opt_find(ctx->priv_data,"live_poc_verified_even",NULL,0,0)){assert(!av_opt_get_int(ctx->priv_data,"live_poc_verified_even",0,&got));assert(got==0);}
   assert(!avcodec_open2(ctx,codec,NULL));AVCodecAsyncContext *owner=NULL;
   if(async){assert(!avcodec_async_create(&owner,&ctx,1,NULL,NULL));avcodec_async_free(&owner);}else avcodec_free_context(&ctx);
  }
 }
 puts("PASS config default/false/type/fingerprint and HEVC/H264 independent async x early initialization");
}
