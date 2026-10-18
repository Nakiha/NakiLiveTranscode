#ifndef NAKI_PIPELINE_H
#define NAKI_PIPELINE_H

/* Worker media-thread entry; no AVFrame/AVPacket crosses IPC. */
void *pipeline_main(void *opaque);

#endif
