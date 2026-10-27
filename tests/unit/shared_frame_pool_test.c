#define _GNU_SOURCE
#include "media/shared_frame_pool.h"
#include <limits.h>
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
/* Test-only same-executable messages. Production media descriptors must use
 * their own explicit versioned encoding, never memcpy a runtime AVFrame. */
typedef struct Message {NakiPoolToken token;unsigned char pattern;} Message;
typedef struct Child {pid_t pid;int control,pidfd;} Child;
static void send_exact(int fd,const void *p,size_t n){assert(send(fd,p,n,MSG_NOSIGNAL)==(ssize_t)n);}
static void receive_exact(int fd,void *p,size_t n){assert(recv(fd,p,n,MSG_TRUNC)==(ssize_t)n);}
static int child_main(const char *id) {
    uint64_t pool=strtoull(id,NULL,10);NakiPoolView view={0};
    assert(!naki_pool_view_open(&view,3,pool));
    assert(fcntl(3,F_GETFL)>=0&&(fcntl(3,F_GETFL)&O_ACCMODE)==O_RDONLY);
    assert(mprotect((void *)view.mapping,view.mapping_bytes,PROT_READ|PROT_WRITE)<0&&errno==EACCES);
    char path[64];snprintf(path,sizeof(path),"/proc/self/fd/3");
    int rw=open(path,O_RDWR|O_CLOEXEC);assert(rw>=0);
    unsigned char value=99;assert(pwrite(rw,&value,1,(off_t)view.data_offset)<0&&errno==EPERM);
    assert(ftruncate(rw,(off_t)view.mapping_bytes+4096)<0&&errno==EPERM);
    assert(mmap(NULL,view.mapping_bytes,PROT_READ|PROT_WRITE,MAP_SHARED,rw,0)==MAP_FAILED&&errno==EPERM);
    assert(fcntl(rw,F_ADD_SEALS,F_SEAL_WRITE)<0&&errno==EPERM);close(rw);
    send_exact(4,"R",1);
    Message msg;
    while(recv(4,&msg,sizeof(msg),MSG_TRUNC)==sizeof(msg)) {
        const uint8_t *pixels=naki_pool_view_claim(&view,&msg.token);assert(pixels);
        assert(!naki_pool_view_claim(&view,&msg.token));
        for(size_t i=0;i<msg.token.bytes;i++)assert(pixels[i]==msg.pattern);
        send_exact(4,"A",1); /* No pixel access after this acknowledgement. */
    }
    naki_pool_view_close(&view);return 0;
}
static Child spawn_reader(int readonly,uint64_t pool) {
    int sockets[2];assert(!socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,sockets));
    struct timeval timeout={.tv_sec=5};assert(!setsockopt(sockets[0],SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout)));
    int ro=fcntl(readonly,F_DUPFD_CLOEXEC,16),ctl=fcntl(sockets[1],F_DUPFD_CLOEXEC,16);assert(ro>=0&&ctl>=0);
    posix_spawn_file_actions_t actions;assert(!posix_spawn_file_actions_init(&actions));
    assert(!posix_spawn_file_actions_adddup2(&actions,ro,3));assert(!posix_spawn_file_actions_adddup2(&actions,ctl,4));
    assert(!posix_spawn_file_actions_addclosefrom_np(&actions,5));
    char id[32];snprintf(id,sizeof(id),"%llu",(unsigned long long)pool);
    char *args[]={"shared-frame-pool-test","--reader",id,NULL};Child c={.control=sockets[0],.pidfd=-1};
    assert(!posix_spawn(&c.pid,"/proc/self/exe",&actions,NULL,args,environ));posix_spawn_file_actions_destroy(&actions);
    close(ro);close(ctl);close(sockets[1]);c.pidfd=(int)syscall(SYS_pidfd_open,c.pid,0);assert(c.pidfd>=0);
    char ready;receive_exact(c.control,&ready,1);assert(ready=='R');return c;
}
static void signal_child(Child *c,int sig){assert(!syscall(SYS_pidfd_send_signal,c->pidfd,sig,NULL,0));}
static void stop_child(Child *c) {
    signal_child(c,SIGKILL);int status;assert(waitpid(c->pid,&status,0)==c->pid);assert(WIFSIGNALED(status)&&WTERMSIG(status)==SIGKILL);
    close(c->control);close(c->pidfd);
}
static NakiPoolToken acquire(NakiFramePool *p,unsigned char pattern) {
    uint8_t *data;NakiPoolToken token;assert(!naki_pool_acquire(p,&token,&data));memset(data,pattern,128);
    assert(!naki_pool_publish(p,&token,128));return token;
}
static void inspect(Child *c,const NakiPoolToken *token,unsigned char pattern) {
    Message msg={.token=*token,.pattern=pattern};send_exact(c->control,&msg,sizeof(msg));char ack;receive_exact(c->control,&ack,1);assert(ack=='A');
}
static unsigned descriptors(void) {
    DIR *d=opendir("/proc/self/fd");assert(d);unsigned count=0;struct dirent *e;
    while((e=readdir(d)))if(strcmp(e->d_name,".")&&strcmp(e->d_name,".."))count++;
    closedir(d);return count;
}
int main(int argc,char **argv) {
    if(argc==3&&!strcmp(argv[1],"--reader"))return child_main(argv[2]);
    assert(argc==1);struct rlimit cores={0};assert(!setrlimit(RLIMIT_CORE,&cores));
    unsigned before=descriptors();NakiFramePool *pool=NULL;
    assert(naki_pool_open(&pool,0,4096,1<<20)==-EINVAL&&pool==NULL);
    assert(naki_pool_open(&pool,1025,4096,1<<20)==-EINVAL&&pool==NULL);
    assert(naki_pool_open(&pool,5,SIZE_MAX,1<<20)==-EOVERFLOW&&pool==NULL);
    assert(naki_pool_open(&pool,5,4096,4096)==-ENOSPC&&pool==NULL);
    assert(!naki_pool_open(&pool,5,4096,1<<20));int fd=naki_pool_read_fd(pool);assert(fd>=0);
    int fake=eventfd(0,EFD_CLOEXEC);assert(fake>=0);assert(naki_pool_join(pool,0,1,2,fake)==-EINVAL);close(fake);
    Child stalled=spawn_reader(fd,naki_pool_id(pool)),healthy=spawn_reader(fd,naki_pool_id(pool));
    assert(!naki_pool_join(pool,0,1,2,stalled.pidfd));assert(!naki_pool_join(pool,1,1,2,healthy.pidfd));
    assert(naki_pool_join(pool,2,1,1,healthy.pidfd)==-ENOSPC);
    NakiPoolToken held[2];
    for(unsigned i=0;i<2;i++) {
        held[i]=acquire(pool,0xa0+i);assert(!naki_pool_offer(pool,&held[i],0,1));inspect(&stalled,&held[i],0xa0+i);
        assert(naki_pool_offer(pool,&held[i],0,1)==-EALREADY);assert(!naki_pool_producer_release(pool,&held[i]));
    }
    signal_child(&stalled,SIGSTOP);
    assert(naki_pool_reap(pool,0,1)==-EAGAIN);assert(naki_pool_close(&pool)==-EBUSY&&pool);
    NakiPoolView view={0};assert(!naki_pool_view_open(&view,fd,naki_pool_id(pool)));
    for(unsigned i=0;i<128;i++) {
        NakiPoolToken t=acquire(pool,(unsigned char)i);
        assert(t.slot!=held[0].slot&&t.slot!=held[1].slot);
        assert(naki_pool_offer(pool,&t,0,1)==-ENOBUFS);
        assert(!naki_pool_offer(pool,&t,1,1));inspect(&healthy,&t,(unsigned char)i);
        assert(!naki_pool_producer_release(pool,&t));assert(!naki_pool_release(pool,&t,1,1));
        assert(naki_pool_release(pool,&t,1,1)==-ESTALE);
    }
    for(unsigned i=0;i<2;i++) {const uint8_t *data=naki_pool_view_data(&view,&held[i]);assert(data&&data[0]==0xa0+i);}
    NakiPoolStats stats;naki_pool_stats(pool,&stats);assert(stats.occupied==2&&stats.retained[0]==2&&!stats.retained[1]);
    stop_child(&stalled);assert(!naki_pool_reap(pool,0,1));naki_pool_stats(pool,&stats);assert(!stats.occupied&&!stats.retained[0]);
    assert(naki_pool_release(pool,&held[0],0,1)==-ESTALE);
    assert(naki_pool_join(pool,0,1,2,healthy.pidfd)==-ESTALE);
    assert(!naki_pool_join(pool,0,2,2,healthy.pidfd));
    NakiPoolToken token=acquire(pool,0x77);assert(!naki_pool_offer(pool,&token,0,2));assert(!naki_pool_producer_release(pool,&token));
    assert(naki_pool_release(pool,&token,0,1)==-ESTALE);assert(!naki_pool_release(pool,&token,0,2));
    token=acquire(pool,0x78);NakiPoolToken wrong=token;wrong.pool_id++;
    assert(naki_pool_offer(pool,&wrong,1,1)==-ESTALE);assert(!naki_pool_producer_release(pool,&token));
    stop_child(&healthy);assert(!naki_pool_reap(pool,1,1));assert(!naki_pool_reap(pool,0,2));
    naki_pool_view_close(&view);close(fd);assert(!naki_pool_close(&pool)&&!pool);assert(descriptors()==before);
    assert(!naki_pool_open(&pool,3,4096,16384));assert(!naki_pool_join_local(pool,0,1,2));
    /* An immediately returned frame must reuse its committed slot rather than
     * fault every sparse slot into physical memory during serial playback. */
    unsigned reused=UINT_MAX;
    for(unsigned i=0;i<20;i++){NakiPoolToken t=acquire(pool,0x70);if(i)assert(t.slot==reused);reused=t.slot;assert(!naki_pool_producer_release(pool,&t));}
    assert(naki_pool_reap(pool,0,1)==-ESTALE); /* Local proof cannot impersonate a pidfd. */
    token=acquire(pool,0x79);assert(!naki_pool_offer(pool,&token,0,1));assert(!naki_pool_producer_release(pool,&token));
    assert(!naki_pool_reap_local_joined(pool,0,1));assert(!naki_pool_close(&pool));assert(descriptors()==before);
    puts("PASS read-only/sealed mappings; independent credits; stopped/crashed holder; stale ACK/instance; exact FD cleanup");return 0;
}
