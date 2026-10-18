#define _GNU_SOURCE
#include "crash/crash.h"
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static pthread_mutex_t held=PTHREAD_MUTEX_INITIALIZER;
static atomic_int start_faults;
static void *sleeper(void *unused) {
    (void)unused; crash_thread_init();
    for(;;)poll(NULL,0,100);
    return NULL;
}
static void *lock_holder(void *unused) {
    (void)unused; crash_thread_init();
    pthread_mutex_lock(&held); flockfile(stdout);
    for(;;)poll(NULL,0,100);
    return NULL;
}
__attribute__((noinline,no_sanitize_address,no_sanitize_undefined)) void crash_fixture_level3(void) {
    volatile uintptr_t invalid=1;
    *(volatile int *)invalid=42;
}
__attribute__((noinline)) void crash_fixture_level2(void) {crash_fixture_level3(); __asm__ volatile("" ::: "memory");}
__attribute__((noinline)) void crash_fixture_level1(void) {crash_fixture_level2(); __asm__ volatile("" ::: "memory");}
__attribute__((noinline)) static void overflow(unsigned n) {
    volatile unsigned char buffer[4096]; buffer[n%sizeof(buffer)]=(unsigned char)n;
    if(n!=UINT_MAX)overflow(n+1);
    __asm__ volatile("" : : "r"(buffer) : "memory");
}
static void *faulting_thread(void *unused) {(void)unused; crash_thread_init(); crash_fixture_level1(); return NULL;}
static void *simultaneous_fault(void *unused) {
    (void)unused; crash_thread_init();
    while(!atomic_load(&start_faults))poll(NULL,0,1);
    crash_fixture_level1(); return NULL;
}
int main(int argc,char **argv) {
    CrashOptions options;
    if(argc!=2 || crash_options_env(&options) || crash_client_start(&options,"fixture"))return 2;
    crash_set_job("fixture-job");
    printf("%d %d\n",getpid(),crash_collector_pid()); fflush(stdout);
    if(!strcmp(argv[1],"wait")) {for(;;)poll(NULL,0,100);}
    if(!strcmp(argv[1],"overflow"))overflow(0);
    if(!strcmp(argv[1],"locks")) {
        pthread_t thread; pthread_create(&thread,NULL,lock_holder,NULL); poll(NULL,0,50);
    }
    if(!strcmp(argv[1],"threads"))for(int i=0;i<8;i++) {pthread_t thread; pthread_create(&thread,NULL,sleeper,NULL);}
    if(!strcmp(argv[1],"thread")) {
        pthread_t thread; pthread_create(&thread,NULL,faulting_thread,NULL); pthread_join(thread,NULL);
    }
    if(!strcmp(argv[1],"double")) {
        pthread_t a,b;
        pthread_create(&a,NULL,simultaneous_fault,NULL); pthread_create(&b,NULL,simultaneous_fault,NULL);
        atomic_store(&start_faults,1); pthread_join(a,NULL); pthread_join(b,NULL);
    }
    crash_fixture_level1(); return 0;
}
