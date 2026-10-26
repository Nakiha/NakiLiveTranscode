#define _DEFAULT_SOURCE
#include "rendition_message.h"
#include <errno.h>
#include <endian.h>
#include <string.h>
#include <sys/socket.h>
static void put32(uint8_t *p,uint32_t v){v=htole32(v);memcpy(p,&v,4);}
static void put64(uint8_t *p,uint64_t v){v=htole64(v);memcpy(p,&v,8);}
static uint32_t get32(const uint8_t *p){uint32_t v;memcpy(&v,p,4);return le32toh(v);}
static uint64_t get64(const uint8_t *p){uint64_t v;memcpy(&v,p,8);return le64toh(v);}
static int id(const char *s){if(strlen(s)!=32)return 0;for(unsigned i=0;i<32;i++)if(!((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f')))return 0;return 1;}
static int valid(const NakiRenditionMessage *m) {
    if(!id(m->session)||!id(m->instance)||!m->rendition_generation||m->kind<NAKI_RMSG_VIDEO||m->kind>NAKI_RMSG_RELEASE)return 0;
    if(m->kind==NAKI_RMSG_EOF)return !m->media&&!m->token.pool_id&&!m->token.bytes&&!m->token.generation&&!m->frame_id;
    return (m->media==NAKI_RMSG_VIDEO||m->media==NAKI_RMSG_AUDIO)&&m->source_generation&&m->frame_id&&m->ingress_us>0&&m->token.pool_id&&m->token.generation&&m->token.bytes&&m->token.slot<NAKI_POOL_MAX_SLOTS;
}
int naki_rendition_send(int fd,const NakiRenditionMessage *m) {
    if(!m||!valid(m))return -EINVAL;
    uint8_t b[NAKI_RENDITION_MESSAGE_BYTES]={0};memcpy(b,"NKRM",4);put32(b+4,1);put32(b+8,m->kind);put32(b+12,m->media);
    memcpy(b+16,m->session,32);memcpy(b+48,m->instance,32);put64(b+80,m->rendition_generation);put64(b+88,m->source_generation);
    put64(b+96,m->frame_id);put64(b+104,m->ingress_us);put64(b+112,m->token.pool_id);put64(b+120,m->token.generation);
    put64(b+128,m->token.bytes);put32(b+136,m->token.slot);
    ssize_t n;do{n=send(fd,b,sizeof(b),MSG_DONTWAIT|MSG_NOSIGNAL);}while(n<0&&errno==EINTR);
    return n==(ssize_t)sizeof(b)?0:n<0?-errno:-EIO;
}
int naki_rendition_receive(int fd,NakiRenditionMessage *m) {
    uint8_t b[NAKI_RENDITION_MESSAGE_BYTES];ssize_t n;
    do{n=recv(fd,b,sizeof(b),MSG_DONTWAIT|MSG_TRUNC);}while(n<0&&errno==EINTR);
    if(n<0)return -errno;
    if(!n)return -EPIPE;
    if(n!=sizeof(b)||memcmp(b,"NKRM",4)||get32(b+4)!=1)return -EPROTO;
    for(unsigned i=140;i<sizeof(b);i++)if(b[i])return -EPROTO;
    *m=(NakiRenditionMessage){.kind=get32(b+8),.media=get32(b+12),.rendition_generation=get64(b+80),.source_generation=get64(b+88),
        .frame_id=get64(b+96),.ingress_us=(int64_t)get64(b+104),.token={.pool_id=get64(b+112),.generation=get64(b+120),.bytes=get64(b+128),.slot=get32(b+136)}};
    memcpy(m->session,b+16,32);memcpy(m->instance,b+48,32);return valid(m)?0:-EPROTO;
}
