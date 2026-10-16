#ifndef NAKI_IPC_H
#define NAKI_IPC_H
#include <yyjson.h>
#define IPC_VERSION 1
#define IPC_MAX_BYTES 65536
/* One bounded JSON document per Unix SOCK_SEQPACKET message. */
int ipc_send(int fd, yyjson_mut_doc *doc);
/* 1=document, 0=would block, -1=closed/protocol/I/O error. */
int ipc_receive(int fd, yyjson_doc **doc);
yyjson_mut_doc *ipc_message(const char *kind);
int ipc_kind(yyjson_doc *doc, const char *kind);
int ipc_listen(const char *path);
int ipc_connect(const char *path,int *peer_pid);
int ipc_accept(int listener);
#endif
