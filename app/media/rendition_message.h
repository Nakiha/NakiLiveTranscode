#ifndef NAKI_RENDITION_MESSAGE_H
#define NAKI_RENDITION_MESSAGE_H
#include "shared_frame_pool.h"
#include <stdint.h>
#define NAKI_RENDITION_MESSAGE_BYTES 160
#define NAKI_RENDITION_VIDEO_CREDIT 32U
#define NAKI_RENDITION_AUDIO_CREDIT 16U
/* Separate media/completion sockets; this is not the controller JSON IPC.
 * Explicit LE fields, never send a native struct. Every descriptor is scoped
 * by the session instance and independently generated rendition nonce. */
enum {NAKI_RMSG_VIDEO=1,NAKI_RMSG_AUDIO,NAKI_RMSG_EOF,NAKI_RMSG_ACCEPTED,NAKI_RMSG_RELEASE};
typedef struct NakiRenditionMessage {
    unsigned kind,media;
    char session[33],instance[33];
    uint64_t rendition_generation,source_generation,frame_id;
    int64_t ingress_us;
    NakiPoolToken token;
} NakiRenditionMessage;
int naki_rendition_send(int fd,const NakiRenditionMessage *m);
int naki_rendition_receive(int fd,NakiRenditionMessage *m);
#endif
