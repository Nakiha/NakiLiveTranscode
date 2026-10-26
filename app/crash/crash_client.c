#define _GNU_SOURCE
#include "crash.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

extern char **environ;
_Static_assert(ATOMIC_INT_LOCK_FREE==2,"Fatal guard must be lock-free");
_Static_assert(sizeof(CrashRecord)==384,"Changing the fixed record requires a protocol version update");
static int channel=-1,collector_pid,initialized,ptracer_registered;
static unsigned timeout_ms=2000;
static atomic_int fatal_entered;
static struct sigaction default_action;
static sigset_t fatal_signals;
static char report_directory[4096];
static char process_instance[33];
void crash_set_instance(const char *id) {
    if(!initialized&&id&&strlen(id)==32)memcpy(process_instance,id,33);
}
static pthread_key_t stack_key;
typedef struct AltStack {void *mapping; size_t size;} AltStack;

static __attribute__((no_sanitize_address,no_sanitize_undefined)) uint64_t monotonic_us(void) {
    struct timespec t;
    if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;
    return (uint64_t)t.tv_sec*1000000+(uint64_t)t.tv_nsec/1000;
}
static __attribute__((no_sanitize_address,no_sanitize_undefined)) void finish_fatal(int sig,int pid,int tid) {
    /* Linux-specific terminal syscall, after snapshot: suppress kernel core even with piped core_pattern. */
    if(syscall(SYS_prctl,PR_SET_DUMPABLE,0,0,0,0)<0)_exit(128+sig);
    sigaction(sig,&default_action,NULL);
    syscall(SYS_tgkill,pid,tid,sig);
    sigprocmask(SIG_UNBLOCK,&fatal_signals,NULL);
    _exit(128+sig);
}
static __attribute__((no_sanitize_address,no_sanitize_undefined)) void fatal_signal(int sig,siginfo_t *info,void *context) {
    int saved_errno=errno;
    int pid=getpid(),tid=(int)syscall(SYS_gettid);
    uint64_t began=monotonic_us();
    if(!atomic_exchange_explicit(&fatal_entered,1,memory_order_relaxed)) {
        CrashRecord record={0};
        record.magic=CRASH_MAGIC; record.version=CRASH_VERSION; record.kind=CRASH_EVENT;
        record.pid=pid; record.tid=tid; record.signal=sig; record.signal_code=info->si_code;
        record.monotonic_us=began;
        if(info->si_code>0 && (sig==SIGSEGV || sig==SIGBUS || sig==SIGILL || sig==SIGFPE))record.fault_address=(uintptr_t)info->si_addr;
        ucontext_t *uc=context;
#if defined(__x86_64__)
        record.architecture=CRASH_ARCH_X86_64;
        const int indices[]={REG_RAX,REG_RDX,REG_RCX,REG_RBX,REG_RSI,REG_RDI,REG_RBP,REG_RSP,
                             REG_R8,REG_R9,REG_R10,REG_R11,REG_R12,REG_R13,REG_R14,REG_R15,REG_RIP};
        for(unsigned i=0;i<sizeof(indices)/sizeof(indices[0]);i++)record.registers[i]=(uint64_t)uc->uc_mcontext.gregs[indices[i]];
#elif defined(__aarch64__)
        record.architecture=CRASH_ARCH_AARCH64;
        for(int i=0;i<31;i++)record.registers[i]=uc->uc_mcontext.regs[i];
        record.registers[31]=uc->uc_mcontext.sp; record.registers[32]=uc->uc_mcontext.pc;
#else
#error Crash capture currently supports Linux x86_64 and AArch64
#endif
        /* One fixed packet, no retry queue, JSON, logger, unwinder, allocation or application lock. */
        ssize_t sent=send(channel,&record,sizeof(record),MSG_DONTWAIT|MSG_NOSIGNAL);
        if(sent==(ssize_t)sizeof(record)) {
            for(;;) {
                uint64_t elapsed=monotonic_us()-began;
                if(elapsed>=(uint64_t)timeout_ms*1000)break;
                int remaining=(int)(((uint64_t)timeout_ms*1000-elapsed+999)/1000);
                struct pollfd fd={.fd=channel,.events=POLLIN};
                int n=poll(&fd,1,remaining);
                if(n>=0 || errno!=EINTR)break;
            }
        }
    } else {
        /* Another faulting thread must not tear down the first snapshot immediately. */
        if(!began) {poll(NULL,0,(int)timeout_ms); finish_fatal(sig,pid,tid);}
        while(monotonic_us()-began<(uint64_t)timeout_ms*1000) {
            int n=poll(NULL,0,10); if(n<0 && errno!=EINTR)break;
        }
    }
    errno=saved_errno;
    finish_fatal(sig,pid,tid);
}
static void release_stack(void *opaque) {
    AltStack *stack=opaque;
    stack_t disabled={.ss_flags=SS_DISABLE}; sigaltstack(&disabled,NULL);
    munmap(stack->mapping,stack->size); free(stack);
}
int crash_thread_init(void) {
    if(!initialized || pthread_getspecific(stack_key))return 0;
    stack_t old;
    if(sigaltstack(NULL,&old))return -1;
    if(!(old.ss_flags&SS_DISABLE))return 0; /* Respect an existing thread-specific alternate stack. */
    size_t page=(size_t)sysconf(_SC_PAGESIZE),size=128*1024;
    size=(size+page-1)/page*page;
    AltStack *stack=calloc(1,sizeof(*stack)); if(!stack)return -1;
    stack->size=size+2*page; stack->mapping=mmap(NULL,stack->size,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(stack->mapping==MAP_FAILED) {free(stack); return -1;}
    void *usable=(char *)stack->mapping+page;
    stack_t alternate={.ss_sp=usable,.ss_size=size};
    if(mprotect(usable,size,PROT_READ|PROT_WRITE) || sigaltstack(&alternate,NULL)) {munmap(stack->mapping,stack->size); free(stack); return -1;}
    if(pthread_setspecific(stack_key,stack)) {release_stack(stack); return -1;}
    return 0;
}
void crash_options_default(CrashOptions *o) {
    *o=(CrashOptions){.timeout_ms=2000,.max_report_bytes=262144,.max_reports=32,
        .max_threads=CRASH_MAX_THREADS,.max_frames=CRASH_MAX_FRAMES,.max_total_bytes=16777216,.max_read_bytes=1048576};
}
static int env_number(const char *name,uint64_t low,uint64_t high,uint64_t *value) {
    const char *text=getenv(name); if(!text)return 0;
    char *end; errno=0;
    if(*text<'0' || *text>'9')return -1;
    unsigned long long n=strtoull(text,&end,10);
    if(errno || *end || n<low || n>high)return -1;
    *value=n; return 0;
}
int crash_options_env(CrashOptions *o) {
    crash_options_default(o);
    const char *directory=getenv("NAKI_CRASH_DIR");
    if(directory) {if(strlen(directory)>=sizeof(o->directory))return -1; strcpy(o->directory,directory);}
#define READ(name,field,low,high) do {uint64_t v=o->field; if(env_number(name,low,high,&v))return -1; o->field=v;} while(0)
    READ("NAKI_CRASH_TIMEOUT_MS",timeout_ms,100,10000);
    READ("NAKI_CRASH_MAX_REPORT_BYTES",max_report_bytes,4096,CRASH_MAX_REPORT);
    READ("NAKI_CRASH_MAX_REPORTS",max_reports,1,256);
    READ("NAKI_CRASH_MAX_TOTAL_BYTES",max_total_bytes,4096,UINT64_C(1)<<30);
    READ("NAKI_CRASH_MAX_THREADS",max_threads,1,CRASH_MAX_THREADS);
    READ("NAKI_CRASH_MAX_FRAMES",max_frames,1,CRASH_MAX_FRAMES);
    READ("NAKI_CRASH_MAX_READ_BYTES",max_read_bytes,4096,16*1024*1024);
#undef READ
    return o->max_total_bytes<o->max_report_bytes ? -1 : 0;
}
int crash_options_export(const CrashOptions *o) {
    if(setenv("NAKI_CRASH_DIR",o->directory,1))return -1;
#define SET(name,field) do {char text[32]; snprintf(text,sizeof(text),"%llu",(unsigned long long)o->field); if(setenv(name,text,1))return -1;} while(0)
    SET("NAKI_CRASH_TIMEOUT_MS",timeout_ms); SET("NAKI_CRASH_MAX_REPORT_BYTES",max_report_bytes);
    SET("NAKI_CRASH_MAX_REPORTS",max_reports); SET("NAKI_CRASH_MAX_TOTAL_BYTES",max_total_bytes);
    SET("NAKI_CRASH_MAX_THREADS",max_threads); SET("NAKI_CRASH_MAX_FRAMES",max_frames);
    SET("NAKI_CRASH_MAX_READ_BYTES",max_read_bytes);
#undef SET
    return 0;
}
static void stop_collector(void) {
    if(channel>=0) {close(channel); channel=-1;}
    if(collector_pid>0) {
        for(int i=0;i<20;i++) {
            if(waitpid(collector_pid,NULL,WNOHANG)!=0)break;
            poll(NULL,0,10);
        }
    }
}
int crash_client_start(const CrashOptions *o,const char *role) {
    if(initialized)return -1;
    char executable[PATH_MAX]; ssize_t n=readlink("/proc/self/exe",executable,sizeof(executable)-1);
    if(n<=0 || n>=(ssize_t)sizeof(executable)-1)return -1;
    executable[n]=0; char *slash=strrchr(executable,'/');
    if(!slash || (size_t)(slash-executable)+sizeof("/naki-crash-collector")>sizeof(executable))return -1;
    strcpy(slash,"/naki-crash-collector");
    int pair[2],child_fd;
    if(socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC|SOCK_NONBLOCK,0,pair))return -1;
    child_fd=fcntl(pair[1],F_DUPFD_CLOEXEC,10);
    if(child_fd<0) {close(pair[0]); close(pair[1]); return -1;}
    posix_spawn_file_actions_t actions;
    int error=posix_spawn_file_actions_init(&actions);
    if(error) {close(pair[0]); close(pair[1]); close(child_fd); return -1;}
    error=posix_spawn_file_actions_adddup2(&actions,child_fd,3);
    if(!error)error=posix_spawn_file_actions_addopen(&actions,0,"/dev/null",O_RDONLY,0);
    if(!error)error=posix_spawn_file_actions_addopen(&actions,1,"/dev/null",O_WRONLY,0);
    if(!error)error=posix_spawn_file_actions_addopen(&actions,2,"/dev/null",O_WRONLY,0);
    if(!error)error=posix_spawn_file_actions_addclosefrom_np(&actions,4);
    char target[24]; snprintf(target,sizeof(target),"%d",getpid());
    char *args[]={executable,target,(char *)role,process_instance,NULL}; pid_t pid=0;
    if(!error)error=posix_spawn(&pid,executable,&actions,NULL,args,environ);
    posix_spawn_file_actions_destroy(&actions); close(child_fd); close(pair[1]);
    if(error) {close(pair[0]); errno=error; return -1;}
    /* Hardened kernels may reject this request; retain fixed context and report actual attach failures. */
    ptracer_registered=prctl(PR_SET_PTRACER,(unsigned long)pid,0,0,0)==0;
    struct pollfd fd={.fd=pair[0],.events=POLLIN}; char ready=0;
    if(poll(&fd,1,1000)<=0 || recv(pair[0],&ready,1,MSG_DONTWAIT)!=1 || ready!='R') {
        close(pair[0]); kill(pid,SIGKILL); waitpid(pid,NULL,0); return -1;
    }
    if(pthread_key_create(&stack_key,release_stack)) {close(pair[0]); return -1;}
    channel=pair[0]; collector_pid=pid; timeout_ms=o->timeout_ms;
    strcpy(report_directory,o->directory);
    atomic_init(&fatal_entered,0); sigemptyset(&fatal_signals);
    const int signals[]={SIGSEGV,SIGBUS,SIGILL,SIGFPE,SIGABRT};
    default_action=(struct sigaction){.sa_handler=SIG_DFL}; sigemptyset(&default_action.sa_mask);
    for(unsigned i=0;i<sizeof(signals)/sizeof(signals[0]);i++)sigaddset(&fatal_signals,signals[i]);
    struct sigaction action={.sa_sigaction=fatal_signal,.sa_flags=SA_SIGINFO|SA_ONSTACK}; sigfillset(&action.sa_mask);
    initialized=1;
    if(crash_thread_init())return -1;
    for(unsigned i=0;i<sizeof(signals)/sizeof(signals[0]);i++)if(sigaction(signals[i],&action,NULL))return -1;
    atexit(stop_collector); return 0;
}
void crash_set_job(const char *id) {
    if(channel<0)return;
    CrashRecord record={.magic=CRASH_MAGIC,.version=CRASH_VERSION,.kind=CRASH_JOB,.pid=getpid()};
    snprintf(record.job_id,sizeof(record.job_id),"%s",id);
    send(channel,&record,sizeof(record),MSG_DONTWAIT|MSG_NOSIGNAL);
}
int crash_set_rendition(const char *session,const char *output,uint64_t generation) {
    if(channel<0||!session||strlen(session)!=32||!output||!strlen(output)||strlen(output)>32||!generation)return -1;
    CrashRecord record={.magic=CRASH_MAGIC,.version=CRASH_VERSION,.kind=CRASH_RENDITION,.pid=getpid()};
    snprintf(record.job_id,sizeof(record.job_id),"%s",output);record.registers[0]=generation;
    memcpy(&record.registers[1],session,32);
    ssize_t n;do{n=send(channel,&record,sizeof(record),MSG_DONTWAIT|MSG_NOSIGNAL);}while(n<0&&errno==EINTR);
    return n==sizeof(record)?0:-1;
}
int crash_collector_pid(void) {return collector_pid;}
int crash_client_enabled(void) {
    if(!initialized || channel<0)return 0;
    struct pollfd fd={.fd=channel,.events=POLLIN};
    return poll(&fd,1,0)>=0 && !(fd.revents&(POLLERR|POLLHUP|POLLNVAL));
}
int crash_ptracer_registered(void) {return ptracer_registered;}
const char *crash_report_directory(void) {return report_directory;}
