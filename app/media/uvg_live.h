#ifndef NAKI_UVG_LIVE_H
#define NAKI_UVG_LIVE_H
#include <libavcodec/avcodec.h>

typedef struct NakiUvg NakiUvg;
int naki_uvg_open(NakiUvg **owner, AVCodecContext *context, int quality,
                  int follow_policy, int extra_tools);
int naki_uvg_send(NakiUvg *enc, const AVFrame *frame, int requested_qp);
int naki_uvg_receive(NakiUvg *enc, AVPacket *packet);
void naki_uvg_close(NakiUvg **owner);
#endif
