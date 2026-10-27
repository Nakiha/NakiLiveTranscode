#define _GNU_SOURCE
#include "platform/rendition_guard.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
static void *run(void *p){*(int *)p=1;return NULL;}
int main(void) {
    pid_t child=fork();assert(child>=0);
    if(!child) {
        int sockets[2];assert(!socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,sockets));
        int exported=open("/dev/null",O_RDONLY|O_CLOEXEC);assert(exported>=0);
        int ret=naki_rendition_guard();
        if(ret){fprintf(stderr,"guard unavailable: %d\n",ret);_exit(77);}
        int done=0;pthread_t thread;assert(!pthread_create(&thread,NULL,run,&done));assert(!pthread_join(thread,NULL));assert(done);
        errno=0;assert(fork()==-1&&errno==EPERM);
        char *args[]={"true",NULL};errno=0;assert(execv("/usr/bin/true",args)==-1&&errno==EPERM);
        char byte=1,control[CMSG_SPACE(sizeof(int))]={0};struct iovec iov={.iov_base=&byte,.iov_len=1};
        struct msghdr message={.msg_iov=&iov,.msg_iovlen=1,.msg_control=control,.msg_controllen=sizeof(control)};
        struct cmsghdr *header=CMSG_FIRSTHDR(&message);header->cmsg_level=SOL_SOCKET;header->cmsg_type=SCM_RIGHTS;header->cmsg_len=CMSG_LEN(sizeof(int));
        *(int *)CMSG_DATA(header)=exported;errno=0;assert(sendmsg(sockets[0],&message,MSG_NOSIGNAL)==-1&&errno==EPERM);
        assert(send(sockets[0],&byte,1,MSG_NOSIGNAL)==1);char got=0;assert(recv(sockets[1],&got,1,0)==1&&got==1);
        close(exported);close(sockets[0]);close(sockets[1]);_exit(0);
    }
    int status;assert(waitpid(child,&status,0)==child);
    if(!WIFEXITED(status)||WEXITSTATUS(status)) {
        fprintf(stderr,"guard did not pass: status=%d signal=%d exit=%d\n",status,WIFSIGNALED(status)?WTERMSIG(status):0,WIFEXITED(status)?WEXITSTATUS(status):-1);return 1;
    }
    puts("PASS codec threads allowed; fork/exec/FD export denied by the kernel; bounded normal IPC allowed");return 0;
}
