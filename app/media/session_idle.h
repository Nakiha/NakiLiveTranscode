#ifndef NAKI_SESSION_IDLE_H
#define NAKI_SESSION_IDLE_H
#include <stdint.h>

/* Pipeline-owner state only. Use CLOCK_MONOTONIC microseconds. Count committed
 * output memberships, including failed outputs awaiting manual recovery, not
 * healthy encoder processes. Candidate preparation is not an attachment.
 * Update this state within the topology commit, before publishing membership;
 * publish only on a nonnegative return. Once closed, the owner
 * must reject attachments even if a candidate was accepted before expiry. */
typedef enum NakiSessionIdleState {
    NAKI_SESSION_ACTIVE, NAKI_SESSION_IDLE, NAKI_SESSION_EXPIRED,
    NAKI_SESSION_FINISHED
} NakiSessionIdleState;
typedef struct NakiSessionIdle {
    NakiSessionIdleState state;
    unsigned outputs;
    int64_t timeout_us, deadline_us, last_now_us;
} NakiSessionIdle;
/* Initial sessions have at least one committed output. No wall clock is read. */
int naki_session_idle_init(NakiSessionIdle *idle,int timeout_ms,unsigned outputs,int64_t now_us);
/* Poll before committing any topology change. Returns 1 after expiry, 0 while
 * live, or a negative errno on invalid/backwards time. Exact deadline expires. */
int naki_session_idle_poll(NakiSessionIdle *idle,int64_t now_us);
/* Set the committed-output count at the frame-boundary commit. An idle session
 * can be attached only before its deadline; duplicate zero updates do not
 * extend it. Returns 1 if removing the last output expires immediately. */
int naki_session_idle_outputs(NakiSessionIdle *idle,unsigned outputs,int64_t now_us);
/* EOF, explicit stop or shared input failure ends the session unconditionally. */
void naki_session_idle_finish(NakiSessionIdle *idle);
#endif
