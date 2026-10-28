#include "rendition_lifecycle.h"
#include <string.h>
#include <errno.h>
_Static_assert(sizeof(NakiRenditionLifeView)<=sizeof(NakiRenditionLife),"lifecycle storage");
NakiRenditionLifeView naki_rl_view(const NakiRenditionLife *l) {NakiRenditionLifeView v;memcpy(&v,l,sizeof(v));return v;}
static int save(NakiRenditionLife *l,NakiRenditionLifeView v){memset(l,0,sizeof(*l));memcpy(l,&v,sizeof(v));return 0;}
static int identity(NakiRenditionLifeView v,uint64_t generation){return v.holder!=NAKI_HOLDER_LIVE?-ESRCH:generation!=v.generation?-ESTALE:0;}
int naki_rl_reserve(NakiRenditionLife *l,int initial) {
    NakiRenditionLifeView v=naki_rl_view(l);if(v.membership!=NAKI_MEMBER_EMPTY||v.holder==NAKI_HOLDER_LIVE)return -EBUSY;
    v.membership=initial?NAKI_MEMBER_ATTACHED:NAKI_MEMBER_CANDIDATE;return save(l,v);
}
int naki_rl_begin(NakiRenditionLife *l,uint64_t generation) {
    NakiRenditionLifeView v=naki_rl_view(l);
    if(v.holder==NAKI_HOLDER_LIVE)return -EBUSY;
    if(!generation||generation<=v.generation)return -ESTALE;
    if(v.membership==NAKI_MEMBER_EMPTY||v.membership==NAKI_MEMBER_REMOVED)return -EINVAL;
    v.generation=generation;v.holder=NAKI_HOLDER_LIVE;v.handshake=NAKI_HANDSHAKE_PENDING;v.stop=NAKI_STOP_RUN;v.outcome=NAKI_OUTCOME_HEALTHY;return save(l,v);
}
int naki_rl_ready(NakiRenditionLife *l,uint64_t generation) {
    NakiRenditionLifeView v=naki_rl_view(l);int ret=identity(v,generation);if(ret)return ret;
    if(v.handshake>=NAKI_HANDSHAKE_ENDED_UNREADY)return -EPROTO;
    v.handshake=NAKI_HANDSHAKE_READY;return save(l,v);
}
int naki_rl_finished_event(NakiRenditionLife *l,uint64_t generation) {
    NakiRenditionLifeView v=naki_rl_view(l);int ret=identity(v,generation);if(ret)return ret;
    if(v.handshake==NAKI_HANDSHAKE_PENDING&&v.outcome==NAKI_OUTCOME_HEALTHY&&v.stop==NAKI_STOP_RUN)return -EPROTO;
    if(v.handshake<NAKI_HANDSHAKE_ENDED_UNREADY)v.handshake=v.handshake==NAKI_HANDSHAKE_READY?NAKI_HANDSHAKE_ENDED_READY:NAKI_HANDSHAKE_ENDED_UNREADY;
    return save(l,v);
}
int naki_rl_stop(NakiRenditionLife *l,NakiStopPolicy policy) {
    NakiRenditionLifeView v=naki_rl_view(l);if(v.holder!=NAKI_HOLDER_LIVE)return -ESRCH;
    if(policy!=NAKI_STOP_DRAIN&&policy!=NAKI_STOP_ABORT)return -EINVAL;
    if(policy>v.stop)v.stop=policy;
    return save(l,v);
}
int naki_rl_fail(NakiRenditionLife *l) {
    NakiRenditionLifeView v=naki_rl_view(l);if(v.holder!=NAKI_HOLDER_LIVE)return -ESRCH;
    v.outcome=NAKI_OUTCOME_FAILED;if(v.stop==NAKI_STOP_RUN)v.stop=NAKI_STOP_DRAIN;return save(l,v);
}
int naki_rl_restart(NakiRenditionLife *l) {
    NakiRenditionLifeView v=naki_rl_view(l);if(!naki_rl_member(l)||v.outcome!=NAKI_OUTCOME_FAILED)return -EINVAL;
    v.membership=NAKI_MEMBER_DETACHED;return save(l,v);
}
int naki_rl_remove(NakiRenditionLife *l) {
    NakiRenditionLifeView v=naki_rl_view(l);if(!naki_rl_member(l))return -ENOENT;
    v.membership=NAKI_MEMBER_REMOVED;return save(l,v);
}
int naki_rl_attach(NakiRenditionLife *l,uint64_t generation) {
    NakiRenditionLifeView v=naki_rl_view(l);int ret=identity(v,generation);if(ret)return ret;
    if(v.handshake!=NAKI_HANDSHAKE_READY||v.stop!=NAKI_STOP_RUN||v.outcome!=NAKI_OUTCOME_HEALTHY)return -EPIPE;
    if(v.membership!=NAKI_MEMBER_CANDIDATE&&v.membership!=NAKI_MEMBER_DETACHED&&v.membership!=NAKI_MEMBER_ATTACHED)return -EINVAL;
    v.membership=NAKI_MEMBER_ATTACHED;return save(l,v);
}
int naki_rl_reaped(NakiRenditionLife *l,uint64_t generation) {
    NakiRenditionLifeView v=naki_rl_view(l);int ret=identity(v,generation);if(ret)return ret;
    v.holder=NAKI_HOLDER_REAPED;return save(l,v);
}
int naki_rl_clear(NakiRenditionLife *l) {
    NakiRenditionLifeView v=naki_rl_view(l);if(v.holder==NAKI_HOLDER_LIVE)return -EBUSY;
    /* Keep terminal evidence until a strictly newer incarnation begins. In
     * particular a failed candidate may be reaped before its owner next polls
     * the operation; clearing its ID must not erase that failure. */
    v.membership=NAKI_MEMBER_EMPTY;v.holder=NAKI_HOLDER_ABSENT;return save(l,v);
}
int naki_topology_advance(NakiTopologyPhase *phase,NakiTopologyPhase next) {
    int valid=(*phase==NAKI_TOPOLOGY_NONE&&next==NAKI_TOPOLOGY_RESERVED)||
        (*phase==NAKI_TOPOLOGY_RESERVED&&(next==NAKI_TOPOLOGY_WAIT_EXIT||next==NAKI_TOPOLOGY_PREPARING))||
        (*phase==NAKI_TOPOLOGY_WAIT_EXIT&&next==NAKI_TOPOLOGY_PREPARING)||
        (*phase==NAKI_TOPOLOGY_PREPARING&&next==NAKI_TOPOLOGY_WAIT_FRAME)||
        (*phase==NAKI_TOPOLOGY_WAIT_FRAME&&next==NAKI_TOPOLOGY_WAIT_FRAME);
    if(!valid)return -EPERM;
    *phase=next;return 0;
}
