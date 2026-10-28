#include "media/rendition_lifecycle.h"
#include "media/destination_lifecycle.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
static void rendition_sequences(void) {
    NakiRenditionLife l={0};assert(!naki_rl_reserve(&l,0));assert(!naki_rl_member(&l));
    assert(!naki_rl_begin(&l,1));assert(naki_rl_attach(&l,1)==-EPIPE);assert(naki_rl_finished_event(&l,1)==-EPROTO);
    assert(!naki_rl_ready(&l,1));assert(!naki_rl_ready(&l,1));assert(!naki_rl_attach(&l,1));assert(naki_rl_member(&l)&&naki_rl_attached(&l));
    assert(!naki_rl_fail(&l));assert(!naki_rl_restart(&l));assert(naki_rl_member(&l)&&!naki_rl_attached(&l));
    assert(naki_rl_begin(&l,2)==-EBUSY);assert(naki_rl_clear(&l)==-EBUSY);
    assert(!naki_rl_stop(&l,NAKI_STOP_ABORT));assert(!naki_rl_fail(&l));assert(!naki_rl_stop(&l,NAKI_STOP_DRAIN));assert(naki_rl_view(&l).stop==NAKI_STOP_ABORT);
    assert(!naki_rl_finished_event(&l,1));assert(naki_rl_ready(&l,1)==-EPROTO);assert(naki_rl_clear(&l)==-EBUSY);
    assert(!naki_rl_reaped(&l,1));assert(!naki_rl_begin(&l,2));assert(!naki_rl_failed(&l)&&!naki_rl_finished(&l)&&!naki_rl_stopping(&l));
    assert(naki_rl_ready(&l,1)==-ESTALE);assert(!naki_rl_ready(&l,2));assert(!naki_rl_remove(&l));assert(!naki_rl_member(&l));assert(naki_rl_attach(&l,2)==-EINVAL);
    assert(naki_rl_clear(&l)==-EBUSY);assert(!naki_rl_reaped(&l,2));assert(!naki_rl_clear(&l));assert(!naki_rl_reserve(&l,1));
    assert(naki_rl_begin(&l,2)==-ESTALE);assert(!naki_rl_begin(&l,3));assert(naki_rl_member(&l));
    NakiRenditionLife failed={0};assert(!naki_rl_reserve(&failed,0)&&!naki_rl_begin(&failed,1));
    assert(!naki_rl_fail(&failed)&&!naki_rl_finished_event(&failed,1));assert(!naki_rl_prepared(&failed)&&naki_rl_finished(&failed));
    assert(!naki_rl_reaped(&failed,1)&&!naki_rl_clear(&failed));assert(naki_rl_failed(&failed));
    assert(!naki_rl_reserve(&failed,0)&&!naki_rl_begin(&failed,2));assert(!naki_rl_failed(&failed));
}
static int event(NakiRenditionLife *l,unsigned e) {
    switch(e) {
    case 0:return naki_rl_reserve(l,0);case 1:return naki_rl_reserve(l,1);
    case 2:return naki_rl_begin(l,1);case 3:return naki_rl_begin(l,2);
    case 4:return naki_rl_ready(l,1);case 5:return naki_rl_ready(l,2);
    case 6:return naki_rl_finished_event(l,1);case 7:return naki_rl_finished_event(l,2);
    case 8:return naki_rl_fail(l);case 9:return naki_rl_stop(l,NAKI_STOP_DRAIN);
    case 10:return naki_rl_stop(l,NAKI_STOP_ABORT);case 11:return naki_rl_restart(l);
    case 12:return naki_rl_remove(l);case 13:return naki_rl_attach(l,1);case 14:return naki_rl_attach(l,2);
    case 15:return naki_rl_reaped(l,1);case 16:return naki_rl_reaped(l,2);default:return naki_rl_clear(l);
    }
}
static int same(NakiRenditionLifeView a,NakiRenditionLifeView b) {
    return a.generation==b.generation&&a.membership==b.membership&&a.holder==b.holder&&a.handshake==b.handshake&&a.stop==b.stop&&a.outcome==b.outcome;
}
static void reachable_states(void) {
    NakiRenditionLife states[4096]={0};unsigned count=1,attempts=0;
    for(unsigned i=0;i<count;i++)for(unsigned e=0;e<18;e++) {
        NakiRenditionLife next=states[i];NakiRenditionLifeView before=naki_rl_view(&next);int ret=event(&next,e);attempts++;
        NakiRenditionLifeView after=naki_rl_view(&next);
        if(ret<0){assert(!memcmp(&next,&states[i],sizeof(next)));continue;}
        assert(after.generation>=before.generation);
        if(after.generation==before.generation&&e!=17) {
            assert(after.stop>=before.stop);assert(after.outcome>=before.outcome);
            if(before.holder==NAKI_HOLDER_LIVE&&e!=15&&e!=16)assert(after.holder==NAKI_HOLDER_LIVE);
        }
        if(after.generation>before.generation)assert(before.holder!=NAKI_HOLDER_LIVE);
        if(after.membership==NAKI_MEMBER_EMPTY)assert(after.holder!=NAKI_HOLDER_LIVE);
        if(after.handshake==NAKI_HANDSHAKE_ENDED_UNREADY)assert(after.outcome==NAKI_OUTCOME_FAILED||after.stop!=NAKI_STOP_RUN);
        unsigned k;for(k=0;k<count;k++)if(same(after,naki_rl_view(&states[k])))break;
        if(k==count){assert(count<4096);states[count++]=next;}
    }
    printf("reachable rendition states: %u, event attempts: %u\n",count,attempts);
}
static void destination_sequences(void) {
    NakiWriterLife w;naki_writer_init(&w);NakiDestinationLife l={0};
    assert(!naki_dl_reserve(&l,0));assert(!naki_dl_started(&l));
    assert(naki_dl_ready(&l,&w)==-EPIPE);assert(!naki_writer_ready(&w));assert(!naki_dl_ready(&l,&w));
    assert(naki_dl_attach(&l,&w,0,1)==-EPROTO);assert(naki_dl_attach(&l,&w,1,0)==-EPROTO);assert(!naki_dl_active(&l));
    assert(!naki_dl_attach(&l,&w,1,1));assert(naki_dl_active(&l));
    assert(!naki_dl_remove(&l));naki_writer_stop(&w);assert(naki_dl_attach(&l,&w,1,1)==-EPIPE);
    naki_writer_finish(&w);assert(naki_writer_done(&w));assert(naki_dl_clear(&l)==-EBUSY);
    assert(naki_dl_reserve(&l,0)==-EBUSY);assert(!naki_dl_joined(&l));assert(!naki_dl_clear(&l));assert(!naki_dl_reserve(&l,1));assert(naki_dl_active(&l));
    assert(naki_writer_ready(&w)==-EALREADY);
    naki_writer_init(&w);naki_writer_fail(&w);assert(naki_writer_ready(&w)==-ECANCELED);naki_writer_stop(&w);assert(naki_writer_view(&w).stop==NAKI_STOP_ABORT);
}
static void topology_sequence(void) {
    NakiTopologyPhase phase=NAKI_TOPOLOGY_NONE;
    assert(naki_topology_advance(&phase,NAKI_TOPOLOGY_WAIT_FRAME)==-EPERM&&phase==NAKI_TOPOLOGY_NONE);
    assert(!naki_topology_advance(&phase,NAKI_TOPOLOGY_RESERVED));
    assert(naki_topology_advance(&phase,NAKI_TOPOLOGY_WAIT_FRAME)==-EPERM&&phase==NAKI_TOPOLOGY_RESERVED);
    assert(!naki_topology_advance(&phase,NAKI_TOPOLOGY_WAIT_EXIT));
    assert(naki_topology_advance(&phase,NAKI_TOPOLOGY_WAIT_FRAME)==-EPERM&&phase==NAKI_TOPOLOGY_WAIT_EXIT);
    assert(!naki_topology_advance(&phase,NAKI_TOPOLOGY_PREPARING));
    assert(!naki_topology_advance(&phase,NAKI_TOPOLOGY_WAIT_FRAME));
    assert(!naki_topology_advance(&phase,NAKI_TOPOLOGY_WAIT_FRAME));
    assert(naki_topology_advance(&phase,NAKI_TOPOLOGY_WAIT_EXIT)==-EPERM&&phase==NAKI_TOPOLOGY_WAIT_FRAME);
}
typedef struct Race {NakiWriterLife life;pthread_barrier_t barrier;} Race;
static void *ready_race(void *opaque){Race *r=opaque;pthread_barrier_wait(&r->barrier);(void)naki_writer_ready(&r->life);return NULL;}
static void *fail_race(void *opaque){Race *r=opaque;pthread_barrier_wait(&r->barrier);naki_writer_fail(&r->life);naki_writer_stop(&r->life);return NULL;}
static void *finish_race(void *opaque){Race *r=opaque;pthread_barrier_wait(&r->barrier);naki_writer_finish(&r->life);return NULL;}
static void writer_races(void) {
    for(unsigned i=0;i<256;i++) {
        Race r;naki_writer_init(&r.life);assert(!pthread_barrier_init(&r.barrier,NULL,3));pthread_t threads[3];
        assert(!pthread_create(&threads[0],NULL,ready_race,&r));assert(!pthread_create(&threads[1],NULL,fail_race,&r));assert(!pthread_create(&threads[2],NULL,finish_race,&r));
        for(unsigned j=0;j<3;j++)assert(!pthread_join(threads[j],NULL));
        NakiWriterView view=naki_writer_view(&r.life);assert(view.phase==NAKI_WRITER_DONE&&view.outcome==NAKI_OUTCOME_FAILED&&view.stop==NAKI_STOP_ABORT);
        assert(naki_writer_ready(&r.life)==-EALREADY);pthread_barrier_destroy(&r.barrier);
    }
}
int main(void){rendition_sequences();reachable_states();topology_sequence();destination_sequences();writer_races();puts("PASS typed lifecycle: generation, membership, exit proof, IDR admission and atomic terminal races");return 0;}
