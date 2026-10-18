#ifndef NAKI_JOB_API_H
#define NAKI_JOB_API_H
#include <stdint.h>
#include <yyjson.h>
typedef struct Job Job;
void wake_job(void *opaque);
void request_stop(Job *job);
yyjson_mut_doc *new_event(const char *event, const Job *job);
yyjson_mut_doc *job_snapshot(Job *job);
yyjson_mut_doc *job_report(Job *job, int final);
void naki_branch_release(Job *job,uint64_t id,uint64_t bytes);

#endif
