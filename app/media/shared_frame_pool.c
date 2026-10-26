#define _GNU_SOURCE
#include "shared_frame_pool.h"
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>
#ifndef F_SEAL_FUTURE_WRITE
#define F_SEAL_FUTURE_WRITE 0x0010
#endif
#define REQUIRED_SEALS (F_SEAL_FUTURE_WRITE|F_SEAL_GROW|F_SEAL_SHRINK|F_SEAL_SEAL)
#define PRODUCER_BIT (1U<<NAKI_POOL_MEMBERS)
#define HEADER_SIZE 64U
/* The first page is a versioned, explicit little-endian descriptor. There are
 * no process-shared pointers, locks, lease counters or writable credit fields. */
struct Slot {uint64_t generation,bytes;unsigned refs;int published;};
struct Member {uint64_t instance;unsigned credit,retained;int pidfd,active,local;};
struct NakiFramePool {
    pthread_mutex_t mutex;
    uint8_t *mapping;
    size_t mapping_bytes,data_offset,slot_bytes;
    uint64_t id;
    unsigned count,cursor;
    int readonly_fd,producer;
    struct Member members[NAKI_POOL_MEMBERS];
    struct Slot slots[];
};
static void put32(uint8_t *p,uint32_t v){v=htole32(v);memcpy(p,&v,4);}
static void put64(uint8_t *p,uint64_t v){v=htole64(v);memcpy(p,&v,8);}
static uint32_t get32(const uint8_t *p){uint32_t v;memcpy(&v,p,4);return le32toh(v);}
static uint64_t get64(const uint8_t *p){uint64_t v;memcpy(&v,p,8);return le64toh(v);}
static int duplicate(int fd){int copy=fcntl(fd,F_DUPFD_CLOEXEC,5);return copy<0?-errno:copy;}
static int valid_pidfd(int fd) {
    char path[64],target[64];snprintf(path,sizeof(path),"/proc/self/fd/%d",fd);
    ssize_t n=readlink(path,target,sizeof(target)-1);
    if(n<0)return -errno;
    target[n]=0;return strcmp(target,"anon_inode:[pidfd]")?-EINVAL:0;
}
int naki_pool_open(NakiFramePool **out,unsigned count,size_t bytes,uint64_t limit) {
    if(!out||*out||!count||count>NAKI_POOL_MAX_SLOTS||!bytes)return -EINVAL;
    long page=sysconf(_SC_PAGESIZE);
    if(page<HEADER_SIZE||((unsigned long)page&((unsigned long)page-1)))return -ENOSYS;
    if(bytes>SIZE_MAX-(size_t)page+1)return -EOVERFLOW;
    bytes=(bytes+(size_t)page-1)&~((size_t)page-1);
    if(bytes>((size_t)PTRDIFF_MAX-(size_t)page)/count)return -EOVERFLOW;
    size_t total=(size_t)page+bytes*count;
    if(total>limit)return -ENOSPC;
    NakiFramePool *p=calloc(1,sizeof(*p)+count*sizeof(*p->slots));
    if(!p)return -ENOMEM;
    p->readonly_fd=-1;p->producer=-1;
    int ret=pthread_mutex_init(&p->mutex,NULL);if(ret){free(p);return -ret;}
    int fd=memfd_create("naki-frame-pool",MFD_CLOEXEC|MFD_ALLOW_SEALING);
    if(fd<0){ret=-errno;goto fail;}
    if(ftruncate(fd,(off_t)total)){ret=-errno;goto fail_fd;}
    p->mapping=mmap(NULL,total,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    if(p->mapping==MAP_FAILED){p->mapping=NULL;ret=-errno;goto fail_fd;}
    p->mapping_bytes=total;p->data_offset=(size_t)page;p->slot_bytes=bytes;p->count=count;
    ssize_t got;
    do {got=getrandom(&p->id,sizeof(p->id),0);} while(got<0&&errno==EINTR);
    if(got!=(ssize_t)sizeof(p->id)||!p->id){ret=got<0?-errno:-EIO;goto fail_fd;}
    memcpy(p->mapping,"NKFP",4);put32(p->mapping+4,1);put64(p->mapping+8,total);
    put64(p->mapping+16,p->id);put32(p->mapping+24,count);
    put64(p->mapping+32,bytes);put64(p->mapping+40,p->data_offset);
    /* Existing producer mapping stays writable. New shared writable mappings,
     * pwrite and resize are prohibited even if a consumer reopens its FD. */
    if(fcntl(fd,F_ADD_SEALS,REQUIRED_SEALS)<0){ret=-errno;goto fail_fd;}
    char path[64];snprintf(path,sizeof(path),"/proc/self/fd/%d",fd);
    p->readonly_fd=open(path,O_RDONLY|O_CLOEXEC);
    if(p->readonly_fd<0){ret=-errno;goto fail_fd;}
    close(fd);*out=p;return 0;
fail_fd:
    close(fd);if(p->mapping)munmap(p->mapping,p->mapping_bytes);
fail:
    pthread_mutex_destroy(&p->mutex);free(p);return ret;
}
int naki_pool_read_fd(NakiFramePool *p){return p?duplicate(p->readonly_fd):-EINVAL;}
uint64_t naki_pool_id(const NakiFramePool *p){return p?p->id:0;}
size_t naki_pool_stride(const NakiFramePool *p){return p?p->slot_bytes:0;}
static int join_member(NakiFramePool *p,unsigned member,uint64_t instance,unsigned credit,int pidfd,int local) {
    if(!p||member>=NAKI_POOL_MEMBERS||!instance||!credit||credit>=p->count)return -EINVAL;
    int ret=local?0:valid_pidfd(pidfd);if(ret<0)return ret;
    int copy=local?-1:duplicate(pidfd);if(!local&&copy<0)return copy;
    pthread_mutex_lock(&p->mutex);
    struct Member *m=&p->members[member];unsigned reserved=1;
    for(unsigned i=0;i<NAKI_POOL_MEMBERS;i++)if(p->members[i].active)reserved+=p->members[i].credit;
    ret=m->active?-EBUSY:instance<=m->instance?-ESTALE:credit>p->count-reserved?-ENOSPC:0;
    if(!ret)*m=(struct Member){.active=1,.instance=instance,.credit=credit,.pidfd=copy,.local=local};
    pthread_mutex_unlock(&p->mutex);if(ret&&copy>=0)close(copy);return ret;
}
int naki_pool_join(NakiFramePool *p,unsigned member,uint64_t instance,unsigned credit,int pidfd){return join_member(p,member,instance,credit,pidfd,0);}
int naki_pool_join_local(NakiFramePool *p,unsigned member,uint64_t instance,unsigned credit){return join_member(p,member,instance,credit,-1,1);}
static struct Slot *find(NakiFramePool *p,const NakiPoolToken *t) {
    if(!t||t->pool_id!=p->id||t->slot>=p->count)return NULL;
    struct Slot *s=&p->slots[t->slot];
    return s->refs&&s->generation==t->generation&&s->bytes==t->bytes?s:NULL;
}
int naki_pool_acquire(NakiFramePool *p,NakiPoolToken *t,uint8_t **pixels) {
    if(!p||!t||!pixels)return -EINVAL;
    *pixels=NULL;pthread_mutex_lock(&p->mutex);int ret=-ENOBUFS;
    if(p->producer>=0){ret=-EBUSY;goto done;}
    for(unsigned n=0;n<p->count;n++) {
        unsigned i=(p->cursor+n)%p->count;struct Slot *s=&p->slots[i];
        if(s->refs)continue;
        if(s->generation==UINT64_MAX){ret=-EOVERFLOW;goto done;}
        s->generation++;s->bytes=0;s->refs=PRODUCER_BIT;s->published=0;
        p->producer=(int)i;p->cursor=(i+1)%p->count;
        *t=(NakiPoolToken){.pool_id=p->id,.generation=s->generation,.slot=i};
        *pixels=p->mapping+p->data_offset+i*p->slot_bytes;ret=0;break;
    }
done:
    pthread_mutex_unlock(&p->mutex);return ret;
}
int naki_pool_publish(NakiFramePool *p,NakiPoolToken *t,size_t bytes) {
    if(!p||!bytes||bytes>p->slot_bytes)return -EINVAL;
    pthread_mutex_lock(&p->mutex);struct Slot *s=find(p,t);int ret=-ESTALE;
    if(s&&p->producer==(int)t->slot&&!s->published) {
        atomic_thread_fence(memory_order_release);s->bytes=bytes;t->bytes=bytes;s->published=1;ret=0;
    }
    pthread_mutex_unlock(&p->mutex);return ret;
}
int naki_pool_offer(NakiFramePool *p,const NakiPoolToken *t,unsigned member,uint64_t instance) {
    if(!p||member>=NAKI_POOL_MEMBERS)return -EINVAL;
    pthread_mutex_lock(&p->mutex);struct Member *m=&p->members[member];struct Slot *s=find(p,t);int ret;
    if(!s||!s->published||p->producer!=(int)t->slot||!m->active||m->instance!=instance)ret=-ESTALE;
    else if(s->refs&(1U<<member))ret=-EALREADY;
    else if(m->retained==m->credit)ret=-ENOBUFS;
    else {s->refs|=1U<<member;m->retained++;ret=0;}
    pthread_mutex_unlock(&p->mutex);return ret;
}
int naki_pool_release(NakiFramePool *p,const NakiPoolToken *t,unsigned member,uint64_t instance) {
    if(!p||member>=NAKI_POOL_MEMBERS)return -EINVAL;
    pthread_mutex_lock(&p->mutex);struct Member *m=&p->members[member];struct Slot *s=find(p,t);int ret=-ESTALE;
    if(s&&s->published&&m->active&&m->instance==instance&&(s->refs&(1U<<member))) {
        s->refs&=~(1U<<member);m->retained--;ret=0;
    }
    pthread_mutex_unlock(&p->mutex);return ret;
}
int naki_pool_producer_release(NakiFramePool *p,const NakiPoolToken *t) {
    if(!p)return -EINVAL;
    pthread_mutex_lock(&p->mutex);struct Slot *s=find(p,t);int ret=-ESTALE;
    if(s&&p->producer==(int)t->slot&&(s->refs&PRODUCER_BIT)) {
        s->refs&=~PRODUCER_BIT;p->producer=-1;ret=0;
    }
    pthread_mutex_unlock(&p->mutex);return ret;
}
static int reap_member(NakiFramePool *p,unsigned member,uint64_t instance,int local) {
    if(!p||member>=NAKI_POOL_MEMBERS)return -EINVAL;
    pthread_mutex_lock(&p->mutex);struct Member *m=&p->members[member];int ret=-ESTALE;
    if(m->active&&m->instance==instance&&m->local==local) {
        struct pollfd fd={.fd=m->pidfd,.events=POLLIN};int polled;
        do {polled=local?1:poll(&fd,1,0);} while(polled<0&&errno==EINTR);
        if(local)fd.revents=POLLIN;
        ret=polled<0?-errno:!(fd.revents&POLLIN)?-EAGAIN:0;
        if(!ret) {
            for(unsigned i=0;i<p->count;i++)p->slots[i].refs&=~(1U<<member);
            if(m->pidfd>=0)close(m->pidfd);
            m->pidfd=-1;m->active=0;m->retained=m->credit=0;
        }
    }
    pthread_mutex_unlock(&p->mutex);return ret;
}
int naki_pool_reap(NakiFramePool *p,unsigned member,uint64_t instance){return reap_member(p,member,instance,0);}
int naki_pool_reap_local_joined(NakiFramePool *p,unsigned member,uint64_t instance){return reap_member(p,member,instance,1);}
void naki_pool_stats(NakiFramePool *p,NakiPoolStats *out) {
    if(!p||!out)return;
    pthread_mutex_lock(&p->mutex);*out=(NakiPoolStats){.slots=p->count,.mapped_bytes=p->mapping_bytes,.producer_reserved=p->producer>=0};
    for(unsigned i=0;i<p->count;i++)out->occupied+=p->slots[i].refs!=0;
    for(unsigned i=0;i<NAKI_POOL_MEMBERS;i++){out->credits[i]=p->members[i].credit;out->retained[i]=p->members[i].retained;}
    pthread_mutex_unlock(&p->mutex);
}
int naki_pool_close(NakiFramePool **owner) {
    if(!owner||!*owner)return 0;
    NakiFramePool *p=*owner;pthread_mutex_lock(&p->mutex);int busy=p->producer>=0;
    for(unsigned i=0;i<NAKI_POOL_MEMBERS;i++)busy|=p->members[i].active;
    pthread_mutex_unlock(&p->mutex);if(busy)return -EBUSY;
    munmap(p->mapping,p->mapping_bytes);close(p->readonly_fd);pthread_mutex_destroy(&p->mutex);free(p);*owner=NULL;return 0;
}
int naki_pool_view_open(NakiPoolView *v,int fd,uint64_t expected) {
    if(!v||v->mapping||!expected)return -EINVAL;
    int flags=fcntl(fd,F_GETFL),seals=fcntl(fd,F_GET_SEALS);
    if(flags<0||seals<0)return -errno;
    if((flags&O_ACCMODE)!=O_RDONLY||(seals&REQUIRED_SEALS)!=REQUIRED_SEALS)return -EPERM;
    struct stat st;if(fstat(fd,&st))return -errno;
    if(!S_ISREG(st.st_mode)||st.st_size<HEADER_SIZE||(uint64_t)st.st_size>PTRDIFF_MAX)return -EINVAL;
    uint8_t header[HEADER_SIZE];if(pread(fd,header,sizeof(header),0)!=(ssize_t)sizeof(header))return -EIO;
    uint64_t total=get64(header+8),id=get64(header+16),stride=get64(header+32),offset=get64(header+40);unsigned slots=get32(header+24);
    long page=sysconf(_SC_PAGESIZE);
    if(page<HEADER_SIZE||memcmp(header,"NKFP",4)||get32(header+4)!=1||id!=expected||get32(header+28)||
       !slots||slots>NAKI_POOL_MAX_SLOTS||!stride||stride%(size_t)page||offset!=(uint64_t)page||
       total!=(uint64_t)st.st_size||offset>total||stride>(total-offset)/slots||offset+stride*slots!=total)return -EINVAL;
    for(unsigned i=48;i<HEADER_SIZE;i++)if(header[i])return -EINVAL;
    void *data=mmap(NULL,(size_t)total,PROT_READ,MAP_SHARED,fd,0);
    if(data==MAP_FAILED)return -errno;
    *v=(NakiPoolView){.mapping=data,.mapping_bytes=total,.data_offset=offset,.pool_id=id,.slots=slots,.slot_bytes=stride};return 0;
}
const uint8_t *naki_pool_view_data(const NakiPoolView *v,const NakiPoolToken *t) {
    if(!v||!v->mapping||!t||t->pool_id!=v->pool_id||t->slot>=v->slots||!t->generation||!t->bytes||t->bytes>v->slot_bytes)return NULL;
    atomic_thread_fence(memory_order_acquire);return v->mapping+v->data_offset+t->slot*v->slot_bytes;
}
const uint8_t *naki_pool_view_claim(NakiPoolView *v,const NakiPoolToken *t) {
    const uint8_t *pixels=naki_pool_view_data(v,t);
    if(!pixels||t->generation<=v->seen[t->slot])return NULL;
    v->seen[t->slot]=t->generation;return pixels;
}
void naki_pool_view_close(NakiPoolView *v) {
    if(!v)return;
    if(v->mapping)munmap((void *)v->mapping,v->mapping_bytes);
    *v=(NakiPoolView){0};
}
