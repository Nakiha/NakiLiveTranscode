#include "media/session_idle.h"
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>

int main(void) {
    NakiSessionIdle idle;
    assert(naki_session_idle_init(&idle,-1,1,0)==-EINVAL);
    assert(naki_session_idle_init(&idle,60000,0,0)==-EINVAL);
    assert(!naki_session_idle_init(&idle,60000,2,100));
    assert(!naki_session_idle_outputs(&idle,1,200));
    assert(idle.state==NAKI_SESSION_ACTIVE&&!idle.deadline_us);
    assert(!naki_session_idle_outputs(&idle,0,300));
    assert(idle.state==NAKI_SESSION_IDLE&&idle.deadline_us==60000300);
    /* Polling, a repeated removal and candidate preparation cannot extend it. */
    assert(!naki_session_idle_poll(&idle,1000000));
    assert(!naki_session_idle_outputs(&idle,0,2000000));
    assert(idle.deadline_us==60000300);
    assert(!naki_session_idle_outputs(&idle,1,60000299));
    assert(idle.state==NAKI_SESSION_ACTIVE&&!idle.deadline_us);
    /* A later last removal starts a fresh full interval. */
    assert(!naki_session_idle_outputs(&idle,0,60000300));
    assert(idle.deadline_us==120000300);
    assert(naki_session_idle_outputs(&idle,1,120000300)==-ESHUTDOWN);
    assert(idle.state==NAKI_SESSION_EXPIRED&&!idle.outputs);
    assert(naki_session_idle_outputs(&idle,1,120000301)==-ESHUTDOWN);

    assert(!naki_session_idle_init(&idle,0,1,0));
    assert(naki_session_idle_outputs(&idle,0,1)==1);
    assert(naki_session_idle_outputs(&idle,1,1)==-ESHUTDOWN);
    assert(!naki_session_idle_init(&idle,1234,1,0));
    assert(!naki_session_idle_outputs(&idle,0,100));
    assert(idle.deadline_us==1234100);
    assert(!naki_session_idle_poll(&idle,1234099));
    assert(naki_session_idle_poll(&idle,1234100)==1);

    assert(!naki_session_idle_init(&idle,INT_MAX,1,INT64_MAX-2));
    assert(!naki_session_idle_outputs(&idle,0,INT64_MAX-1));
    assert(idle.deadline_us==INT64_MAX);
    assert(naki_session_idle_poll(&idle,INT64_MAX)==1);
    assert(!naki_session_idle_init(&idle,60000,1,50));
    assert(naki_session_idle_outputs(&idle,0,49)==-EINVAL);
    assert(idle.outputs==1&&idle.state==NAKI_SESSION_ACTIVE);
    /* Stop/EOF cancels idle and active sessions regardless of the setting. */
    naki_session_idle_finish(&idle);assert(naki_session_idle_poll(&idle,50)==1);
    assert(naki_session_idle_outputs(&idle,1,51)==-ESHUTDOWN);
    assert(!naki_session_idle_init(&idle,60000,1,0));
    assert(!naki_session_idle_outputs(&idle,0,10));
    naki_session_idle_finish(&idle);assert(!idle.deadline_us);
    assert(naki_session_idle_outputs(&idle,1,11)==-ESHUTDOWN);
    puts("PASS configurable idle: immediate/finite expiry, attach boundary, no extension, terminal stop/EOF, monotonic overflow");
    return 0;
}
