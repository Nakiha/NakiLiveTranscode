#include "input_internal.h"
#include "branch_queue.h"

/* Reader alone reads mutable input streams and snapshots audio parameters. */
void pipeline_input_free(InputItem *item) {
    av_packet_free(&item->packet); avcodec_parameters_free(&item->params);item->hook_observed=0;
}

int pipeline_reader_sei_obligation(Pipeline *p,int64_t sequence,int add) {
    int result=AVERROR_INVALIDDATA;
    pthread_mutex_lock(&p->in->reader.ring_mutex);
    if(add) {
        result=AVERROR(ENOBUFS);
        for(unsigned i=0;i<NAKI_INFLIGHT_CAPACITY;i++)if(!p->in->reader.sei_pending[i]) {p->in->reader.sei_pending[i]=sequence+1;result=0;break;}
    } else for(unsigned i=0;i<NAKI_INFLIGHT_CAPACITY;i++)if(p->in->reader.sei_pending[i]==sequence+1) {p->in->reader.sei_pending[i]=0;result=0;break;}
    pthread_mutex_unlock(&p->in->reader.ring_mutex);return result;
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
    while (!naki_job_cancelled(job)) {
        /* Reserve reader space before consuming network bytes. One producer;
         * a consumer can only make more space while this read is in progress. */
        pthread_mutex_lock(&p->in->reader.ring_mutex);
        while(p->in->reader.count>=naki_job_config(job)->reader_queue_capacity&&!naki_job_cancelled(job))
            pthread_cond_wait(&p->in->reader.ring_space,&p->in->reader.ring_mutex);
        pthread_mutex_unlock(&p->in->reader.ring_mutex);
        if(naki_job_cancelled(job))break;
        AVPacket *packet=av_packet_alloc();
        if (!packet) {ret=AVERROR(ENOMEM); break;}
        if(!read_started)read_started=now_us();
        int64_t began=read_started;
        atomic_store(&p->in->read_interrupt.deadline,began+IO_TIMEOUT_US);
        ret=av_read_frame(p->in->input,packet);
        int64_t finished=now_us();
        if(ret!=AVERROR(EAGAIN)) {atomic_store(&p->in->read_interrupt.deadline,0);read_started=0;}
        if (ret<0) {
            av_packet_free(&packet);
            if (ret==AVERROR(EAGAIN)) {av_usleep(1000); continue;}
            break;
        }
        if (packet->size<=0) {av_packet_free(&packet); continue;}
        if((unsigned)packet->size>NAKI_INPUT_PACKET_BYTES) {av_packet_free(&packet);ret=AVERROR(ENOBUFS);break;}
        pthread_mutex_lock(&p->in->reader.ring_mutex);
        int audio_subscribed=p->in->reader.audio_subscribed;int64_t subscription_generation=p->in->reader.subscription_generation;NakiSeiConfig sei_snapshot=p->in->sei_input;
        pthread_mutex_unlock(&p->in->reader.ring_mutex);
        int video=packet->stream_index==p->in->stream_index;
        InputItem item={.packet=packet,.tb=p->input_tb,.demuxed_us=finished};
        if (!video) {
            AVStream *stream=p->in->input->streams[packet->stream_index];
            int deliver=audio_subscribed && (naki_job_config(job)->output_count||strcmp(naki_job_config(job)->output_format,"mp4"));
            if (stream->codecpar->codec_type!=AVMEDIA_TYPE_AUDIO || (!deliver && !p->hooks.audio_packet)) {
                pipeline_input_free(&item); continue;
            }
            if(stream->codecpar->extradata_size<0 || stream->codecpar->extradata_size>65536||stream->codecpar->ch_layout.nb_channels>64||stream->codecpar->nb_coded_side_data>32) {
                naki_job_count(job,NAKI_COUNT_AUDIO_UNSUPPORTED,1);
                pipeline_input_free(&item);continue;
            }
            uint64_t side_bytes=0;
            for(int k=0;k<stream->codecpar->nb_coded_side_data;k++)side_bytes+=stream->codecpar->coded_side_data[k].size;
            if(side_bytes>262144) {naki_job_count(job,NAKI_COUNT_AUDIO_UNSUPPORTED,1);pipeline_input_free(&item);continue;}
            item.params=avcodec_parameters_alloc(); item.tb=stream->time_base;
            if (!item.params) {pipeline_input_free(&item); ret=AVERROR(ENOMEM); break;}
            ret=avcodec_parameters_copy(item.params,stream->codecpar);
            if(ret<0) {pipeline_input_free(&item); break;}
        }
        int64_t due=0;
        if (naki_job_config(job)->pace) {
            int64_t ts=packet->dts!=AV_NOPTS_VALUE ? packet->dts : packet->pts;
            if (ts==AV_NOPTS_VALUE) ts=av_rescale_q(seq,av_inv_q(p->frame_rate),p->input_tb);
            ts=av_rescale_q(ts,item.tb,AV_TIME_BASE_Q);
            if (first_ts==AV_NOPTS_VALUE) {first_ts=ts; epoch=finished;}
            due=epoch+ts-first_ts;
            while (!naki_job_cancelled(job) && now_us()<due) {
                int64_t left=due-now_us();
                if (left>0) av_usleep((unsigned)(left>10000 ? 10000 : left));
            }
        }
        if (naki_job_cancelled(job)) {pipeline_input_free(&item); break;}
        ret=av_packet_make_refcounted(packet);
        if (ret<0) {pipeline_input_free(&item); break;}
        if (video) {
            const AVReadOrigin *origin=avformat_packet_read_origin(packet);
            AVReadOrigin source_origin=origin?*origin:(AVReadOrigin){0};
            if(naki_job_config(job)->network_trace&&!origin) {pipeline_input_free(&item);ret=AVERROR_INVALIDDATA;break;}
            if(naki_job_config(job)->source_rate_limit) {
                pthread_mutex_lock(&p->in->reader.ring_mutex);
                while(p->in->reader.rate_count&&p->in->reader.rate[p->in->reader.rate_head].us<=finished-1000000) {
                    p->in->reader.rate_head=(p->in->reader.rate_head+1)%1024;p->in->reader.rate_count--;
                }
                int full=p->in->reader.rate_count==1024;
                if(!full) {
                    int pos=(p->in->reader.rate_head+p->in->reader.rate_count++)%1024;
                    int64_t bits=8*((int64_t)packet->size+(seq==0?p->in->input->streams[p->in->stream_index]->codecpar->extradata_size:0));
                    p->in->reader.rate[pos].us=source_origin.complete_us;p->in->reader.rate[pos].bits=bits;
                }
                pthread_mutex_unlock(&p->in->reader.ring_mutex);
                if(full){pipeline_input_free(&item);ret=AVERROR(ENOBUFS);break;}
            }
            av_buffer_unref(&packet->opaque_ref);
            NakiSeiBundle *sei=NULL;
            ret=naki_sei_extract(packet,p->in->sei_codec,p->in->sei_length_size,&sei_snapshot,&sei);
            if(ret<0) {p->in->sei_read_error=1;pipeline_input_free(&item);break;}
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
            stamp->network_first_read_us=source_origin.first_read_us;
            stamp->network_complete_us=source_origin.complete_us;
            stamp->network_tag_position=source_origin.tag_position;
            stamp->demux_finished_us=finished;
            if(naki_job_config(job)->network_trace) {
                yyjson_mut_doc *event=new_event("input_video_packet",job);
                json_int(event,"sequence",seq);json_int(event,"bytes",packet->size);
                json_int(event,"config_bytes",seq==0?p->in->input->streams[p->in->stream_index]->codecpar->extradata_size:0);
                json_int(event,"first_read_monotonic_us",source_origin.first_read_us);
                json_int(event,"rtmp_complete_monotonic_us",source_origin.complete_us);
                json_int(event,"demux_finished_monotonic_us",finished);
                json_int(event,"tag_position",source_origin.tag_position);
                pipeline_trace_pts(event,"pts",packet->pts);pipeline_trace_pts(event,"dts",packet->dts);
                json_int(event,"tb_num",p->input_tb.num);json_int(event,"tb_den",p->input_tb.den);
                logger_submit(naki_job_logger(job),naki_job_trace(job),event);
            }
            stamp->sei_size=(uint32_t)sei_size;stamp->subscription_generation=subscription_generation;
            if(seq==INT64_MAX) {pipeline_input_free(&item); ret=AVERROR(ERANGE); break;}
            stamp->sequence=seq++; stamp->read_us=finished-began;
            if(sei_size) {
                ret=pipeline_reader_sei_obligation(p,stamp->sequence,1);
                if(ret<0) {pipeline_input_free(&item);break;}
                naki_job_count(job,NAKI_COUNT_SEI_EXTRACTED,pipeline_stamp_sei(packet->opaque_ref)->count);
            }
            stamp->ingress_us=now_us(); stamp->scheduled_us=due;
            stamp->packet_pts=packet->pts; stamp->packet_dts=packet->dts;
            ret=naki_policy_track(job,NAKI_TRACK_PACKET,(uint64_t)stamp->sequence,stamp->ingress_us);
            if(ret<0) {pipeline_input_free(&item); break;}
            stamp->tracked_packet=1;
            if(naki_job_renditions(job)){
                int registered=0;pthread_mutex_lock(&p->in->reader.ring_mutex);
                for(unsigned k=0;k<NAKI_INFLIGHT_CAPACITY;k++)if(!p->in->reader.epochs[k].sequence){p->in->reader.epochs[k].sequence=stamp->sequence+1;p->in->reader.epochs[k].generation=subscription_generation;registered=1;break;}
                pthread_mutex_unlock(&p->in->reader.ring_mutex);
                if(!registered){pipeline_input_free(&item);ret=AVERROR(ENOBUFS);break;}
            }
            naki_job_checkpoint(job,NAKI_CP_FIRST_PACKET,0);
            stamp->input_key=!!(packet->flags&AV_PKT_FLAG_KEY);
        }
        AVCodecParameters video_parameters={0};
        NakiBranchItem accounting={.packet=item.packet,.parameters=item.params?item.params:&video_parameters,.media_us=1,.ingress_us=item.demuxed_us};
        ret=naki_branch_item_charge(&accounting);
        if(ret<0||accounting.charged_bytes>NAKI_INPUT_RETAINED_ITEM_BYTES) {pipeline_input_free(&item);if(ret>=0)ret=AVERROR(ENOBUFS);break;}
        item.retained_bytes=accounting.charged_bytes;
        pthread_mutex_lock(&p->in->reader.ring_mutex);
        while ((p->in->reader.count==READER_CAPACITY||item.retained_bytes>NAKI_READER_RETAINED_BYTES-p->in->reader.reader_bytes) && !naki_job_cancelled(job)) pthread_cond_wait(&p->in->reader.ring_space,&p->in->reader.ring_mutex);
        if (naki_job_cancelled(job)) {pthread_mutex_unlock(&p->in->reader.ring_mutex); pipeline_input_free(&item); break;}
        p->in->reader.ring[(p->in->reader.head+p->in->reader.count)%READER_CAPACITY]=item; p->in->reader.count++;p->in->reader.reader_bytes+=item.retained_bytes;
        pthread_mutex_unlock(&p->in->reader.ring_mutex);
        naki_job_input_received(job,video,0,now_us());
        pipeline_decode_notify(p);
    }
    pthread_mutex_lock(&p->in->reader.ring_mutex);
    p->in->reader.read_done=1; p->in->reader.read_error=ret==AVERROR_EOF ? 0 : ret;
    pthread_mutex_unlock(&p->in->reader.ring_mutex);
    pipeline_decode_notify(p); return NULL;
}

InputItem pipeline_reader_pop(Pipeline *p, int *done, int *error) {
    InputItem item={0};
    pthread_mutex_lock(&p->in->reader.ring_mutex);
    if (p->in->reader.count) {
        item=p->in->reader.ring[p->in->reader.head]; p->in->reader.ring[p->in->reader.head]=(InputItem){0};
        p->in->reader.head=(p->in->reader.head+1)%READER_CAPACITY; p->in->reader.count--;p->in->reader.reader_bytes-=item.retained_bytes;
        pthread_cond_signal(&p->in->reader.ring_space);
    }
    *done=p->in->reader.read_done && !p->in->reader.count && !item.packet; *error=p->in->reader.read_error;
    pthread_mutex_unlock(&p->in->reader.ring_mutex);
    return item;
}

void pipeline_reader_init(Pipeline *p) {
    pthread_mutex_init(&p->in->reader.ring_mutex,NULL);
    pthread_cond_init(&p->in->reader.ring_space,NULL);
}
int pipeline_reader_start(Pipeline *p) {
    int ret=pthread_create(&p->in->reader.thread,NULL,reader_main,p);
    if(!ret)p->in->reader.started=1;
    return ret;
}
void pipeline_reader_wake(Pipeline *p) {
    if(!p->in)return;
    pthread_mutex_lock(&p->in->reader.ring_mutex);
    pthread_cond_broadcast(&p->in->reader.ring_space);
    pthread_mutex_unlock(&p->in->reader.ring_mutex);
}
void pipeline_reader_join(Pipeline *p) {
    if(p->in->reader.started) {
        if(pthread_join(p->in->reader.thread,NULL)){abort();}
        p->in->reader.started=0;
    }
}
void pipeline_reader_clear(Pipeline *p) {
    for(int i=0;i<READER_CAPACITY;i++)pipeline_input_free(&p->in->reader.ring[i]);
}
void pipeline_reader_destroy(Pipeline *p) {
    pthread_cond_destroy(&p->in->reader.ring_space);
    pthread_mutex_destroy(&p->in->reader.ring_mutex);
}
int pipeline_reader_pending_sei(Pipeline *p) {
    int pending=0;
    pthread_mutex_lock(&p->in->reader.ring_mutex);
    for(unsigned i=0;i<NAKI_INFLIGHT_CAPACITY;i++)if(p->in->reader.sei_pending[i]) {pending=1;break;}
    pthread_mutex_unlock(&p->in->reader.ring_mutex);
    return pending;
}

int64_t pipeline_source_rate(Pipeline *p,int64_t now) {
    if(naki_job_remote(p->job)==2)return naki_job_source_rate(p->job);
    pthread_mutex_lock(&p->in->reader.ring_mutex);
    while(p->in->reader.rate_count&&p->in->reader.rate[p->in->reader.rate_head].us<=now-1000000) {
        p->in->reader.rate_head=(p->in->reader.rate_head+1)%1024;p->in->reader.rate_count--;
    }
    int64_t bits=0;
    for(int i=0;i<p->in->reader.rate_count;i++) {
        int pos=(p->in->reader.rate_head+i)%1024;
        if(p->in->reader.rate[pos].us<=now)bits+=p->in->reader.rate[pos].bits;
    }
    pthread_mutex_unlock(&p->in->reader.ring_mutex);return bits;
}

int pipeline_reader_subscribe(Pipeline *p,const NakiSeiConfig *sei,int audio,int64_t *generation) {
    pthread_mutex_lock(&p->in->reader.ring_mutex);int ret=0;
    if(audio!=p->in->reader.audio_subscribed||sei->count!=p->in->sei_input.count||memcmp(sei->types,p->in->sei_input.types,sei->count*sizeof(sei->types[0]))) {
        if(p->in->reader.subscription_generation==INT64_MAX)ret=AVERROR(EOVERFLOW);
        else {p->in->sei_input=*sei;p->in->reader.audio_subscribed=audio;p->in->reader.subscription_generation++;}
    }
    *generation=p->in->reader.subscription_generation;pthread_mutex_unlock(&p->in->reader.ring_mutex);return ret;
}
int pipeline_reader_epoch_consumed(Pipeline *p,int64_t sequence) {
    int ret=AVERROR_INVALIDDATA;pthread_mutex_lock(&p->in->reader.ring_mutex);
    for(unsigned k=0;k<NAKI_INFLIGHT_CAPACITY;k++)if(p->in->reader.epochs[k].sequence==sequence+1){p->in->reader.epochs[k].sequence=0;ret=0;break;}
    pthread_mutex_unlock(&p->in->reader.ring_mutex);return ret;
}
int pipeline_reader_epoch_ready(Pipeline *p,int64_t generation) {
    int ready=1;pthread_mutex_lock(&p->in->reader.ring_mutex);
    for(unsigned k=0;k<NAKI_INFLIGHT_CAPACITY;k++)if(p->in->reader.epochs[k].sequence&&p->in->reader.epochs[k].generation<generation){ready=0;break;}
    pthread_mutex_unlock(&p->in->reader.ring_mutex);return ready;
}
