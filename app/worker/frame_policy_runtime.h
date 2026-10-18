#ifndef NAKI_FRAME_POLICY_RUNTIME_H
#define NAKI_FRAME_POLICY_RUNTIME_H
#include "job_api.h"
void naki_policy_tick(Job *job, int64_t current_us);
int naki_policy_track(Job *job, int kind, uint64_t id, int64_t ingress_us);
int naki_policy_untrack(Job *job, int kind, uint64_t id);

#endif
