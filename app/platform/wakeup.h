#ifndef NAKI_WAKEUP_H
#define NAKI_WAKEUP_H
#include <errno.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sys/eventfd.h>
#include <unistd.h>

/* One consumer, any number of notifiers. Clear BEFORE checking shared work;
 * a notification between that check and poll remains level-triggered. No
 * producer takes a consumer mutex or waits for the counter to be drained. */
typedef struct NakiWakeup { int fd; atomic_int error; } NakiWakeup;
static inline int naki_wakeup_init(NakiWakeup *w,int enabled) {
    w->fd=-1;atomic_init(&w->error,0);
    if(enabled&&(w->fd=eventfd(0,EFD_NONBLOCK|EFD_CLOEXEC))<0)return -errno;
    return 0;
}
static inline void naki_wakeup_notify(NakiWakeup *w) {
    if(w->fd<0)return;
    uint64_t one=1;ssize_t n;
    do {n=write(w->fd,&one,sizeof(one));} while(n<0&&errno==EINTR);
    if(n<0&&errno!=EAGAIN)atomic_store(&w->error,errno);
}
static inline int naki_wakeup_clear(NakiWakeup *w) {
    int error=atomic_load(&w->error);if(error)return -error;
    uint64_t count;ssize_t n;
    do {n=read(w->fd,&count,sizeof(count));} while(n<0&&errno==EINTR);
    return n<0&&errno!=EAGAIN?-errno:0;
}
static inline int naki_wakeup_wait(NakiWakeup *w,int timeout_ms) {
    int error=atomic_load(&w->error);if(error)return -error;
    struct pollfd fd={.fd=w->fd,.events=POLLIN};
    int n=poll(&fd,1,timeout_ms);
    if(n<0)return errno==EINTR?0:-errno;
    if(fd.revents&POLLNVAL)return -EBADF;
    if(fd.revents&(POLLERR|POLLHUP))return -EIO;
    return 0;
}
static inline void naki_wakeup_destroy(NakiWakeup *w) {
    if(w->fd>=0)close(w->fd);
    w->fd=-1;
}
#endif
