#include "intent.h"
#include "identity.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavutil/mem.h>
#include <libavutil/sha.h>
typedef struct Key {const char *name;yyjson_val *value;} Key;
static int invalid(char *error,size_t cap,const char *why) {if(error&&cap)snprintf(error,cap,"%s",why);return -1;}
static int validate(yyjson_val *v,unsigned depth,unsigned *nodes,char *error,size_t cap) {
    if(!v||depth>16||++*nodes>4096)return invalid(error,cap,"JSON depth/node budget exceeded");
    if(yyjson_is_obj(v)) {
        if(yyjson_obj_size(v)>256)return invalid(error,cap,"JSON object exceeds 256 keys");
        const char *seen[256];size_t count=0,index,max;yyjson_val *key,*value;
        yyjson_obj_foreach(v,index,max,key,value) {
            const char *name=yyjson_get_str(key);size_t length=yyjson_get_len(key);
            if(!length||length>128||strlen(name)!=length)return invalid(error,cap,"Invalid JSON key");
            for(size_t n=0;n<count;n++)if(!strcmp(name,seen[n]))return invalid(error,cap,"Duplicate JSON key");
            seen[count++]=name;
            if(validate(value,depth+1,nodes,error,cap))return -1;
        }
    } else if(yyjson_is_arr(v)) {
        if(yyjson_arr_size(v)>512)return invalid(error,cap,"JSON array exceeds 512 items");
        size_t index,max;yyjson_val *value;
        yyjson_arr_foreach(v,index,max,value)if(validate(value,depth+1,nodes,error,cap))return -1;
    } else if(yyjson_is_str(v)) {
        if(yyjson_get_len(v)>4096||strlen(yyjson_get_str(v))!=yyjson_get_len(v))return invalid(error,cap,"Invalid JSON string");
    } else if(yyjson_is_real(v)&&!isfinite(yyjson_get_real(v)))return invalid(error,cap,"Nonfinite JSON number");
    return 0;
}
int naki_intent_validate(yyjson_val *value,char *error,size_t cap) {unsigned nodes=0;return validate(value,0,&nodes,error,cap);}
int naki_json_id(yyjson_val *value,size_t maximum) {
    return yyjson_is_str(value)&&strlen(yyjson_get_str(value))==yyjson_get_len(value)&&naki_id_valid(yyjson_get_str(value),maximum);
}
static void size_hash(struct AVSHA *sha,uint64_t size) {
    uint8_t encoded[8];for(unsigned i=0;i<8;i++)encoded[i]=(uint8_t)(size>>(i*8));av_sha_update(sha,encoded,sizeof(encoded));
}
static void token(struct AVSHA *sha,char tag,const char *bytes,size_t count) {
    av_sha_update(sha,(const uint8_t *)&tag,1);size_hash(sha,count);if(count)av_sha_update(sha,(const uint8_t *)bytes,count);
}
static int key_compare(const void *a,const void *b) {return strcmp(((const Key *)a)->name,((const Key *)b)->name);}
static void hash_value(struct AVSHA *sha,yyjson_val *v) {
    if(yyjson_is_obj(v)) {
        Key keys[256];size_t count=0,index,max;yyjson_val *key,*value;
        yyjson_obj_foreach(v,index,max,key,value)keys[count++]=(Key){yyjson_get_str(key),value};
        qsort(keys,count,sizeof(keys[0]),key_compare);token(sha,'o',NULL,0);size_hash(sha,count);
        for(size_t n=0;n<count;n++) {token(sha,'k',keys[n].name,strlen(keys[n].name));hash_value(sha,keys[n].value);}
    } else if(yyjson_is_arr(v)) {
        token(sha,'a',NULL,0);size_hash(sha,yyjson_arr_size(v));size_t index,max;yyjson_val *value;
        yyjson_arr_foreach(v,index,max,value)hash_value(sha,value);
    } else if(yyjson_is_str(v))token(sha,'s',yyjson_get_str(v),yyjson_get_len(v));
    else if(yyjson_is_num(v)) {
        char text[64];
        if(yyjson_is_sint(v))snprintf(text,sizeof(text),"%lld",(long long)yyjson_get_sint(v));
        else if(yyjson_is_uint(v))snprintf(text,sizeof(text),"%llu",(unsigned long long)yyjson_get_uint(v));
        else {double n=yyjson_get_real(v);snprintf(text,sizeof(text),"%.17g",n==0?0:n);}
        token(sha,'d',text,strlen(text));
    } else token(sha,yyjson_is_null(v)?'n':yyjson_get_bool(v)?'t':'f',NULL,0);
}
int naki_intent_fingerprint(yyjson_val *value,char out[65],char *error,size_t cap) {
    if(naki_intent_validate(value,error,cap))return -1;
    struct AVSHA *sha=av_sha_alloc();if(!sha)return invalid(error,cap,"Fingerprint allocation failed");
    uint8_t digest[32];av_sha_init(sha,256);token(sha,'v',"naki-intent-v1",14);hash_value(sha,value);av_sha_final(sha,digest);av_free(sha);
    const char hex[]="0123456789abcdef";for(unsigned i=0;i<32;i++) {out[i*2]=hex[digest[i]>>4];out[i*2+1]=hex[digest[i]&15];}
    out[64]=0;return 0;
}
