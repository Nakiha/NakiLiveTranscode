#include "mux_internal.h"

int pipeline_mux_prepare(Pipeline *p) {
    if(p->mux)return 0;
    NakiMux *m=calloc(1,sizeof(*m));if(!m)return AVERROR(ENOMEM);
    m->custom_output_fd=-1;m->mux_max_dts_us=INT64_MIN;
    m->interrupt.job=p->job;atomic_init(&m->interrupt.deadline,0);atomic_init(&m->interrupt.cleanup,0);
    p->mux=m;return 0;
}
int pipeline_mux_format(Pipeline *p,int *global_header) {
    int ret=pipeline_mux_prepare(p);if(ret<0)return ret;
    const Config *cfg=naki_job_config(p->job);
    if(!p->mux->output) {
        ret=avformat_alloc_output_context2(&p->mux->output,NULL,cfg->output_format,cfg->output);
        if(ret<0)return ret;
        if(cfg->policy.timestamps==NAKI_TS_SOURCE_ALL)p->mux->output->avoid_negative_ts=AVFMT_AVOID_NEG_TS_DISABLED;
    }
    *global_header=!!(p->mux->output->oformat->flags&AVFMT_GLOBALHEADER);return 0;
}
int pipeline_mux_move(Pipeline *from,Pipeline *to) {
    if(from==to||!from->mux||to->mux||from->job!=to->job)return AVERROR(EINVAL);
    to->mux=from->mux;to->video=from->video;from->mux=NULL;
    return 0;
}
int pipeline_mux_live(const Pipeline *p){return p->mux&&p->mux->live_mux;}
uint64_t pipeline_mux_bytes(const Pipeline *p){return p->mux&&p->mux->output&&p->mux->output->pb?(uint64_t)p->mux->output->pb->bytes_written:0;}
void pipeline_mux_cancel(Pipeline *p){if(p->mux)atomic_store(&p->mux->interrupt.deadline,now_us());}
void pipeline_mux_retire_audio(Pipeline *p) {
    if(!p->mux)return;
    /* Finalize must close protocol callbacks before their storage is freed. */
    if(p->mux->output||p->mux->custom_output_fd>=0)abort();
    for(int i=0;i<p->mux->deferred_count;i++)pipeline_input_free(&p->mux->deferred_audio[i]);
    for(int i=0;i<p->mux->audio_count;i++) {
        av_bsf_free(&p->mux->audio[i].bsf);naki_audio_signature_clear(&p->mux->audio[i].accepted);
    }
    p->mux->deferred_count=0;p->mux->deferred_bytes=0;p->mux->audio_count=0;
}
void pipeline_mux_release(Pipeline *p) {
    if(!p->mux)return;
    pipeline_mux_retire_audio(p);
    free(p->mux);p->mux=NULL;
}

/* Called only by the owning shared/branch pipeline thread. The orchestration
 * layer stops readers/async or aborts branch queues before releasing media.
 * Finalize preserves the first error and closes the hook before references. */
/* The output owner alone runs this sequence. Preserve the first media error and
 * publish every cleanup result before the hook observes the terminal boundary. */
int pipeline_finalize_output(Pipeline *p,int primary,int stopped) {
    Job *job=p->job;int trailer=0,flush=0,closed=0;int64_t began=now_us();
    int64_t deadline=began+(p->io_timeout_us?p->io_timeout_us:IO_TIMEOUT_US);
    if(p->mux){atomic_store(&p->mux->interrupt.cleanup,1);atomic_store(&p->mux->interrupt.deadline,deadline);}
    naki_job_finalize_begin(job,began,deadline);
    if(p->mux&&p->mux->header_written)trailer=av_write_trailer(p->mux->output);
    if(p->mux&&p->mux->output&&p->mux->output->pb) {
        avio_flush(p->mux->output->pb);flush=p->mux->output->pb->error;
        naki_job_bytes_written(job,p->mux->output->pb->bytes_written);
        if(p->mux->output->flags&AVFMT_FLAG_CUSTOM_IO) {av_freep(&p->mux->output->pb->buffer);avio_context_free(&p->mux->output->pb);}
        else closed=avio_closep(&p->mux->output->pb);
    }
    if(p->mux&&p->mux->custom_output_fd>=0) {
        if(close(p->mux->custom_output_fd)<0&&closed>=0)closed=AVERROR(errno);
        p->mux->custom_output_fd=-1;
    }
    if(primary>=0&&!stopped) {
        if(trailer<0) {primary=trailer;p->stage="output trailer";}
        else if(flush<0) {primary=flush;p->stage="output flush";}
        else if(closed<0) {primary=closed;p->stage="output protocol close";}
    }
    naki_job_finalize_result(job,trailer,flush,closed,primary,now_us());
    if(p->hooks.close) {
        NakiMediaEnd end={.cancelled=stopped,.error_code=primary<0?primary:0,.stage=p->stage};
        if(naki_job_parent(job))end.output_failures=naki_job_failed(job)||(primary<0&&!stopped);
        else end.output_failures=naki_job_output_failures(job);
        p->hooks.close(p->hooks.opaque,&end);p->hooks=(NakiMediaHooks){0};
    }
    if(p->mux) {
        if(p->mux->output) {avformat_free_context(p->mux->output);p->mux->output=NULL;}
        p->mux->header_written=p->mux->live_mux=0;
        atomic_store(&p->mux->interrupt.deadline,0);
    }
    return primary;
}

static int local_write(void *opaque, const uint8_t *buffer, int size) {
    NakiMux *m=opaque; int offset=0;
    while (offset<size) {
        ssize_t n=write(m->custom_output_fd,buffer+offset,(size_t)(size-offset));
        if (n<0 && errno==EINTR) continue;
        if (n<=0) return AVERROR(n<0 ? errno : EIO);
        offset+=(int)n;
    }
    return size;
}

static int64_t local_seek(void *opaque, int64_t offset, int whence) {
    NakiMux *m=opaque;
    if (whence==AVSEEK_SIZE) {struct stat st; return fstat(m->custom_output_fd,&st) ? AVERROR(errno) : st.st_size;}
    off_t pos=lseek(m->custom_output_fd,(off_t)offset,whence&~AVSEEK_FORCE);
    return pos<0 ? AVERROR(errno) : (int64_t)pos;
}

/* One mux owner; encoder parameters are immutable after codec open. This also
 * prepares an independent destination without opening a second encoder. */
int pipeline_open_mux(Pipeline *p) {
    const Config *cfg=naki_job_config(p->job);int ret;
    if((ret=pipeline_mux_prepare(p))<0)return ret;
    int global_header=0;if((ret=pipeline_mux_format(p,&global_header))<0)return ret;
    AVStream *video=avformat_new_stream(p->mux->output,NULL);
    if (!video) return AVERROR(ENOMEM);
    ret=naki_encoded_parameters(p->encoded,video->codecpar);
    if (ret<0) return ret;
    video->time_base=naki_encoded_info(p->encoded).time_base;video->avg_frame_rate=p->output_rate.value;
    p->mux->output->flags|=AVFMT_FLAG_FLUSH_PACKETS;
    p->mux->output->max_delay=0; p->mux->output->max_interleave_delta=0;
    p->mux->output->interrupt_callback=(AVIOInterruptCB){pipeline_interrupted,&p->mux->interrupt};
    p->stage="output open/header";
    atomic_store(&p->mux->interrupt.deadline,now_us()+(p->io_timeout_us?p->io_timeout_us:IO_TIMEOUT_US));
    if (!strstr(cfg->output,"://") && !strchr(cfg->output,':')) {
        p->mux->custom_output_fd=open(cfg->output,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0644);
        if (p->mux->custom_output_fd<0) return AVERROR(errno);
        unsigned char *buffer=av_malloc(32768);
        if (!buffer) return AVERROR(ENOMEM);
        p->mux->output->pb=avio_alloc_context(buffer,32768,1,p->mux,NULL,local_write,local_seek);
        if (!p->mux->output->pb) {av_free(buffer); return AVERROR(ENOMEM);}
        p->mux->output->flags|=AVFMT_FLAG_CUSTOM_IO;
    } else {
        AVDictionary *io_options=NULL;
        if(cfg->rtmp_tcp_nodelay&&!strncmp(cfg->output,"rtmp://",7))av_dict_set(&io_options,"tcp_nodelay","1",0);
        ret=avio_open2(&p->mux->output->pb,cfg->output,AVIO_FLAG_WRITE,&p->mux->output->interrupt_callback,&io_options);
        if(ret>=0&&av_dict_count(io_options))ret=AVERROR_OPTION_NOT_FOUND;
        av_dict_free(&io_options);
        if (ret<0) return ret;
    }
    ret=avformat_write_header(p->mux->output,NULL);
    atomic_store(&p->mux->interrupt.deadline,0);
    if (ret<0) return ret;
    p->video.time_base=video->time_base;p->video.index=video->index;
    p->mux->header_written=1;
    if(strcmp(cfg->output_format,"mp4")) {
        ret=avformat_live_mux_init(p->mux->output,(int64_t)cfg->mux_wait_ms*1000,(p->destination_owner||naki_job_remote(p->job)==2)?4:256,4*1024*1024);
        if(ret<0)return ret;
        p->mux->live_mux=1;
    }
    return 0;
}
