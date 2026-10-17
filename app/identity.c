#define _GNU_SOURCE
#include "identity.h"
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/random.h>
static char process_id[33];
int naki_process_init(void) {return *process_id?0:naki_id_generate(process_id);}
const char *naki_process_id(void) {return process_id;}
int naki_id_valid(const char *value,size_t maximum) {
    if(!value||!maximum)return 0;
    size_t n=strnlen(value,maximum+1);if(!n||n>maximum)return 0;
    for(size_t i=0;i<n;i++) {
        unsigned char c=(unsigned char)value[i];
        if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'))return 0;
    }
    return 1;
}
int naki_id_generate(char out[33]) {
    uint8_t bytes[16];size_t done=0;const char hex[]="0123456789abcdef";
    while(done<sizeof(bytes)) {
        ssize_t n=getrandom(bytes+done,sizeof(bytes)-done,GRND_NONBLOCK);
        if(n<0&&errno==EINTR)continue;
        if(n<=0)return -1;
        done+=(size_t)n;
    }
    for(size_t i=0;i<sizeof(bytes);i++) {out[i*2]=hex[bytes[i]>>4];out[i*2+1]=hex[bytes[i]&15];}
    out[32]=0;return 0;
}
