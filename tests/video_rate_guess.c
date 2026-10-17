/* Test-only dependency injection for an unknown initial library rate. The
 * encoded source and timestamps are real and are never relabelled or retimed. */
#include <stdlib.h>
#include <libavformat/avformat.h>
AVRational __real_av_guess_frame_rate(AVFormatContext *format,AVStream *stream,AVFrame *frame);
AVRational __wrap_av_guess_frame_rate(AVFormatContext *format,AVStream *stream,AVFrame *frame) {
    return getenv("NAKI_TEST_FRAME_RATE_UNKNOWN")?(AVRational){0,1}:__real_av_guess_frame_rate(format,stream,frame);
}
