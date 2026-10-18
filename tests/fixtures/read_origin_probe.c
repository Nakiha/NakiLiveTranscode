/* Probe buffering must retain exact immutable RTMP receive origins. */
#include "media/input_probe.h"
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <libavformat/read_origin.h>
#include <libavutil/time.h>
#include <libavutil/adler32.h>
int main(int argc,char **argv) {
    if(argc!=3)return 2;
    int enabled=atoi(argv[2]);
    AVFormatContext *input=NULL;AVDictionary *options=NULL;
    if(enabled)av_dict_set(&options,"rtmp_read_origin","1",0);
    int ret=avformat_open_input(&input,argv[1],NULL,&options);
    if(ret<0||av_dict_count(options))return 3;
    av_dict_free(&options);
    int video=naki_probe_video(input);if(video<0)return 4;
    int64_t probed=av_gettime_relative();
    av_usleep(200000);
    AVPacket *packet=av_packet_alloc();if(!packet)return 5;
    int count=0;puts("[");
    while(count<16&&(ret=av_read_frame(input,packet))>=0) {
        if(packet->stream_index==video) {
            const AVReadOrigin *origin=avformat_packet_read_origin(packet);
            if(enabled&&!origin)return 6;
            printf("%s{\"pts\":%"PRId64",\"dts\":%"PRId64",\"bytes\":%d,\"checksum\":%u,"
                   "\"first_read_us\":%"PRId64",\"complete_us\":%"PRId64",\"probed_us\":%"PRId64",\"returned_us\":%"PRId64"}",
                   count?",\n":"",packet->pts,packet->dts,packet->size,av_adler32_update(1,packet->data,packet->size),
                   origin?origin->first_read_us:0,origin?origin->complete_us:0,probed,av_gettime_relative());
            count++;
        }
        av_packet_unref(packet);
    }
    puts("\n]");av_packet_free(&packet);avformat_close_input(&input);
    return count==16?0:7;
}
