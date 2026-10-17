#include "live.h"
#include "watermark.h"
#include "video_filter.h"
#include "branch_queue.h"
#include "crash.h"
#include "media_hooks.h"
#include "input_probe.h"
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
#include <libavformat/live_mux.h>
#include <libavcodec/bsf.h>
#include <libavutil/buffer.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libavutil/mem.h>
#include <libavutil/time.h>
#include <libavutil/imgutils.h>

#define READER_CAPACITY 8
#define LEDGER_CAPACITY 256
#define IO_TIMEOUT_US 10000000

typedef struct Stamp {
    int64_t sequence, read_us, ingress_us, submitted_us, decoded_us, filtered_us;
    int64_t encoder_submit_us, encoder_accepted_us, scheduled_us, filter_started_us;
    int picture_type, decoded_key, input_key, first_i;
    int intra_reason, timestamp_source, tracked_packet;
    int64_t frame_id, packet_pts, packet_dts, raw_pts, source_pts, source_us;
    uint64_t watermark_revision;
    uint64_t branch_charge;
    unsigned generation;
    uint64_t timestamp_wraps;
    int64_t branch_enqueued_us,branch_started_us;
} Stamp;
typedef struct Interrupt { Job *job; atomic_int_fast64_t deadline; } Interrupt;
typedef struct Ledger { int used; int64_t pts; Stamp stamp; } Ledger;
typedef struct InputItem { AVPacket *packet; AVCodecParameters *params; AVRational tb; int64_t demuxed_us; uint64_t retained_bytes; } InputItem;
typedef struct MuxTrace { Stamp stamp; int64_t encoded_us, pts_us, hook_us; int bytes, picture_type; } MuxTrace;
typedef struct AudioTrack {
    int input_index, rejected, have_clock;
    AVStream *output; AVBSFContext *bsf;
    int64_t last_dts, next_input_dts, previous_input_dts;
} AudioTrack;
typedef struct Branch Branch;
typedef struct Pipeline {
    Job *job;
    NakiMediaHooks hooks;
    const char *stage;
    AVFormatContext *input, *output;
    AVCodecContext *decoder, *encoder;
    AVCodecAsyncContext *async;
    AVFilterGraph *graph;
    AVFilterContext *source, *sink;
    NakiWatermarkCompositor *watermark_compositor;
    uint64_t watermark_revision;
    AVStream *video;
    int stream_index, reader_started, header_written, custom_output_fd;
    int input_width, input_height, input_format, seen_input_i;
    AVRational input_tb, frame_rate;
    int input_wrap_bits;
    int64_t next_frame_id;
    NakiTimeline timeline;
    NakiIntraState intra;
    Interrupt read_interrupt, write_interrupt;
    InputItem ring[READER_CAPACITY];
    int head, count, read_done, read_error;
    pthread_mutex_t ring_mutex;
    pthread_cond_t ring_space;
    pthread_t reader;
    Ledger ledger[LEDGER_CAPACITY];
    int64_t last_pts, synthetic_pts, next_stats_us;
    int have_output_display_pts;
    int64_t last_output_display_pts;
    int64_t encoder_api_us;
    int64_t first_encoder_pts, previous_key_pts;
    int seen_output_key;
    int live_mux, video_started;
    int64_t mux_max_dts_us;
    AudioTrack audio[16];
    int audio_count;
    InputItem deferred_audio[128];
    int deferred_count;
    uint64_t deferred_bytes;
    Branch *branches;
    int branch_count;
    int64_t io_timeout_us;
    enum AVColorSpace input_colorspace;
    enum AVColorRange input_range;
} Pipeline;
struct Branch {
    Pipeline pipeline;
    NakiBranchQueue queue;
    pthread_t thread;
    int initialized,started;
    const OutputConfig *spec;
};

static void free_input(InputItem *item) {
    av_packet_free(&item->packet); avcodec_parameters_free(&item->params);
}
static int handle_audio(Pipeline *p, const InputItem *item);
static int flush_deferred_audio(Pipeline *p);
static void trace_pts(yyjson_mut_doc *doc, const char *key, int64_t pts) {
    if(pts==AV_NOPTS_VALUE)yyjson_mut_obj_add_null(doc,yyjson_mut_doc_get_root(doc),key);
    else json_int(doc,key,pts);
}
static void packet_stamp_free(void *opaque, uint8_t *data) {
    Stamp *stamp=(Stamp *)data;
    if(stamp->tracked_packet)naki_policy_untrack(opaque,NAKI_TRACK_PACKET,(uint64_t)stamp->sequence);
    av_free(data);
}

static void checkpoint(Pipeline *p, int64_t *field) {
    pthread_mutex_lock(&p->job->mutex); if(!*field)*field=now_us(); pthread_mutex_unlock(&p->job->mutex);
}
static int encoder_send(Pipeline *p, const AVFrame *frame) {
    int64_t began=now_us(); int ret=avcodec_send_frame(p->encoder,frame);
    p->encoder_api_us+=now_us()-began; return ret;
}
static int encoder_receive(Pipeline *p, AVPacket *packet) {
    int64_t began=now_us(); int ret=avcodec_receive_packet(p->encoder,packet);
    p->encoder_api_us+=now_us()-began; return ret;
}

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
        if (packet->size<=0) {av_packet_free(&packet); continue;}
        if(job->config.output_count&&packet->size>8*1024*1024) {av_packet_free(&packet);ret=AVERROR(ENOBUFS);break;}
        int video=packet->stream_index==p->stream_index;
        InputItem item={.packet=packet,.tb=p->input_tb,.demuxed_us=finished};
        if (!video) {
            AVStream *stream=p->input->streams[packet->stream_index];
            int deliver=job->config.audio_copy && (job->config.output_count||strcmp(job->config.output_format,"mp4"));
            if (stream->codecpar->codec_type!=AVMEDIA_TYPE_AUDIO || (!deliver && !p->hooks.audio_packet)) {
                free_input(&item); continue;
            }
            if(stream->codecpar->extradata_size<0 || stream->codecpar->extradata_size>65536||stream->codecpar->ch_layout.nb_channels>64||stream->codecpar->nb_coded_side_data>32) {
                pthread_mutex_lock(&job->mutex);job->audio_unsupported++;pthread_mutex_unlock(&job->mutex);
                free_input(&item);continue;
            }
            uint64_t side_bytes=0;
            for(int k=0;k<stream->codecpar->nb_coded_side_data;k++)side_bytes+=stream->codecpar->coded_side_data[k].size;
            if(side_bytes>262144) {pthread_mutex_lock(&job->mutex);job->audio_unsupported++;pthread_mutex_unlock(&job->mutex);free_input(&item);continue;}
            item.params=avcodec_parameters_alloc(); item.tb=stream->time_base;
            if (!item.params) {free_input(&item); ret=AVERROR(ENOMEM); break;}
            ret=avcodec_parameters_copy(item.params,stream->codecpar);
            if(ret<0) {free_input(&item); break;}
        }
        int64_t due=0;
        if (job->config.pace) {
            int64_t ts=packet->dts!=AV_NOPTS_VALUE ? packet->dts : packet->pts;
            if (ts==AV_NOPTS_VALUE) ts=av_rescale_q(seq,av_inv_q(p->frame_rate),p->input_tb);
            ts=av_rescale_q(ts,item.tb,AV_TIME_BASE_Q);
            if (first_ts==AV_NOPTS_VALUE) {first_ts=ts; epoch=finished;}
            due=epoch+ts-first_ts;
            while (!atomic_load(&job->cancel) && now_us()<due) {
                int64_t left=due-now_us();
                if (left>0) av_usleep((unsigned)(left>10000 ? 10000 : left));
            }
        }
        if (atomic_load(&job->cancel)) {free_input(&item); break;}
        ret=av_packet_make_refcounted(packet);
        if (ret<0) {free_input(&item); break;}
        if (video) {
            av_buffer_unref(&packet->opaque_ref);
            if(job->config.policy.overload_levels) {
                uint8_t *data=av_mallocz(sizeof(Stamp));
                packet->opaque_ref=data ? av_buffer_create(data,sizeof(Stamp),packet_stamp_free,job,0) : NULL;
                if(!packet->opaque_ref)av_free(data);
            } else packet->opaque_ref=av_buffer_allocz(sizeof(Stamp));
            if (!packet->opaque_ref) {free_input(&item); ret=AVERROR(ENOMEM); break;}
            Stamp *stamp=(Stamp *)packet->opaque_ref->data;
            if(seq==INT64_MAX) {free_input(&item); ret=AVERROR(ERANGE); break;}
            stamp->sequence=seq++; stamp->read_us=finished-began;
            stamp->ingress_us=now_us(); stamp->scheduled_us=due;
            stamp->packet_pts=packet->pts; stamp->packet_dts=packet->dts;
            ret=naki_policy_track(job,NAKI_TRACK_PACKET,(uint64_t)stamp->sequence,stamp->ingress_us);
            if(ret<0) {free_input(&item); break;}
            stamp->tracked_packet=job->config.policy.overload_levels!=0;
            checkpoint(p,&job->first_packet_us);
            stamp->input_key=!!(packet->flags&AV_PKT_FLAG_KEY);
        }
        pthread_mutex_lock(&p->ring_mutex);
        while (p->count==READER_CAPACITY && !atomic_load(&job->cancel)) pthread_cond_wait(&p->ring_space,&p->ring_mutex);
        if (atomic_load(&job->cancel)) {pthread_mutex_unlock(&p->ring_mutex); free_input(&item); break;}
        p->ring[(p->head+p->count)%READER_CAPACITY]=item; p->count++;
        pthread_mutex_unlock(&p->ring_mutex);
        if(video) {pthread_mutex_lock(&job->mutex); job->packets_in++; pthread_mutex_unlock(&job->mutex);}
        wake_job(job);
    }
    pthread_mutex_lock(&p->ring_mutex);
    p->read_done=1; p->read_error=ret==AVERROR_EOF ? 0 : ret;
    pthread_mutex_unlock(&p->ring_mutex);
    wake_job(job); return NULL;
}
static InputItem pop_input(Pipeline *p, int *done, int *error) {
    InputItem item={0};
    pthread_mutex_lock(&p->ring_mutex);
    if (p->count) {
        item=p->ring[p->head]; p->ring[p->head]=(InputItem){0};
        p->head=(p->head+1)%READER_CAPACITY; p->count--;
        pthread_cond_signal(&p->ring_space);
    }
    *done=p->read_done && !p->count && !item.packet; *error=p->read_error;
    pthread_mutex_unlock(&p->ring_mutex);
    return item;
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
    int vvc=!strcmp(cfg->encoder,"libvvenc");
    const char *pixel_format=vvc ? "yuv420p10le" : "yuv420p";
    char chain[256];
    if(cfg->width)snprintf(chain,sizeof(chain),"scale=%d:%d:flags=bilinear,format=pix_fmts=%s",cfg->width,cfg->height,pixel_format);
    else snprintf(chain,sizeof(chain),"null,format=pix_fmts=%s",pixel_format);
    int ret;
    p->stage="filter initialization";
    ret=naki_video_filter_prepare(&p->graph,&p->source,&p->sink,frame,p->input_tb,
        cfg->width,cfg->height,vvc?AV_PIX_FMT_YUV420P10LE:AV_PIX_FMT_YUV420P);
    if(ret<0)return ret;
    p->input_width=frame->width; p->input_height=frame->height; p->input_format=frame->format;
    p->input_colorspace=frame->colorspace;p->input_range=frame->color_range;
    pthread_mutex_lock(&p->job->mutex);p->job->filter_generations=1;pthread_mutex_unlock(&p->job->mutex);

    p->stage="encoder initialization";
    checkpoint(p,&p->job->encoder_open_started_us);
    const AVCodec *codec=avcodec_find_encoder_by_name(cfg->encoder);
    if (!codec) return AVERROR_ENCODER_NOT_FOUND;
    p->encoder=avcodec_alloc_context3(codec);
    if (!p->encoder) return AVERROR(ENOMEM);
    p->encoder->width=av_buffersink_get_w(p->sink);
    p->encoder->height=av_buffersink_get_h(p->sink);
    pthread_mutex_lock(&p->job->mutex);p->job->fixed_output_width=p->encoder->width;p->job->fixed_output_height=p->encoder->height;pthread_mutex_unlock(&p->job->mutex);
    p->encoder->sample_aspect_ratio=av_buffersink_get_sample_aspect_ratio(p->sink);
    p->encoder->pix_fmt=vvc ? AV_PIX_FMT_YUV420P10LE : AV_PIX_FMT_YUV420P;
    /* The live VVenC fork preserves PTS at 90 kHz and generates DTS from the
       input timestamp FIFO. The reference profile retains upstream timing. */
    int pts_gop=vvc && strcmp(cfg->encoder_profile,"reference");
    p->encoder->time_base=vvc && !pts_gop ? av_inv_q(p->frame_rate) : (AVRational){1,90000};
    p->encoder->framerate=p->frame_rate;
    p->encoder->bit_rate=cfg->bitrate;
    p->encoder->thread_count=cfg->encoder_threads;
    p->encoder->thread_type=FF_THREAD_SLICE;
    p->encoder->gop_size=cfg->fps*2; p->encoder->max_b_frames=0;
    p->encoder->color_range=av_buffersink_get_color_range(p->sink);
    p->encoder->color_primaries=frame->color_primaries;
    p->encoder->color_trc=frame->color_trc; p->encoder->colorspace=av_buffersink_get_colorspace(p->sink);
    ret=avformat_alloc_output_context2(&p->output,NULL,cfg->output_format,cfg->output);
    if (ret<0) return ret;
    if(cfg->policy.timestamps==NAKI_TS_SOURCE_ALL)p->output->avoid_negative_ts=AVFMT_AVOID_NEG_TS_DISABLED;
    if (p->output->oformat->flags&AVFMT_GLOBALHEADER) p->encoder->flags|=AV_CODEC_FLAG_GLOBAL_HEADER;
    AVDictionary *options=NULL;
    av_dict_set(&options,"preset",cfg->encoder_preset,0);
    char vvenc_params[2048]="";
    int intra_frames=0;
    if (vvc && strcmp(cfg->encoder_profile,"reference")) {
        int temporal_filter=strcmp(cfg->encoder_profile,"live_3b_2s_no_mctf")!=0;
        intra_frames=(int)av_rescale_q(2,(AVRational){1,1},av_inv_q(p->frame_rate));
        if (intra_frames<4) return AVERROR(EINVAL);
        p->encoder->gop_size=intra_frames; p->encoder->max_b_frames=3;
        /* Display order I B B B P ... ; coding order P4 B2 B1 B3. The
           nominal frame period remains an RC hint; CTS schedules CRA.
           Frame-parallel=0 keeps intra-frame threading and avoids the extra
           stage queue. LookAhead stays enabled for single-pass bitrate RC.
           BIM=1 with this P-anchor layout + MCTF reproducibly aborts v1.14
           on the 320x180 10-bit regression; keep BIM off independently. */
        snprintf(vvenc_params,sizeof(vvenc_params),
            "GOPSize=4:IntraPeriod=%d:RefreshSec=2:POC0IDR=1:STA=0:MaxParallelFrames=0:"
            "MCTF=%d:BIM=0:MCTFFutureReference=0:"
            "Frame1=P 4 0 0 0 0 0 1.0 0 0 0 0 0 0 0 1 1 4 0 0:"
            "Frame2=B 2 1 0 0 0 0 1.0 0 0 0 0 0 0 1 1 1 2 1 1 -2:"
            "Frame3=B 1 2 0 0 0 0 1.0 0 0 0 0 0 0 2 1 1 1 1 1 -1:"
            "Frame4=B 3 2 0 0 0 0 1.0 0 0 0 0 0 0 2 1 1 1 1 1 -1",
            intra_frames,temporal_filter);
        av_dict_set(&options,"vvenc-params",vvenc_params,0);
        av_dict_set(&options,"live-pts-gop","1",0);
    }
    else if (vvc) av_dict_set(&options,"period","2",0);
    else av_dict_set(&options,"tune","zerolatency",0);
    if(cfg->policy.intra)av_dict_set(&options,"live-frame-intra","1",0);
    ret=avcodec_open2(p->encoder,codec,&options);
    if (ret>=0 && av_dict_count(options)) ret=AVERROR_OPTION_NOT_FOUND;
    av_dict_free(&options);
    if (ret<0) return ret;
    checkpoint(p,&p->job->encoder_open_done_us);
    p->video=avformat_new_stream(p->output,NULL);
    if (!p->video) return AVERROR(ENOMEM);
    ret=avcodec_parameters_from_context(p->video->codecpar,p->encoder);
    if (ret<0) return ret;
    p->video->time_base=p->encoder->time_base; p->video->avg_frame_rate=p->frame_rate;
    p->output->flags|=AVFMT_FLAG_FLUSH_PACKETS;
    p->output->max_delay=0; p->output->max_interleave_delta=0;
    p->output->interrupt_callback=(AVIOInterruptCB){interrupted,&p->write_interrupt};
    p->stage="output open/header";
    atomic_store(&p->write_interrupt.deadline,now_us()+(p->io_timeout_us?p->io_timeout_us:IO_TIMEOUT_US));
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
    if(strcmp(cfg->output_format,"mp4")) {
        ret=avformat_live_mux_init(p->output,(int64_t)cfg->mux_wait_ms*1000,256,4*1024*1024);
        if(ret<0)return ret;
        p->live_mux=1;
    }
    checkpoint(p,&p->job->output_header_done_us);
    yyjson_mut_doc *doc=new_event("pipeline_ready",p->job);
    json_int(doc,"input_width",frame->width); json_int(doc,"input_height",frame->height);
    json_int(doc,"output_width",p->encoder->width); json_int(doc,"output_height",p->encoder->height);
    json_str(doc,"encoder",cfg->encoder); json_str(doc,"encoder_preset",cfg->encoder_preset);
    json_str(doc,"encoder_profile",cfg->encoder_profile);
    if (vvc) {
        json_str(doc,"vvenc_params",vvenc_params);
        json_int(doc,"rate_control_intra_period_frames",intra_frames);
        json_str(doc,"keyframe_schedule",pts_gop ? "pts_grid" : "reference_frames");
        if (pts_gop) json_int(doc,"keyframe_period_ms",2000);
    }
    json_str(doc,"pixel_format",pixel_format); json_str(doc,"filter",chain);
    json_int(doc,"encoder_time_base_num",p->encoder->time_base.num);
    json_int(doc,"encoder_time_base_den",p->encoder->time_base.den);
    json_int(doc,"trace_ledger_capacity",LEDGER_CAPACITY);
    json_num(doc,"startup_ms",(now_us()-p->job->started_us)/1000.0);
    logger_submit(p->job->logger,NULL,doc); return 0;
}
static void sample_latencies(Latencies *m, const Stamp *s, int64_t encoded, int64_t begin,
                             int64_t interleaved, int64_t written, double slow) {
#define SAMPLE(name,us) metric_add(&m->name,(us)/1000.0,slow)
    SAMPLE(demux_read,s->read_us); SAMPLE(input_queue,s->submitted_us-s->ingress_us);
    SAMPLE(decode,s->decoded_us-s->submitted_us); SAMPLE(setup,s->filter_started_us-s->decoded_us);
    SAMPLE(branch_queue,s->branch_started_us-s->branch_enqueued_us);
    SAMPLE(filter,s->filtered_us-s->filter_started_us); SAMPLE(encode,encoded-s->encoder_submit_us);
    SAMPLE(encode_submit,s->encoder_accepted_us-s->encoder_submit_us); SAMPLE(encode_wait,encoded-s->encoder_accepted_us);
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
static int write_packet_now(Pipeline *p, AVPacket *packet, int64_t *interleaved, int64_t *written) {
    Job *job=p->job;
    int64_t dts_us=av_rescale_q(packet->dts,p->output->streams[packet->stream_index]->time_base,AV_TIME_BASE_Q);
    p->stage="mux/write";
    pthread_mutex_lock(&job->mutex);
    job->write_started_us=now_us(); strcpy(job->write_phase,"mux_write");
    pthread_mutex_unlock(&job->mutex);
    atomic_store(&p->write_interrupt.deadline,now_us()+(p->io_timeout_us?p->io_timeout_us:IO_TIMEOUT_US));
    int ret=av_write_frame(p->output,packet);
    /* Flush TS audio PES too: AVIO flushing alone cannot release those bytes. */
    if(ret>=0 && !strcmp(job->config.output_format,"mpegts")) ret=av_write_frame(p->output,NULL);
    *interleaved=now_us();
    pthread_mutex_lock(&job->mutex); strcpy(job->write_phase,"avio_flush"); pthread_mutex_unlock(&job->mutex);
    avio_flush(p->output->pb); *written=now_us();
    atomic_store(&p->write_interrupt.deadline,0);
    pthread_mutex_lock(&job->mutex); job->write_started_us=0; *job->write_phase=0; pthread_mutex_unlock(&job->mutex);
    if(ret>=0 && p->output->pb->error<0)ret=p->output->pb->error;
    if(ret>=0 && dts_us>p->mux_max_dts_us)p->mux_max_dts_us=dts_us;
    return ret<0 ? ret : 0;
}

static int write_video_packet(Pipeline *p, AVPacket *packet) {
    if(!packet->opaque_ref || packet->opaque_ref->size!=sizeof(MuxTrace)) return AVERROR_INVALIDDATA;
    MuxTrace *meta=(MuxTrace *)packet->opaque_ref->data;
    Stamp stamp=meta->stamp;
    int bytes=meta->bytes;
    int64_t encoded=meta->encoded_us;
    int output_key=!!(packet->flags&AV_PKT_FLAG_KEY);
    int64_t pts_us=meta->pts_us;
    int actual_picture_type=meta->picture_type;
    int64_t hook_us=meta->hook_us;
    int pts_gop=!p->job->config.policy.intra && !strcmp(p->job->config.encoder,"libvvenc") && strcmp(p->job->config.encoder_profile,"reference");
    int64_t key_target=0, key_interval=0;
    int had_output_key=p->seen_output_key;
    if (output_key && pts_gop) {
        int64_t origin=av_rescale_q(p->first_encoder_pts,p->encoder->time_base,AV_TIME_BASE_Q);
        key_target=origin+((pts_us-origin)/2000000)*2000000;
        key_interval=pts_us-p->previous_key_pts;
        p->previous_key_pts=pts_us; p->seen_output_key=1;
    }
    int64_t mux_start=encoded, write_begin=now_us(), interleaved=0, written=0;
    int ret=write_packet_now(p,packet,&interleaved,&written);
    if(ret<0)return ret;
    if(!naki_policy_untrack(p->job,NAKI_TRACK_FRAME,(uint64_t)stamp.frame_id))return AVERROR_INVALIDDATA;
    naki_branch_release(p->job,(uint64_t)stamp.frame_id,stamp.branch_charge);
    if(p->job->parent) {
        Job *parent=p->job->parent;pthread_mutex_lock(&parent->mutex);
        if(!parent->first_video_output_us)parent->first_video_output_us=written;
        if(output_key&&!parent->first_key_output_us)parent->first_key_output_us=written;
        pthread_mutex_unlock(&parent->mutex);
    }
    p->video_started=1;
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
    int first_i=stamp.first_i;
    double slow=p->job->config.slow_frame_ms;
    sample_latencies(&p->job->lifetime,&stamp,encoded,mux_start,interleaved,written,slow);
    sample_latencies(&p->job->window,&stamp,encoded,mux_start,interleaved,written,slow);
    sample_groups(&p->job->groups,&stamp,output_key,first_i,total,slow);
    sample_groups(&p->job->window_groups,&stamp,output_key,first_i,total,slow);
    if(output_key) {
        sample_latencies(&p->job->key_lifetime,&stamp,encoded,mux_start,interleaved,written,slow);
        sample_latencies(&p->job->key_window,&stamp,encoded,mux_start,interleaved,written,slow);
    }
    if(stamp.picture_type==AV_PICTURE_TYPE_I) {
        sample_latencies(&p->job->i_lifetime,&stamp,encoded,mux_start,interleaved,written,slow);
        sample_latencies(&p->job->i_window,&stamp,encoded,mux_start,interleaved,written,slow);
        p->job->i_frames++; p->job->last_i_output_us=written;
    }
    p->job->last_output_us=written;
    uint64_t ordinal=p->job->packets_out;
    uint64_t submitted=p->job->frames_filtered;
    pthread_mutex_unlock(&p->job->mutex);
    int trace=p->job->config.frame_log && (!((ordinal-1)%p->job->config.frame_log_every) ||
       stamp.picture_type==AV_PICTURE_TYPE_I || output_key || total>=slow);
    if(!trace && !first_output && !first_output_key)return 0;
    yyjson_mut_doc *doc=new_event("frame",p->job);
    json_int(doc,"frame_id",stamp.frame_id); json_int(doc,"generation",stamp.generation);
    json_int(doc,"source_time_base_num",p->input_tb.num); json_int(doc,"source_time_base_den",p->input_tb.den);
    trace_pts(doc,"source_packet_pts",stamp.packet_pts); trace_pts(doc,"source_packet_dts",stamp.packet_dts);
    trace_pts(doc,"decoded_raw_pts",stamp.raw_pts); trace_pts(doc,"source_pts",stamp.source_pts);
    json_str(doc,"timestamp_provenance",stamp.timestamp_source==1 ? "source_all" : stamp.timestamp_source==3 ? "legacy_synthetic" : "legacy_best_effort");
    json_int(doc,"intra_reason",stamp.intra_reason);
    char output_type[2]={av_get_picture_type_char(actual_picture_type),0}; json_str(doc,"output_picture_type",output_type);
    json_int(doc,"input_sequence",stamp.sequence); json_int(doc,"pts_us",pts_us);
    json_int(doc,"encoded_bytes",bytes);
    char picture_type[2]={av_get_picture_type_char(stamp.picture_type),0};
    json_str(doc,"decoded_picture_type",picture_type);
    json_int(doc,"input_packet_key",stamp.input_key); json_int(doc,"decoded_key",stamp.decoded_key);
    json_int(doc,"output_key",output_key); json_int(doc,"first_i",first_i && stamp.picture_type==AV_PICTURE_TYPE_I);
    json_int(doc,"watermark_revision",(int64_t)stamp.watermark_revision);
    json_num(doc,"branch_queue_ms",(stamp.branch_started_us-stamp.branch_enqueued_us)/1000.0);
    if (output_key && pts_gop) {
        json_int(doc,"keyframe_target_pts_us",key_target);
        json_num(doc,"keyframe_lateness_ms",(pts_us-key_target)/1000.0);
        if(had_output_key)json_num(doc,"previous_key_interval_ms",key_interval/1000.0);
        else yyjson_mut_obj_add_null(doc,yyjson_mut_doc_get_root(doc),"previous_key_interval_ms");
    }
    json_num(doc,"demux_read_ms",stamp.read_us/1000.0);
    json_num(doc,"input_queue_ms",(stamp.submitted_us-stamp.ingress_us)/1000.0);
    json_num(doc,"decode_ms",decode); json_num(doc,"filter_ms",filter);
    json_num(doc,"setup_or_dispatch_ms",(stamp.filter_started_us-stamp.decoded_us)/1000.0);
    json_num(doc,"encode_ms",encode); json_num(doc,"mux_ms",mux);
    json_num(doc,"media_hook_ms",hook_us/1000.0);
    json_num(doc,"encode_submit_ms",(stamp.encoder_accepted_us-stamp.encoder_submit_us)/1000.0);
    json_num(doc,"encode_wait_ms",(encoded-stamp.encoder_accepted_us)/1000.0);
    json_num(doc,"interleave_call_ms",(interleaved-mux_start)/1000.0);
    json_num(doc,"interleave_queue_ms",(write_begin-encoded)/1000.0);
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
        json_num(first,"encoder_api_wall_to_output_ms",p->encoder_api_us/1000.0);
        json_int(first,"encoder_submitted_frames_at_output",submitted);
        yyjson_mut_doc *status=job_snapshot(p->job);
        yyjson_mut_val *startup=yyjson_mut_obj_get(yyjson_mut_doc_get_root(status),"startup");
        if(startup) yyjson_mut_obj_add_val(first,yyjson_mut_doc_get_root(first),"startup",yyjson_mut_val_mut_copy(first,startup));
        yyjson_mut_doc_free(status);
        pthread_mutex_lock(&p->job->mutex);
        int checkpoint=first_output ? 0 : 1;
        p->job->first_chain[checkpoint]=yyjson_mut_doc_mut_copy(first,NULL);
        pthread_mutex_unlock(&p->job->mutex);
        logger_submit(p->job->logger,NULL,first);
    }
    if(trace)logger_submit(p->job->logger,p->job->trace,doc);
    else yyjson_mut_doc_free(doc);
    return 0;
}

static int drain_mux(Pipeline *p, int force) {
    if(!p->live_mux)return 0;
    AVPacket *packet=av_packet_alloc();
    if(!packet)return AVERROR(ENOMEM);
    int ret;
    while((ret=avformat_live_mux_receive(p->output,packet,force))>0) {
        if(packet->stream_index==p->video->index)ret=write_video_packet(p,packet);
        else {
            int64_t dts=av_rescale_q(packet->dts,p->output->streams[packet->stream_index]->time_base,AV_TIME_BASE_Q);
            /* A stalled audio track may return behind already-emitted media.
               Drop its stale packets explicitly rather than rewinding output. */
            if(dts<p->mux_max_dts_us) {
                pthread_mutex_lock(&p->job->mutex);p->job->audio_dropped++;pthread_mutex_unlock(&p->job->mutex);
                ret=0;
            } else {
                int64_t interleaved,written;
                ret=write_packet_now(p,packet,&interleaved,&written);
                if(ret>=0) {pthread_mutex_lock(&p->job->mutex);p->job->audio_packets_out++;pthread_mutex_unlock(&p->job->mutex);}
            }
        }
        av_packet_unref(packet);
        if(ret<0)break;
    }
    av_packet_free(&packet);
    AVLiveMuxStats stats;
    if(!avformat_live_mux_stats(p->output,&stats)) {
        pthread_mutex_lock(&p->job->mutex);
        p->job->mux_queued_packets=stats.queued_packets;p->job->mux_queued_bytes=stats.queued_bytes;
        p->job->mux_high_water=stats.high_water_packets;p->job->mux_timeouts=stats.timeout_releases;p->job->mux_pressure=stats.pressure_releases;
        p->job->mux_forced=stats.forced_releases;
        pthread_mutex_unlock(&p->job->mutex);
    }
    return ret<0 ? ret : 0;
}
static int submit_mux(Pipeline *p, AVPacket *packet) {
    if(!p->live_mux)return write_video_packet(p,packet);
    int ret;
    while((ret=avformat_live_mux_send(p->output,packet))==AVERROR(EAGAIN))
        if((ret=drain_mux(p,1))<0)return ret;
    if(ret<0)return ret;
    return drain_mux(p,0);
}
static int receive_encoded(Pipeline *p) {
    AVPacket *packet=av_packet_alloc();
    if(!packet)return AVERROR(ENOMEM);
    int ret;
    while((ret=encoder_receive(p,packet))>=0) {
        int64_t encoded_us=now_us(), hook_us=0;
        int slot=-1;
        for(int i=0;i<LEDGER_CAPACITY;i++)if(p->ledger[i].used && p->ledger[i].pts==packet->pts){slot=i;break;}
        if(slot<0){ret=AVERROR_INVALIDDATA;break;}
        if(p->job->config.policy.timestamps==NAKI_TS_SOURCE_ALL && !strcmp(p->job->config.output_format,"flv")) {
            int64_t pts=av_rescale_q(packet->pts,p->encoder->time_base,(AVRational){1,1000});
            int64_t dts=av_rescale_q(packet->dts,p->encoder->time_base,(AVRational){1,1000});
            if(packet->pts==AV_NOPTS_VALUE || packet->dts==AV_NOPTS_VALUE || pts<0 || dts<0 ||
               pts>INT32_MAX || dts>INT32_MAX || pts-dts>8388607 || pts-dts< -8388608) {
                yyjson_mut_doc *event=new_event("strict_timestamp_rejected",p->job);
                json_int(event,"encoder_pts",packet->pts);json_int(event,"encoder_dts",packet->dts);
                json_int(event,"encoder_time_base_num",p->encoder->time_base.num);
                json_int(event,"encoder_time_base_den",p->encoder->time_base.den);
                json_int(event,"pts_ms",pts);json_int(event,"dts_ms",dts);
                json_int(event,"first_encoder_pts",p->first_encoder_pts);
                logger_submit(p->job->logger,NULL,event);
                p->stage="strict FLV timestamp range requires explicit retiming";ret=AVERROR(ERANGE);break;
            }
        }
        Stamp *mapped=&p->ledger[slot].stamp;
        size_t picture_size=0; const uint8_t *picture_stats=av_packet_get_side_data(packet,AV_PKT_DATA_QUALITY_STATS,&picture_size);
        int output_type=picture_size>=5 ? picture_stats[4] : AV_PICTURE_TYPE_NONE;
        if(mapped->intra_reason) {
            size_t size=0; const uint8_t *stats=av_packet_get_side_data(packet,AV_PKT_DATA_QUALITY_STATS,&size);
            if(size<5 || stats[4]!=AV_PICTURE_TYPE_I) {p->stage="encoder did not confirm requested I picture"; ret=AVERROR_INVALIDDATA; break;}
            pthread_mutex_lock(&p->job->mutex);p->job->intra_confirmed++;pthread_mutex_unlock(&p->job->mutex);
        }
        if(p->hooks.video_packet) {
            size_t stats_size=0;
            const uint8_t *stats=av_packet_get_side_data(packet,AV_PKT_DATA_QUALITY_STATS,&stats_size);
            Stamp *source=&p->ledger[slot].stamp;
            NakiVideoEncodedEvent event={.codec_id=p->encoder->codec_id,.time_base=p->encoder->time_base,
                .encoded_us=encoded_us,.input_sequence=source->sequence,.input_picture_type=source->picture_type,
                .frame_id=source->frame_id,.source_pts=source->source_pts,.source_time_base=p->input_tb,.generation=source->generation,
                .output_picture_type=stats_size>=5 ? stats[4] : AV_PICTURE_TYPE_NONE,
                .output_key=!!(packet->flags&AV_PKT_FLAG_KEY)};
            int64_t pts=packet->pts,dts=packet->dts,duration=packet->duration;
            int flags=packet->flags,index=packet->stream_index,size=packet->size;
            p->stage="encoded video media hook";
            int64_t began=now_us();
            ret=p->hooks.video_packet(p->hooks.opaque,&event,packet);
            hook_us=now_us()-began;
            if(ret>0)ret=AVERROR(EINVAL);
            if(ret>=0 && (packet->pts!=pts || packet->dts!=dts || packet->duration!=duration ||
                packet->flags!=flags || packet->stream_index!=index || packet->size<=0 || !packet->data ||
                (int64_t)packet->size-size>NAKI_MEDIA_HOOK_MAX_ADDED_BYTES))ret=AVERROR(EINVAL);
            if(ret<0)break;
        }
        av_buffer_unref(&packet->opaque_ref);packet->opaque_ref=av_buffer_alloc(sizeof(MuxTrace));
        if(!packet->opaque_ref){ret=AVERROR(ENOMEM);break;}
        *(MuxTrace *)packet->opaque_ref->data=(MuxTrace){.stamp=p->ledger[slot].stamp,.encoded_us=encoded_us,.hook_us=hook_us,
            .pts_us=av_rescale_q(packet->pts,p->encoder->time_base,AV_TIME_BASE_Q),.bytes=packet->size,.picture_type=output_type};
        p->ledger[slot].used=0;
        av_packet_rescale_ts(packet,p->encoder->time_base,p->video->time_base);packet->stream_index=p->video->index;
        if(!packet->duration)packet->duration=av_rescale_q(1,av_inv_q(p->frame_rate),p->video->time_base);
        ret=submit_mux(p,packet);av_packet_unref(packet);
        if(ret<0)break;
    }
    av_packet_free(&packet);
    return ret==AVERROR(EAGAIN) || ret==AVERROR_EOF ? 0 : ret;
}

static void audio_count(Pipeline *p, int unsupported) {
    pthread_mutex_lock(&p->job->mutex);
    if(unsupported)p->job->audio_unsupported++; else p->job->audio_dropped++;
    pthread_mutex_unlock(&p->job->mutex);
}
static int prepare_audio_params(AVCodecParameters *par, const AVPacket *pkt) {
    if(par->codec_id!=AV_CODEC_ID_AAC && par->codec_id!=AV_CODEC_ID_MP3)return AVERROR(ENOSYS);
    if(par->extradata_size>65536 || par->extradata_size<0)return AVERROR(EINVAL);
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
    for(int i=0;i<p->audio_count;i++)if(p->audio[i].input_index==item->packet->stream_index)track=&p->audio[i];
    if(!track) {
        if(p->audio_count==16) {audio_count(p,1);return 0;}
        track=&p->audio[p->audio_count++];
        *track=(AudioTrack){.input_index=item->packet->stream_index,.last_dts=AV_NOPTS_VALUE};
    }
    if(track->rejected){audio_count(p,1);return 0;}
    if(!track->output) {
        int ret=prepare_audio_params(item->params,item->packet);
        if(ret==AVERROR(EAGAIN)){audio_count(p,0);return 0;}
        if(ret==AVERROR(ENOMEM))return ret;
        if(ret>=0 && item->params->codec_id==AV_CODEC_ID_AAC) {
            const AVBitStreamFilter *filter=av_bsf_get_by_name("aac_adtstoasc");
            ret=filter ? av_bsf_alloc(filter,&track->bsf) : AVERROR_BSF_NOT_FOUND;
            if(ret>=0)ret=avcodec_parameters_copy(track->bsf->par_in,item->params);
            if(ret>=0) {track->bsf->time_base_in=item->tb;ret=av_bsf_init(track->bsf);}
        }
        if(ret>=0)ret=avformat_live_add_audio(p->output,item->params,item->tb,&track->output);
        if(ret<0) {
            if(ret==AVERROR(ENOMEM))return ret;
            track->rejected=1;audio_count(p,1);
            yyjson_mut_doc *doc=new_event("audio_track_rejected",p->job);
            json_int(doc,"input_stream",track->input_index);json_str(doc,"codec",avcodec_get_name(item->params->codec_id));
            json_int(doc,"error_code",ret);logger_submit(p->job->logger,NULL,doc);
            return 0;
        }
        pthread_mutex_lock(&p->job->mutex);p->job->audio_tracks++;pthread_mutex_unlock(&p->job->mutex);
        yyjson_mut_doc *doc=new_event("audio_track_added",p->job);
        json_int(doc,"input_stream",track->input_index);json_int(doc,"output_stream",track->output->index);
        json_str(doc,"codec",avcodec_get_name(item->params->codec_id));json_int(doc,"sample_rate",item->params->sample_rate);
        json_str(doc,"action","registered_without_reopening_output");logger_submit(p->job->logger,NULL,doc);
    }
    if(item->params->codec_id!=track->output->codecpar->codec_id ||
       (item->params->extradata_size>0 && (item->params->extradata_size!=track->output->codecpar->extradata_size ||
        memcmp(item->params->extradata,track->output->codecpar->extradata,item->params->extradata_size)))) {
        track->rejected=1;audio_count(p,1);
        yyjson_mut_doc *doc=new_event("audio_track_changed_unsupported",p->job);
        json_int(doc,"input_stream",track->input_index);logger_submit(p->job->logger,NULL,doc);
        return 0;
    }
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
           av_rescale_q(dts,track->output->time_base,AV_TIME_BASE_Q)<p->mux_max_dts_us) {
            audio_count(p,0);ret=0;
        } else {
            track->last_dts=dts;ret=submit_mux(p,packet);
        }
    } else if(ret!=AVERROR(ENOMEM)) {audio_count(p,0);ret=0;}
    av_packet_free(&packet);
    return ret;
}
static int handle_audio(Pipeline *p, const InputItem *item) {
    if(p->video_started)return process_audio(p,item);
    NakiBranchItem accounting={.packet=item->packet,.parameters=item->params,.media_us=1,.ingress_us=0};
    int charged=naki_branch_item_charge(&accounting);if(charged<0)return charged;
    uint64_t cost=accounting.charged_bytes;
    if(cost>4*1024*1024){audio_count(p,0);return 0;}
    while(p->deferred_count && (p->deferred_count==128 || p->deferred_bytes+cost>4*1024*1024)) {
        InputItem *old=&p->deferred_audio[0];
        p->deferred_bytes-=old->retained_bytes;
        free_input(old);p->deferred_count--;
        memmove(p->deferred_audio,p->deferred_audio+1,p->deferred_count*sizeof(*old));audio_count(p,0);
    }
    InputItem copy={.packet=av_packet_clone(item->packet),.params=avcodec_parameters_alloc(),.tb=item->tb,.retained_bytes=cost};
    if(!copy.packet || !copy.params) {free_input(&copy);return AVERROR(ENOMEM);}
    int ret=avcodec_parameters_copy(copy.params,item->params);
    if(ret<0){free_input(&copy);return ret;}
    p->deferred_audio[p->deferred_count++]=copy;p->deferred_bytes+=cost;
    return 0;
}
static int flush_deferred_audio(Pipeline *p) {
    if(!p->video_started)return 0;
    while(p->deferred_count) {
        InputItem item=p->deferred_audio[0];
        p->deferred_count--;p->deferred_bytes-=item.retained_bytes;
        memmove(p->deferred_audio,p->deferred_audio+1,p->deferred_count*sizeof(item));
        int ret=process_audio(p,&item);free_input(&item);
        if(ret<0)return ret;
    }
    return 0;
}
static int encode_frame(Pipeline *p, AVFrame *frame) {
    if (!frame->opaque_ref || frame->opaque_ref->size!=sizeof(Stamp)) return AVERROR_INVALIDDATA;
    Stamp stamp=*(Stamp *)frame->opaque_ref->data;
    stamp.filtered_us=now_us();
    AVRational filter_tb=av_buffersink_get_time_base(p->sink);
    frame->pts=av_rescale_q(frame->pts,filter_tb,p->encoder->time_base);
    frame->duration=av_rescale_q(frame->duration,filter_tb,p->encoder->time_base);
    frame->time_base=p->encoder->time_base;
    frame->pict_type=stamp.intra_reason ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    if (p->last_pts!=AV_NOPTS_VALUE && frame->pts<=p->last_pts) {
        p->stage="non-monotonic encoder PTS"; return AVERROR_INVALIDDATA;
    }
    int strict=p->job->config.policy.timestamps==NAKI_TS_SOURCE_ALL;
    int64_t output_display_pts=0;
    if(strict) {
        /* Check the retained display sequence, before encoder B-picture reorder.
         * Packet PTS in encode/write order must not be required to increase. */
        output_display_pts=av_rescale_q(frame->pts,p->encoder->time_base,p->video->time_base);
        if(output_display_pts==AV_NOPTS_VALUE || output_display_pts==INT64_MAX ||
           (p->have_output_display_pts && output_display_pts<=p->last_output_display_pts)) {
            p->stage="strict output PTS precision collision"; return AVERROR_INVALIDDATA;
        }
    }
    if (p->last_pts==AV_NOPTS_VALUE) p->first_encoder_pts=frame->pts;
    int slot=-1;
    for (int i=0;i<LEDGER_CAPACITY;i++) if(!p->ledger[i].used) {slot=i; break;}
    if (slot<0) return AVERROR(ENOBUFS);
    p->stage="encode";
    int ret;
    stamp.encoder_submit_us=now_us();
    checkpoint(p,&p->job->first_encoder_submit_us);
    while ((ret=encoder_send(p,frame))==AVERROR(EAGAIN)) {
        if ((ret=receive_encoded(p))<0) return ret;
    }
    if (ret<0) return ret;
    stamp.encoder_accepted_us=now_us();
    if(strict) {p->last_output_display_pts=output_display_pts;p->have_output_display_pts=1;}
    naki_intra_accept(&p->job->config.policy,&p->intra,stamp.source_us,stamp.intra_reason);
    if(stamp.intra_reason) {pthread_mutex_lock(&p->job->mutex);p->job->intra_requested++;pthread_mutex_unlock(&p->job->mutex);}
    p->ledger[slot]=(Ledger){.used=1,.pts=frame->pts,.stamp=stamp}; p->last_pts=frame->pts;
    pthread_mutex_lock(&p->job->mutex); p->job->frames_filtered++; pthread_mutex_unlock(&p->job->mutex);
    return receive_encoded(p);
}
static int filter_output(Pipeline *p) {
    AVFrame *frame=av_frame_alloc();
    if (!frame) return AVERROR(ENOMEM);
    int ret;
    while ((ret=av_buffersink_get_frame(p->sink,frame))>=0) {
        frame->sample_aspect_ratio=p->encoder->sample_aspect_ratio;
        if(p->job->filter_generations>1) {frame->colorspace=p->encoder->colorspace;frame->color_range=p->encoder->color_range;}
        if(!frame->opaque_ref||frame->opaque_ref->size!=sizeof(Stamp)) {ret=AVERROR_INVALIDDATA;break;}
        uint64_t watermark_revision=0;
        p->stage="watermark composition";
        ret=naki_watermark_frame(p->job->watermark,&p->watermark_compositor,frame,
            av_buffersink_get_time_base(p->sink),((Stamp *)frame->opaque_ref->data)->source_us,&watermark_revision);
        if(ret>=0)ret=av_buffer_make_writable(&frame->opaque_ref);
        if(ret>=0) {
            ((Stamp *)frame->opaque_ref->data)->watermark_revision=watermark_revision;
            if(p->watermark_revision!=watermark_revision) {
                yyjson_mut_doc *event=new_event("watermark_applied",p->job);
                naki_watermark_status(p->job->watermark,event);logger_submit(p->job->logger,NULL,event);
                p->watermark_revision=watermark_revision;
            }
        }
        if(ret>=0)ret=encode_frame(p,frame);
        av_frame_unref(frame);
        /* Only EAGAIN/EOF from the main sink below are normal draining.
         * An inner compositor/encoder error must never silently drop a frame. */
        if(ret<0) {av_frame_free(&frame);return ret;}
    }
    av_frame_free(&frame);
    return ret==AVERROR(EAGAIN) || ret==AVERROR_EOF ? 0 : ret;
}
static int prepare_decoded_frame(Pipeline *p, AVFrame *frame) {
    if (!frame->opaque_ref || frame->opaque_ref->size!=sizeof(Stamp)) return AVERROR_INVALIDDATA;
    naki_policy_untrack(p->job,NAKI_TRACK_PACKET,(uint64_t)((Stamp *)frame->opaque_ref->data)->sequence);
    /* Stamp is a private packet property propagated by AV_CODEC_FLAG_COPY_OPAQUE. */
    int ret=av_buffer_make_writable(&frame->opaque_ref);
    if (ret<0) return ret;
    Stamp *stamp=(Stamp *)frame->opaque_ref->data;
    stamp->decoded_us=now_us();
    checkpoint(p,&p->job->first_decoded_us);
    stamp->picture_type=frame->pict_type;
    stamp->first_i=frame->pict_type==AV_PICTURE_TYPE_I && !p->seen_input_i;
    if (frame->pict_type==AV_PICTURE_TYPE_I) p->seen_input_i=1;
    stamp->decoded_key=!!(frame->flags&AV_FRAME_FLAG_KEY);
    if(p->next_frame_id==INT64_MAX)return AVERROR(ERANGE);
    stamp->frame_id=++p->next_frame_id; stamp->raw_pts=frame->pts;
    if(p->job->config.policy.timestamps==NAKI_TS_SOURCE_ALL) {
        p->stage="strict source PTS";
        if((ret=naki_timeline_resolve(&p->timeline,frame->pts,p->input_wrap_bits,&frame->pts))<0)return ret;
        stamp->timestamp_source=1;
    } else {
        if(frame->best_effort_timestamp!=AV_NOPTS_VALUE)frame->pts=frame->best_effort_timestamp;
        if(frame->pts==AV_NOPTS_VALUE) {frame->pts=p->synthetic_pts; stamp->timestamp_source=3;}
        else stamp->timestamp_source=2;
        p->synthetic_pts=frame->pts+av_rescale_q(1,av_inv_q(p->frame_rate),p->input_tb);
    }
    stamp->source_pts=frame->pts;
    stamp->source_us=av_rescale_q(frame->pts,p->input_tb,AV_TIME_BASE_Q);
    stamp->timestamp_wraps=p->timeline.wraps;
    if(stamp->source_us==INT64_MIN || stamp->source_us==INT64_MAX)return AVERROR(ERANGE);
    return 0;
}
static int rebuild_input_filter(Pipeline *p,const AVFrame *frame) {
    if(frame->width==p->input_width&&frame->height==p->input_height&&frame->format==p->input_format&&
       frame->colorspace==p->input_colorspace&&frame->color_range==p->input_range&&
       frame->color_primaries==p->encoder->color_primaries&&frame->color_trc==p->encoder->color_trc)return 0;
    p->stage="dynamic input validation";
    int ret=naki_watermark_video(p->job->watermark,frame,p->encoder->width,p->encoder->height);if(ret<0)return ret;
    if(frame->color_primaries!=p->encoder->color_primaries||frame->color_trc!=p->encoder->color_trc) {
        p->stage="input primaries/transfer changed; tone mapping unsupported";return AVERROR(ENOSYS);
    }
    if(frame->format!=AV_PIX_FMT_YUV420P&&frame->format!=AV_PIX_FMT_YUV420P10LE)return AVERROR(ENOSYS);
    AVFilterGraph *graph=NULL;AVFilterContext *source=NULL,*sink=NULL;
    p->stage="dynamic filter prepare";
    ret=naki_video_filter_prepare_fixed(&graph,&source,&sink,frame,p->input_tb,p->encoder->width,p->encoder->height,
        p->encoder->pix_fmt,p->encoder->colorspace,p->encoder->color_range);
    if(ret<0)return ret;
    p->stage="old filter generation drain";
    ret=av_buffersrc_add_frame_flags(p->source,NULL,0);if(ret>=0)ret=filter_output(p);
    if(ret<0) {avfilter_graph_free(&graph);return ret;}
    avfilter_graph_free(&p->graph);p->graph=graph;p->source=source;p->sink=sink;
    p->input_width=frame->width;p->input_height=frame->height;p->input_format=frame->format;
    p->input_colorspace=frame->colorspace;p->input_range=frame->color_range;
    pthread_mutex_lock(&p->job->mutex);uint64_t generation=++p->job->filter_generations;pthread_mutex_unlock(&p->job->mutex);
    yyjson_mut_doc *event=new_event("input_filter_rebuilt",p->job);json_int(event,"generation",generation);
    json_int(event,"input_width",frame->width);json_int(event,"input_height",frame->height);json_int(event,"input_format",frame->format);
    json_int(event,"output_width",p->encoder->width);json_int(event,"output_height",p->encoder->height);
    json_str(event,"encoder_action","unchanged");logger_submit(p->job->logger,NULL,event);return 0;
}
static int process_frame(Pipeline *p,AVFrame *frame) {
    int ret=p->job->parent?av_buffer_make_writable(&frame->opaque_ref):prepare_decoded_frame(p,frame);
    if(ret<0)return ret;
    Stamp *stamp=(Stamp *)frame->opaque_ref->data;
    if(p->job->parent) {
        stamp->branch_started_us=now_us();
        pthread_mutex_lock(&p->job->mutex);
        if(!p->job->first_decoded_us)p->job->first_decoded_us=stamp->decoded_us;
        if(!p->job->first_packet_us)p->job->first_packet_us=stamp->ingress_us;
        pthread_mutex_unlock(&p->job->mutex);
    }
    if((ret=naki_policy_track(p->job,NAKI_TRACK_FRAME,(uint64_t)stamp->frame_id,stamp->ingress_us))<0)return ret;
    if(p->graph&&(ret=rebuild_input_filter(p,frame))<0)return ret;
    stamp->intra_reason=naki_intra_decide(&p->job->config.policy,&p->intra,stamp->source_us,stamp->picture_type==AV_PICTURE_TYPE_I);
    int protect=stamp->intra_reason || stamp->picture_type==AV_PICTURE_TYPE_I || stamp->frame_id==1;
    pthread_mutex_lock(&p->job->mutex);
    p->job->frames_decoded++; p->job->frame_id_last=(uint64_t)stamp->frame_id; p->job->timestamp_wraps=stamp->timestamp_wraps;
    int level=p->job->overload.level;
    pthread_mutex_unlock(&p->job->mutex);
    int keep=naki_frame_keep(&p->job->config.policy,level,(uint64_t)stamp->frame_id,protect);
    if(keep<0)return keep;
    pthread_mutex_lock(&p->job->mutex);
    if(protect)p->job->frames_protected++;
    if(keep)p->job->frames_selected++; else p->job->frames_dropped_overload++;
    pthread_mutex_unlock(&p->job->mutex);
    if(!keep) {
        if(!naki_policy_untrack(p->job,NAKI_TRACK_FRAME,(uint64_t)stamp->frame_id))return AVERROR_INVALIDDATA;
        naki_branch_release(p->job,(uint64_t)stamp->frame_id,stamp->branch_charge);
        if(p->job->config.frame_log && (stamp->frame_id-1)%p->job->config.frame_log_every==0) {
            yyjson_mut_doc *event=new_event("frame_drop",p->job);
            json_int(event,"frame_id",stamp->frame_id); json_int(event,"input_sequence",stamp->sequence);
            json_int(event,"source_pts",stamp->source_pts); json_int(event,"overload_level",level);
            json_str(event,"reason","overload"); logger_submit(p->job->logger,p->job->trace,event);
        }
        return 0;
    }
    if(!p->graph) {
        p->stage="watermark video validation";
        if((ret=naki_watermark_video(p->job->watermark,frame,p->job->config.width?p->job->config.width:frame->width,
              p->job->config.height?p->job->config.height:frame->height))<0)return ret;
        if((ret=open_output(p,frame))<0)return ret;
    }
    stamp->generation=(unsigned)p->job->filter_generations;
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
    if(p->job->config.policy.timestamps==NAKI_TS_SOURCE_ALL)p->input->flags|=AVFMT_FLAG_NOFILLIN;
    p->input->interrupt_callback=(AVIOInterruptCB){interrupted,&p->read_interrupt};
    p->input->probesize=1024*1024; p->input->max_analyze_duration=1000000;
    p->input->max_streams=64;
    atomic_store(&p->read_interrupt.deadline,now_us()+IO_TIMEOUT_US);
    checkpoint(p,&p->job->input_open_started_us);
    int ret=avformat_open_input(&p->input,p->job->config.input,NULL,NULL);
    if (ret<0) return ret;
    checkpoint(p,&p->job->input_open_done_us);
    ret=naki_probe_video(p->input);
    atomic_store(&p->read_interrupt.deadline,0);
    if (ret<0) return ret;
    p->stream_index=ret;
    checkpoint(p,&p->job->input_probe_done_us);
    if((ret=avformat_live_input_probe_limit(p->input,100000))<0)return ret;
    AVStream *stream=p->input->streams[p->stream_index];
    if (stream->codecpar->codec_id!=AV_CODEC_ID_H264 && stream->codecpar->codec_id!=AV_CODEC_ID_HEVC) return AVERROR(ENOSYS);
    p->input_tb=stream->time_base;
    p->input_wrap_bits=stream->pts_wrap_bits;
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
    if(p->job->config.output_count)p->decoder->max_pixels=4096LL*2160;
    if ((ret=avcodec_open2(p->decoder,codec,NULL))<0) return ret;
    if ((ret=avcodec_async_create(&p->async,&p->decoder,p->job->config.queue_capacity,wake_job,p->job))<0) return ret;
    AVCodecAsyncBudget *budget=NULL;
    ret=avcodec_async_budget_create(&budget,p->job->config.budget_slots,NULL);
    if (ret>=0) ret=avcodec_async_enable_auto(p->async,p->job->config.min_threads,budget);
    avcodec_async_budget_free(&budget);
    if (ret<0) return ret;
    refresh_auto(p);
    checkpoint(p,&p->job->decoder_ready_us);
    pthread_mutex_lock(&p->job->mutex); strcpy(p->job->state,"running"); pthread_mutex_unlock(&p->job->mutex);
    return event_log(p,"decoder_ready");
}
static void branch_queue_stats(Branch *b) {
    pthread_mutex_lock(&b->queue.mutex);
    unsigned count=b->queue.count,high=b->queue.high_water;uint64_t bytes=b->queue.bytes,media=b->queue.media_us;
    pthread_mutex_unlock(&b->queue.mutex);Job *j=b->pipeline.job;
    pthread_mutex_lock(&j->mutex);j->branch_queued=count;j->branch_queue_bytes=bytes;j->branch_queue_media_us=media;j->branch_queue_high=high;pthread_mutex_unlock(&j->mutex);
}
static void fail_branch(Branch *b,int ret,const char *stage) {
    Job *j=b->pipeline.job;if(atomic_load(&j->done))return;
    pthread_mutex_lock(&j->mutex);
    if(!j->branch_failed) {
        char error[AV_ERROR_MAX_STRING_SIZE];av_strerror(ret,error,sizeof(error));
        j->branch_failed=1;snprintf(j->error,sizeof(j->error),"%s: %s",stage,error);strcpy(j->state,"failing");
    }
    pthread_mutex_unlock(&j->mutex);request_stop(j);naki_branch_queue_abort(&b->queue);branch_queue_stats(b);
}
static int check_branches(Pipeline *p,int finishing) {
    int active=0;int64_t now=now_us();
    for(int i=0;i<p->branch_count;i++) {
        Branch *b=&p->branches[i];Job *j=b->pipeline.job;
        if(!b->started||atomic_load(&j->done))continue;
        if(atomic_load(&p->job->cancel)) {request_stop(j);naki_branch_queue_abort(&b->queue);continue;}
        pthread_mutex_lock(&j->mutex);
        int64_t age=naki_inflight_age(&j->branch_pending,now),write=j->write_started_us?now-j->write_started_us:0;
        int failed=j->branch_failed;pthread_mutex_unlock(&j->mutex);
        if(!failed&&(age>(int64_t)b->spec->max_lag_ms*1000||write>(int64_t)b->spec->io_timeout_ms*1000||
           naki_branch_queue_check(&b->queue,now)==AVERROR(ETIMEDOUT)))fail_branch(b,AVERROR(ETIMEDOUT),"branch ingress/write deadline");
        if(!atomic_load(&j->cancel))active++;
        branch_queue_stats(b);
    }
    return !finishing&&!active?AVERROR(EPIPE):0;
}
static int drain_output(Pipeline *p) {
    if(!p->graph) {p->stage="empty video";return AVERROR(ENODATA);}
    p->stage="filter drain";int ret=av_buffersrc_add_frame_flags(p->source,NULL,0);
    if(ret<0||(ret=filter_output(p))<0)return ret;
    p->stage="encoder drain";ret=encoder_send(p,NULL);
    if(ret<0||(ret=receive_encoded(p))<0)return ret;
    if((ret=flush_deferred_audio(p))<0||(ret=drain_mux(p,1))<0)return ret;
    for(int i=0;i<LEDGER_CAPACITY;i++)if(p->ledger[i].used)return AVERROR_INVALIDDATA;
    return 0;
}
static void *branch_main(void *opaque) {
    (void)crash_thread_init();Branch *b=opaque;Pipeline *p=&b->pipeline;Job *j=p->job;
    NakiMediaJobInfo info={.job_id=j->id,.request_id=j->request_id,.config=&j->config,.request_us=j->started_us,.output_id=j->output_id};
    p->stage="branch media hook initialization";int ret=naki_media_hooks_open(&info,&p->hooks);
    if(ret>0||(ret>=0&&p->hooks.api_version!=NAKI_MEDIA_HOOK_API))ret=AVERROR(EINVAL);
    if(ret>=0) {pthread_mutex_lock(&j->mutex);strcpy(j->state,"running");pthread_mutex_unlock(&j->mutex);}
    while(ret>=0&&!atomic_load(&j->cancel)) {
        NakiBranchItem item={0};ret=naki_branch_queue_pop(&b->queue,&item,now_us()+20000);branch_queue_stats(b);
        if(ret==AVERROR_EOF) {ret=drain_output(p);break;}
        if(ret==AVERROR(ETIMEDOUT)||ret==AVERROR(EAGAIN)) {ret=p->live_mux?drain_mux(p,0):0;continue;}
        if(ret<0)break;
        if(item.frame)ret=process_frame(p,item.frame);
        else if(j->config.audio_copy&&strcmp(j->config.output_format,"mp4")) {
            InputItem audio={.packet=item.packet,.params=item.parameters,.tb=item.time_base,.demuxed_us=item.ingress_us};
            ret=handle_audio(p,&audio);
        }
        naki_branch_item_free(&item);
        if(ret>=0&&p->live_mux) {ret=flush_deferred_audio(p);if(ret>=0)ret=drain_mux(p,0);}
    }
    if(!atomic_load(&j->parent->cancel)&&atomic_load(&j->cancel)&&!j->branch_failed)fail_branch(b,AVERROR(ETIMEDOUT),"branch overload cancellation");
    if(ret<0&&!atomic_load(&j->parent->cancel))fail_branch(b,ret,p->stage);
    naki_branch_queue_abort(&b->queue);naki_watermark_stop(j->watermark);
    if(p->header_written) {
        atomic_store(&p->write_interrupt.deadline,now_us()+p->io_timeout_us);
        int trailer=av_write_trailer(p->output);if(p->output->pb) {avio_flush(p->output->pb);if(trailer>=0&&p->output->pb->error<0)trailer=p->output->pb->error;}
        if(ret>=0&&trailer<0&&!atomic_load(&j->parent->cancel)) {ret=trailer;fail_branch(b,ret,"output trailer");}
    }
    if(p->hooks.close) {NakiMediaEnd end={.cancelled=atomic_load(&j->parent->cancel),.error_code=ret<0?ret:0,.stage=p->stage,.output_failures=atomic_load(&j->branch_failed)};p->hooks.close(p->hooks.opaque,&end);}
    if(p->output) {
        if(p->output->pb) {
            pthread_mutex_lock(&j->mutex);j->bytes_out=p->output->pb->bytes_written;pthread_mutex_unlock(&j->mutex);
            if(p->output->flags&AVFMT_FLAG_CUSTOM_IO) {av_freep(&p->output->pb->buffer);avio_context_free(&p->output->pb);}
            else avio_closep(&p->output->pb);
        }
        avformat_free_context(p->output);
    }
    if(p->custom_output_fd>=0)close(p->custom_output_fd);
    for(int i=0;i<p->deferred_count;i++)free_input(&p->deferred_audio[i]);
    for(int i=0;i<p->audio_count;i++)av_bsf_free(&p->audio[i].bsf);
    avcodec_free_context(&p->encoder);avfilter_graph_free(&p->graph);naki_watermark_compositor_free(&p->watermark_compositor);
    pthread_mutex_lock(&j->mutex);j->policy_closing=1;j->ended_us=now_us();
    memset(&j->policy_inflight,0,sizeof(j->policy_inflight));memset(&j->branch_pending,0,sizeof(j->branch_pending));
    j->branch_inflight_bytes=j->branch_inflight_frames=0;j->branch_queued=j->branch_queue_bytes=j->branch_queue_media_us=0;
    strcpy(j->state,j->branch_failed?"failed":atomic_load(&j->parent->cancel)?"stopped":"completed");
    pthread_mutex_unlock(&j->mutex);logger_submit(j->logger,NULL,job_report(j,1));atomic_store(&j->done,1);wake_job(j->parent);return NULL;
}
static int start_branches(Pipeline *p) {
    p->branch_count=p->job->config.output_count;p->branches=calloc(p->branch_count,sizeof(*p->branches));if(!p->branches)return AVERROR(ENOMEM);
    for(int i=0;i<p->branch_count;i++)p->branches[i].pipeline.job=p->job->outputs[i];
    for(int i=0;i<p->branch_count;i++) {
        Branch *b=&p->branches[i];Job *j=p->job->outputs[i];b->spec=&p->job->config.outputs[i];
        b->pipeline=(Pipeline){.job=j,.input_tb=p->input_tb,.frame_rate=p->frame_rate,.input_wrap_bits=p->input_wrap_bits,
            .custom_output_fd=-1,.last_pts=AV_NOPTS_VALUE,.mux_max_dts_us=INT64_MIN,.stage="branch initialization",.io_timeout_us=(int64_t)b->spec->io_timeout_ms*1000};
        b->pipeline.write_interrupt.job=j;atomic_init(&b->pipeline.write_interrupt.deadline,0);
        pthread_mutex_lock(&j->mutex);j->input_open_started_us=p->job->input_open_started_us;j->input_open_done_us=p->job->input_open_done_us;
        j->input_probe_done_us=p->job->input_probe_done_us;j->decoder_ready_us=p->job->decoder_ready_us;pthread_mutex_unlock(&j->mutex);
        int ret=naki_branch_queue_init(&b->queue,b->spec->queue_items,b->spec->queue_bytes,(int64_t)b->spec->queue_media_ms*1000,(int64_t)b->spec->max_lag_ms*1000);
        if(ret<0)return ret;
        b->initialized=1;
        if(atomic_load(&j->done))continue;
        ret=pthread_create(&b->thread,NULL,branch_main,b);
        if(ret) {
            fail_branch(b,AVERROR(ret),"branch thread creation");naki_watermark_stop(j->watermark);
            pthread_mutex_lock(&j->mutex);strcpy(j->state,"failed");j->policy_closing=1;j->ended_us=now_us();pthread_mutex_unlock(&j->mutex);
            atomic_store(&j->done,1);continue;
        }
        b->started=1;
    }
    return check_branches(p,0);
}
static int fanout_frame(Pipeline *p,AVFrame *frame) {
    int ret=prepare_decoded_frame(p,frame);if(ret<0)return ret;Stamp *source=(Stamp *)frame->opaque_ref->data;
    pthread_mutex_lock(&p->job->mutex);p->job->frames_decoded++;p->job->frame_id_last=source->frame_id;p->job->timestamp_wraps=p->timeline.wraps;
    metric_add(&p->job->lifetime.decode,(source->decoded_us-source->submitted_us)/1000.0,p->job->config.slow_frame_ms);
    metric_add(&p->job->window.decode,(source->decoded_us-source->submitted_us)/1000.0,p->job->config.slow_frame_ms);pthread_mutex_unlock(&p->job->mutex);
    for(int i=0;i<p->branch_count;i++) {
        Branch *b=&p->branches[i];Job *j=b->pipeline.job;if(!b->started||atomic_load(&j->done)||atomic_load(&j->cancel))continue;
        NakiBranchItem item={.frame=av_frame_clone(frame),.source_us=source->source_us,.ingress_us=source->ingress_us,.sequence=source->sequence,.time_base=p->input_tb};
        /* Queue work-duration budget never turns legal source PTS holes or
         * long display durations into an admission failure. No PTS is retimed. */
        int64_t duration=av_rescale_q(1,(AVRational){1,p->job->config.fps},AV_TIME_BASE_Q);
        item.media_us=duration>0?(uint64_t)duration:1;
        ret=item.frame?av_buffer_make_writable(&item.frame->opaque_ref):AVERROR(ENOMEM);
        if(ret>=0)ret=naki_branch_item_charge(&item);
        pthread_mutex_lock(&j->mutex);
        if(!j->fixed_output_width) {j->fixed_output_width=j->config.width?j->config.width:frame->width;j->fixed_output_height=j->config.height?j->config.height:frame->height;}
        int width=j->fixed_output_width,height=j->fixed_output_height;pthread_mutex_unlock(&j->mutex);
        int output_bytes=av_image_get_buffer_size(!strcmp(j->config.encoder,"libvvenc")?AV_PIX_FMT_YUV420P10LE:AV_PIX_FMT_YUV420P,width,height,32);
        uint64_t reservation=item.charged_bytes+(output_bytes>0?(uint64_t)output_bytes:0);
        int reserved=0;
        if(ret>=0&&output_bytes>0) {
            pthread_mutex_lock(&j->mutex);
            if(j->branch_inflight_frames>=128||reservation>b->spec->inflight_bytes-j->branch_inflight_bytes)ret=AVERROR(ENOBUFS);
            else if(!(ret=naki_inflight_add(&j->branch_pending,NAKI_TRACK_FRAME,(uint64_t)source->frame_id,source->ingress_us))) {
                j->branch_inflight_bytes+=reservation;j->branch_inflight_frames++;reserved=1;
                if(j->branch_inflight_frames>j->branch_inflight_high)j->branch_inflight_high=j->branch_inflight_frames;
            }
            pthread_mutex_unlock(&j->mutex);
        } else if(ret>=0)ret=output_bytes<0?output_bytes:AVERROR(EINVAL);
        if(ret>=0) {
            Stamp *stamp=(Stamp *)item.frame->opaque_ref->data;stamp->branch_charge=reservation;stamp->branch_enqueued_us=now_us();
            ret=naki_branch_queue_push(&b->queue,&item,now_us());
        }
        if(ret<0) {if(reserved)naki_branch_release(j,(uint64_t)source->frame_id,reservation);fail_branch(b,ret,"branch frame admission budget");}
        naki_branch_item_free(&item);branch_queue_stats(b);
    }
    return check_branches(p,0);
}
static int fanout_audio(Pipeline *p,const InputItem *audio) {
    for(int i=0;i<p->branch_count;i++) {
        Branch *b=&p->branches[i];Job *j=b->pipeline.job;
        if(!b->started||atomic_load(&j->done)||atomic_load(&j->cancel)||!j->config.audio_copy||!strcmp(j->config.output_format,"mp4"))continue;
        NakiBranchItem item={.packet=av_packet_clone(audio->packet),.parameters=avcodec_parameters_alloc(),.time_base=audio->tb,
            .source_us=audio->packet->pts==AV_NOPTS_VALUE?AV_NOPTS_VALUE:av_rescale_q(audio->packet->pts,audio->tb,AV_TIME_BASE_Q),.ingress_us=audio->demuxed_us};
        int ret=item.packet&&item.parameters?avcodec_parameters_copy(item.parameters,audio->params):AVERROR(ENOMEM);
        int64_t duration=audio->packet->duration>0?av_rescale_q(audio->packet->duration,audio->tb,AV_TIME_BASE_Q):0;
        if(duration<=0)duration=audio->params->sample_rate>0?av_rescale(1152,1000000,audio->params->sample_rate):40000;
        item.media_us=duration>0?(uint64_t)duration:1;
        if(ret>=0)ret=naki_branch_queue_push(&b->queue,&item,now_us());
        if(ret<0)fail_branch(b,ret,"branch audio admission budget");
        naki_branch_item_free(&item);branch_queue_stats(b);
    }
    return check_branches(p,0);
}
static int finish_branches(Pipeline *p,int aborting) {
    for(int i=0;i<p->branch_count;i++)if(p->branches[i].initialized) {
        if(aborting) {request_stop(p->branches[i].pipeline.job);naki_branch_queue_abort(&p->branches[i].queue);}
        else naki_branch_queue_finish(&p->branches[i].queue);
    }
    for(;;) {
        int waiting=0;for(int i=0;i<p->branch_count;i++)if(p->branches[i].started&&!atomic_load(&p->branches[i].pipeline.job->done))waiting=1;
        if(!waiting)break;
        check_branches(p,1);struct pollfd fd={.fd=p->job->wake_fd,.events=POLLIN};poll(&fd,1,20);
        uint64_t value;while(read(p->job->wake_fd,&value,sizeof(value))>0) {}
    }
    int completed=0;
    for(int i=0;i<p->branch_count;i++) {
        Branch *b=&p->branches[i];if(b->started) {pthread_join(b->thread,NULL);b->started=0;}
        if(!strcmp(b->pipeline.job->state,"completed"))completed++;
    }
    return completed?0:AVERROR(EPIPE);
}
void *pipeline_main(void *opaque) {
    (void)crash_thread_init();
    Job *job=opaque;
    Pipeline p={.job=job,.custom_output_fd=-1,.last_pts=AV_NOPTS_VALUE,.mux_max_dts_us=INT64_MIN,.stage="initialization"};
    p.read_interrupt.job=job; p.write_interrupt.job=job;
    atomic_init(&p.read_interrupt.deadline,0); atomic_init(&p.write_interrupt.deadline,0);
    pthread_mutex_init(&p.ring_mutex,NULL); pthread_cond_init(&p.ring_space,NULL);
    InputItem pending={0}; AVFrame *frame=av_frame_alloc();int branches_joined=0;
    NakiMediaJobInfo info={.job_id=job->id,.request_id=job->request_id,.config=&job->config,.request_us=job->started_us};
    p.stage="media hook initialization";
    int ret=frame ? naki_media_hooks_open(&info,&p.hooks) : AVERROR(ENOMEM);
    if(ret>0 || (ret>=0 && p.hooks.api_version!=NAKI_MEDIA_HOOK_API))ret=AVERROR(EINVAL);
    if(ret>=0) {
        p.stage="required watermark startup gate";
        pthread_mutex_lock(&job->mutex);
        if(job->config.watermark.required)strcpy(job->state,"waiting_assets");
        pthread_mutex_unlock(&job->mutex);
        ret=naki_watermark_wait(job->watermark);
        if(ret>=0&&job->config.output_count) {
            int ready=0;pthread_mutex_lock(&job->mutex);strcpy(job->state,"waiting_assets");pthread_mutex_unlock(&job->mutex);
            for(int i=0;i<job->config.output_count;i++) {
                Job *b=job->outputs[i];int gate=naki_watermark_wait(b->watermark);
                if(gate<0) {
                    pthread_mutex_lock(&b->mutex);b->branch_failed=1;strcpy(b->state,"failed");snprintf(b->error,sizeof(b->error),"required watermark startup failed: %d",gate);b->ended_us=now_us();pthread_mutex_unlock(&b->mutex);
                    naki_watermark_stop(b->watermark);atomic_store(&b->done,1);
                } else ready++;
            }
            if(!ready) {p.stage="all output startup gates failed";ret=AVERROR(EPIPE);}
        }
    }
    if(ret>=0)ret=initialize_decoder(&p);
    if (ret<0) goto cleanup;
    if(job->config.output_count&&(ret=start_branches(&p))<0)goto cleanup;
    ret=pthread_create(&p.reader,NULL,reader_main,&p);
    if (ret) {ret=AVERROR(ret); goto cleanup;}
    p.reader_started=1;
    int eof_sent=0, decoder_eof=0;
    while (!atomic_load(&job->cancel) && !decoder_eof) {
        if(p.branches&&(ret=check_branches(&p,0))<0) {p.stage="all outputs failed";goto cleanup;}
        uint64_t wake;
        while (read(job->wake_fd,&wake,sizeof(wake))<0 && errno==EINTR) {}
        int progress=0;
        p.stage="async decode receive";
        for (int batch=0;batch<8;batch++) {
            ret=avcodec_async_receive_frame(p.async,frame);
            if (ret==AVERROR_EOF) {decoder_eof=1; ret=0; break;}
            if (ret==AVERROR(EAGAIN)) {ret=0; break;}
            if (ret<0) goto cleanup;
            ret=p.branches?fanout_frame(&p,frame):process_frame(&p,frame); av_frame_unref(frame);
            if (ret<0) goto cleanup;
            progress=1;
        }
        if (decoder_eof) break;
        int input_done=0, input_error=0;
        if (!pending.packet && !eof_sent) pending=pop_input(&p,&input_done,&input_error);
        if(pending.params) {
            if(p.hooks.audio_packet) {
                NakiAudioPacketEvent event={.packet=pending.packet,.parameters=pending.params,.time_base=pending.tb,
                    .input_stream_index=pending.packet->stream_index,.demuxed_us=pending.demuxed_us,.dispatched_us=now_us()};
                p.stage="audio media hook";
                ret=p.hooks.audio_packet(p.hooks.opaque,&event);
                if(ret>0)ret=AVERROR(EINVAL);
            }
            if(ret>=0&&p.branches)ret=fanout_audio(&p,&pending);
            else if(ret>=0 && job->config.audio_copy && strcmp(job->config.output_format,"mp4"))ret=handle_audio(&p,&pending);
            free_input(&pending);
            if(ret<0)goto cleanup;
            progress=1;
        }
        if (input_done && input_error<0) {p.stage="input read"; ret=input_error; goto cleanup;}
        if (pending.packet || (input_done && !eof_sent)) {
            p.stage="async decode submit";
            if (pending.packet) ((Stamp *)pending.packet->opaque_ref->data)->submitted_us=now_us();
            ret=avcodec_async_send_packet(p.async,pending.packet);
            if (!ret) {
                if (pending.packet) free_input(&pending); else eof_sent=1;
                progress=1;
            } else if (ret==AVERROR(EAGAIN)) ret=0;
            else goto cleanup;
        }
        if(p.live_mux) {
            ret=flush_deferred_audio(&p);
            if(ret<0 || (ret=drain_mux(&p,0))<0)goto cleanup;
        }
        if (now_us()>=p.next_stats_us) {
            refresh_auto(&p);
            p.next_stats_us=now_us()+1000000;
        }
        if (!progress) {
            struct pollfd fd={.fd=job->wake_fd,.events=POLLIN};
            int n;
            do {n=poll(&fd,1,p.branches?20:p.live_mux ? 10 : 1000);} while(n<0 && errno==EINTR && !atomic_load(&job->cancel));
            if (n<0) {ret=AVERROR(errno); goto cleanup;}
        }
    }
    if(p.branches) {
        ret=finish_branches(&p,atomic_load(&job->cancel));branches_joined=1;
        if(ret<0) {p.stage="all outputs failed";goto cleanup;}
    } else if (!atomic_load(&job->cancel) && p.graph) {
        p.stage="filter drain";
        ret=av_buffersrc_add_frame_flags(p.source,NULL,0);
        if (ret<0 || (ret=filter_output(&p))<0) goto cleanup;
        p.stage="encoder drain";
        ret=encoder_send(&p,NULL);
        if (ret<0 || (ret=receive_encoded(&p))<0) goto cleanup;
        if((ret=flush_deferred_audio(&p))<0 || (ret=drain_mux(&p,1))<0)goto cleanup;
        for (int i=0;i<LEDGER_CAPACITY;i++) if(p.ledger[i].used) {ret=AVERROR_INVALIDDATA; goto cleanup;}
    } else if (!atomic_load(&job->cancel)) {p.stage="empty video"; ret=AVERROR(ENODATA);}
cleanup:
    {
        int stopped=atomic_load(&job->cancel);
        if(ret<0)atomic_store(&job->cancel,1);
        if(stopped||ret<0) {
            /* Release a full shared reader ring before any private output
             * teardown/join can wait for its bounded I/O cleanup. */
            pthread_mutex_lock(&p.ring_mutex);pthread_cond_broadcast(&p.ring_space);pthread_mutex_unlock(&p.ring_mutex);
        }
        if(p.branches&&!branches_joined)finish_branches(&p,1);
        for(int i=0;i<job->config.output_count;i++) {
            Job *b=job->outputs[i];naki_watermark_stop(b->watermark);
            if(!atomic_load(&b->done)) {
                pthread_mutex_lock(&b->mutex);strcpy(b->state,stopped?"stopped":"failed");b->branch_failed=!stopped;b->ended_us=now_us();
                snprintf(b->error,sizeof(b->error),"shared pipeline terminated at %s",p.stage);pthread_mutex_unlock(&b->mutex);atomic_store(&b->done,1);
            }
        }
        naki_watermark_stop(job->watermark);
        pthread_mutex_lock(&job->mutex);job->policy_closing=1;int policy_failed=job->overload.failed;pthread_mutex_unlock(&job->mutex);
        if(policy_failed) {stopped=0; ret=AVERROR(ETIMEDOUT); p.stage="continuous overload budget exhausted";}
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
        if(p.hooks.close) {
            NakiMediaEnd end={.cancelled=stopped,.error_code=ret<0 ? ret : 0,.stage=p.stage};
            for(int i=0;i<job->config.output_count;i++)end.output_failures+=atomic_load(&job->outputs[i]->branch_failed);
            p.hooks.close(p.hooks.opaque,&end);
            p.hooks=(NakiMediaHooks){0};
        }
        if (p.output) {
            if (p.output->pb) {pthread_mutex_lock(&job->mutex); job->bytes_out=(uint64_t)p.output->pb->bytes_written; pthread_mutex_unlock(&job->mutex);}
            if (p.output->flags&AVFMT_FLAG_CUSTOM_IO) {
                if(p.output->pb) {av_freep(&p.output->pb->buffer); avio_context_free(&p.output->pb);}
            } else if (p.output->pb) avio_closep(&p.output->pb);
            avformat_free_context(p.output);
        }
        if (p.custom_output_fd>=0) close(p.custom_output_fd);
        free_input(&pending);
        for(int i=0;i<READER_CAPACITY;i++) free_input(&p.ring[i]);
        for(int i=0;i<p.deferred_count;i++)free_input(&p.deferred_audio[i]);
        for(int i=0;i<p.audio_count;i++)av_bsf_free(&p.audio[i].bsf);
        av_frame_free(&frame); avcodec_free_context(&p.decoder); avcodec_free_context(&p.encoder);
        avfilter_graph_free(&p.graph); naki_watermark_compositor_free(&p.watermark_compositor); avformat_close_input(&p.input);
        pthread_cond_destroy(&p.ring_space); pthread_mutex_destroy(&p.ring_mutex);
        if(p.branches) {
            for(int i=0;i<p.branch_count;i++)if(p.branches[i].initialized)naki_branch_queue_destroy(&p.branches[i].queue);
            free(p.branches);
        }
        pthread_mutex_lock(&job->mutex);
        memset(&job->policy_inflight,0,sizeof(job->policy_inflight));
        job->ended_us=now_us();
        int partial=0;for(int i=0;i<job->config.output_count;i++)if(job->outputs[i]->branch_failed)partial=1;
        strcpy(job->state,stopped ? "stopped" : ret<0 ? "failed" : partial?"completed_partial":"completed");
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
