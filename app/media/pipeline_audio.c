#include "mux_internal.h"
#include "branch_queue.h"

static void audio_count(Pipeline *p,int unsupported){naki_job_count(p->job,unsupported?NAKI_COUNT_AUDIO_UNSUPPORTED:NAKI_COUNT_AUDIO_DROPPED,1);}
static int prepare_audio_params(AVCodecParameters *par, const AVPacket *pkt) {
    if(par->codec_id!=AV_CODEC_ID_AAC && par->codec_id!=AV_CODEC_ID_MP3)return AVERROR(ENOSYS);
    if(par->extradata_size>65536 || par->extradata_size<0)return AVERROR(EINVAL);
    size_t extra_bytes=0;const uint8_t *extra=av_packet_get_side_data(pkt,AV_PKT_DATA_NEW_EXTRADATA,&extra_bytes);
    if(extra) {
        if(extra_bytes<2||extra_bytes>65536)return AVERROR_INVALIDDATA;
        uint8_t *copy=av_mallocz(extra_bytes+AV_INPUT_BUFFER_PADDING_SIZE);if(!copy)return AVERROR(ENOMEM);
        memcpy(copy,extra,extra_bytes);av_freep(&par->extradata);par->extradata=copy;par->extradata_size=(int)extra_bytes;
    }
    if(par->codec_id==AV_CODEC_ID_AAC && pkt->size>=7 && pkt->data[0]==0xff && (pkt->data[1]&0xf6)==0xf0) {
        static const int rates[]={96000,88200,64000,48000,44100,32000,24000,22050,16000,12000,11025,8000,7350};
        int aot=(pkt->data[2]>>6)+1, frequency=(pkt->data[2]>>2)&15;
        int channels=((pkt->data[2]&1)<<2)|(pkt->data[3]>>6);
        if(frequency>=13 || !channels)return AVERROR(ENOSYS); /* PCE requires a parser, never guess it. */
        par->sample_rate=rates[frequency];
        av_channel_layout_uninit(&par->ch_layout); av_channel_layout_default(&par->ch_layout,channels==7 ? 8 : channels);
        if(!par->extradata_size) {
            par->extradata=av_mallocz(2+AV_INPUT_BUFFER_PADDING_SIZE);
            if(!par->extradata)return AVERROR(ENOMEM);
            par->extradata_size=2;
            par->extradata[0]=(aot<<3)|(frequency>>1);
            par->extradata[1]=((frequency&1)<<7)|(channels<<3);
        }
    }
    if((par->codec_id==AV_CODEC_ID_AAC && par->extradata_size>=2) ||
       (par->codec_id==AV_CODEC_ID_MP3 && (par->sample_rate<=0 || par->ch_layout.nb_channels<=0))) {
        /* Enhanced AAC has no legacy sound-rate/channel flags. Resolve its
           parameters with a bounded one-packet decode probe; the original
           compressed packet still goes through unchanged. */
        const AVCodec *codec=avcodec_find_decoder(par->codec_id);
        if(!codec)return AVERROR_DECODER_NOT_FOUND;
        AVCodecContext *probe=codec ? avcodec_alloc_context3(codec) : NULL;
        if(!probe)return AVERROR(ENOMEM);
        int ret=avcodec_parameters_to_context(probe,par);
        /* This isolated packet determines coded rate/layout, not audible
         * stream startup. Reapplying source initial_padding to a fresh probe
         * can trim its only frame and return EAGAIN for every AAC packet. */
        probe->flags2|=AV_CODEC_FLAG2_SKIP_MANUAL;probe->thread_count=1;
        if(ret>=0)ret=avcodec_open2(probe,codec,NULL);
        AVFrame *decoded=av_frame_alloc();
        if(!decoded){avcodec_free_context(&probe);return AVERROR(ENOMEM);}
        if(ret>=0)ret=avcodec_send_packet(probe,pkt);
        if(ret>=0)ret=avcodec_receive_frame(probe,decoded);
        if(ret>=0) {
            probe->sample_rate=decoded->sample_rate;
            probe->frame_size=decoded->nb_samples;
            av_channel_layout_uninit(&probe->ch_layout);
            ret=av_channel_layout_copy(&probe->ch_layout,&decoded->ch_layout);
        }
        if(ret>=0 && probe->sample_rate>0 && probe->ch_layout.nb_channels>0) {
            par->sample_rate=probe->sample_rate;par->frame_size=probe->frame_size;
            av_channel_layout_uninit(&par->ch_layout);ret=av_channel_layout_copy(&par->ch_layout,&probe->ch_layout);
        }
        av_frame_free(&decoded);avcodec_free_context(&probe);
        if(ret<0)return ret;
    }
    if(par->sample_rate<=0 || par->ch_layout.nb_channels<=0 ||
       (par->codec_id==AV_CODEC_ID_AAC && par->extradata_size<2))return AVERROR(EAGAIN);
    return 0;
}
static int process_audio(Pipeline *p, const InputItem *item) {
    AudioTrack *track=NULL;
    for(int i=0;i<p->mux->audio_count;i++)if(p->mux->audio[i].input_index==item->packet->stream_index)track=&p->mux->audio[i];
    if(!track) {
        if(p->mux->audio_count==16) {audio_count(p,1);return 0;}
        track=&p->mux->audio[p->mux->audio_count++];
        *track=(AudioTrack){.input_index=item->packet->stream_index,.last_dts=AV_NOPTS_VALUE};
    }
    if(track->rejected){audio_count(p,1);return 0;}
    /* Read the immutable input snapshot before prepare_audio_params performs
     * any container/decoder normalization on this branch-owned copy. */
    NakiAudioSignature observed={0};int signature_ret=naki_audio_signature_read(&observed,item->params,item->packet);
    if(signature_ret==AVERROR(ENOMEM))return signature_ret;
    if(track->output) {
        const char *reason=signature_ret<0?"input_configuration_unreadable":naki_audio_signature_change(&track->accepted,&observed);
        if(reason) {
            track->rejected=1;audio_count(p,1);
            yyjson_mut_doc *doc=new_event("audio_track_changed_unsupported",p->job);
            json_int(doc,"input_stream",track->input_index);json_str(doc,"reason",reason);json_int(doc,"error_code",signature_ret);
            json_int(doc,"accepted_core_sample_rate",track->accepted.core_rate);json_int(doc,"observed_core_sample_rate",observed.core_rate);
            json_int(doc,"accepted_coded_channels",track->accepted.layout.nb_channels);json_int(doc,"observed_coded_channels",observed.layout.nb_channels);
            json_int(doc,"accepted_object_type",track->accepted.object_type);json_int(doc,"observed_object_type",observed.object_type);
            logger_submit(naki_job_logger(p->job),NULL,doc);naki_audio_signature_clear(&observed);return 0;
        }
    } else if(signature_ret<0) {
        if(signature_ret==AVERROR(EAGAIN)&&!(track->pending_reported&1)){track->pending_reported|=1;yyjson_mut_doc *event=new_event("audio_parameters_pending",p->job);json_str(event,"stage","immutable input signature");json_int(event,"extradata_bytes",item->params->extradata_size);json_int(event,"packet_bytes",item->packet->size);logger_submit(naki_job_logger(p->job),NULL,event);}
        if(signature_ret!=AVERROR(EAGAIN)) {
            track->rejected=1;yyjson_mut_doc *doc=new_event("audio_track_rejected",p->job);
            json_int(doc,"input_stream",track->input_index);json_str(doc,"codec",avcodec_get_name(item->params->codec_id));
            json_str(doc,"reason","input_configuration_unreadable");json_int(doc,"error_code",signature_ret);logger_submit(naki_job_logger(p->job),NULL,doc);
        }
        audio_count(p,signature_ret!=AVERROR(EAGAIN));naki_audio_signature_clear(&observed);return 0;
    }
    if(!track->output) {
        int ret=prepare_audio_params(item->params,item->packet);
        if(ret==AVERROR(EAGAIN)){if(!(track->pending_reported&2)){track->pending_reported|=2;yyjson_mut_doc *event=new_event("audio_parameters_pending",p->job);json_str(event,"stage","bounded audio decoder probe");json_int(event,"extradata_bytes",item->params->extradata_size);json_int(event,"packet_bytes",item->packet->size);logger_submit(naki_job_logger(p->job),NULL,event);}audio_count(p,0);naki_audio_signature_clear(&observed);return 0;}
        if(ret==AVERROR(ENOMEM)) {naki_audio_signature_clear(&observed);return ret;}
        if(ret>=0 && item->params->codec_id==AV_CODEC_ID_AAC) {
            const AVBitStreamFilter *filter=av_bsf_get_by_name("aac_adtstoasc");
            ret=filter ? av_bsf_alloc(filter,&track->bsf) : AVERROR_BSF_NOT_FOUND;
            if(ret>=0)ret=avcodec_parameters_copy(track->bsf->par_in,item->params);
            if(ret>=0) {track->bsf->time_base_in=item->tb;ret=av_bsf_init(track->bsf);}
        }
        if(ret>=0)ret=avformat_live_add_audio(p->mux->output,item->params,item->tb,&track->output);
        if(ret<0) {
            naki_audio_signature_clear(&observed);if(ret==AVERROR(ENOMEM))return ret;
            track->rejected=1;audio_count(p,1);
            yyjson_mut_doc *doc=new_event("audio_track_rejected",p->job);
            json_int(doc,"input_stream",track->input_index);json_str(doc,"codec",avcodec_get_name(item->params->codec_id));
            json_int(doc,"error_code",ret);logger_submit(naki_job_logger(p->job),NULL,doc);
            return 0;
        }
        track->accepted=observed;memset(&observed,0,sizeof(observed));
        naki_job_count(p->job,NAKI_COUNT_AUDIO_TRACKS,1);
        yyjson_mut_doc *doc=new_event("audio_track_added",p->job);
        json_int(doc,"input_stream",track->input_index);json_int(doc,"output_stream",track->output->index);
        json_str(doc,"codec",avcodec_get_name(item->params->codec_id));json_int(doc,"sample_rate",item->params->sample_rate);
        json_str(doc,"action","registered_without_reopening_output");logger_submit(naki_job_logger(p->job),NULL,doc);
    }
    naki_audio_signature_clear(&observed);
    AVPacket *packet=av_packet_clone(item->packet);
    if(!packet)return AVERROR(ENOMEM);
    if(item->params->codec_id==AV_CODEC_ID_AAC && packet->size>=7 && packet->data[0]==0xff && (packet->data[1]&0xf6)==0xf0) {
        static const int rates[]={96000,88200,64000,48000,44100,32000,24000,22050,16000,12000,11025,8000,7350};
        int frequency=(packet->data[2]>>2)&15;
        int64_t raw=packet->dts!=AV_NOPTS_VALUE ? packet->dts : packet->pts;
        if(frequency<13) {
            /* Late ADTS is parsed without find_stream_info's decoded AAC
               parameters. Intermediate frames in one PES can lack timestamps.
               ADTS sample counts/rate give an exact duration in seconds even
               with implicit SBR; preserve each new PES anchor, expand only the
               missing/repeated timestamps between anchors. */
            int samples=1024*((packet->data[6]&3)+1);
            if(track->have_clock && (raw==AV_NOPTS_VALUE || raw==track->previous_input_dts)) {
                packet->pts=packet->dts=track->next_input_dts;
            } else if(raw!=AV_NOPTS_VALUE)packet->pts=packet->dts=raw;
            if(packet->dts!=AV_NOPTS_VALUE) {
                int64_t next=av_add_stable(item->tb,packet->dts,(AVRational){1,rates[frequency]},samples);
                packet->duration=next-packet->dts;track->next_input_dts=next;track->have_clock=1;
                if(raw!=AV_NOPTS_VALUE)track->previous_input_dts=raw;
            }
        }
    }
    int ret=0;
    AVRational tb=item->tb;
    if(track->bsf) {
        ret=av_bsf_send_packet(track->bsf,packet);
        if(ret>=0)ret=av_bsf_receive_packet(track->bsf,packet);
        tb=track->bsf->time_base_out;
    }
    if(ret>=0) {
        if(packet->dts==AV_NOPTS_VALUE)packet->dts=packet->pts;
        if(packet->pts==AV_NOPTS_VALUE)packet->pts=packet->dts;
        av_packet_rescale_ts(packet,tb,track->output->time_base);packet->stream_index=track->output->index;
        int64_t dts=packet->dts;
        if(dts==AV_NOPTS_VALUE || (track->last_dts!=AV_NOPTS_VALUE && dts<=track->last_dts) ||
           av_rescale_q(dts,track->output->time_base,AV_TIME_BASE_Q)<p->mux->mux_max_dts_us) {
            audio_count(p,0);ret=0;
        } else {
            track->last_dts=dts;ret=pipeline_submit_mux(p,packet);
        }
    } else if(ret!=AVERROR(ENOMEM)) {audio_count(p,0);ret=0;}
    av_packet_free(&packet);
    return ret;
}
int pipeline_handle_audio(Pipeline *p, const InputItem *item) {
    int prepared=pipeline_mux_prepare(p);if(prepared<0)return prepared;
    if(p->mux->video_started)return process_audio(p,item);
    NakiBranchItem accounting={.packet=item->packet,.parameters=item->params,.media_us=1,.ingress_us=0};
    int charged=naki_branch_item_charge(&accounting);if(charged<0)return charged;
    uint64_t cost=accounting.charged_bytes;
    if(cost>4*1024*1024){audio_count(p,0);return 0;}
    while(p->mux->deferred_count && (p->mux->deferred_count==128 || p->mux->deferred_bytes+cost>4*1024*1024)) {
        InputItem *old=&p->mux->deferred_audio[0];
        p->mux->deferred_bytes-=old->retained_bytes;
        pipeline_input_free(old);p->mux->deferred_count--;
        memmove(p->mux->deferred_audio,p->mux->deferred_audio+1,p->mux->deferred_count*sizeof(*old));audio_count(p,0);
    }
    InputItem copy={.packet=av_packet_clone(item->packet),.params=avcodec_parameters_alloc(),.tb=item->tb,.retained_bytes=cost};
    if(!copy.packet || !copy.params) {pipeline_input_free(&copy);return AVERROR(ENOMEM);}
    int ret=avcodec_parameters_copy(copy.params,item->params);
    if(ret<0){pipeline_input_free(&copy);return ret;}
    p->mux->deferred_audio[p->mux->deferred_count++]=copy;p->mux->deferred_bytes+=cost;
    return 0;
}
int pipeline_flush_deferred_audio(Pipeline *p) {
    if(!p->mux)return 0;
    if(!p->mux->video_started)return 0;
    while(p->mux->deferred_count) {
        InputItem item=p->mux->deferred_audio[0];
        p->mux->deferred_count--;p->mux->deferred_bytes-=item.retained_bytes;
        memmove(p->mux->deferred_audio,p->mux->deferred_audio+1,p->mux->deferred_count*sizeof(item));
        int ret=process_audio(p,&item);pipeline_input_free(&item);
        if(ret<0)return ret;
    }
    return 0;
}
