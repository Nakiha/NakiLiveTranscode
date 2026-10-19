#ifndef NAKI_CODEC_PROGRESS_H
#define NAKI_CODEC_PROGRESS_H
#include <stdint.h>
#include <errno.h>
/* Used only by the opt-in bounded scheduler. receive drains already available
 * AUs; progress is a monotonically increasing AU counter, not API call count. */
typedef struct NakiCodecProgress {
    void *opaque;
    int (*send)(void *,const void *);
    int (*receive)(void *);
    uint64_t (*progress)(void *);
} NakiCodecProgress;
static inline int naki_codec_submit(NakiCodecProgress *io,const void *frame,
                                   int again,int *null_sent) {
    if(!frame&&*null_sent)return 0;
    for(int attempt=0;attempt<5;attempt++) {
        int ret=io->send(io->opaque,frame);
        if(ret>=0) {if(!frame)*null_sent=1;return 0;}
        if(ret!=again)return ret;
        uint64_t before=io->progress(io->opaque);
        ret=io->receive(io->opaque);
        if(ret<0)return ret;
        if(io->progress(io->opaque)<=before)return -EDEADLK;
    }
    return -EDEADLK;
}
#endif
