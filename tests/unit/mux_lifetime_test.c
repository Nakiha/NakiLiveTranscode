#define _GNU_SOURCE
#include "media/mux_internal.h"
#include "worker/job.h"
#include <assert.h>

/* The isolated test links the real mux open/move/finalize/release implementation.
 * No input reader or cancellation loop is needed for this local file writer. */
int pipeline_interrupted(void *opaque){(void)opaque;return 0;}
void pipeline_input_free(InputItem *item) {
    av_packet_free(&item->packet);avcodec_parameters_free(&item->params);
}
static int releases;
static void payload_free(void *opaque,uint8_t *bytes){(void)opaque;releases++;av_free(bytes);}

int main(void) {
    Job *job=calloc(1,sizeof(*job)),*other=calloc(1,sizeof(*other));
    Pipeline *source=calloc(1,sizeof(*source)),*writer=calloc(1,sizeof(*writer));
    assert(job&&other&&source&&writer);assert(!pthread_mutex_init(&job->mutex,NULL));
    source->job=writer->job=job;source->output_rate.value=(AVRational){25,1};
    char directory[]="/tmp/naki-mux-owner-XXXXXX";assert(mkdtemp(directory));
    snprintf(job->config.output,sizeof(job->config.output),"%s/output.ts",directory);
    strcpy(job->config.output_format,"mpegts");job->config.mux_wait_ms=50;
    AVCodecContext *encoder=avcodec_alloc_context3(NULL);assert(encoder);source->encoder=encoder;
    encoder->codec_type=AVMEDIA_TYPE_VIDEO;encoder->codec_id=AV_CODEC_ID_H264;
    encoder->width=encoder->height=16;encoder->pix_fmt=AV_PIX_FMT_YUV420P;encoder->time_base=(AVRational){1,90000};
    assert(!pipeline_open_mux(source));NakiMux *original=source->mux;
    void *interrupt=original->output->interrupt_callback.opaque,*avio=original->output->pb->opaque;
    int fd=original->custom_output_fd;assert(interrupt==&original->interrupt&&avio==original);
    original->mux_max_dts_us=1234;original->video_started=1;
    InputItem *audio=&original->deferred_audio[0];audio->packet=av_packet_alloc();assert(audio->packet);
    uint8_t *bytes=av_malloc(4);assert(bytes);audio->packet->buf=av_buffer_create(bytes,4,payload_free,NULL,0);assert(audio->packet->buf);
    audio->packet->data=bytes;audio->packet->size=4;audio->params=avcodec_parameters_alloc();assert(audio->params);
    original->deferred_count=1;original->deferred_bytes=4;
    assert(pipeline_mux_move(source,source)==AVERROR(EINVAL));
    writer->job=other;assert(pipeline_mux_move(source,writer)==AVERROR(EINVAL));writer->job=job;
    assert(!pipeline_mux_prepare(writer));assert(pipeline_mux_move(source,writer)==AVERROR(EINVAL));pipeline_mux_release(writer);
    assert(!pipeline_mux_move(source,writer));assert(!source->mux&&writer->mux==original);
    assert(original->output->interrupt_callback.opaque==interrupt&&original->output->pb->opaque==avio);
    assert(original->mux_max_dts_us==1234&&original->deferred_count==1&&!releases);
    /* Model pthread_create failure: returning the object restores all resources. */
    assert(!pipeline_mux_move(writer,source));assert(source->mux==original&&!writer->mux&&!releases);
    assert(!pipeline_mux_move(source,writer));free(source);
    /* The protocol/local writer no longer depends on the old Pipeline's lifetime. */
    const uint8_t marker[]={1,2,3,4};avio_write(original->output->pb,marker,sizeof(marker));avio_flush(original->output->pb);
    assert(!original->output->pb->error&&pipeline_mux_bytes(writer)>=sizeof(marker));
    assert(fcntl(fd,F_GETFD)>=0);pipeline_mux_cancel(writer);assert(atomic_load(&original->interrupt.deadline)>0);
    assert(!pipeline_finalize_output(writer,0,0));assert(!original->output&&original->custom_output_fd==-1);
    assert(fcntl(fd,F_GETFD)<0&&errno==EBADF&&!releases);
    pipeline_mux_retire_audio(writer);assert(releases==1&&writer->mux==original);
    /* done/failure can race cancellation until the writer is joined. Payloads
     * are gone but the atomic interrupt target must remain alive and stable. */
    pipeline_mux_cancel(writer);assert(atomic_load(&original->interrupt.deadline)>0);
    pipeline_mux_release(writer);assert(!writer->mux&&releases==1);pipeline_mux_release(writer);assert(releases==1);
    avcodec_free_context(&encoder);assert(!unlink(job->config.output));assert(!rmdir(directory));
    pthread_mutex_destroy(&job->mutex);free(writer);free(job);free(other);
    puts("PASS stable mux callbacks, exclusive move/rollback, close and exactly-once reference release");return 0;
}
