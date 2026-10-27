#ifndef NAKI_RENDITION_RUNTIME_H
#define NAKI_RENDITION_RUNTIME_H
#include "worker/job.h"
struct Pipeline;
struct InputItem;
typedef struct NakiRenditions NakiRenditions;
int naki_renditions_create(Job *job);
yyjson_mut_doc *naki_renditions_snapshot(Job *job,const char *output_id);
int naki_renditions_submit(Job *job,yyjson_val *request,const char *origin,yyjson_mut_doc **result,char *error,size_t cap);
int naki_renditions_open(struct Pipeline *p);
int naki_renditions_poll(struct Pipeline *p,int finishing);
int naki_renditions_capacity(struct Pipeline *p);
int naki_renditions_frame(struct Pipeline *p,AVFrame *frame);
int naki_renditions_audio(struct Pipeline *p,const struct InputItem *audio);
int naki_renditions_finish(struct Pipeline *p,int stopped);
void naki_renditions_close(Job *job);
/* Takes ownership of fds only on success; done follows full cleanup. */
int naki_rendition_thread_start(int fds[5],const char *instance,pthread_t *thread,atomic_int *done);
int naki_rendition_child_main(int argc,char **argv);
#endif
