#include "session_idle.h"
#include <errno.h>
#include <limits.h>

int naki_session_idle_init(NakiSessionIdle *idle,int timeout_ms,unsigned outputs,int64_t now) {
    if(!idle||timeout_ms<0||!outputs||now<0)return -EINVAL;
    *idle=(NakiSessionIdle){.state=NAKI_SESSION_ACTIVE,.outputs=outputs,
        .timeout_us=(int64_t)timeout_ms*1000,.last_now_us=now};
    return 0;
}
int naki_session_idle_poll(NakiSessionIdle *idle,int64_t now) {
    if(!idle||now<0||now<idle->last_now_us)return -EINVAL;
    idle->last_now_us=now;
    if(idle->state==NAKI_SESSION_IDLE&&now>=idle->deadline_us)idle->state=NAKI_SESSION_EXPIRED;
    return idle->state==NAKI_SESSION_EXPIRED||idle->state==NAKI_SESSION_FINISHED;
}
int naki_session_idle_outputs(NakiSessionIdle *idle,unsigned outputs,int64_t now) {
    int ret=naki_session_idle_poll(idle,now);
    if(ret)return ret<0?ret:-ESHUTDOWN;
    if(outputs) {
        idle->outputs=outputs;idle->deadline_us=0;idle->state=NAKI_SESSION_ACTIVE;
    } else if(idle->outputs) {
        /* Saturation keeps the deadline monotonic near INT64_MAX. */
        idle->deadline_us=now>INT64_MAX-idle->timeout_us?INT64_MAX:now+idle->timeout_us;
        idle->outputs=0;idle->state=idle->timeout_us?NAKI_SESSION_IDLE:NAKI_SESSION_EXPIRED;
    }
    return idle->state==NAKI_SESSION_EXPIRED;
}
void naki_session_idle_finish(NakiSessionIdle *idle) {
    if(idle){idle->state=NAKI_SESSION_FINISHED;idle->deadline_us=0;}
}
