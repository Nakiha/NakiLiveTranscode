#define _GNU_SOURCE
#include "ipc.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

yyjson_mut_doc *ipc_message(const char *kind) {
    yyjson_mut_doc *doc=yyjson_mut_doc_new(NULL);
    if (!doc) return NULL;
    yyjson_mut_val *root=yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc,root);
    yyjson_mut_obj_add_int(doc,root,"ipc_version",IPC_VERSION);
    yyjson_mut_obj_add_strcpy(doc,root,"ipc_kind",kind);
    return doc;
}
int ipc_send(int fd, yyjson_mut_doc *doc) {
    size_t size=0;
    char *text=doc ? yyjson_mut_write(doc,0,&size) : NULL;
    if (!text) {errno=ENOMEM; return -1;}
    if (size>IPC_MAX_BYTES) {free(text); errno=EMSGSIZE; return -1;}
    ssize_t n;
    do {n=send(fd,text,size,MSG_DONTWAIT|MSG_NOSIGNAL);} while(n<0 && errno==EINTR);
    int saved=errno; free(text); errno=saved;
    return n==(ssize_t)size ? 0 : -1;
}
int ipc_receive(int fd, yyjson_doc **doc) {
    char text[IPC_MAX_BYTES]; ssize_t n;
    *doc=NULL;
    do {n=recv(fd,text,sizeof(text),MSG_DONTWAIT|MSG_TRUNC);} while(n<0 && errno==EINTR);
    if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK))return 0;
    if(n<=0 || n>(ssize_t)sizeof(text))return -1;
    *doc=yyjson_read(text,(size_t)n,0);
    yyjson_val *root=*doc ? yyjson_doc_get_root(*doc) : NULL;
    if(!yyjson_is_obj(root) || yyjson_get_int(yyjson_obj_get(root,"ipc_version"))!=IPC_VERSION ||
       !yyjson_is_str(yyjson_obj_get(root,"ipc_kind"))) {
        if(*doc)yyjson_doc_free(*doc);
        *doc=NULL; return -1;
    }
    return 1;
}
int ipc_kind(yyjson_doc *doc,const char *kind) {
    const char *actual=yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(doc),"ipc_kind"));
    return actual && !strcmp(actual,kind);
}
static int address(const char *path,struct sockaddr_un *addr) {
    if(strlen(path)>=sizeof(addr->sun_path)) {errno=ENAMETOOLONG; return -1;}
    memset(addr,0,sizeof(*addr)); addr->sun_family=AF_UNIX; strcpy(addr->sun_path,path); return 0;
}
int ipc_listen(const char *path) {
    struct sockaddr_un addr; if(address(path,&addr))return -1;
    int fd=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC|SOCK_NONBLOCK,0);
    if(fd<0)return -1;
    if(bind(fd,(struct sockaddr *)&addr,sizeof(addr)) || listen(fd,4)) {int e=errno; close(fd); errno=e; return -1;}
    return fd;
}
int ipc_connect(const char *path,int *peer_pid) {
    struct sockaddr_un addr; if(address(path,&addr))return -1;
    int fd=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC|SOCK_NONBLOCK,0);
    if(fd<0)return -1;
    struct ucred peer; socklen_t size=sizeof(peer);
    if(connect(fd,(struct sockaddr *)&addr,sizeof(addr)) || getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&peer,&size) || peer.uid!=geteuid()) {
        int e=errno ? errno : EACCES; close(fd); errno=e; return -1;
    }
    *peer_pid=peer.pid; return fd;
}
int ipc_accept(int listener) {
    int fd=accept4(listener,NULL,NULL,SOCK_NONBLOCK|SOCK_CLOEXEC);
    if(fd<0)return -1;
    struct ucred peer; socklen_t size=sizeof(peer);
    if(getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&peer,&size) || peer.uid!=geteuid()) {close(fd); errno=EACCES; return -1;}
    return fd;
}
