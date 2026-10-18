#include "sei_config.h"
#include <stdio.h>
#include <string.h>
static int bad(char *e,size_t n,const char *why) {snprintf(e,n,"sei: %s",why);return -1;}
int naki_sei_selected(const NakiSeiConfig *c,uint32_t type) {
    for(unsigned i=0;i<c->count;i++)if(c->types[i]==type)return 1;
    return 0;
}
int naki_sei_union(NakiSeiConfig *out,const NakiSeiConfig *add) {
    for(unsigned i=0;i<add->count;i++)if(!naki_sei_selected(out,add->types[i])) {
        if(out->count==NAKI_SEI_UNION_TYPES)return -1;
        out->types[out->count++]=add->types[i];
    }
    for(unsigned i=1;i<out->count;i++)for(unsigned j=i;j&&out->types[j]<out->types[j-1];j--) {
        uint32_t t=out->types[j];out->types[j]=out->types[j-1];out->types[j-1]=t;
    }
    return 0;
}
int naki_sei_config(yyjson_val *v,NakiSeiConfig *out,char *e,size_t n) {
    *out=(NakiSeiConfig){0};if(!v)return 0;
    if(!yyjson_is_obj(v)||yyjson_obj_size(v)!=1)return bad(e,n,"expected {types: [...]} only");
    yyjson_val *a=yyjson_obj_get(v,"types");
    if(!yyjson_is_arr(a)||yyjson_arr_size(a)>NAKI_SEI_TYPES)return bad(e,n,"types must contain 0..64 entries");
    size_t i,m;yyjson_val *item;
    yyjson_arr_foreach(a,i,m,item) {
        uint32_t type;
        if(yyjson_is_str(item)&&yyjson_get_len(item)==4&&!memcmp(yyjson_get_str(item),"uuid",4))type=5;
        else if(yyjson_is_uint(item)&&yyjson_get_uint(item)<=65535)type=(uint32_t)yyjson_get_uint(item);
        else return bad(e,n,"type must be uuid or an integer in 0..65535");
        if(naki_sei_selected(out,type))return bad(e,n,"duplicate type (uuid aliases 5)");
        NakiSeiConfig one={.count=1,.types={type}};if(naki_sei_union(out,&one))return bad(e,n,"type budget exceeded");
    }
    return 0;
}
