#ifndef NAKI_RENDITION_MEDIA_H
#define NAKI_RENDITION_MEDIA_H
#include "worker/job.h"
#include "pipeline_reader.h"
struct Pipeline;
int naki_rendition_media_open(struct Pipeline **out,Job *job,const AVFrame *prototype,AVRational tb,NakiVideoRate rate,int wrap_bits,int io_timeout_ms);
int naki_rendition_media_frame(struct Pipeline *p,AVFrame *frame,uint64_t byte_limit);
int naki_rendition_media_audio(struct Pipeline *p,const InputItem *audio);
int naki_rendition_media_poll(struct Pipeline *p);
int naki_rendition_media_close(struct Pipeline **p,int error,int stopped);
#endif
