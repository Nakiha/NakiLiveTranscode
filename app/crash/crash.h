#ifndef NAKI_CRASH_H
#define NAKI_CRASH_H
#include <stdint.h>
#include <sys/types.h>

#define CRASH_MAGIC UINT32_C(0x4e435231)
#define CRASH_VERSION 1
#define CRASH_MAX_THREADS 32
#define CRASH_MAX_FRAMES 48
#define CRASH_MAX_MODULES 96
#define CRASH_MAX_REPORT (2*1024*1024)
enum {CRASH_ARCH_X86_64=1,CRASH_ARCH_AARCH64=2};
enum {CRASH_EVENT=1,CRASH_JOB=2,CRASH_RENDITION=3};
/* No pointers, strings requiring formatting, or allocator-owned objects in fatal context. */
typedef struct CrashRecord {
    uint32_t magic,version,kind,architecture;
    int32_t pid,tid,signal,signal_code;
    uint64_t monotonic_us,fault_address;
    /* x86_64: RAX,RDX,RCX,RBX,RSI,RDI,RBP,RSP,R8..R15,RIP; AArch64: X0..30,SP,PC. */
    uint64_t registers[34];
    char job_id[64]; /* Only CRASH_JOB normal-context messages use this field. */
} CrashRecord;
typedef struct CrashOptions {
    char directory[4096];
    unsigned timeout_ms,max_report_bytes,max_reports,max_threads,max_frames;
    uint64_t max_total_bytes,max_read_bytes;
} CrashOptions;

void crash_options_default(CrashOptions *options);
int crash_options_env(CrashOptions *options);
int crash_options_export(const CrashOptions *options);
/* Must run before application threads; collector execs independently of the control service. */
int crash_client_start(const CrashOptions *options,const char *role);
/* Normal startup context only; fatal record/layout and signal path stay fixed. */
void crash_set_instance(const char *instance_id);
int crash_thread_init(void);
void crash_set_job(const char *id);
/* Normal-context metadata; fixed fatal handler record remains unchanged. */
int crash_set_rendition(const char *session,const char *output,uint64_t generation);
int crash_collector_pid(void);
int crash_client_enabled(void);
int crash_ptracer_registered(void);
const char *crash_report_directory(void);
#endif
