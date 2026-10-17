#define _GNU_SOURCE
#include "crash.h"
#include <yyjson.h>
#include <dirent.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <libunwind-ptrace.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef struct Frame {uint64_t pc,sp,symbol_offset; int module; char function[96];} Frame;
typedef struct Thread {
    int tid,error,frame_count,limited,attached;
    char name[32]; Frame frames[CRASH_MAX_FRAMES];
} Thread;
typedef struct Module {
    uint64_t start,end,bias; int bias_valid;
    char name[256],build_id[129];
} Module;
typedef struct Capture {
    int thread_count,module_count,threads_limited,modules_limited,read_limited;
    uint64_t read_bytes;
    Thread threads[CRASH_MAX_THREADS]; Module modules[CRASH_MAX_MODULES];
} Capture;
static const CrashRecord *fault;
static Capture *capture;
static const CrashOptions *limits;
static int current_fault_thread;
static __attribute__((no_sanitize_address,no_sanitize_undefined)) void collector_fault(int sig) {_exit(128+sig);}
static uint64_t now_us(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000+(uint64_t)t.tv_nsec/1000;
}
static int access_reg(unw_addr_space_t as,unw_regnum_t reg,unw_word_t *value,int write,void *opaque) {
    if(current_fault_thread) {
        if(write)return -UNW_EREADONLYREG;
#if defined(__x86_64__)
        if(fault->architecture==CRASH_ARCH_X86_64 && reg>=UNW_X86_64_RAX && reg<=UNW_X86_64_RIP) {
            *value=fault->registers[reg-UNW_X86_64_RAX]; return 0;
        }
#elif defined(__aarch64__)
        if(fault->architecture==CRASH_ARCH_AARCH64 && reg>=UNW_AARCH64_X0 && reg<=UNW_AARCH64_X30) {
            *value=fault->registers[reg-UNW_AARCH64_X0]; return 0;
        }
        if(reg==UNW_AARCH64_SP) {*value=fault->registers[31]; return 0;}
        if(reg==UNW_AARCH64_PC) {*value=fault->registers[32]; return 0;}
#endif
    }
    return _UPT_access_reg(as,reg,value,write,opaque);
}
static int access_mem(unw_addr_space_t as,unw_word_t address,unw_word_t *value,int write,void *opaque) {
    if(write)return -UNW_EREADONLYREG;
    if(capture->read_bytes+sizeof(*value)>limits->max_read_bytes) {capture->read_limited=1; return -UNW_EUNSPEC;}
    capture->read_bytes+=sizeof(*value);
    return _UPT_access_mem(as,address,value,0,opaque);
}
static void elf_identity(const char *path,uint64_t map_start,uint64_t map_offset,Module *module) {
    int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    if(fd<0)return;
    struct stat st; Elf64_Ehdr eh;
    if(fstat(fd,&st) || !S_ISREG(st.st_mode) || pread(fd,&eh,sizeof(eh),0)!=(ssize_t)sizeof(eh) ||
       memcmp(eh.e_ident,ELFMAG,SELFMAG) || eh.e_ident[EI_CLASS]!=ELFCLASS64 || eh.e_ident[EI_DATA]!=ELFDATA2LSB ||
       eh.e_phentsize!=sizeof(Elf64_Phdr) || eh.e_phnum>256 || eh.e_phoff>(uint64_t)st.st_size) {close(fd); return;}
    uint64_t page=(uint64_t)sysconf(_SC_PAGESIZE);
    for(unsigned i=0;i<eh.e_phnum;i++) {
        Elf64_Phdr ph; uint64_t offset=eh.e_phoff+(uint64_t)i*sizeof(ph);
        if(offset>(uint64_t)st.st_size || (uint64_t)st.st_size-offset<sizeof(ph) || pread(fd,&ph,sizeof(ph),(off_t)offset)!=(ssize_t)sizeof(ph))break;
        if(ph.p_type==PT_LOAD) {
            uint64_t file_page=ph.p_offset/page*page,virtual_page=ph.p_vaddr/page*page;
            if(map_offset>=file_page && map_offset-file_page<ph.p_memsz+(ph.p_vaddr-virtual_page) &&
               map_start>=virtual_page+map_offset-file_page) {
                module->bias=map_start-(virtual_page+map_offset-file_page); module->bias_valid=1;
            }
        }
        if(ph.p_type!=PT_NOTE || ph.p_filesz>65536 || ph.p_offset>(uint64_t)st.st_size || ph.p_filesz>(uint64_t)st.st_size-ph.p_offset)continue;
        unsigned char notes[65536];
        if(pread(fd,notes,(size_t)ph.p_filesz,(off_t)ph.p_offset)!=(ssize_t)ph.p_filesz)continue;
        for(size_t p=0;p+sizeof(Elf64_Nhdr)<=ph.p_filesz;) {
            Elf64_Nhdr note; memcpy(&note,notes+p,sizeof(note)); p+=sizeof(note);
            size_t name=((size_t)note.n_namesz+3)&~(size_t)3,desc=((size_t)note.n_descsz+3)&~(size_t)3;
            if(name>ph.p_filesz-p || desc>ph.p_filesz-p-name)break;
            if(note.n_type==NT_GNU_BUILD_ID && note.n_namesz==4 && !memcmp(notes+p,"GNU",4) && note.n_descsz<=64) {
                static const char hex[]="0123456789abcdef";
                for(unsigned b=0;b<note.n_descsz;b++) {
                    module->build_id[2*b]=hex[notes[p+name+b]>>4];
                    module->build_id[2*b+1]=hex[notes[p+name+b]&15];
                }
                module->build_id[2*note.n_descsz]=0;
            }
            p+=name+desc;
        }
    }
    close(fd);
}
static void modules_for(int pid,Capture *out) {
    char path[80]; snprintf(path,sizeof(path),"/proc/%d/maps",pid);
    FILE *file=fopen(path,"re"); if(!file)return;
    char line[8192]; size_t bytes=0;
    while(fgets(line,sizeof(line),file)) {
        bytes+=strlen(line); if(bytes>262144) {out->modules_limited=1; break;}
        unsigned long long start,end,offset; char permissions[5]; int used=0;
        if(sscanf(line,"%llx-%llx %4s %llx %*s %*s %n",&start,&end,permissions,&offset,&used)!=4 || !used || !strchr(permissions,'x'))continue;
        if(out->module_count>=CRASH_MAX_MODULES) {out->modules_limited=1; break;}
        char *name=line+used; name[strcspn(name,"\n")]=0;
        Module *module=&out->modules[out->module_count++];
        module->start=start; module->end=end;
        const char *base=strrchr(name,'/'); snprintf(module->name,sizeof(module->name),"%s",base ? base+1 : name);
        if(*name=='/')elf_identity(name,start,offset,module);
    }
    fclose(file);
}
static int belongs_to(int tid,int pid) {
    char path[80],line[128]; snprintf(path,sizeof(path),"/proc/%d/status",tid);
    FILE *file=fopen(path,"re"); if(!file)return 0;
    int tgid=0;
    while(fgets(line,sizeof(line),file))if(sscanf(line,"Tgid: %d",&tgid)==1)break;
    fclose(file); return tgid==pid;
}
static void thread_name(Thread *thread) {
    char path[80]; snprintf(path,sizeof(path),"/proc/%d/comm",thread->tid);
    int fd=open(path,O_RDONLY|O_CLOEXEC);
    if(fd>=0) {
        ssize_t n=read(fd,thread->name,sizeof(thread->name)-1); close(fd);
        if(n>0)thread->name[strcspn(thread->name,"\n")]=0;
    }
}
static void snapshot(const CrashRecord *record,const CrashOptions *options,Capture *out,uint64_t deadline) {
    fault=record; limits=options; capture=out;
    char path[80]; snprintf(path,sizeof(path),"/proc/%d/task",record->pid);
    out->threads[out->thread_count++].tid=record->tid;
    DIR *tasks=opendir(path);
    if(tasks) {
        struct dirent *entry;
        while((entry=readdir(tasks))) {
            char *end; long tid=strtol(entry->d_name,&end,10);
            if(*end || tid<=0 || tid==record->tid)continue;
            if(out->thread_count>=(int)options->max_threads) {out->threads_limited=1; break;}
            out->threads[out->thread_count++].tid=(int)tid;
        }
        closedir(tasks);
    }
    for(int i=0;i<out->thread_count;i++) {
        Thread *thread=&out->threads[i]; thread_name(thread);
        if(!belongs_to(thread->tid,record->pid)) {thread->error=ESRCH; continue;}
        if(ptrace(PTRACE_SEIZE,thread->tid,NULL,NULL)) {thread->error=errno; continue;}
        thread->attached=1;
        if(!belongs_to(thread->tid,record->pid) || ptrace(PTRACE_INTERRUPT,thread->tid,NULL,NULL)) {thread->error=errno ? errno : ESRCH; continue;}
        for(;;) {
            int status=0; pid_t got=waitpid(thread->tid,&status,__WALL|WNOHANG);
            if(got==thread->tid) {if(!WIFSTOPPED(status))thread->error=ESRCH; break;}
            if(got<0) {thread->error=errno; break;}
            if(now_us()>=deadline) {thread->error=ETIMEDOUT; break;}
            poll(NULL,0,1);
        }
    }
    modules_for(record->pid,out);
    unw_accessors_t accessors=_UPT_accessors; accessors.access_reg=access_reg; accessors.access_mem=access_mem;
    unw_addr_space_t as=unw_create_addr_space(&accessors,0);
    for(int i=0;i<out->thread_count && as;i++) {
        Thread *thread=&out->threads[i]; if(thread->error)continue;
        current_fault_thread=thread->tid==record->tid;
        void *upt=_UPT_create(thread->tid); unw_cursor_t cursor;
        int ret=upt ? unw_init_remote(&cursor,as,upt) : -UNW_ENOMEM;
        if(ret<0)thread->error=ret;
        while(ret>=0 && thread->frame_count<(int)options->max_frames && now_us()<deadline) {
            Frame *frame=&thread->frames[thread->frame_count]; unw_word_t pc=0,sp=0,offset=0;
            if(unw_get_reg(&cursor,UNW_REG_IP,&pc)<0 || !pc)break;
            unw_get_reg(&cursor,UNW_REG_SP,&sp);
            frame->pc=pc; frame->sp=sp; frame->module=-1;
            if(unw_get_proc_name(&cursor,frame->function,sizeof(frame->function),&offset)>=0)frame->symbol_offset=offset;
            for(int m=0;m<out->module_count;m++)if(pc>=out->modules[m].start && pc<out->modules[m].end) {frame->module=m; break;}
            thread->frame_count++;
            ret=unw_step(&cursor); if(ret<=0) {if(ret<0)thread->error=ret; break;}
        }
        if(thread->frame_count==(int)options->max_frames || now_us()>=deadline)thread->limited=1;
        if(upt)_UPT_destroy(upt);
    }
    if(as)unw_destroy_addr_space(as);
    for(int i=out->thread_count-1;i>=0;i--)if(out->threads[i].attached)ptrace(PTRACE_DETACH,out->threads[i].tid,NULL,NULL);
}
static int collect(const CrashRecord *record,const CrashOptions *options,int target_fd,Capture *out,char *reason,size_t cap) {
    int pipefd[2]; if(pipe2(pipefd,O_CLOEXEC|O_NONBLOCK)) {snprintf(reason,cap,"pipe_error"); return -1;}
    uint64_t started=now_us(),budget=(uint64_t)options->timeout_ms*700; pid_t coordinator=getpid();
    pid_t child=fork();
    if(child==0) {
        close(pipefd[0]); close(3);
        if(prctl(PR_SET_PDEATHSIG,SIGKILL) || getppid()!=coordinator)_exit(1);
        int flags=fcntl(pipefd[1],F_GETFL); fcntl(pipefd[1],F_SETFL,flags&~O_NONBLOCK);
        snapshot(record,options,out,started+budget);
        size_t done=0;
        while(done<sizeof(*out)) {
            ssize_t n=write(pipefd[1],(char *)out+done,sizeof(*out)-done);
            if(n<0 && errno==EINTR)continue;
            if(n<=0)_exit(2);
            done+=(size_t)n;
        }
        _exit(0);
    }
    close(pipefd[1]);
    if(child<0) {close(pipefd[0]); snprintf(reason,cap,"fork_error"); return -1;}
    size_t received=0; int status=0,exited=0,timed_out=0;
    while(now_us()-started<budget) {
        ssize_t n=read(pipefd[0],(char *)out+received,sizeof(*out)-received);
        if(n>0)received+=(size_t)n;
        pid_t got=waitpid(child,&status,WNOHANG);
        if(got==child) {exited=1; if(received==sizeof(*out))break;}
        if(exited && n==0)break;
        struct pollfd fd={.fd=pipefd[0],.events=POLLIN}; poll(&fd,1,5);
    }
    int forced_target=0;
    if(!exited) {
        timed_out=1; kill(child,SIGKILL);
        uint64_t grace=(uint64_t)options->timeout_ms*100;
        if(grace>100000)grace=100000;
        uint64_t reap_deadline=now_us()+grace;
        while(now_us()<reap_deadline) {
            if(waitpid(child,&status,WNOHANG)==child) {exited=1; break;}
            poll(NULL,0,1);
        }
        /* Never wait indefinitely for a tracer in uninterruptible kernel I/O. */
        if(!exited) {syscall(SYS_pidfd_send_signal,target_fd,SIGKILL,NULL,0); forced_target=1;}
    }
    close(pipefd[0]);
    if(timed_out || received!=sizeof(*out) || !WIFEXITED(status) || WEXITSTATUS(status)) {
        memset(out,0,sizeof(*out));
        snprintf(reason,cap,"%s",forced_target ? "capture_timeout_forced_target_exit" : timed_out ? "capture_timeout" : "capture_process_failed"); return -1;
    }
    if(out->thread_count<0 || out->thread_count>CRASH_MAX_THREADS || out->module_count<0 || out->module_count>CRASH_MAX_MODULES) {
        memset(out,0,sizeof(*out)); snprintf(reason,cap,"invalid_capture_result"); return -1;
    }
    for(int i=0;i<out->thread_count;i++) {
        Thread *t=&out->threads[i]; t->name[sizeof(t->name)-1]=0;
        if(t->frame_count<0 || t->frame_count>CRASH_MAX_FRAMES) {memset(out,0,sizeof(*out)); snprintf(reason,cap,"invalid_capture_result"); return -1;}
        for(int f=0;f<t->frame_count;f++) {
            t->frames[f].function[sizeof(t->frames[f].function)-1]=0;
            if(t->frames[f].module<0 || t->frames[f].module>=out->module_count)t->frames[f].module=-1;
        }
    }
    for(int i=0;i<out->module_count;i++) {out->modules[i].name[255]=0; out->modules[i].build_id[128]=0;}
    snprintf(reason,cap,"captured"); return 0;
}
static void integer(yyjson_mut_doc *doc,yyjson_mut_val *obj,const char *name,uint64_t value) {yyjson_mut_obj_add_uint(doc,obj,name,value);}
static void hexadecimal(yyjson_mut_doc *doc,yyjson_mut_val *obj,const char *name,uint64_t value) {
    char text[24]; snprintf(text,sizeof(text),"0x%llx",(unsigned long long)value);
    yyjson_mut_obj_add(obj,yyjson_mut_strcpy(doc,name),yyjson_mut_strcpy(doc,text));
}
static yyjson_mut_doc *make_report(const CrashRecord *record,const char *role,const char *job,const CrashOptions *o,
                                  const Capture *result,const char *reason,unsigned frame_limit,int brief) {
    yyjson_mut_doc *doc=yyjson_mut_doc_new(NULL); if(!doc)return NULL;
    yyjson_mut_val *root=yyjson_mut_obj(doc); yyjson_mut_doc_set_root(doc,root);
    yyjson_mut_obj_add_str(doc,root,"event","crash_report"); integer(doc,root,"schema_version",1);
    struct timespec wall; clock_gettime(CLOCK_REALTIME,&wall);
    integer(doc,root,"reported_unix_us",(uint64_t)wall.tv_sec*1000000+(uint64_t)wall.tv_nsec/1000);
    yyjson_mut_obj_add_strcpy(doc,root,"role",role); yyjson_mut_obj_add_strcpy(doc,root,"job_id",job);
    integer(doc,root,"pid",record->pid); integer(doc,root,"tid",record->tid); integer(doc,root,"signal",record->signal);
    yyjson_mut_obj_add_sint(doc,root,"signal_code",record->signal_code);
    integer(doc,root,"monotonic_us",record->monotonic_us); integer(doc,root,"architecture",record->architecture);
    yyjson_mut_obj_add_str(doc,root,"architecture_name",record->architecture==CRASH_ARCH_X86_64 ? "x86_64" : "aarch64");
    yyjson_mut_obj_add_strcpy(doc,root,"capture_result",reason);
    yyjson_mut_obj_add_bool(doc,root,"full_core",0);
    int frame_limited=0,thread_errors=0;
    for(int i=0;i<result->thread_count;i++) {if(result->threads[i].limited)frame_limited=1; if(result->threads[i].error)thread_errors=1;}
    int truncated=brief || frame_limit<o->max_frames || frame_limited || result->threads_limited || result->modules_limited || result->read_limited;
    yyjson_mut_obj_add_bool(doc,root,"truncated",truncated);
    yyjson_mut_obj_add_bool(doc,root,"complete",!truncated && !thread_errors && !strcmp(reason,"captured"));
    integer(doc,root,"timeout_ms",o->timeout_ms); integer(doc,root,"max_report_bytes",o->max_report_bytes);
    integer(doc,root,"remote_word_read_bytes",result->read_bytes);
    yyjson_mut_obj_add_bool(doc,root,"thread_limit_hit",result->threads_limited);
    yyjson_mut_obj_add_bool(doc,root,"module_limit_hit",result->modules_limited);
    yyjson_mut_obj_add_bool(doc,root,"read_limit_hit",result->read_limited);
    yyjson_mut_obj_add_bool(doc,root,"report_size_limit_hit",brief || frame_limit<o->max_frames);
    if(record->signal_code>0 && (record->signal==SIGSEGV || record->signal==SIGBUS || record->signal==SIGILL || record->signal==SIGFPE))hexadecimal(doc,root,"fault_address",record->fault_address);
    else yyjson_mut_obj_add_null(doc,root,"fault_address");
    uint64_t pc=record->registers[record->architecture==CRASH_ARCH_X86_64 ? 16 : 32];
    uint64_t sp=record->registers[record->architecture==CRASH_ARCH_X86_64 ? 7 : 31];
    hexadecimal(doc,root,"fault_pc",pc); hexadecimal(doc,root,"fault_sp",sp);
    yyjson_mut_val *registers=yyjson_mut_obj(doc);
    static const char *x86_names[]={"rax","rdx","rcx","rbx","rsi","rdi","rbp","rsp","r8","r9","r10","r11","r12","r13","r14","r15","rip"};
    for(unsigned i=0;i<(record->architecture==CRASH_ARCH_X86_64 ? 17u : 33u);i++) {
        char name[16];
        if(record->architecture==CRASH_ARCH_X86_64)snprintf(name,sizeof(name),"%s",x86_names[i]);
        else if(i<31)snprintf(name,sizeof(name),"x%u",i);
        else snprintf(name,sizeof(name),"%s",i==31 ? "sp" : "pc");
        hexadecimal(doc,registers,name,record->registers[i]);
    }
    yyjson_mut_obj_add_val(doc,root,"registers",registers);
    yyjson_mut_val *modules=yyjson_mut_arr(doc),*threads=yyjson_mut_arr(doc);
    if(!brief)for(int i=0;i<result->module_count;i++) {
        const Module *m=&result->modules[i]; yyjson_mut_val *item=yyjson_mut_obj(doc);
        integer(doc,item,"id",i); yyjson_mut_obj_add_strcpy(doc,item,"name",m->name);
        yyjson_mut_obj_add_strcpy(doc,item,"build_id",m->build_id);
        hexadecimal(doc,item,"start",m->start); hexadecimal(doc,item,"end",m->end);
        if(m->bias_valid)hexadecimal(doc,item,"load_bias",m->bias);
        else yyjson_mut_obj_add_null(doc,item,"load_bias");
        yyjson_mut_arr_add_val(modules,item);
    }
    if(!brief)for(int i=0;i<result->thread_count;i++) {
        const Thread *t=&result->threads[i]; yyjson_mut_val *item=yyjson_mut_obj(doc),*frames=yyjson_mut_arr(doc);
        integer(doc,item,"tid",t->tid); yyjson_mut_obj_add_strcpy(doc,item,"name",t->name);
        yyjson_mut_obj_add_sint(doc,item,"unwind_or_attach_error",t->error);
        yyjson_mut_obj_add_bool(doc,item,"frame_limit_hit",t->limited || (unsigned)t->frame_count>frame_limit);
        yyjson_mut_obj_add_bool(doc,item,"fault_thread",t->tid==record->tid);
        for(int f=0;f<t->frame_count && (unsigned)f<frame_limit;f++) {
            const Frame *frame=&t->frames[f]; yyjson_mut_val *value=yyjson_mut_obj(doc);
            hexadecimal(doc,value,"pc",frame->pc); hexadecimal(doc,value,"sp",frame->sp);
            yyjson_mut_obj_add_strcpy(doc,value,"function",frame->function);
            hexadecimal(doc,value,"function_offset",frame->symbol_offset);
            yyjson_mut_obj_add_sint(doc,value,"module",frame->module);
            if(frame->module>=0 && result->modules[frame->module].bias_valid)hexadecimal(doc,value,"elf_address",frame->pc-result->modules[frame->module].bias);
            else yyjson_mut_obj_add_null(doc,value,"elf_address");
            yyjson_mut_arr_add_val(frames,value);
        }
        yyjson_mut_obj_add_val(doc,item,"frames",frames); yyjson_mut_arr_add_val(threads,item);
    }
    yyjson_mut_obj_add_val(doc,root,"modules",modules); yyjson_mut_obj_add_val(doc,root,"threads",threads);
    return doc;
}
typedef struct Stored {char name[128]; uint64_t bytes,stamp;} Stored;
static int oldest_first(const void *a,const void *b) {
    const Stored *x=a,*y=b; return x->stamp<y->stamp ? -1 : x->stamp>y->stamp;
}
static int store_report(int directory,const CrashOptions *o,const CrashRecord *r,const char *text,size_t size) {
    if(size+1>o->max_report_bytes || size+1>o->max_total_bytes)return -1;
    int lock=openat(directory,".retention.lock",O_CREAT|O_RDWR|O_CLOEXEC|O_NOFOLLOW,0600);
    if(lock<0)return -1;
    uint64_t start=now_us();
    while(flock(lock,LOCK_EX|LOCK_NB)) {
        if(errno!=EWOULDBLOCK || now_us()-start>100000) {close(lock); return -1;}
        poll(NULL,0,1);
    }
    DIR *stream=fdopendir(openat(directory,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC));
    Stored entries[1024]; size_t count=0; uint64_t total=0; struct dirent *entry;
    if(!stream) {close(lock); return -1;}
    int overflow=0;
    while((entry=readdir(stream))) {
        int pid,tid,used=0; unsigned long long stamp;
        int writer=0;
        if(sscanf(entry->d_name,".crash-%d-%llu-%d.json.%d.tmp%n",&pid,&stamp,&tid,&writer,&used)==4 && used && !entry->d_name[used]) {
            struct stat temporary;
            /* Every report writer holds this lock until its temporary file is closed and published. */
            if(!fstatat(directory,entry->d_name,&temporary,AT_SYMLINK_NOFOLLOW) && S_ISREG(temporary.st_mode) && temporary.st_uid==geteuid() && unlinkat(directory,entry->d_name,0)) {overflow=1; break;}
            continue;
        }
        used=0;
        if(sscanf(entry->d_name,"crash-%d-%llu-%d.json%n",&pid,&stamp,&tid,&used)!=3 || !used || entry->d_name[used])continue;
        struct stat st;
        if(fstatat(directory,entry->d_name,&st,AT_SYMLINK_NOFOLLOW) || !S_ISREG(st.st_mode) || st.st_uid!=geteuid())continue;
        if(count==sizeof(entries)/sizeof(entries[0])) {overflow=1; break;}
        if(strlen(entry->d_name)>=sizeof(entries[count].name))continue;
        memcpy(entries[count].name,entry->d_name,strlen(entry->d_name)+1);
        entries[count].bytes=(uint64_t)st.st_size;
        entries[count].stamp=(uint64_t)st.st_mtim.tv_sec*1000000000+(uint64_t)st.st_mtim.tv_nsec;
        total+=(uint64_t)st.st_size; count++;
    }
    closedir(stream);
    if(overflow) {close(lock); return -1;}
    qsort(entries,count,sizeof(entries[0]),oldest_first);
    size_t removed=0;
    while(removed<count && (count-removed>=o->max_reports || total+size+1>o->max_total_bytes)) {
        if(unlinkat(directory,entries[removed].name,0)) {close(lock); return -1;}
        total-=entries[removed++].bytes;
    }
    char name[128],temporary[160];
    snprintf(name,sizeof(name),"crash-%d-%llu-%d.json",r->pid,(unsigned long long)r->monotonic_us,r->tid);
    snprintf(temporary,sizeof(temporary),".%s.%d.tmp",name,getpid());
    int fd=openat(directory,temporary,O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC|O_NOFOLLOW,0600);
    int failed=fd<0; size_t done=0;
    while(!failed && done<size) {
        ssize_t n=write(fd,text+done,size-done);
        if(n<0 && errno==EINTR)continue;
        if(n<=0)failed=1; else done+=(size_t)n;
    }
    if(!failed && write(fd,"\n",1)!=1)failed=1;
    if(fd>=0 && close(fd))failed=1;
    if(!failed && renameat(directory,temporary,directory,name))failed=1;
    if(failed)unlinkat(directory,temporary,0);
    close(lock); return failed ? -1 : 0;
}
int main(int argc,char **argv) {
    if(argc!=3&&argc!=4)return 2;
    int target=atoi(argv[1]); CrashOptions options;
    if(target<=0 || crash_options_env(&options) || !*options.directory)return 2;
    struct ucred peer; socklen_t length=sizeof(peer);
    if(getsockopt(3,SOL_SOCKET,SO_PEERCRED,&peer,&length) || peer.pid!=target || peer.uid!=geteuid())return 2;
    int process=(int)syscall(SYS_pidfd_open,target,0);
    int directory=open(options.directory,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    struct stat st;
    if(process<0 || directory<0 || fstat(directory,&st) || st.st_uid!=geteuid() || (st.st_mode&0777)!=0700)return 2;
    signal(SIGPIPE,SIG_IGN);
    /* A broken unwinder must fail locally, release its tracees and not create its own core. */
    const int fatal[]={SIGSEGV,SIGBUS,SIGILL,SIGFPE,SIGABRT};
    struct sigaction action={.sa_handler=collector_fault}; sigfillset(&action.sa_mask);
    for(unsigned i=0;i<sizeof(fatal)/sizeof(fatal[0]);i++)sigaction(fatal[i],&action,NULL);
    if(send(3,"R",1,MSG_DONTWAIT|MSG_NOSIGNAL)!=1)return 1;
    char job[64]="";
    for(;;) {
        struct pollfd fd={.fd=3,.events=POLLIN};
        int ready=poll(&fd,1,-1);
        if(ready<0 && errno==EINTR)continue;
        if(ready<=0)return 1;
        CrashRecord record;
        ssize_t n=recv(3,&record,sizeof(record),MSG_DONTWAIT|MSG_TRUNC);
        if(n==0)return 0;
        if(n<0 && (errno==EAGAIN || errno==EINTR))continue;
        if(n!=(ssize_t)sizeof(record) || record.magic!=CRASH_MAGIC || record.version!=CRASH_VERSION || record.pid!=target)return 2;
        if(record.kind==CRASH_JOB) {memcpy(job,record.job_id,sizeof(job)); job[sizeof(job)-1]=0; continue;}
        if(record.kind!=CRASH_EVENT)return 2;
        Capture *result=calloc(1,sizeof(*result)); if(!result)return 1;
        char reason[64]; struct pollfd alive={.fd=process,.events=POLLIN};
        if(poll(&alive,1,0)>0)snprintf(reason,sizeof(reason),"target_already_exited");
        else collect(&record,&options,process,result,reason,sizeof(reason));
        char *text=NULL; size_t size=0; unsigned frames=options.max_frames;
        for(;;) {
            yyjson_mut_doc *doc=make_report(&record,argv[2],job,&options,result,reason,frames,frames==0);
            if(doc) {
                yyjson_mut_val *root=yyjson_mut_doc_get_root(doc);
                yyjson_mut_obj_add_bool(doc,root,"instance_id_available",argc==4&&strlen(argv[3])==32);
                if(argc==4&&strlen(argv[3])==32)yyjson_mut_obj_add_strcpy(doc,root,"instance_id",argv[3]);
            }
            text=doc ? yyjson_mut_write(doc,0,&size) : NULL;
            if(doc)yyjson_mut_doc_free(doc);
            if(!text || size+1<=options.max_report_bytes || !frames)break;
            free(text); text=NULL; frames/=2;
        }
        int success=text && !store_report(directory,&options,&record,text,size);
        free(text); free(result);
        send(3,success ? "C" : "F",1,MSG_DONTWAIT|MSG_NOSIGNAL);
        close(directory); close(process); return success ? 0 : 1;
    }
}
