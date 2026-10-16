#include "live.h"
#include "crash.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/buffer.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>

#define READER_CAPACITY 8
#define LEDGER_CAPACITY 256
#define IO_TIMEOUT_US 10000000

typedef struct Stamp {
    int64_t sequence, read_us, ingress_us, submitted_us, decoded_us, filtered_us;
    int64_t encoder_submit_us, scheduled_us, filter_started_us;
    int picture_type, decoded_key, input_key;
} Stamp;
typedef struct Interrupt { Job *job; atomic_int_fast64_t deadline; } Interrupt;
typedef struct Ledger { int used; int64_t pts; Stamp stamp; } Ledger;
typedef struct Pipeline {
    Job *job;
    const char *stage;
    AVFormatContext *input, *output;
    AVCodecContext *decoder, *encoder;
    AVCodecAsyncContext *async;
    AVFilterGraph *graph;
    AVFilterContext *source, *sink;
    AVStream *video;
    int stream_index, reader_started, header_written, custom_output_fd;
    int input_width, input_height, input_format;
    AVRational input_tb, frame_rate;
    Interrupt read_interrupt, write_interrupt;
    AVPacket *ring[READER_CAPACITY];
    int head, count, read_done, read_error;
    pthread_mutex_t ring_mutex;
    pthread_cond_t ring_space;
    pthread_t reader;
    Ledger ledger[LEDGER_CAPACITY];
    int64_t last_pts, synthetic_pts, next_stats_us;
} Pipeline;

static int interrupted(void *opaque) {
    Interrupt *io=opaque;
    int64_t deadline=atomic_load(&io->deadline);
    return atomic_load(&io->job->cancel) || (deadline && now_us()>deadline);
}
static void refresh_auto(Pipeline *p) {
    AVCodecAsyncAutoStats stats={0};
    if (p->async) avcodec_async_get_auto_stats(p->async,&stats);
    pthread_mutex_lock(&p->job->mutex);
    p->job->auto_stats=stats;
    p->job->auto_updated_us=now_us();
    pthread_mutex_unlock(&p->job->mutex);
}
static int event_log(Pipeline *p, const char *event) {
    logger_submit(p->job->logger,NULL,new_event(event,p->job)); return 0;
}
static void *reader_main(void *opaque) {
    (void)crash_thread_init();
    Pipeline *p=opaque; Job *job=p->job;
    int ret=0; int64_t seq=0, first_ts=AV_NOPTS_VALUE, epoch=0;
    while (!atomic_load(&job->cancel)) {
        AVPacket *packet=av_packet_alloc();
        if (!packet) {ret=AVERROR(ENOMEM); break;}
        int64_t began=now_us();
        atomic_store(&p->read_interrupt.deadline,began+IO_TIMEOUT_US);
        ret=av_read_frame(p->input,packet);
        int64_t finished=now_us();
        atomic_store(&p->read_interrupt.deadline,0);
        if (ret<0) {
            av_packet_free(&packet);
            if (ret==AVERROR(EAGAIN)) {av_usleep(1000); continue;}
            break;
        }
        if (packet->stream_index!=p->stream_index || packet->size<=0) {av_packet_free(&packet); continue;}
        int64_t due=0;
        if (job->config.pace) {
            int64_t ts=packet->dts!=AV_NOPTS_VALUE ? packet->dts : packet->pts;
            if (ts==AV_NOPTS_VALUE) ts=av_rescale_q(seq,av_inv_q(p->frame_rate),p->input_tb);
            if (first_ts==AV_NOPTS_VALUE) {first_ts=ts; epoch=finished;}
            due=epoch+av_rescale_q(ts-first_ts,p->input_tb,AV_TIME_BASE_Q);
            while (!atomic_load(&job->cancel) && now_us()<due) {
                int64_t left=due-now_us();
                if (left>0) av_usleep((unsigned)(left>10000 ? 10000 : left));
            }
        }
        if (atomic_load(&job->cancel)) {av_packet_free(&packet); break;}
        ret=av_packet_make_refcounted(packet);
        if (ret<0) {av_packet_free(&packet); break;}
        av_buffer_unref(&packet->opaque_ref);
        packet->opaque_ref=av_buffer_allocz(sizeof(Stamp));
        if (!packet->opaque_ref) {av_packet_free(&packet); ret=AVERROR(ENOMEM); break;}
        Stamp *stamp=(Stamp *)packet->opaque_ref->data;
        stamp->sequence=seq++; stamp->read_us=finished-began;
        stamp->ingress_us=now_us(); stamp->scheduled_us=due;
        stamp->input_key=!!(packet->flags&AV_PKT_FLAG_KEY);
        pthread_mutex_lock(&p->ring_mutex);
        while (p->count==READER_CAPACITY && !atomic_load(&job->cancel)) pthread_cond_wait(&p->ring_space,&p->ring_mutex);
        if (atomic_load(&job->cancel)) {pthread_mutex_unlock(&p->ring_mutex); av_packet_free(&packet); break;}
        p->ring[(p->head+p->count)%READER_CAPACITY]=packet; p->count++;
        pthread_mutex_unlock(&p->ring_mutex);
        pthread_mutex_lock(&job->mutex); job->packets_in++; pthread_mutex_unlock(&job->mutex);
        wake_job(job);
    }
    pthread_mutex_lock(&p->ring_mutex);
    p->read_done=1; p->read_error=ret==AVERROR_EOF ? 0 : ret;
    pthread_mutex_unlock(&p->ring_mutex);
    wake_job(job); return NULL;
}
static AVPacket *pop_input(Pipeline *p, int *done, int *error) {
    AVPacket *packet=NULL;
    pthread_mutex_lock(&p->ring_mutex);
    if (p->count) {
        packet=p->ring[p->head]; p->ring[p->head]=NULL;
        p->head=(p->head+1)%READER_CAPACITY; p->count--;
        pthread_cond_signal(&p->ring_space);
    }
    *done=p->read_done && !p->count && !packet; *error=p->read_error;
    pthread_mutex_unlock(&p->ring_mutex);
    return packet;
}
static int local_write(void *opaque, const uint8_t *buffer, int size) {
    Pipeline *p=opaque; int offset=0;
    while (offset<size) {
        ssize_t n=write(p->custom_output_fd,buffer+offset,(size_t)(size-offset));
        if (n<0 && errno==EINTR) continue;
        if (n<=0) return AVERROR(n<0 ? errno : EIO);
        offset+=(int)n;
    }
    return size;
}
static int64_t local_seek(void *opaque, int64_t offset, int whence) {
    Pipeline *p=opaque;
    if (whence==AVSEEK_SIZE) {struct stat st; return fstat(p->custom_output_fd,&st) ? AVERROR(errno) : st.st_size;}
    off_t pos=lseek(p->custom_output_fd,(off_t)offset,whence&~AVSEEK_FORCE);
    return pos<0 ? AVERROR(errno) : (int64_t)pos;
}
static int open_output(Pipeline *p, AVFrame *frame) {
    const Config *cfg=&p->job->config;
    int ret;
    p->stage="filter initialization";
    p->graph=avfilter_graph_alloc();
    if (!p->graph) return AVERROR(ENOMEM);
    /* Keep filter threading independent from the adaptive decoder budget. */
    p->graph->nb_threads=1;
    char args[512], chain[256];
    AVRational sar=frame->sample_aspect_ratio;
    if (!sar.num || !sar.den) sar=(AVRational){1,1};
    snprintf(args,sizeof(args),"video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
             frame->width,frame->height,frame->format,p->input_tb.num,p->input_tb.den,sar.num,sar.den);
    ret=avfilter_graph_create_filter(&p->source,avfilter_get_by_name("buffer"),"in",args,NULL,p->graph);
    if (ret<0) return ret;
    ret=avfilter_graph_create_filter(&p->sink,avfilter_get_by_name("buffersink"),"out",NULL,NULL,p->graph);
    if (ret<0) return ret;
    if (cfg->width) snprintf(chain,sizeof(chain),"scale=%d:%d:flags=bilinear,format=pix_fmts=yuv420p",cfg->width,cfg->height);
    else strcpy(chain,"null,format=pix_fmts=yuv420p");
    AVFilterInOut *outputs=avfilter_inout_alloc(), *inputs=avfilter_inout_alloc();
    if (!outputs || !inputs) {avfilter_inout_free(&outputs); avfilter_inout_free(&inputs); return AVERROR(ENOMEM);}
    outputs->name=av_strdup("in"); outputs->filter_ctx=p->source; outputs->pad_idx=0;
    inputs->name=av_strdup("out"); inputs->filter_ctx=p->sink; inputs->pad_idx=0;
    ret=avfilter_graph_parse_ptr(p->graph,chain,&inputs,&outputs,NULL);
    avfilter_inout_free(&inputs); avfilter_inout_free(&outputs);
    if (ret<0 || (ret=avfilter_graph_config(p->graph,NULL))<0) return ret;
    p->input_width=frame->width; p->input_height=frame->height; p->input_format=frame->format;

    p->stage="encoder initialization";
    const AVCodec *codec=avcodec_find_encoder_by_name("libx264");
    if (!codec) return AVERROR_ENCODER_NOT_FOUND;
    p->encoder=avcodec_alloc_context3(codec);
    if (!p->encoder) return AVERROR(ENOMEM);
    p->encoder->width=av_buffersink_get_w(p->sink);
    p->encoder->height=av_buffersink_get_h(p->sink);
    p->encoder->sample_aspect_ratio=av_buffersink_get_sample_aspect_ratio(p->sink);
    p->encoder->pix_fmt=AV_PIX_FMT_YUV420P;
    p->encoder->time_base=(AVRational){1,90000};
    p->encoder->framerate=p->frame_rate;
    p->encoder->bit_rate=cfg->bitrate;
    p->encoder->thread_count=cfg->encoder_threads;
    p->encoder->thread_type=FF_THREAD_SLICE;
    p->encoder->gop_size=cfg->fps*2; p->encoder->max_b_frames=0;
    p->encoder->color_range=frame->color_range;
    p->encoder->color_primaries=frame->color_primaries;
    p->encoder->color_trc=frame->color_trc; p->encoder->colorspace=frame->colorspace;
    ret=avformat_alloc_output_context2(&p->output,NULL,cfg->output_format,cfg->output);
    if (ret<0) return ret;
    if (p->output->oformat->flags&AVFMT_GLOBALHEADER) p->encoder->flags|=AV_CODEC_FLAG_GLOBAL_HEADER;
    AVDictionary *options=NULL;
    av_dict_set(&options,"preset","veryfast",0); av_dict_set(&options,"tune","zerolatency",0);
    ret=avcodec_open2(p->encoder,codec,&options); av_dict_free(&options);
    if (ret<0) return ret;
    p->video=avformat_new_stream(p->output,NULL);
    if (!p->video) return AVERROR(ENOMEM);
    ret=avcodec_parameters_from_context(p->video->codecpar,p->encoder);
    if (ret<0) return ret;
    p->video->time_base=p->encoder->time_base; p->video->avg_frame_rate=p->frame_rate;
    p->output->flags|=AVFMT_FLAG_FLUSH_PACKETS;
    p->output->max_delay=0; p->output->max_interleave_delta=0;
    p->output->interrupt_callback=(AVIOInterruptCB){interrupted,&p->write_interrupt};
    p->stage="output open/header";
    atomic_store(&p->write_interrupt.deadline,now_us()+IO_TIMEOUT_US);
    if (!strstr(cfg->output,"://") && !strchr(cfg->output,':')) {
        p->custom_output_fd=open(cfg->output,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0644);
        if (p->custom_output_fd<0) return AVERROR(errno);
        unsigned char *buffer=av_malloc(32768);
        if (!buffer) return AVERROR(ENOMEM);
        p->output->pb=avio_alloc_context(buffer,32768,1,p,NULL,local_write,local_seek);
        if (!p->output->pb) {av_free(buffer); return AVERROR(ENOMEM);}
        p->output->flags|=AVFMT_FLAG_CUSTOM_IO;
    } else {
        ret=avio_open2(&p->output->pb,cfg->output,AVIO_FLAG_WRITE,&p->output->interrupt_callback,NULL);
        if (ret<0) return ret;
    }
    ret=avformat_write_header(p->output,NULL);
    atomic_store(&p->write_interrupt.deadline,0);
    if (ret<0) return ret;
    p->header_written=1;
    yyjson_mut_doc *doc=new_event("pipeline_ready",p->job);
    json_int(doc,"input_width",frame->width); json_int(doc,"input_height",frame->height);
    json_int(doc,"output_width",p->encoder->width); json_int(doc,"output_height",p->encoder->height);
    json_str(doc,"encoder","libx264"); json_str(doc,"filter",chain);
    json_num(doc,"startup_ms",(now_us()-p->job->started_us)/1000.0);
    logger_submit(p->job->logger,NULL,doc); return 0;
}
static void sample_latencies(Latencies *m, const Stamp *s, int64_t encoded, int64_t begin,
                             int64_t interleaved, int64_t written, double slow) {
#define SAMPLE(name,us) metric_add(&m->name,(us)/1000.0,slow)
    SAMPLE(demux_read,s->read_us); SAMPLE(input_queue,s->submitted_us-s->ingress_us);
    SAMPLE(decode,s->decoded_us-s->submitted_us); SAMPLE(setup,s->filter_started_us-s->decoded_us);
    SAMPLE(filter,s->filtered_us-s->filter_started_us); SAMPLE(encode,encoded-s->encoder_submit_us);
    SAMPLE(mux,written-begin); SAMPLE(ingress_to_mux,written-s->ingress_us);
    SAMPLE(interleave_call,interleaved-begin); SAMPLE(avio_flush,written-interleaved);
    if(s->scheduled_us) {SAMPLE(scheduled_to_mux,written-s->scheduled_us);}
#undef SAMPLE
}
static void sample_groups(FrameGroups *g, const Stamp *s, int output_key, int first_i, double total, double slow) {
    if(s->picture_type==AV_PICTURE_TYPE_I) {
        metric_add(&g->i_frame,total,slow);
        metric_add(first_i ? &g->first_i : &g->recurring_i,total,slow);
    } else if(s->picture_type==AV_PICTURE_TYPE_P) metric_add(&g->p_frame,total,slow);
    else if(s->picture_type==AV_PICTURE_TYPE_B) metric_add(&g->b_frame,total,slow);
    if(s->decoded_key)metric_add(&g->decoded_key,total,slow);
    if(output_key)metric_add(&g->output_key,total,slow);
}
static int receive_encoded(Pipeline *p) {
    int ret; AVPacket *packet=av_packet_alloc();
    if (!packet) return AVERROR(ENOMEM);
    while ((ret=avcodec_receive_packet(p->encoder,packet))>=0) {
        int64_t encoded=now_us(); int slot=-1;
        for (int i=0;i<LEDGER_CAPACITY;i++) if(p->ledger[i].used && p->ledger[i].pts==packet->pts) {slot=i; break;}
        if (slot<0) {ret=AVERROR_INVALIDDATA; break;}
        Stamp stamp=p->ledger[slot].stamp;
        p->ledger[slot].used=0;
        int bytes=packet->size;
        int output_key=!!(packet->flags&AV_PKT_FLAG_KEY);
        int64_t pts_us=av_rescale_q(packet->pts,p->encoder->time_base,AV_TIME_BASE_Q);
        av_packet_rescale_ts(packet,p->encoder->time_base,p->video->time_base);
        packet->stream_index=p->video->index;
        if (!packet->duration) packet->duration=av_rescale_q(1,av_inv_q(p->frame_rate),p->video->time_base);
        p->stage="mux/write";
        int64_t mux_start=now_us();
        pthread_mutex_lock(&p->job->mutex);
        p->job->write_started_us=mux_start; strcpy(p->job->write_phase,"interleaved_write");
        pthread_mutex_unlock(&p->job->mutex);
        atomic_store(&p->write_interrupt.deadline,mux_start+IO_TIMEOUT_US);
        ret=av_interleaved_write_frame(p->output,packet);
        int64_t interleaved=now_us();
        pthread_mutex_lock(&p->job->mutex); strcpy(p->job->write_phase,"avio_flush"); pthread_mutex_unlock(&p->job->mutex);
        avio_flush(p->output->pb);
        int64_t written=now_us();
        pthread_mutex_lock(&p->job->mutex);
        p->job->write_started_us=0; *p->job->write_phase=0;
        pthread_mutex_unlock(&p->job->mutex);
        atomic_store(&p->write_interrupt.deadline,0);
        if (ret<0 || p->output->pb->error<0) {if(ret>=0)ret=p->output->pb->error; break;}
        double decode=(stamp.decoded_us-stamp.submitted_us)/1000.0;
        double filter=(stamp.filtered_us-stamp.filter_started_us)/1000.0;
        double encode=(encoded-stamp.encoder_submit_us)/1000.0;
        double mux=(written-mux_start)/1000.0;
        double total=(written-stamp.ingress_us)/1000.0;
        pthread_mutex_lock(&p->job->mutex);
        p->job->packets_out++; p->job->bytes_out=(uint64_t)p->output->pb->bytes_written;
        int first_output=!p->job->first_video_output_us;
        int first_output_key=output_key && !p->job->first_key_output_us;
        if(first_output)p->job->first_video_output_us=written;
        if(first_output_key)p->job->first_key_output_us=written;
        int first_i=!p->job->i_frames;
        double slow=p->job->config.slow_frame_ms;
        sample_latencies(&p->job->lifetime,&stamp,encoded,mux_start,interleaved,written,slow);
        sample_latencies(&p->job->window,&stamp,encoded,mux_start,interleaved,written,slow);
        sample_groups(&p->job->groups,&stamp,output_key,first_i,total,slow);
        sample_groups(&p->job->window_groups,&stamp,output_key,first_i,total,slow);
        if(stamp.picture_type==AV_PICTURE_TYPE_I) {
            sample_latencies(&p->job->i_lifetime,&stamp,encoded,mux_start,interleaved,written,slow);
            sample_latencies(&p->job->i_window,&stamp,encoded,mux_start,interleaved,written,slow);
            p->job->i_frames++; p->job->last_i_output_us=written;
        }
        p->job->last_output_us=written;
        uint64_t ordinal=p->job->packets_out;
        pthread_mutex_unlock(&p->job->mutex);
        int trace=p->job->config.frame_log && (!((ordinal-1)%p->job->config.frame_log_every) ||
           stamp.picture_type==AV_PICTURE_TYPE_I || output_key || total>=slow);
        if(!trace && !first_output && !first_output_key)continue;
        yyjson_mut_doc *doc=new_event("frame",p->job);
        json_int(doc,"input_sequence",stamp.sequence); json_int(doc,"pts_us",pts_us);
        json_int(doc,"encoded_bytes",bytes);
        char picture_type[2]={av_get_picture_type_char(stamp.picture_type),0};
        json_str(doc,"decoded_picture_type",picture_type);
        json_int(doc,"input_packet_key",stamp.input_key); json_int(doc,"decoded_key",stamp.decoded_key);
        json_int(doc,"output_key",output_key); json_int(doc,"first_i",first_i && stamp.picture_type==AV_PICTURE_TYPE_I);
        json_num(doc,"demux_read_ms",stamp.read_us/1000.0);
        json_num(doc,"input_queue_ms",(stamp.submitted_us-stamp.ingress_us)/1000.0);
        json_num(doc,"decode_ms",decode); json_num(doc,"filter_ms",filter);
        json_num(doc,"setup_or_dispatch_ms",(stamp.filter_started_us-stamp.decoded_us)/1000.0);
        json_num(doc,"encode_ms",encode); json_num(doc,"mux_ms",mux);
        json_num(doc,"interleave_call_ms",(interleaved-mux_start)/1000.0);
        json_num(doc,"avio_flush_ms",(written-interleaved)/1000.0);
        json_num(doc,"ingress_to_mux_ms",total);
        if (stamp.scheduled_us) json_num(doc,"scheduled_to_mux_ms",(written-stamp.scheduled_us)/1000.0);
        if(first_output || first_output_key) {
            yyjson_mut_doc *first=yyjson_mut_doc_mut_copy(doc,NULL);
            yyjson_mut_obj_remove_key(yyjson_mut_doc_get_root(first),"event");
            json_str(first,"event","first_frame");
            json_str(first,"boundary","complete_http_request_handler_to_mux_flush_not_peer_display");
            json_int(first,"first_video",first_output); json_int(first,"first_output_key",first_output_key);
            json_int(first,"prewarmed",p->job->prewarmed);
            json_int(first,"request_monotonic_us",p->job->started_us);
            json_int(first,"dispatch_monotonic_us",p->job->assigned_us);
            json_int(first,"worker_received_monotonic_us",p->job->worker_received_us);
            json_int(first,"packet_ingress_monotonic_us",stamp.ingress_us);
            json_int(first,"decoder_submit_monotonic_us",stamp.submitted_us);
            json_int(first,"decoded_monotonic_us",stamp.decoded_us);
            json_int(first,"filter_submit_monotonic_us",stamp.filter_started_us);
            json_int(first,"filtered_monotonic_us",stamp.filtered_us);
            json_int(first,"encoder_submit_monotonic_us",stamp.encoder_submit_us);
            json_int(first,"encoded_monotonic_us",encoded);
            json_int(first,"mux_begin_monotonic_us",mux_start);
            json_int(first,"mux_flush_done_monotonic_us",written);
            json_num(first,"request_to_dispatch_ms",(p->job->assigned_us-p->job->started_us)/1000.0);
            json_num(first,"dispatch_to_worker_ms",(p->job->worker_received_us-p->job->assigned_us)/1000.0);
            json_num(first,"worker_to_packet_ingress_ms",(stamp.ingress_us-p->job->worker_received_us)/1000.0);
            json_num(first,"request_to_mux_ms",(written-p->job->started_us)/1000.0);
            pthread_mutex_lock(&p->job->mutex);
            int checkpoint=first_output ? 0 : 1;
            p->job->first_chain[checkpoint]=yyjson_mut_doc_mut_copy(first,NULL);
            pthread_mutex_unlock(&p->job->mutex);
            logger_submit(p->job->logger,NULL,first);
        }
        if(trace)logger_submit(p->job->logger,p->job->trace,doc);
        else yyjson_mut_doc_free(doc);
    }
    av_packet_free(&packet);
    return ret==AVERROR(EAGAIN) || ret==AVERROR_EOF ? 0 : ret;
}
static int encode_frame(Pipeline *p, AVFrame *frame) {
    if (!frame->opaque_ref || frame->opaque_ref->size!=sizeof(Stamp)) return AVERROR_INVALIDDATA;
    Stamp stamp=*(Stamp *)frame->opaque_ref->data;
    stamp.filtered_us=now_us();
    AVRational filter_tb=av_buffersink_get_time_base(p->sink);
    frame->pts=av_rescale_q(frame->pts,filter_tb,p->encoder->time_base);
    frame->duration=av_rescale_q(frame->duration,filter_tb,p->encoder->time_base);
    frame->time_base=p->encoder->time_base;
    /* Input picture type is trace metadata, not a forced output GOP decision. */
    frame->pict_type=AV_PICTURE_TYPE_NONE;
    if (p->last_pts!=AV_NOPTS_VALUE && frame->pts<=p->last_pts) return AVERROR_INVALIDDATA;
    int slot=-1;
    for (int i=0;i<LEDGER_CAPACITY;i++) if(!p->ledger[i].used) {slot=i; break;}
    if (slot<0) return AVERROR(ENOBUFS);
    p->stage="encode";
    int ret;
    stamp.encoder_submit_us=now_us();
    while ((ret=avcodec_send_frame(p->encoder,frame))==AVERROR(EAGAIN)) {
        if ((ret=receive_encoded(p))<0) return ret;
    }
    if (ret<0) return ret;
    p->ledger[slot]=(Ledger){.used=1,.pts=frame->pts,.stamp=stamp}; p->last_pts=frame->pts;
    pthread_mutex_lock(&p->job->mutex); p->job->frames_filtered++; pthread_mutex_unlock(&p->job->mutex);
    return receive_encoded(p);
}
static int filter_output(Pipeline *p) {
    AVFrame *frame=av_frame_alloc();
    if (!frame) return AVERROR(ENOMEM);
    int ret;
    while ((ret=av_buffersink_get_frame(p->sink,frame))>=0) {
        ret=encode_frame(p,frame); av_frame_unref(frame);
        if (ret<0) break;
    }
    av_frame_free(&frame);
    return ret==AVERROR(EAGAIN) || ret==AVERROR_EOF ? 0 : ret;
}
static int process_frame(Pipeline *p, AVFrame *frame) {
    if (!frame->opaque_ref || frame->opaque_ref->size!=sizeof(Stamp)) return AVERROR_INVALIDDATA;
    /* Stamp is a private packet property propagated by AV_CODEC_FLAG_COPY_OPAQUE. */
    int ret=av_buffer_make_writable(&frame->opaque_ref);
    if (ret<0) return ret;
    Stamp *stamp=(Stamp *)frame->opaque_ref->data;
    stamp->decoded_us=now_us();
    stamp->picture_type=frame->pict_type;
    stamp->decoded_key=!!(frame->flags&AV_FRAME_FLAG_KEY);
    if (frame->best_effort_timestamp!=AV_NOPTS_VALUE) frame->pts=frame->best_effort_timestamp;
    if (frame->pts==AV_NOPTS_VALUE) frame->pts=p->synthetic_pts;
    p->synthetic_pts=frame->pts+av_rescale_q(1,av_inv_q(p->frame_rate),p->input_tb);
    pthread_mutex_lock(&p->job->mutex); p->job->frames_decoded++; pthread_mutex_unlock(&p->job->mutex);
    if (!p->graph && (ret=open_output(p,frame))<0) return ret;
    /* Do not silently reuse an encoder/filter configuration across format changes. */
    if (frame->width!=p->input_width || frame->height!=p->input_height || frame->format!=p->input_format) {
        p->stage="input format changed; pipeline rebuild required";
        return AVERROR(ENOSYS);
    }
    p->stage="filter";
    stamp->filter_started_us=now_us();
    ret=av_buffersrc_add_frame_flags(p->source,frame,AV_BUFFERSRC_FLAG_KEEP_REF);
    if (ret<0) return ret;
    return filter_output(p);
}
static int initialize_decoder(Pipeline *p) {
    p->stage="input open/probe";
    p->input=avformat_alloc_context();
    if (!p->input) return AVERROR(ENOMEM);
    p->input->interrupt_callback=(AVIOInterruptCB){interrupted,&p->read_interrupt};
    p->input->probesize=1024*1024; p->input->max_analyze_duration=1000000;
    atomic_store(&p->read_interrupt.deadline,now_us()+IO_TIMEOUT_US);
    int ret=avformat_open_input(&p->input,p->job->config.input,NULL,NULL);
    if (ret<0) return ret;
    ret=avformat_find_stream_info(p->input,NULL);
    atomic_store(&p->read_interrupt.deadline,0);
    if (ret<0) return ret;
    ret=av_find_best_stream(p->input,AVMEDIA_TYPE_VIDEO,-1,-1,NULL,0);
    if (ret<0) return ret;
    p->stream_index=ret;
    AVStream *stream=p->input->streams[p->stream_index];
    if (stream->codecpar->codec_id!=AV_CODEC_ID_H264 && stream->codecpar->codec_id!=AV_CODEC_ID_HEVC) return AVERROR(ENOSYS);
    p->input_tb=stream->time_base;
    p->frame_rate=av_guess_frame_rate(p->input,stream,NULL);
    if (p->frame_rate.num<=0 || p->frame_rate.den<=0) p->frame_rate=(AVRational){p->job->config.fps,1};
    p->stage="async decoder initialization";
    const AVCodec *codec=avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) return AVERROR_DECODER_NOT_FOUND;
    p->decoder=avcodec_alloc_context3(codec);
    if (!p->decoder) return AVERROR(ENOMEM);
    if ((ret=avcodec_parameters_to_context(p->decoder,stream->codecpar))<0) return ret;
    p->decoder->pkt_timebase=p->input_tb;
    p->decoder->thread_count=p->job->config.max_threads;
    p->decoder->thread_type=FF_THREAD_FRAME; p->decoder->flags|=AV_CODEC_FLAG_COPY_OPAQUE;
    if ((ret=avcodec_open2(p->decoder,codec,NULL))<0) return ret;
    if ((ret=avcodec_async_create(&p->async,&p->decoder,p->job->config.queue_capacity,wake_job,p->job))<0) return ret;
    AVCodecAsyncBudget *budget=NULL;
    ret=avcodec_async_budget_create(&budget,p->job->config.budget_slots,NULL);
    if (ret>=0) ret=avcodec_async_enable_auto(p->async,p->job->config.min_threads,budget);
    avcodec_async_budget_free(&budget);
    if (ret<0) return ret;
    refresh_auto(p);
    pthread_mutex_lock(&p->job->mutex); strcpy(p->job->state,"running"); pthread_mutex_unlock(&p->job->mutex);
    return event_log(p,"decoder_ready");
}
void *pipeline_main(void *opaque) {
    (void)crash_thread_init();
    Job *job=opaque;
    Pipeline p={.job=job,.custom_output_fd=-1,.last_pts=AV_NOPTS_VALUE,.stage="initialization"};
    p.read_interrupt.job=job; p.write_interrupt.job=job;
    atomic_init(&p.read_interrupt.deadline,0); atomic_init(&p.write_interrupt.deadline,0);
    pthread_mutex_init(&p.ring_mutex,NULL); pthread_cond_init(&p.ring_space,NULL);
    AVPacket *pending=NULL; AVFrame *frame=av_frame_alloc();
    int ret=frame ? initialize_decoder(&p) : AVERROR(ENOMEM);
    if (ret<0) goto cleanup;
    ret=pthread_create(&p.reader,NULL,reader_main,&p);
    if (ret) {ret=AVERROR(ret); goto cleanup;}
    p.reader_started=1;
    int eof_sent=0, decoder_eof=0;
    while (!atomic_load(&job->cancel) && !decoder_eof) {
        uint64_t wake;
        while (read(job->wake_fd,&wake,sizeof(wake))<0 && errno==EINTR) {}
        int progress=0;
        p.stage="async decode receive";
        for (int batch=0;batch<8;batch++) {
            ret=avcodec_async_receive_frame(p.async,frame);
            if (ret==AVERROR_EOF) {decoder_eof=1; ret=0; break;}
            if (ret==AVERROR(EAGAIN)) {ret=0; break;}
            if (ret<0) goto cleanup;
            ret=process_frame(&p,frame); av_frame_unref(frame);
            if (ret<0) goto cleanup;
            progress=1;
        }
        if (decoder_eof) break;
        int input_done=0, input_error=0;
        if (!pending && !eof_sent) pending=pop_input(&p,&input_done,&input_error);
        if (input_done && input_error<0) {p.stage="input read"; ret=input_error; goto cleanup;}
        if (pending || (input_done && !eof_sent)) {
            p.stage="async decode submit";
            if (pending) ((Stamp *)pending->opaque_ref->data)->submitted_us=now_us();
            ret=avcodec_async_send_packet(p.async,pending);
            if (!ret) {
                if (pending) av_packet_free(&pending); else eof_sent=1;
                progress=1;
            } else if (ret==AVERROR(EAGAIN)) ret=0;
            else goto cleanup;
        }
        if (now_us()>=p.next_stats_us) {
            refresh_auto(&p);
            p.next_stats_us=now_us()+1000000;
        }
        if (!progress) {
            struct pollfd fd={.fd=job->wake_fd,.events=POLLIN};
            int n;
            do {n=poll(&fd,1,1000);} while(n<0 && errno==EINTR && !atomic_load(&job->cancel));
            if (n<0) {ret=AVERROR(errno); goto cleanup;}
        }
    }
    if (!atomic_load(&job->cancel) && p.graph) {
        p.stage="filter drain";
        ret=av_buffersrc_add_frame_flags(p.source,NULL,0);
        if (ret<0 || (ret=filter_output(&p))<0) goto cleanup;
        p.stage="encoder drain";
        ret=avcodec_send_frame(p.encoder,NULL);
        if (ret<0 || (ret=receive_encoded(&p))<0) goto cleanup;
        for (int i=0;i<LEDGER_CAPACITY;i++) if(p.ledger[i].used) {ret=AVERROR_INVALIDDATA; goto cleanup;}
    } else if (!atomic_load(&job->cancel)) {p.stage="empty video"; ret=AVERROR(ENODATA);}
cleanup:
    {
        int stopped=atomic_load(&job->cancel);
        if(ret<0 && !stopped) {
            char message[AV_ERROR_MAX_STRING_SIZE]; av_strerror(ret,message,sizeof(message));
            yyjson_mut_doc *error_event=new_event("pipeline_error",job);
            json_str(error_event,"stage",p.stage); json_str(error_event,"error",message);
            logger_submit(job->logger,NULL,error_event);
        }
        /* Wake a blocked producer and interrupt network reads before joining. */
        if (ret<0) atomic_store(&job->cancel,1);
        pthread_mutex_lock(&p.ring_mutex); pthread_cond_broadcast(&p.ring_space); pthread_mutex_unlock(&p.ring_mutex);
        if (p.reader_started) pthread_join(p.reader,NULL);
        refresh_auto(&p);
        avcodec_async_free(&p.async);
        if (p.header_written) {
            int trailer_ret=av_write_trailer(p.output);
            if (p.output->pb) {avio_flush(p.output->pb); if (trailer_ret>=0 && p.output->pb->error<0) trailer_ret=p.output->pb->error;}
            if (ret>=0 && !stopped && trailer_ret<0) {p.stage="output trailer"; ret=trailer_ret;}
        }
        if (p.output) {
            if (p.output->pb) {pthread_mutex_lock(&job->mutex); job->bytes_out=(uint64_t)p.output->pb->bytes_written; pthread_mutex_unlock(&job->mutex);}
            if (p.output->flags&AVFMT_FLAG_CUSTOM_IO) {
                if(p.output->pb) {av_freep(&p.output->pb->buffer); avio_context_free(&p.output->pb);}
            } else if (p.output->pb) avio_closep(&p.output->pb);
            avformat_free_context(p.output);
        }
        if (p.custom_output_fd>=0) close(p.custom_output_fd);
        av_packet_free(&pending);
        for(int i=0;i<READER_CAPACITY;i++) av_packet_free(&p.ring[i]);
        av_frame_free(&frame); avcodec_free_context(&p.decoder); avcodec_free_context(&p.encoder);
        avfilter_graph_free(&p.graph); avformat_close_input(&p.input);
        pthread_cond_destroy(&p.ring_space); pthread_mutex_destroy(&p.ring_mutex);
        pthread_mutex_lock(&job->mutex);
        job->ended_us=now_us();
        strcpy(job->state,stopped ? "stopped" : ret<0 ? "failed" : "completed");
        if (ret<0 && !stopped) {
            char message[AV_ERROR_MAX_STRING_SIZE]; av_strerror(ret,message,sizeof(message));
            snprintf(job->error,sizeof(job->error),"%s: %s",p.stage,message);
        }
        pthread_mutex_unlock(&job->mutex);
        logger_submit(job->logger,NULL,job_report(job,1));
        logger_trace_finish(job->logger,job->trace);
        atomic_store(&job->done,1);
    }
    return NULL;
}
