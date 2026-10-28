#include "destination_lifecycle.h"
#include <errno.h>
#define PHASE_MASK 3U
#define STOP_MASK 12U
#define FAILED 16U
void naki_writer_init(NakiWriterLife *l){atomic_init(&l->_private_writer,0);}
NakiWriterView naki_writer_view(const NakiWriterLife *l) {
    unsigned v=atomic_load_explicit(&l->_private_writer,memory_order_acquire);
    return (NakiWriterView){.phase=v&PHASE_MASK,.stop=(v&STOP_MASK)>>2,.outcome=(v&FAILED)?NAKI_OUTCOME_FAILED:NAKI_OUTCOME_HEALTHY};
}
int naki_writer_ready(NakiWriterLife *l) {
    unsigned old=atomic_load(&l->_private_writer);
    for(;;) {
        if((old&PHASE_MASK)==NAKI_WRITER_DONE)return -EALREADY;
        if(old&FAILED)return -ECANCELED;
        unsigned next=(old&~PHASE_MASK)|NAKI_WRITER_READY;
        if(atomic_compare_exchange_weak(&l->_private_writer,&old,next))return 0;
    }
}
void naki_writer_stop(NakiWriterLife *l) {
    unsigned old=atomic_load(&l->_private_writer);
    for(;;) {
        if(old&STOP_MASK)return;
        if(atomic_compare_exchange_weak(&l->_private_writer,&old,old|(NAKI_STOP_DRAIN<<2)))return;
    }
}
void naki_writer_fail(NakiWriterLife *l) {
    unsigned old=atomic_load(&l->_private_writer);
    while(!atomic_compare_exchange_weak(&l->_private_writer,&old,(old&~STOP_MASK)|FAILED|(NAKI_STOP_ABORT<<2))){}
}
void naki_writer_finish(NakiWriterLife *l) {
    unsigned old=atomic_load(&l->_private_writer);
    while(!atomic_compare_exchange_weak(&l->_private_writer,&old,(old&~PHASE_MASK)|NAKI_WRITER_DONE)){}
}
NakiDestinationView naki_dl_view(const NakiDestinationLife *l){return (NakiDestinationView){.phase=l->_private_destination&7U,.holder=l->_private_destination>>3};}
static int save(NakiDestinationLife *l,NakiDestinationView v){l->_private_destination=(unsigned)v.phase|((unsigned)v.holder<<3);return 0;}
int naki_dl_reserve(NakiDestinationLife *l,int primary) {
    NakiDestinationView v=naki_dl_view(l);if(v.phase!=NAKI_DEST_EMPTY||v.holder==NAKI_HOLDER_LIVE)return -EBUSY;
    v.phase=primary?NAKI_DEST_ACTIVE:NAKI_DEST_PREPARING;v.holder=NAKI_HOLDER_ABSENT;return save(l,v);
}
int naki_dl_started(NakiDestinationLife *l) {
    NakiDestinationView v=naki_dl_view(l);if(v.phase==NAKI_DEST_EMPTY||v.holder!=NAKI_HOLDER_ABSENT)return -EINVAL;
    v.holder=NAKI_HOLDER_LIVE;return save(l,v);
}
int naki_dl_ready(NakiDestinationLife *l,const NakiWriterLife *writer) {
    NakiDestinationView v=naki_dl_view(l);if(v.holder!=NAKI_HOLDER_LIVE||!naki_writer_is_ready(writer)||naki_writer_stopping(writer))return -EPIPE;
    if(v.phase==NAKI_DEST_WAIT_RAP)return 0;
    if(v.phase!=NAKI_DEST_PREPARING)return -EINVAL;
    v.phase=NAKI_DEST_WAIT_RAP;return save(l,v);
}
int naki_dl_attach(NakiDestinationLife *l,const NakiWriterLife *writer,int verified_idr,int committed) {
    NakiDestinationView v=naki_dl_view(l);
    if(v.phase!=NAKI_DEST_WAIT_RAP||v.holder!=NAKI_HOLDER_LIVE||!naki_writer_is_ready(writer)||naki_writer_stopping(writer))return -EPIPE;
    if(!verified_idr||!committed)return -EPROTO;
    v.phase=NAKI_DEST_ACTIVE;return save(l,v);
}
int naki_dl_remove(NakiDestinationLife *l) {
    NakiDestinationView v=naki_dl_view(l);if(v.phase==NAKI_DEST_EMPTY||v.phase==NAKI_DEST_REMOVING)return -ENOENT;
    v.phase=NAKI_DEST_REMOVING;return save(l,v);
}
int naki_dl_joined(NakiDestinationLife *l) {
    NakiDestinationView v=naki_dl_view(l);if(v.holder!=NAKI_HOLDER_LIVE)return -EINVAL;
    v.holder=NAKI_HOLDER_REAPED;return save(l,v);
}
int naki_dl_clear(NakiDestinationLife *l) {
    if(naki_dl_view(l).holder==NAKI_HOLDER_LIVE)return -EBUSY;
    return save(l,(NakiDestinationView){0});
}
