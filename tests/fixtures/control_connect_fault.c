#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <time.h>
#include <stdint.h>

/* Flip only the version digit in an actual worker packet after the test creates its marker. */
ssize_t send(int fd,const void *buffer,size_t length,int flags) {
    static ssize_t (*real_send)(int,const void *,size_t,int);
    if(!real_send)*(void **)(&real_send)=dlsym(RTLD_NEXT,"send");
    const char *marker=getenv("NAKI_TEST_OLD_PROTOCOL_MARKER"),*result_marker=getenv("NAKI_TEST_OLD_RESULT_VERSION_MARKER");
    int old_protocol=marker&&access(marker,F_OK)==0,old_result=result_marker&&access(result_marker,F_OK)==0;
    if((old_protocol||old_result)&&length<65536) {
        char packet[65536];memcpy(packet,buffer,length);packet[length]=0;
        if(old_protocol) {char *version=strstr(packet,"\"ipc_version\":2");if(version)version[strlen("\"ipc_version\":")]='1';}
        if(old_result) {char *version=strstr(packet,"\"operation_result_version_api\":1");if(version)version[strlen("\"operation_result_version_api\":")]='0';}
        return real_send(fd,packet,length,flags);
    }
    return real_send(fd,buffer,length,flags);
}

ssize_t recv(int fd,void *buffer,size_t length,int flags) {
    static ssize_t (*real_recv)(int,void *,size_t,int);
    if(!real_recv)*(void **)(&real_recv)=dlsym(RTLD_NEXT,"recv");
    ssize_t result=real_recv(fd,buffer,length,flags);
    const char *marker=getenv("NAKI_TEST_DROP_ACCEPTANCE_MARKER");
    if(marker&&access(marker,F_OK)==0&&result>0&&(size_t)result<length) {
        char packet[65536];
        if((size_t)result<sizeof(packet)) {
            memcpy(packet,buffer,(size_t)result);packet[result]=0;
            if(strstr(packet,"\"ipc_kind\":\"accepted\"")||strstr(packet,"\"ipc_kind\":\"snapshot\"")||strstr(packet,"\"ipc_kind\":\"finished\"")) {
                errno=EAGAIN;return -1;
            }
        }
    }
    return result;
}

/* Test-only preload: force transient recovery connect failures without timing a backlog. */
int connect(int fd,const struct sockaddr *address,socklen_t length) {
    static int (*real_connect)(int,const struct sockaddr *,socklen_t);
    static int initialized,remaining;
    static int64_t until_ms;
    if(!real_connect)*(void **)(&real_connect)=dlsym(RTLD_NEXT,"connect");
    struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);int64_t now_ms=(int64_t)now.tv_sec*1000+now.tv_nsec/1000000;
    if(!initialized) {
        const char *value=getenv("NAKI_TEST_CONNECT_FAILURES");remaining=value?atoi(value):0;
        value=getenv("NAKI_TEST_CONNECT_FAILURE_MS");until_ms=now_ms+(value?atoi(value):0);initialized=1;
    }
    if(address&&address->sa_family==AF_UNIX&&length>=sizeof(sa_family_t)+4&&(remaining>0||now_ms<until_ms)) {
        const struct sockaddr_un *local=(const struct sockaddr_un *)address;
        if(strstr(local->sun_path,"/w-")) {if(remaining>0)remaining--;errno=EAGAIN;return -1;}
    }
    return real_connect(fd,address,length);
}
