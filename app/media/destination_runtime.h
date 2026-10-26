#ifndef NAKI_DESTINATION_RUNTIME_H
#define NAKI_DESTINATION_RUNTIME_H
#include "control/config.h"
#include "control/component_update.h"
#include <libavutil/frame.h>
#include "control/operation_store.h"
#include <libavcodec/packet.h>
struct Pipeline;
struct InputItem;
typedef struct NakiDestinations NakiDestinations;
int naki_destinations_open(struct Pipeline *pipeline);
int naki_destinations_validate(const Config *config,const NakiUpdateRequest *request,char *error,size_t capacity);
int naki_destinations_submit(struct Pipeline *pipeline,const NakiUpdateRequest *request,const char *origin,yyjson_mut_doc **result,char *error,size_t capacity);
int naki_destinations_poll(struct Pipeline *pipeline);
int naki_destinations_packet(struct Pipeline *pipeline,AVPacket *packet);
int naki_destinations_audio(struct Pipeline *pipeline,const struct InputItem *audio);
int naki_destinations_force(struct Pipeline *pipeline,AVFrame *frame);
void naki_destinations_audio_written(struct Pipeline *pipeline);
int naki_destinations_written(struct Pipeline *pipeline,const AVPacket *packet,int64_t written_us);
int naki_destinations_close(struct Pipeline *pipeline,int stopped);
#endif
