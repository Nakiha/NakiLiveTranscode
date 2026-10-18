#ifndef NAKI_INPUT_PROBE_H
#define NAKI_INPUT_PROBE_H

#include <libavformat/avformat.h>

/* Called before starting the demux reader. Keeps every probed packet buffered;
 * returns the first supported, non-attached video stream, or a negative error.
 * Audio and stream topology may still be incomplete on return. */
int naki_probe_video(AVFormatContext *input);

#endif
