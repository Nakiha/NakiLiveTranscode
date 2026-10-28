#ifndef NAKI_RENDITION_LIFECYCLE_H
#define NAKI_RENDITION_LIFECYCLE_H
#include <stdint.h>
#include "lifecycle_types.h"
/* Inline opaque value, zero initializes an empty slot. The topology owner
 * serializes these events. It must prove exit by pidfd/wait or actual thread
 * join before reaped; a child's finished message alone never proves exit. */
typedef struct NakiRenditionLife {uint64_t _private_life[4];} NakiRenditionLife;
typedef enum NakiMembership {NAKI_MEMBER_EMPTY,NAKI_MEMBER_CANDIDATE,NAKI_MEMBER_ATTACHED,NAKI_MEMBER_DETACHED,NAKI_MEMBER_REMOVED} NakiMembership;
typedef enum NakiHandshake {NAKI_HANDSHAKE_PENDING,NAKI_HANDSHAKE_READY,NAKI_HANDSHAKE_ENDED_UNREADY,NAKI_HANDSHAKE_ENDED_READY} NakiHandshake;
typedef struct NakiRenditionLifeView {
    uint64_t generation;
    NakiMembership membership;
    NakiHolderPhase holder;
    NakiHandshake handshake;
    NakiStopPolicy stop;
    NakiOutcome outcome;
} NakiRenditionLifeView;
NakiRenditionLifeView naki_rl_view(const NakiRenditionLife *life);
int naki_rl_reserve(NakiRenditionLife *life,int initial);
int naki_rl_begin(NakiRenditionLife *life,uint64_t generation);
int naki_rl_ready(NakiRenditionLife *life,uint64_t generation);
int naki_rl_finished_event(NakiRenditionLife *life,uint64_t generation);
int naki_rl_fail(NakiRenditionLife *life);
int naki_rl_stop(NakiRenditionLife *life,NakiStopPolicy policy);
int naki_rl_restart(NakiRenditionLife *life);
int naki_rl_remove(NakiRenditionLife *life);
int naki_rl_attach(NakiRenditionLife *life,uint64_t generation);
int naki_rl_reaped(NakiRenditionLife *life,uint64_t generation);
int naki_rl_clear(NakiRenditionLife *life);
static inline int naki_rl_member(const NakiRenditionLife *l){NakiMembership m=naki_rl_view(l).membership;return m==NAKI_MEMBER_ATTACHED||m==NAKI_MEMBER_DETACHED;}
static inline int naki_rl_attached(const NakiRenditionLife *l){return naki_rl_view(l).membership==NAKI_MEMBER_ATTACHED;}
static inline int naki_rl_removed(const NakiRenditionLife *l){return naki_rl_view(l).membership==NAKI_MEMBER_REMOVED;}
static inline int naki_rl_prepared(const NakiRenditionLife *l){NakiHandshake h=naki_rl_view(l).handshake;return h==NAKI_HANDSHAKE_READY||h==NAKI_HANDSHAKE_ENDED_READY;}
static inline int naki_rl_finished(const NakiRenditionLife *l){return naki_rl_view(l).handshake>=NAKI_HANDSHAKE_ENDED_UNREADY;}
static inline int naki_rl_failed(const NakiRenditionLife *l){return naki_rl_view(l).outcome==NAKI_OUTCOME_FAILED;}
static inline int naki_rl_stopping(const NakiRenditionLife *l){return naki_rl_view(l).stop!=NAKI_STOP_RUN;}
static inline int naki_rl_draining(const NakiRenditionLife *l){return naki_rl_view(l).stop==NAKI_STOP_DRAIN;}
typedef enum NakiTopologyPhase {NAKI_TOPOLOGY_NONE,NAKI_TOPOLOGY_RESERVED,NAKI_TOPOLOGY_WAIT_EXIT,NAKI_TOPOLOGY_PREPARING,NAKI_TOPOLOGY_WAIT_FRAME} NakiTopologyPhase;
int naki_topology_advance(NakiTopologyPhase *phase,NakiTopologyPhase next);
#endif
