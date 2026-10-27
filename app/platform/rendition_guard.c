#define _GNU_SOURCE
#include "rendition_guard.h"
#include <errno.h>
#include <stddef.h>
#include <sched.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#if defined(__x86_64__)
#define EXPECTED_ARCH AUDIT_ARCH_X86_64
#elif defined(__aarch64__)
#define EXPECTED_ARCH AUDIT_ARCH_AARCH64
#endif
int naki_rendition_guard(void) {
#ifndef EXPECTED_ARCH
    return -ENOTSUP;
#else
    struct sock_filter code[]={
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,arch)),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,EXPECTED_ARCH,1,0),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,nr)),
#if defined(__x86_64__)
        BPF_JUMP(BPF_JMP|BPF_JGE|BPF_K,0x40000000U,0,1),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|ENOSYS),
#endif
#ifdef SYS_clone3
        /* Classic BPF cannot safely inspect the pointed clone_args. ENOSYS
         * makes libc's pthread implementation use its clone fallback. */
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_clone3,0,1),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|ENOSYS),
#endif
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_clone,0,4),
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,args[0])),
        BPF_JUMP(BPF_JMP|BPF_JSET|BPF_K,CLONE_THREAD,1,0),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|EPERM),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW),
#ifdef SYS_fork
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_fork,0,1),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|EPERM),
#endif
#ifdef SYS_vfork
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_vfork,0,1),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|EPERM),
#endif
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_execve,0,1),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|EPERM),
#ifdef SYS_execveat
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_execveat,0,1),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|EPERM),
#endif
        /* No SCM_RIGHTS export. Classic BPF cannot inspect msghdr safely;
         * v1 denies these calls entirely and advertises that capability bound. */
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_sendmsg,0,1),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|EPERM),
#ifdef SYS_sendmmsg
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_sendmmsg,0,1),
        /* libc DNS has a sendto fallback for unavailable batching. */
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|ENOSYS),
#endif
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW)
    };
    struct sock_fprog program={.len=sizeof(code)/sizeof(*code),.filter=code};
    if(prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0))return -errno;
    long ret=syscall(SYS_seccomp,SECCOMP_SET_MODE_FILTER,SECCOMP_FILTER_FLAG_TSYNC,&program);
    return ret<0?-errno:ret?-ENOTSUP:0;
#endif
}
