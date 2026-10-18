#include "pipeline_internal.h"

/* Reader alone reads mutable input streams and snapshots audio parameters. */
void pipeline_input_free(InputItem *item) {
    av_packet_free(&item->packet); avcodec_parameters_free(&item->params);
}

int pipeline_reader_sei_obligation(Pipeline *p,int64_t sequence,int add) {
    int result=AVERROR_INVALIDDATA;
    pthread_mutex_lock(&p->reader.ring_mutex);
    if(add) {
        result=AVERROR(ENOBUFS);
        for(unsigned i=0;i<NAKI_INFLIGHT_CAPACITY;i++)if(!p->reader.sei_pending[i]) {p->reader.sei_pending[i]=sequence+1;result=0;break;}
    } else for(unsigned i=0;i<NAKI_INFLIGHT_CAPACITY;i++)if(p->reader.sei_pending[i]==sequence+1) {p->reader.sei_pending[i]=0;result=0;break;}
    pthread_mutex_unlock(&p->reader.ring_mutex);return result;
}

static void packet_stamp_free(void *opaque, uint8_t *data) {
    Stamp *stamp=(Stamp *)data;
    if(stamp->tracked_packet)naki_policy_untrack(opaque,NAKI_TRACK_PACKET,(uint64_t)stamp->sequence);
    av_free(data);
}

static void *reader_main(void *opaque) {
    (void)crash_thread_init();
    Pipeline *p=opaque; Job *job=p->job;
    int ret=0; int64_t seq=0, first_ts=AV_NOPTS_VALUE, epoch=0, read_started=0;
    while (!atomic_load(&job->lifecycle.cancel)) {
        AVPacket *packet=av_packet_alloc();
        if (!packet) {ret=AVERROR(ENOMEM); break;}
        if(!read_started)read_started=now_us();
        int64_t began=read_started;
        atomic_store(&p->read_interrupt.deadline,began+IO_TIMEOUT_US);
        ret=av_read_frame(p->input,packet);
        int64_t finished=now_us();
        if(ret!=AVERROR(EAGAIN)) {atomic_store(&p->read_interrupt.deadline,0);read_started=0;}
        if (ret<0) {
            av_packet_free(&packet);
            if (ret==AVERROR(EAGAIN)) {av_usleep(1000); continue;}
            break;
        }
        if (packet->size<=0) {av_packet_free(&packet); continue;}
        if((unsigned)packet->size>NAKI_INPUT_PACKET_BYTES) {av_packet_free(&packet);ret=AVERROR(ENOBUFS);break;}
        int video=packet->stream_index==p->stream_index;
        InputItem item={.packet=packet,.tb=p->input_tb,.demuxed_us=finished};
        if (!video) {
            AVStream *stream=p->input->streams[packet->stream_index];
            int deliver=job->config.audio_copy && (job->config.output_count||strcmp(job->config.output_format,"mp4"));
            if (stream->codecpar->codec_type!=AVMEDIA_TYPE_AUDIO || (!deliver && !p->hooks.audio_packet)) {
                pipeline_input_free(&item); continue;
            }
            if(stream->codecpar->extradata_size<0 || stream->codecpar->extradata_size>65536||stream->codecpar->ch_layout.nb_channels>64||stream->codecpar->nb_coded_side_data>32) {
                pthread_mutex_lock(&job->mutex);job->stats.audio_unsupported++;pthread_mutex_unlock(&job->mutex);
                pipeline_input_free(&item);continue;
            }
            uint64_t side_bytes=0;
            for(int k=0;k<stream->codecpar->nb_coded_side_data;k++)side_bytes+=stream->codecpar->coded_side_data[k].size;
            if(side_bytes>262144) {pthread_mutex_lock(&job->mutex);job->stats.audio_unsupported++;pthread_mutex_unlock(&job->mutex);pipeline_input_free(&item);continue;}
            item.params=avcodec_parameters_alloc(); item.tb=stream->time_base;
            if (!item.params) {pipeline_input_free(&item); ret=AVERROR(ENOMEM); break;}
            ret=avcodec_parameters_copy(item.params,stream->codecpar);
            if(ret<0) {pipeline_input_free(&item); break;}
        }
        int64_t due=0;
        if (job->config.pace) {
            int64_t ts=packet->dts!=AV_NOPTS_VALUE ? packet->dts : packet->pts;
            if (ts==AV_NOPTS_VALUE) ts=av_rescale_q(seq,av_inv_q(p->frame_rate),p->input_tb);
            ts=av_rescale_q(ts,item.tb,AV_TIME_BASE_Q);
            if (first_ts==AV_NOPTS_VALUE) {first_ts=ts; epoch=finished;}
            due=epoch+ts-first_ts;
            while (!atomic_load(&job->lifecycle.cancel) && now_us()<due) {
                int64_t left=due-now_us();
                if (left>0) av_usleep((unsigned)(left>10000 ? 10000 : left));
            }
        }
        if (atomic_load(&job->lifecycle.cancel)) {pipeline_input_free(&item); break;}
        ret=av_packet_make_refcounted(packet);
        if (ret<0) {pipeline_input_free(&item); break;}
        if (video) {
            av_buffer_unref(&packet->opaque_ref);
            NakiSeiBundle *sei=NULL;
            ret=naki_sei_extract(packet,p->sei_codec,p->sei_length_size,&p->sei_input,&sei);
            if(ret<0) {p->sei_read_error=1;pipeline_input_free(&item);break;}
            size_t sei_size=naki_sei_bundle_size(sei);
            {
                uint8_t *data=av_mallocz(sizeof(Stamp)+sei_size);
                if(data&&sei)memcpy(data+sizeof(Stamp),sei,sei_size);
                packet->opaque_ref=data ? av_buffer_create(data,sizeof(Stamp)+sei_size,packet_stamp_free,job,0) : NULL;
                if(!packet->opaque_ref)av_free(data);
            }
            av_free(sei);
            if (!packet->opaque_ref) {pipeline_input_free(&item); ret=AVERROR(ENOMEM); break;}
            Stamp *stamp=(Stamp *)packet->opaque_ref->data;
            stamp->sei_size=(uint32_t)sei_size;
            if(seq==INT64_MAX) {pipeline_input_free(&item); ret=AVERROR(ERANGE); break;}
            stamp->sequence=seq++; stamp->read_us=finished-began;
            if(sei_size) {
                ret=pipeline_reader_sei_obligation(p,stamp->sequence,1);
                if(ret<0) {pipeline_input_free(&item);break;}
                pthread_mutex_lock(&job->mutex);job->stats.sei_extracted+=pipeline_stamp_sei(packet->opaque_ref)->count;pthread_mutex_unlock(&job->mutex);
            }
            stamp->ingress_us=now_us(); stamp->scheduled_us=due;
            stamp->packet_pts=packet->pts; stamp->packet_dts=packet->dts;
            ret=naki_policy_track(job,NAKI_TRACK_PACKET,(uint64_t)stamp->sequence,stamp->ingress_us);
            if(ret<0) {pipeline_input_free(&item); break;}
            stamp->tracked_packet=1;
            pipeline_checkpoint(p,&job->progress.first_packet_us);
            stamp->input_key=!!(packet->flags&AV_PKT_FLAG_KEY);
        }
        AVCodecParameters video_parameters={0};
        NakiBranchItem accounting={.packet=item.packet,.parameters=item.params?item.params:&video_parameters,.media_us=1,.ingress_us=item.demuxed_us};
        ret=naki_branch_item_charge(&accounting);
        if(ret<0||accounting.charged_bytes>NAKI_INPUT_RETAINED_ITEM_BYTES) {pipeline_input_free(&item);if(ret>=0)ret=AVERROR(ENOBUFS);break;}
        item.retained_bytes=accounting.charged_bytes;
        pthread_mutex_lock(&p->reader.ring_mutex);
        while ((p->reader.count==READER_CAPACITY||item.retained_bytes>NAKI_READER_RETAINED_BYTES-p->reader.reader_bytes) && !atomic_load(&job->lifecycle.cancel)) pthread_cond_wait(&p->reader.ring_space,&p->reader.ring_mutex);
        if (atomic_load(&job->lifecycle.cancel)) {pthread_mutex_unlock(&p->reader.ring_mutex); pipeline_input_free(&item); break;}
        p->reader.ring[(p->reader.head+p->reader.count)%READER_CAPACITY]=item; p->reader.count++;p->reader.reader_bytes+=item.retained_bytes;
        pthread_mutex_unlock(&p->reader.ring_mutex);
        pthread_mutex_lock(&job->mutex);job->progress.last_input_us=now_us();if(video)job->stats.packets_in++;pthread_mutex_unlock(&job->mutex);
        wake_job(job);
    }
    pthread_mutex_lock(&p->reader.ring_mutex);
    p->reader.read_done=1; p->reader.read_error=ret==AVERROR_EOF ? 0 : ret;
    pthread_mutex_unlock(&p->reader.ring_mutex);
    wake_job(job); return NULL;
}

InputItem pipeline_reader_pop(Pipeline *p, int *done, int *error) {
    InputItem item={0};
    pthread_mutex_lock(&p->reader.ring_mutex);
    if (p->reader.count) {
        item=p->reader.ring[p->reader.head]; p->reader.ring[p->reader.head]=(InputItem){0};
        p->reader.head=(p->reader.head+1)%READER_CAPACITY; p->reader.count--;p->reader.reader_bytes-=item.retained_bytes;
        pthread_cond_signal(&p->reader.ring_space);
    }
    *done=p->reader.read_done && !p->reader.count && !item.packet; *error=p->reader.read_error;
    pthread_mutex_unlock(&p->reader.ring_mutex);
    return item;
}

void pipeline_reader_init(Pipeline *p) {
    pthread_mutex_init(&p->reader.ring_mutex,NULL);
    pthread_cond_init(&p->reader.ring_space,NULL);
}
int pipeline_reader_start(Pipeline *p) {
    int ret=pthread_create(&p->reader.thread,NULL,reader_main,p);
    if(!ret)p->reader.started=1;
    return ret;
}
void pipeline_reader_wake(Pipeline *p) {
    pthread_mutex_lock(&p->reader.ring_mutex);
    pthread_cond_broadcast(&p->reader.ring_space);
    pthread_mutex_unlock(&p->reader.ring_mutex);
}
void pipeline_reader_join(Pipeline *p) {
    if(p->reader.started) {
        pthread_join(p->reader.thread,NULL);
        p->reader.started=0;
    }
}
void pipeline_reader_clear(Pipeline *p) {
    for(int i=0;i<READER_CAPACITY;i++)pipeline_input_free(&p->reader.ring[i]);
}
void pipeline_reader_destroy(Pipeline *p) {
    pthread_cond_destroy(&p->reader.ring_space);
    pthread_mutex_destroy(&p->reader.ring_mutex);
}
int pipeline_reader_pending_sei(Pipeline *p) {
    int pending=0;
    pthread_mutex_lock(&p->reader.ring_mutex);
    for(unsigned i=0;i<NAKI_INFLIGHT_CAPACITY;i++)if(p->reader.sei_pending[i]) {pending=1;break;}
    pthread_mutex_unlock(&p->reader.ring_mutex);
    return pending;
}
