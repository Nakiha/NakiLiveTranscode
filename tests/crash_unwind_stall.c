/* Test-only interposition: stall inside the independent capture child, never inside the target. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdarg.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <unistd.h>
long ptrace(enum __ptrace_request request,...) {
    va_list args; va_start(args,request);
    pid_t pid=va_arg(args,pid_t); void *address=va_arg(args,void *),*data=va_arg(args,void *);
    va_end(args);
    if(getenv("NAKI_TEST_DENY_PTRACE")) {errno=EPERM; return -1;}
    if(request==PTRACE_PEEKDATA) {
        if(getenv("NAKI_TEST_CRASH_CAPTURE"))raise(SIGSEGV);
        for(;;)pause();
    }
    long (*real_ptrace)(enum __ptrace_request,...)=dlsym(RTLD_NEXT,"ptrace");
    return real_ptrace(request,pid,address,data);
}
