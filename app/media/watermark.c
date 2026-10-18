#define _GNU_SOURCE
#include "watermark.h"
#include "crash/crash.h"
#include "control/identity.h"
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libavutil/imgutils.h>
#include <libavutil/csp.h>
#include <libavutil/sha.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>

/* Persistent decoded pixels: at most one active and one prepared generation.
 * Loading is serialized, including stale generation destruction. No URI cache. */
#define WM_ENCODED_MAX (8U*1024*1024)
#define WM_PIXELS_MAX (2048U*1024)
#define WM_RAW_MAX (64U*1024*1024)
#define WM_FRAMES_MAX 128
#define WM_DURATION_MAX 60000000LL
#define WM_CANVAS_PIXELS_MAX (4096U*2160)
typedef struct Asset {
    atomic_uint refs;
    AVFrame *frames[WM_FRAMES_MAX];
    int64_t ends[WM_FRAMES_MAX],duration;
    int count;
    size_t bytes;
    char hash[65];
} Asset;
struct NakiWatermark {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    pthread_t loader;
    atomic_int stop;
    atomic_uint_fast64_t wanted;
    atomic_int *cancel;
    NakiWatermarkConfig requested,active_config;
    Asset *active,*candidate;
    uint64_t accepted,prepared,active_revision,applied,failed;
    int pending,ready,required,startup_done,video_known,video_unsupported,activation_set;
    int64_t startup_deadline,activation_us,applied_pts,ready_us;
    char error[192];
    NakiOperations *operations;Logger *logger;
    char job_id[64],output_id[33],create_request_id[129];
    NakiOperationHandle desired_operation,active_operation;
    NakiOperationState desired_state;
    struct {Asset *asset;NakiWatermarkConfig config;uint64_t revision;NakiOperationHandle operation;int present;} claim;
    unsigned disposing;
    int poisoned;
};
struct NakiWatermarkCompositor {
    AVFilterGraph *graph;
    AVFilterContext *main,*logo,*sink;
    AVFrame *canvas[3];
    int width,height,format;
    enum AVColorSpace colorspace;
    enum AVColorRange range;
};
typedef struct Load {
    NakiWatermark *wm;
    uint64_t revision;
    int64_t deadline;
    uint8_t *data;
    size_t size;
} Load;
static int64_t clock_us(void) {return av_gettime_relative();}
static Asset *asset_ref(Asset *a) {if(a)atomic_fetch_add(&a->refs,1);return a;}
static void asset_free(Asset *a) {
    if(!a||atomic_fetch_sub(&a->refs,1)!=1)return;
    for(int i=0;i<a->count;i++)av_frame_free(&a->frames[i]);
    free(a);
}
/* WM mutex -> operation-store mutex is the sole lock order. */
static int operation_state_locked(NakiWatermark *wm,NakiOperationHandle operation,NakiOperationState state,int owner,int error,const char *reason) {
    if(!operation)return 0;
    int ret=naki_operations_transition(wm->operations,operation,state,owner,error,reason,clock_us());
    if(ret<0)wm->poisoned=AVERROR_INVALIDDATA;
    if(!ret&&wm->desired_operation==operation)wm->desired_state=state;
    return ret;
}
static void operation_event_locked(NakiWatermark *wm,NakiOperationHandle operation,const char *name) {
    if(!operation||!wm->logger)return;
    yyjson_mut_doc *doc=naki_operations_get(wm->operations,operation);
    if(!doc) {wm->poisoned=AVERROR_INVALIDDATA;return;}
    yyjson_mut_val *root=yyjson_mut_doc_get_root(doc);
    yyjson_mut_obj_add_str(doc,root,"event",name);yyjson_mut_obj_add_strcpy(doc,root,"output_id",wm->output_id);yyjson_mut_obj_add_str(doc,root,"component","watermark");
    yyjson_mut_obj_add_uint(doc,root,"revision",yyjson_mut_get_uint(yyjson_mut_obj_get(root,"accepted_revision")));
    yyjson_mut_obj_add_strcpy(doc,root,"create_request_id",wm->create_request_id);
    const char *origin=yyjson_mut_get_str(yyjson_mut_obj_get(root,"origin_request_id"));
    if(origin)yyjson_mut_obj_add_strcpy(doc,root,"request_id",origin);
    yyjson_mut_val *frame=!strcmp(name,"update_first_written")?yyjson_mut_obj_get(yyjson_mut_obj_get(root,"delivery"),"first_written"):
        !strcmp(name,"update_applied")?yyjson_mut_obj_get(root,"applied_checkpoint"):NULL;
    if(frame&&!yyjson_mut_is_null(frame)) {
        yyjson_mut_obj_add_uint(doc,root,"frame_id",yyjson_mut_get_uint(yyjson_mut_obj_get(frame,"frame_id")));
        yyjson_mut_obj_add_sint(doc,root,"source_pts",yyjson_mut_get_sint(yyjson_mut_obj_get(frame,"source_pts")));
    }
    logger_submit(wm->logger,NULL,doc);
}
static void dispose_generation(NakiWatermark *wm,Asset *asset,int counted) {
    asset_free(asset);
    if(counted) {pthread_mutex_lock(&wm->mutex);wm->disposing--;pthread_cond_broadcast(&wm->changed);pthread_mutex_unlock(&wm->mutex);}
}
static int claim_candidate_locked(NakiWatermark *wm) {
    if(wm->claim.present)return 0;
    if(!wm->ready||wm->prepared!=wm->accepted)return 0;
    wm->claim.asset=wm->candidate;wm->candidate=NULL;wm->claim.config=wm->requested;
    wm->claim.revision=wm->prepared;wm->claim.operation=wm->desired_operation;wm->claim.present=1;wm->ready=0;
    wm->desired_state=NAKI_OP_APPLYING;
    int ret=operation_state_locked(wm,wm->claim.operation,NAKI_OP_APPLYING,1,0,NULL);
    if(!ret)operation_event_locked(wm,wm->claim.operation,"update_applying");
    return ret;
}
static int aborted(Load *l) {
    return atomic_load(&l->wm->stop)||atomic_load(l->wm->cancel)||
        atomic_load(&l->wm->wanted)!=l->revision||clock_us()>=l->deadline;
}
static size_t curl_write(char *p,size_t size,size_t count,void *opaque) {
    Load *l=opaque;
    if(size&&count>SIZE_MAX/size)return 0;
    size_t n=size*count;
    if(aborted(l)||n>WM_ENCODED_MAX-l->size)return 0;
    memcpy(l->data+l->size,p,n);l->size+=n;return n;
}
static int curl_progress(void *opaque,curl_off_t a,curl_off_t b,curl_off_t c,curl_off_t d) {
    (void)a;(void)b;(void)c;(void)d;return aborted(opaque);
}
static int download(Load *l,const NakiWatermarkConfig *c) {
    l->data=av_mallocz(WM_ENCODED_MAX+AV_INPUT_BUFFER_PADDING_SIZE);
    if(!l->data)return AVERROR(ENOMEM);
    if(!strncmp(c->uri,"http://",7)||!strncmp(c->uri,"https://",8)) {
        CURL *curl=curl_easy_init();if(!curl)return AVERROR(ENOMEM);
        long timeout=(long)((l->deadline-clock_us()+999)/1000);
        if(timeout<1) {curl_easy_cleanup(curl);return AVERROR(ETIMEDOUT);}
        curl_easy_setopt(curl,CURLOPT_URL,c->uri);
        curl_easy_setopt(curl,CURLOPT_PROTOCOLS_STR,"http,https");
        curl_easy_setopt(curl,CURLOPT_REDIR_PROTOCOLS_STR,"http,https");
        curl_easy_setopt(curl,CURLOPT_FOLLOWLOCATION,1L);
        curl_easy_setopt(curl,CURLOPT_MAXREDIRS,3L);
        curl_easy_setopt(curl,CURLOPT_NOSIGNAL,1L);
        curl_easy_setopt(curl,CURLOPT_TIMEOUT_MS,timeout);
        curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT_MS,timeout);
        curl_easy_setopt(curl,CURLOPT_FAILONERROR,1L);
        curl_easy_setopt(curl,CURLOPT_MAXFILESIZE_LARGE,(curl_off_t)WM_ENCODED_MAX);
        curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,curl_write);
        curl_easy_setopt(curl,CURLOPT_WRITEDATA,l);
        curl_easy_setopt(curl,CURLOPT_NOPROGRESS,0L);
        curl_easy_setopt(curl,CURLOPT_XFERINFOFUNCTION,curl_progress);
        curl_easy_setopt(curl,CURLOPT_XFERINFODATA,l);
        CURLcode ret=curl_easy_perform(curl);curl_easy_cleanup(curl);
        if(ret!=CURLE_OK)return aborted(l)?AVERROR(ECANCELED):AVERROR(EIO);
    } else {
        int fd=open(c->uri,O_RDONLY|O_CLOEXEC|O_NONBLOCK);
        if(fd<0)return AVERROR(errno);
        struct stat st;int ret=0;
        if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size<=0||(uint64_t)st.st_size>WM_ENCODED_MAX)ret=AVERROR(EFBIG);
        while(!ret&&l->size<WM_ENCODED_MAX) {
            if(aborted(l)) {ret=AVERROR(ECANCELED);break;}
            ssize_t n=read(fd,l->data+l->size,FFMIN(65536,WM_ENCODED_MAX-l->size));
            if(n<0) {if(errno==EINTR)continue;ret=AVERROR(errno);break;}
            if(!n)break;
            l->size+=(size_t)n;
        }
        /* A file growing after fstat must not silently become a truncated asset. */
        if(!ret&&l->size==WM_ENCODED_MAX) {uint8_t extra;if(read(fd,&extra,1)!=0)ret=AVERROR(EFBIG);}
        close(fd);if(ret)return ret;
    }
    return l->size&&!aborted(l)?0:AVERROR(ECANCELED);
}
/* Parse only container framing and GCE delays; FFmpeg owns LZW, palettes,
 * transparency, interlace and disposal. Splitting at exact image ends avoids
 * the GIF parser's implicit 100ms substitution for a zero delay. */
static int gif_parts(const uint8_t *b,size_t n,size_t ends[WM_FRAMES_MAX],int64_t delays[WM_FRAMES_MAX],int *count) {
    if(n<13)return AVERROR_INVALIDDATA;
    unsigned w=b[6]|b[7]<<8,h=b[8]|b[9]<<8;
    if(!w||!h||(uint64_t)w*h>WM_PIXELS_MAX)return AVERROR(EFBIG);
    size_t p=13+(b[10]&128 ? 3U<<(1+(b[10]&7)):0);
    int delay=0,frames=0,trailer=0;
    while(p<n) {
        unsigned code=b[p++];
        if(code==0x3b) {trailer=1;break;}
        if(code==0x21) {
            if(p>=n)return AVERROR_INVALIDDATA;
            unsigned label=b[p++];
            if(label==0xf9) {
                if(n-p<6||b[p]!=4||b[p+5]!=0)return AVERROR_INVALIDDATA;
                delay=b[p+2]|b[p+3]<<8;p+=6;continue;
            }
        } else if(code==0x2c) {
            if(n-p<9||frames==WM_FRAMES_MAX)return AVERROR(EFBIG);
            unsigned flags=b[p+8];p+=9;
            if(flags&128)p+=3U<<(1+(flags&7));
            if(p>=n)return AVERROR_INVALIDDATA;
            p++; /* LZW minimum code size */
        } else return AVERROR_INVALIDDATA;
        for(;;) {
            if(p>=n)return AVERROR_INVALIDDATA;
            unsigned len=b[p++];if(len>n-p)return AVERROR_INVALIDDATA;
            p+=len;if(!len)break;
        }
        if(code==0x2c) {ends[frames]=p;delays[frames++]=(int64_t)delay*10000;delay=0;}
    }
    if(!trailer||p!=n||!frames)return AVERROR_INVALIDDATA;
    if(frames>1)for(int i=0;i<frames;i++)if(!delays[i])return AVERROR_INVALIDDATA;
    *count=frames;return 0;
}
static int prepare_frame(Asset *a,AVFrame *src,const NakiWatermarkConfig *c) {
    if(src->width<=0||src->height<=0||(uint64_t)src->width*src->height>WM_PIXELS_MAX)return AVERROR(EFBIG);
    if(av_frame_get_side_data(src,AV_FRAME_DATA_ICC_PROFILE)||src->color_trc==AVCOL_TRC_SMPTE2084||src->color_trc==AVCOL_TRC_ARIB_STD_B67||
       (src->color_primaries!=AVCOL_PRI_UNSPECIFIED&&src->color_primaries!=AVCOL_PRI_BT709))return AVERROR(ENOSYS);
    int w=c->width,h=c->height;
    if(!c->stretch) {
        if((int64_t)src->width*h>(int64_t)src->height*w)h=FFMAX(1,(int64_t)src->height*w/src->width);
        else w=FFMAX(1,(int64_t)src->width*h/src->height);
    }
    int layout=av_image_get_buffer_size(AV_PIX_FMT_RGBA,c->width,c->height,32);
    if(layout<0)return layout;
    size_t bytes=(size_t)layout;
    if(bytes>WM_RAW_MAX-a->bytes)return AVERROR(EFBIG);
    AVFrame *f=av_frame_alloc();if(!f)return AVERROR(ENOMEM);
    f->format=AV_PIX_FMT_RGBA;f->width=c->width;f->height=c->height;
    int ret=av_frame_get_buffer(f,32);
    if(ret<0) {av_frame_free(&f);return ret;}
    if((size_t)f->linesize[0]*f->height!=bytes) {av_frame_free(&f);return AVERROR_INVALIDDATA;}
    memset(f->data[0],0,bytes);
    AVFrame *rgba=av_frame_alloc();
    if(!rgba) {av_frame_free(&f);return AVERROR(ENOMEM);}
    rgba->width=src->width;rgba->height=src->height;rgba->format=AV_PIX_FMT_RGBA;
    if((ret=av_frame_get_buffer(rgba,32))<0) {av_frame_free(&rgba);av_frame_free(&f);return ret;}
    struct SwsContext *sws=sws_getContext(src->width,src->height,src->format,src->width,src->height,AV_PIX_FMT_RGBA,SWS_BILINEAR,NULL,NULL,NULL);
    if(!sws) {av_frame_free(&rgba);av_frame_free(&f);return AVERROR(ENOMEM);}
    ret=sws_scale(sws,(const uint8_t *const *)src->data,src->linesize,0,src->height,rgba->data,rgba->linesize);
    sws_freeContext(sws);
    if(ret!=src->height) {av_frame_free(&rgba);av_frame_free(&f);return AVERROR_INVALIDDATA;}
    /* Resample premultiplied components, then return to straight alpha. Hidden
     * RGB in a fully transparent PNG/GIF pixel must not create colored halos. */
    for(int y=0;y<rgba->height;y++)for(int x=0;x<rgba->width;x++) {
        uint8_t *p=rgba->data[0]+y*rgba->linesize[0]+x*4;
        for(int k=0;k<3;k++)p[k]=(p[k]*p[3]+127)/255;
    }
    sws=sws_getContext(rgba->width,rgba->height,AV_PIX_FMT_RGBA,w,h,AV_PIX_FMT_RGBA,SWS_BILINEAR,NULL,NULL,NULL);
    if(!sws) {av_frame_free(&rgba);av_frame_free(&f);return AVERROR(ENOMEM);}
    uint8_t *dst[]={f->data[0]+((c->height-h)/2)*f->linesize[0]+((c->width-w)/2)*4,NULL,NULL,NULL};
    int lines[]={f->linesize[0],0,0,0};
    ret=sws_scale(sws,(const uint8_t *const *)rgba->data,rgba->linesize,0,rgba->height,dst,lines);
    sws_freeContext(sws);av_frame_free(&rgba);
    if(ret!=h) {av_frame_free(&f);return AVERROR_INVALIDDATA;}
    for(int y=0;y<h;y++)for(int x=0;x<w;x++) {
        uint8_t *p=dst[0]+y*lines[0]+x*4;
        for(int k=0;k<3;k++)p[k]=p[3]?FFMIN(255,(p[k]*255+p[3]/2)/p[3]):0;
    }
    a->frames[a->count++]=f;a->bytes+=bytes;return 0;
}
static uint32_t be32(const uint8_t *p) {return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static int png_safe(const uint8_t *b,size_t n) {
    size_t p=8;int header=0,end=0;
    while(n-p>=12) {
        uint32_t length=be32(b+p);const uint8_t *type=b+p+4;
        if(length>n-p-12)return AVERROR_INVALIDDATA;
        if(!memcmp(type,"IHDR",4)) {
            if(header++||length!=13||!be32(b+p+8)||!be32(b+p+12)||
                (uint64_t)be32(b+p+8)*be32(b+p+12)>WM_PIXELS_MAX)return AVERROR(EFBIG);
        }
        /* Compressed ancillary text/profiles can inflate independently of the
         * pixel budget. v1 accepts unprofiled SDR/sRGB still PNGs only. */
        if(!memcmp(type,"zTXt",4)||!memcmp(type,"iTXt",4)||!memcmp(type,"iCCP",4)||
           !memcmp(type,"cICP",4)||!memcmp(type,"acTL",4)||!memcmp(type,"mDCv",4)||!memcmp(type,"cLLi",4))return AVERROR(ENOSYS);
        if(!memcmp(type,"gAMA",4)&&(length!=4||be32(b+p+8)!=45455))return AVERROR(ENOSYS);
        if(!memcmp(type,"cHRM",4)) {
            if(length!=32)return AVERROR_INVALIDDATA;
            /* Unknown cHRM may decode as UNSPECIFIED. Reject it here rather
             * than silently treating unrecognized primaries as sRGB. Use the
             * decoder's classifier/tolerance, including rounded BT.709 tags. */
            AVColorPrimariesDesc desc;
            AVRational *coordinates[]={&desc.wp.x,&desc.wp.y,&desc.prim.r.x,&desc.prim.r.y,
                &desc.prim.g.x,&desc.prim.g.y,&desc.prim.b.x,&desc.prim.b.y};
            for(int i=0;i<8;i++) {
                uint32_t value=be32(b+p+8+i*4);
                if(value>100000)return AVERROR_INVALIDDATA;
                *coordinates[i]=av_make_q((int)value,100000);
            }
            if(av_csp_primaries_id_from_desc(&desc)!=AVCOL_PRI_BT709)return AVERROR(ENOSYS);
        }
        p+=(size_t)length+12;
        if(!memcmp(type,"IEND",4)) {end=1;break;}
    }
    return header&&end&&p==n?0:AVERROR_INVALIDDATA;
}
static int load_asset(Load *l,const NakiWatermarkConfig *c,Asset **result) {
    int ret=download(l,c);if(ret<0)return ret;
    enum AVCodecID id;
    size_t ends[WM_FRAMES_MAX]={l->size};int64_t delays[WM_FRAMES_MAX]={0};int parts=1;
    if(l->size>=8&&!memcmp(l->data,"\211PNG\r\n\032\n",8)) {
        id=AV_CODEC_ID_PNG;if((ret=png_safe(l->data,l->size))<0)return ret;
    }
    else if(l->size>=3&&l->data[0]==255&&l->data[1]==216&&l->data[2]==255)id=AV_CODEC_ID_MJPEG;
    else if(l->size>=6&&(!memcmp(l->data,"GIF87a",6)||!memcmp(l->data,"GIF89a",6))) {
        id=AV_CODEC_ID_GIF;if((ret=gif_parts(l->data,l->size,ends,delays,&parts))<0)return ret;
    } else return AVERROR_INVALIDDATA;
    const AVCodec *decoder=avcodec_find_decoder(id);
    if(!decoder)return AVERROR_DECODER_NOT_FOUND;
    AVCodecContext *ctx=avcodec_alloc_context3(decoder);
    AVFrame *frame=av_frame_alloc();AVPacket *packet=av_packet_alloc();Asset *a=calloc(1,sizeof(*a));
    if(a)atomic_init(&a->refs,1);
    if(!ctx||!frame||!packet||!a) {ret=AVERROR(ENOMEM);goto done;}
    ctx->thread_count=1;ctx->max_pixels=WM_PIXELS_MAX;ctx->err_recognition=AV_EF_EXPLODE;
    if((ret=avcodec_open2(ctx,decoder,NULL))<0)goto done;
    size_t begin=0;
    for(int i=0;i<parts;i++) {
        if(aborted(l)) {ret=AVERROR(ECANCELED);goto done;}
        if((ret=av_new_packet(packet,(int)(ends[i]-begin)))<0)goto done;
        memcpy(packet->data,l->data+begin,ends[i]-begin);begin=ends[i];
        ret=avcodec_send_packet(ctx,packet);av_packet_unref(packet);if(ret<0)goto done;
        if((ret=avcodec_receive_frame(ctx,frame))<0)goto done;
        if((ret=prepare_frame(a,frame,c))<0)goto done;
        av_frame_unref(frame);
        if(avcodec_receive_frame(ctx,frame)!=AVERROR(EAGAIN)) {ret=AVERROR_INVALIDDATA;goto done;}
        a->duration+=delays[i];a->ends[i]=a->duration;
        if(a->duration>WM_DURATION_MAX) {ret=AVERROR(EFBIG);goto done;}
    }
    if(aborted(l)) {ret=AVERROR(ECANCELED);goto done;}
    struct AVSHA *sha=av_sha_alloc();uint8_t hash[32];
    if(!sha) {ret=AVERROR(ENOMEM);goto done;}
    av_sha_init(sha,256);av_sha_update(sha,l->data,l->size);av_sha_final(sha,hash);av_free(sha);
    for(int i=0;i<32;i++)snprintf(a->hash+i*2,3,"%02x",hash[i]);
    *result=a;a=NULL;ret=0;
done:
    asset_free(a);av_frame_free(&frame);av_packet_free(&packet);avcodec_free_context(&ctx);return ret;
}
static void *loader_main(void *opaque) {
    (void)crash_thread_init();
    NakiWatermark *wm=opaque;
    for(;;) {
        pthread_mutex_lock(&wm->mutex);
        while((!wm->pending||wm->claim.present||wm->disposing)&&!atomic_load(&wm->stop))pthread_cond_wait(&wm->changed,&wm->mutex);
        if(atomic_load(&wm->stop)) {pthread_mutex_unlock(&wm->mutex);break;}
        NakiWatermarkConfig c=wm->requested;uint64_t rev=wm->accepted;wm->pending=0;
        NakiOperationHandle operation=wm->desired_operation;wm->desired_state=NAKI_OP_PREPARING;
        operation_state_locked(wm,operation,NAKI_OP_PREPARING,0,0,NULL);
        operation_event_locked(wm,operation,"update_preparing");
        int64_t deadline=clock_us()+(int64_t)c.load_timeout_ms*1000;
        if(wm->required&&!wm->startup_done)deadline=FFMIN(deadline,wm->startup_deadline);
        Asset *a=NULL;
        /* Layout-only revisions can share immutable pixels. A URI replacement,
         * even with identical text, is fetched again when dimensions change or
         * the caller repeats an otherwise identical configuration. */
        if(c.enabled&&wm->active&&!strcmp(c.uri,wm->active_config.uri)&&c.width==wm->active_config.width&&
           c.height==wm->active_config.height&&c.stretch==wm->active_config.stretch&&
           (c.x!=wm->active_config.x||c.y!=wm->active_config.y||c.anchor!=wm->active_config.anchor||c.opacity!=wm->active_config.opacity))a=asset_ref(wm->active);
        pthread_mutex_unlock(&wm->mutex);
        Load load={.wm=wm,.revision=rev,.deadline=deadline};
        int ret=c.enabled&&!a?load_asset(&load,&c,&a):0;
        av_free(load.data);
        pthread_mutex_lock(&wm->mutex);Asset *discard=NULL;
        if(rev==wm->accepted&&!atomic_load(&wm->stop)) {
            int cancelled=atomic_load(wm->cancel);
            if(cancelled) {
                wm->ready=0;wm->desired_state=NAKI_OP_CANCELLED;
                operation_state_locked(wm,operation,NAKI_OP_CANCELLED,0,AVERROR(ECANCELED),"job cancelled before owner claim");
                operation_event_locked(wm,operation,"update_cancelled");
            } else if(c.enabled&&wm->video_known&&wm->video_unsupported)ret=AVERROR(ENOSYS);
            if(cancelled) { /* Cancellation is settled above; never publish a late candidate. */ }
            else if(ret>=0) {
                discard=wm->candidate;wm->candidate=a;a=NULL;wm->prepared=rev;wm->ready=1;wm->ready_us=clock_us();
                wm->desired_state=NAKI_OP_PREPARED;operation_state_locked(wm,operation,NAKI_OP_PREPARED,0,0,NULL);
                operation_event_locked(wm,operation,"update_prepared");
                *wm->error=0;
            } else {
                wm->failed=rev;av_strerror(ret,wm->error,sizeof(wm->error));
                wm->desired_state=NAKI_OP_FAILED;operation_state_locked(wm,operation,NAKI_OP_FAILED,0,ret,wm->error);
                operation_event_locked(wm,operation,"update_failed");
            }
            pthread_cond_broadcast(&wm->changed);
        }
        pthread_mutex_unlock(&wm->mutex);asset_free(a);asset_free(discard);
    }
    return NULL;
}
NakiWatermark *naki_watermark_open(const NakiWatermarkConfig *c,int64_t accepted_us,atomic_int *cancel) {
    NakiWatermark *wm=calloc(1,sizeof(*wm));if(!wm)return NULL;
    pthread_mutex_init(&wm->mutex,NULL);
    pthread_condattr_t attr;pthread_condattr_init(&attr);pthread_condattr_setclock(&attr,CLOCK_MONOTONIC);
    pthread_cond_init(&wm->changed,&attr);pthread_condattr_destroy(&attr);
    wm->cancel=cancel;wm->requested=*c;wm->active_config=*c;wm->required=c->required;wm->accepted=1;wm->pending=c->enabled;
    wm->desired_state=c->enabled?NAKI_OP_ACCEPTED:NAKI_OP_PREPARED;
    /* The initial configuration is the compatibility baseline until a prepared
     * replacement takes effect. Disabled configurations require no resource. */
    if(!c->enabled) {wm->active_revision=wm->prepared=1;wm->ready=1;wm->ready_us=clock_us();}
    wm->startup_deadline=accepted_us+(int64_t)c->startup_timeout_ms*1000;wm->applied_pts=AV_NOPTS_VALUE;
    atomic_init(&wm->wanted,1);atomic_init(&wm->stop,0);
    if(pthread_create(&wm->loader,NULL,loader_main,wm)) {pthread_cond_destroy(&wm->changed);pthread_mutex_destroy(&wm->mutex);free(wm);return NULL;}
    return wm;
}
static Asset *queue_request_locked(NakiWatermark *wm,const NakiWatermarkConfig *c,uint64_t revision,NakiOperationHandle operation) {
    NakiOperationHandle previous=wm->desired_operation;
    if(previous) {
        if(previous!=wm->active_operation&&previous!=wm->claim.operation&&
           (wm->desired_state==NAKI_OP_ACCEPTED||wm->desired_state==NAKI_OP_PREPARING||wm->desired_state==NAKI_OP_PREPARED)) {
            operation_state_locked(wm,previous,NAKI_OP_SUPERSEDED,0,0,"replaced before owner claim");
            operation_event_locked(wm,previous,"update_superseded");
        }
        if(naki_operations_pin(wm->operations,previous,0,NAKI_OP_PIN_DESIRED))wm->poisoned=AVERROR_INVALIDDATA;
    }
    wm->requested=*c;wm->accepted=revision;wm->desired_operation=operation;wm->desired_state=NAKI_OP_ACCEPTED;
    if(operation&&naki_operations_pin(wm->operations,operation,NAKI_OP_PIN_DESIRED,0))wm->poisoned=AVERROR_INVALIDDATA;
    wm->pending=c->enabled;wm->ready=!c->enabled;*wm->error=0;
    Asset *discard=wm->candidate;wm->candidate=NULL;if(discard)wm->disposing++;
    if(!c->enabled) {
        wm->prepared=revision;wm->ready_us=clock_us();wm->desired_state=NAKI_OP_PREPARED;
        operation_state_locked(wm,operation,NAKI_OP_PREPARING,0,0,NULL);
        operation_state_locked(wm,operation,NAKI_OP_PREPARED,0,0,NULL);
        operation_event_locked(wm,operation,"update_prepared");
    }
    atomic_store(&wm->wanted,revision);pthread_cond_broadcast(&wm->changed);return discard;
}
int naki_watermark_bind(NakiWatermark *wm,NakiOperations *operations,const char *job,const char *output,const char *create_request,Logger *logger) {
    if(!operations||!naki_id_valid(job,63)||!naki_id_valid(output,32)||!naki_id_valid(create_request,128))return AVERROR(EINVAL);
    pthread_mutex_lock(&wm->mutex);int ret=wm->operations?AVERROR(EALREADY):0;
    if(!ret) {wm->operations=operations;wm->logger=logger;strcpy(wm->job_id,job);strcpy(wm->output_id,output);strcpy(wm->create_request_id,create_request);}
    pthread_mutex_unlock(&wm->mutex);return ret;
}
int naki_watermark_submit(NakiWatermark *wm,const NakiUpdateRequest *request,const char *origin,yyjson_mut_doc **result,char *e,size_t n) {
    *result=NULL;pthread_mutex_lock(&wm->mutex);
    NakiOperationIntent intent={.kind=NAKI_OP_UPDATE,.job_id=wm->job_id,.operation_id=request->operation_id,.output_id=request->output_id,
        .fingerprint=request->fingerprint,.origin_request_id=origin,.expected_revision=request->expected_revision,.revision=wm->accepted+1};
    int ret=wm->operations?naki_operations_replay(wm->operations,&intent,result):AVERROR(EINVAL);
    if(ret!=-ENOENT) {if(ret<0)snprintf(e,n,"%s",ret==-EEXIST?"operation_id_conflict":"operation lookup unavailable");pthread_mutex_unlock(&wm->mutex);return ret;}
    const char *error=NULL;
    if(strcmp(request->output_id,wm->output_id))error="target.output_id: unknown logical output";
    else if(wm->poisoned)error="operation_mapping_failed";
    else if(atomic_load(&wm->stop)||atomic_load(wm->cancel))error="job_closing";
    else if(request->expected_revision!=wm->accepted)error="revision_conflict";
    else if(wm->accepted>=INT64_MAX)error="revision_exhausted";
    NakiWatermarkConfig config;
    if(!error&&naki_watermark_patch_apply(&wm->requested,&request->patch,&config,e,n))error=e;
    if(!error&&config.enabled&&wm->video_unsupported)error="watermark: HDR or oversized canvas is unsupported";
    if(error) {if(error!=e)snprintf(e,n,"%s",error);pthread_mutex_unlock(&wm->mutex);return AVERROR(EINVAL);}
    NakiOperationHandle operation;ret=naki_operations_reserve(wm->operations,&intent,clock_us(),&operation);
    if(ret<0) {snprintf(e,n,"%s",ret==-ENOSPC?"operation_capacity_exhausted":"operation_id_conflict");pthread_mutex_unlock(&wm->mutex);return ret;}
    if(!ret) {*result=naki_operations_get(wm->operations,operation);pthread_mutex_unlock(&wm->mutex);return *result?0:AVERROR(ENOMEM);}
    operation_event_locked(wm,operation,"update_accepted");
    Asset *discard=queue_request_locked(wm,&config,intent.revision,operation);
    *result=naki_operations_get(wm->operations,operation);ret=wm->poisoned?wm->poisoned:*result?0:AVERROR(ENOMEM);
    pthread_mutex_unlock(&wm->mutex);dispose_generation(wm,discard,discard!=NULL);return ret;
}
int naki_watermark_request(NakiWatermark *wm,const NakiWatermarkConfig *c,uint64_t revision,char *e,size_t n) {
    pthread_mutex_lock(&wm->mutex);const char *error=NULL;
    if(atomic_load(&wm->stop)||atomic_load(wm->cancel))error="watermark: job is closing";
    else if(c->enabled&&wm->video_unsupported)error="watermark: HDR or oversized canvas is unsupported";
    else if(revision!=wm->accepted+1)error="watermark: revision must equal accepted_revision + 1";
    else if(c->required!=wm->required)error="watermark: startup.required is immutable";
    else if(wm->required&&!c->enabled)error="watermark: required watermark cannot be cleared";
    else if(wm->operations)error="watermark: use an operation-bound request";
    if(error) {snprintf(e,n,"%s",error);pthread_mutex_unlock(&wm->mutex);return -1;}
    Asset *discard=queue_request_locked(wm,c,revision,0);pthread_mutex_unlock(&wm->mutex);
    dispose_generation(wm,discard,discard!=NULL);return 0;
}
int naki_watermark_wait(NakiWatermark *wm) {
    pthread_mutex_lock(&wm->mutex);int ret=0;
    while(wm->required&&!wm->ready) {
        if(atomic_load(wm->cancel)||atomic_load(&wm->stop)) {ret=AVERROR(ECANCELED);break;}
        if(wm->failed==wm->accepted) {ret=AVERROR_INVALIDDATA;break;}
        int64_t current=clock_us();
        if(current>=wm->startup_deadline) {ret=AVERROR(ETIMEDOUT);break;}
        int64_t until=FFMIN(current+50000,wm->startup_deadline);
        struct timespec ts={until/1000000,(until%1000000)*1000};pthread_cond_timedwait(&wm->changed,&wm->mutex,&ts);
    }
    if(!ret&&wm->required&&wm->ready_us>wm->startup_deadline)ret=AVERROR(ETIMEDOUT);
    if(!ret) {
        wm->startup_done=1;
        /* Retain the initial required resource before opening the media source.
         * A concurrent hot update can replace its candidate but cannot remove
         * this startup generation and expose an unwatermarked first frame. */
        if(wm->required&&wm->ready) {
            ret=claim_candidate_locked(wm);
        }
    }
    pthread_mutex_unlock(&wm->mutex);return ret;
}
void naki_watermark_stop(NakiWatermark *wm) {
    if(!wm)return;
    atomic_store(&wm->stop,1);pthread_mutex_lock(&wm->mutex);
    if(wm->desired_operation&&wm->desired_operation!=wm->claim.operation&&
       (wm->desired_state==NAKI_OP_ACCEPTED||wm->desired_state==NAKI_OP_PREPARING||wm->desired_state==NAKI_OP_PREPARED)) {
        operation_state_locked(wm,wm->desired_operation,NAKI_OP_CANCELLED,0,0,"component closing before owner claim");
        operation_event_locked(wm,wm->desired_operation,"update_cancelled");
    }
    if(wm->active_operation&&naki_operations_retire(wm->operations,wm->active_operation,clock_us()))wm->poisoned=AVERROR_INVALIDDATA;
    wm->ready=0;wm->pending=0;
    pthread_cond_broadcast(&wm->changed);pthread_mutex_unlock(&wm->mutex);
}
void naki_watermark_close(NakiWatermark **ptr) {
    NakiWatermark *wm=*ptr;if(!wm)return;
    naki_watermark_stop(wm);pthread_join(wm->loader,NULL);
    if(wm->claim.present&&wm->claim.operation)abort(); /* Only the owner may settle an applying claim. */
    asset_free(wm->active);asset_free(wm->candidate);asset_free(wm->claim.asset);
    pthread_cond_destroy(&wm->changed);pthread_mutex_destroy(&wm->mutex);free(wm);*ptr=NULL;
}
void naki_watermark_status(NakiWatermark *wm,yyjson_mut_doc *doc) {
    if(!wm)return;
    pthread_mutex_lock(&wm->mutex);yyjson_mut_val *v=yyjson_mut_obj(doc);
    yyjson_mut_obj_add_uint(doc,v,"accepted_revision",wm->accepted);
    yyjson_mut_obj_add_uint(doc,v,"prepared_revision",wm->prepared);
    yyjson_mut_obj_add_uint(doc,v,"applied_revision",wm->applied);
    yyjson_mut_obj_add_uint(doc,v,"failed_revision",wm->failed);
    yyjson_mut_obj_add_strcpy(doc,v,"error",wm->error);
    yyjson_mut_obj_add_strcpy(doc,v,"content_sha256",wm->active?wm->active->hash:"");
    yyjson_mut_obj_add_str(doc,v,"state",wm->failed==wm->accepted?"failed":wm->applied==wm->accepted?"applied":wm->claim.present&&wm->claim.revision==wm->accepted?"applying":wm->ready?"prepared":"preparing");
    yyjson_mut_obj_add_bool(doc,v,"required",wm->required);
    yyjson_mut_obj_add_bool(doc,v,"video_properties_known",wm->video_known);
    yyjson_mut_obj_add_bool(doc,v,"video_compatible",wm->video_known&&!wm->video_unsupported);
    if(wm->applied_pts==AV_NOPTS_VALUE)yyjson_mut_obj_add_null(doc,v,"applied_source_pts_us");
    else yyjson_mut_obj_add_sint(doc,v,"applied_source_pts_us",wm->applied_pts);
    yyjson_mut_obj_add_uint(doc,v,"active_frames",wm->active?wm->active->count:0);
    yyjson_mut_obj_add_uint(doc,v,"active_raw_bytes",wm->active?wm->active->bytes:0);
    yyjson_mut_obj_add_uint(doc,v,"encoded_limit_bytes",WM_ENCODED_MAX);
    yyjson_mut_obj_add_uint(doc,v,"generation_raw_limit_bytes",WM_RAW_MAX);
    yyjson_mut_obj_add_uint(doc,v,"frame_limit",WM_FRAMES_MAX);
    yyjson_mut_obj_add_val(doc,yyjson_mut_doc_get_root(doc),"watermark",v);
    yyjson_mut_val *components=yyjson_mut_obj(doc),*component=yyjson_mut_obj(doc);
    const char *states[]={"accepted","preparing","prepared","applying","applied","failed","superseded","cancelled","stopping","stopped"};
    yyjson_mut_obj_add_str(doc,component,"state",states[wm->desired_state]);
    yyjson_mut_obj_add_uint(doc,component,"desired_revision",wm->accepted);yyjson_mut_obj_add_uint(doc,component,"effective_revision",wm->active_revision);
    yyjson_mut_obj_add_val(doc,component,"desired",naki_watermark_config_json(doc,&wm->requested));
    if(wm->active_revision)yyjson_mut_obj_add_val(doc,component,"effective",naki_watermark_config_json(doc,&wm->active_config));
    else yyjson_mut_obj_add_null(doc,component,"effective");
    yyjson_mut_obj_add_uint(doc,component,"claimed_revision",wm->claim.present?wm->claim.revision:0);
    yyjson_mut_obj_add_bool(doc,component,"operation_api_bound",wm->operations!=NULL);
    yyjson_mut_obj_add_int(doc,component,"invariant_error",wm->poisoned);
    yyjson_mut_obj_add_val(doc,components,"watermark",component);yyjson_mut_obj_add_val(doc,yyjson_mut_doc_get_root(doc),"components",components);
    pthread_mutex_unlock(&wm->mutex);
}
static int unsupported_color(const AVFrame *input) {
    return input->color_trc==AVCOL_TRC_SMPTE2084||input->color_trc==AVCOL_TRC_ARIB_STD_B67||
        input->color_primaries==AVCOL_PRI_BT2020||
        (input->colorspace!=AVCOL_SPC_UNSPECIFIED&&input->colorspace!=AVCOL_SPC_RGB&&input->colorspace!=AVCOL_SPC_BT709&&
         input->colorspace!=AVCOL_SPC_BT470BG&&input->colorspace!=AVCOL_SPC_SMPTE170M);
}
/* mutex held; retain the disabled configuration while rejecting an optional
 * enable that was accepted before the first video properties were available. */
static Asset *reject_unsupported_update(NakiWatermark *wm) {
    if(!wm->requested.enabled)return NULL;
    Asset *discard=wm->candidate;wm->candidate=NULL;wm->ready=0;wm->pending=0;
    wm->failed=wm->accepted;
    snprintf(wm->error,sizeof(wm->error),"watermark: unsupported video properties; previous configuration retained");
    wm->desired_state=NAKI_OP_FAILED;
    operation_state_locked(wm,wm->desired_operation,NAKI_OP_FAILED,0,AVERROR(ENOSYS),wm->error);
    operation_event_locked(wm,wm->desired_operation,"update_failed");
    atomic_store(&wm->wanted,0); /* Cancel an in-progress obsolete preparation. */
    return discard;
}
int naki_watermark_video(NakiWatermark *wm,const AVFrame *input,int width,int height) {
    pthread_mutex_lock(&wm->mutex);
    /* Reserve a prepared close at this boundary before inspecting the first
     * video. A later pending enable cannot undo that effective configuration. */
    int ret=0;
    if(wm->ready&&wm->prepared==wm->accepted&&!wm->requested.enabled)ret=claim_candidate_locked(wm);
    wm->video_known=1;
    wm->video_unsupported=unsupported_color(input)||
        width<=0||height<=0||(uint64_t)width*height>WM_CANVAS_PIXELS_MAX;
    int enabled=wm->claim.present?wm->claim.config.enabled:wm->active_config.enabled;
    if(!ret&&wm->video_unsupported&&enabled)ret=AVERROR(ENOSYS);
    if(!ret&&wm->poisoned)ret=wm->poisoned;
    Asset *discard=wm->video_unsupported&&!ret?reject_unsupported_update(wm):NULL;
    if(discard)wm->disposing++;
    pthread_mutex_unlock(&wm->mutex);dispose_generation(wm,discard,discard!=NULL);return ret;
}
void naki_watermark_compositor_free(NakiWatermarkCompositor **ptr) {
    NakiWatermarkCompositor *c=*ptr;if(!c)return;
    avfilter_graph_free(&c->graph);
    for(int i=0;i<3;i++)av_frame_free(&c->canvas[i]);
    free(c);*ptr=NULL;
}
static int compositor_open(NakiWatermarkCompositor **ptr,const AVFrame *f,AVRational tb) {
    if(f->format!=AV_PIX_FMT_YUV420P&&f->format!=AV_PIX_FMT_YUV420P10LE)return AVERROR(ENOSYS);
    if(f->width<=0||f->height<=0||(uint64_t)f->width*f->height>WM_CANVAS_PIXELS_MAX)return AVERROR(EFBIG);
    NakiWatermarkCompositor *c=calloc(1,sizeof(*c));if(!c)return AVERROR(ENOMEM);
    c->width=f->width;c->height=f->height;c->format=f->format;
    c->colorspace=f->colorspace==AVCOL_SPC_UNSPECIFIED?AVCOL_SPC_BT470BG:f->colorspace;
    c->range=f->color_range==AVCOL_RANGE_UNSPECIFIED?AVCOL_RANGE_MPEG:f->color_range;
    if(c->colorspace!=AVCOL_SPC_BT709&&c->colorspace!=AVCOL_SPC_BT470BG&&c->colorspace!=AVCOL_SPC_SMPTE170M) {free(c);return AVERROR(ENOSYS);}
    c->graph=avfilter_graph_alloc();int ret=AVERROR(ENOMEM);
    if(!c->graph)goto fail;
    c->graph->nb_threads=1;
    char args[256];
    snprintf(args,sizeof(args),"video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=1/1:colorspace=%d:range=%d",f->width,f->height,f->format,tb.num,tb.den,c->colorspace,c->range);
    if((ret=avfilter_graph_create_filter(&c->main,avfilter_get_by_name("buffer"),"main",args,NULL,c->graph))<0)goto fail;
    snprintf(args,sizeof(args),"video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=1/1",f->width,f->height,AV_PIX_FMT_RGBA,tb.num,tb.den);
    if((ret=avfilter_graph_create_filter(&c->logo,avfilter_get_by_name("buffer"),"logo",args,NULL,c->graph))<0)goto fail;
    AVFilterContext *overlay=NULL,*scale=NULL,*format=NULL;
    snprintf(args,sizeof(args),"w=iw:h=ih:flags=bilinear:out_color_matrix=%d:out_range=%d",c->colorspace,c->range);
    if((ret=avfilter_graph_create_filter(&scale,avfilter_get_by_name("scale"),"logo_color",args,NULL,c->graph))<0)goto fail;
    snprintf(args,sizeof(args),"pix_fmts=%s",f->format==AV_PIX_FMT_YUV420P?"yuva420p":"yuva420p10le");
    if((ret=avfilter_graph_create_filter(&format,avfilter_get_by_name("format"),"logo_format",args,NULL,c->graph))<0)goto fail;
    snprintf(args,sizeof(args),"x=0:y=0:format=%s:alpha=straight:repeatlast=0:shortest=0:eof_action=pass",f->format==AV_PIX_FMT_YUV420P?"yuv420":"yuv420p10");
    if((ret=avfilter_graph_create_filter(&overlay,avfilter_get_by_name("overlay"),"watermark",args,NULL,c->graph))<0||
       (ret=avfilter_graph_create_filter(&c->sink,avfilter_get_by_name("buffersink"),"out",NULL,NULL,c->graph))<0||
       (ret=avfilter_link(c->main,0,overlay,0))<0||(ret=avfilter_link(c->logo,0,scale,0))<0||
       (ret=avfilter_link(scale,0,format,0))<0||(ret=avfilter_link(format,0,overlay,1))<0||
       (ret=avfilter_link(overlay,0,c->sink,0))<0||(ret=avfilter_graph_config(c->graph,NULL))<0)goto fail;
    for(int i=0;i<3;i++) {
        c->canvas[i]=av_frame_alloc();if(!c->canvas[i]) {ret=AVERROR(ENOMEM);goto fail;}
        c->canvas[i]->width=f->width;c->canvas[i]->height=f->height;c->canvas[i]->format=AV_PIX_FMT_RGBA;
        if((ret=av_frame_get_buffer(c->canvas[i],32))<0)goto fail;
    }
    *ptr=c;return 0;
fail:
    naki_watermark_compositor_free(&c);return ret;
}
static int compose_asset(NakiWatermarkCompositor *c,AVFrame *frame,AVRational tb,Asset *a,
                         NakiWatermarkConfig config,int64_t activation,int64_t source_us) {
    int ret=0;
    if(!a)return 0;
    if(!c)return AVERROR_INVALIDDATA;
    if(frame->width!=c->width||frame->height!=c->height||frame->format!=c->format) {ret=AVERROR(ENOSYS);goto done;}
    if((frame->colorspace==AVCOL_SPC_UNSPECIFIED?AVCOL_SPC_BT470BG:frame->colorspace)!=c->colorspace||
       (frame->color_range==AVCOL_RANGE_UNSPECIFIED?AVCOL_RANGE_MPEG:frame->color_range)!=c->range) {ret=AVERROR(ENOSYS);goto done;}
    AVFrame *canvas=NULL;
    for(int i=0;i<3;i++)if(av_frame_is_writable(c->canvas[i])) {canvas=c->canvas[i];break;}
    if(!canvas) {ret=AVERROR(ENOBUFS);goto done;}
    memset(canvas->data[0],0,(size_t)canvas->linesize[0]*canvas->height);
    if(a) {
        int index=0;
        /* Backward legacy timestamps deterministically hold the first frame.
         * Strict source_all already rejects them before reaching this node. */
        if(a->count>1&&source_us>=activation) {
            uint64_t delta=(uint64_t)source_us-(uint64_t)activation;
            int64_t phase=(int64_t)(delta%(uint64_t)a->duration);
            while(index+1<a->count&&phase>=a->ends[index])index++;
        }
        const AVFrame *logo=a->frames[index];
        int x=(frame->width-logo->width)*(config.anchor%3)/2+config.x;
        int y=(frame->height-logo->height)*(config.anchor/3)/2+config.y;
        int left=FFMAX(0,x),top=FFMAX(0,y),right=FFMIN(frame->width,x+logo->width),bottom=FFMIN(frame->height,y+logo->height);
        for(int row=top;row<bottom;row++) {
            uint8_t *dst=canvas->data[0]+row*canvas->linesize[0]+left*4;
            const uint8_t *src=logo->data[0]+(row-y)*logo->linesize[0]+(left-x)*4;
            for(int col=left;col<right;col++,src+=4,dst+=4) {
                memcpy(dst,src,3);dst[3]=(uint8_t)(src[3]*config.opacity+0.5);
            }
        }
    }
    canvas->pts=frame->pts;canvas->duration=frame->duration;canvas->time_base=tb;
    canvas->colorspace=AVCOL_SPC_RGB;canvas->color_range=AVCOL_RANGE_JPEG;
    AVFrame *out=av_frame_alloc();if(!out) {ret=AVERROR(ENOMEM);goto done;}
    if((ret=av_buffersrc_add_frame_flags(c->logo,canvas,AV_BUFFERSRC_FLAG_KEEP_REF))>=0&&
       (ret=av_buffersrc_add_frame_flags(c->main,frame,AV_BUFFERSRC_FLAG_KEEP_REF))>=0)
        ret=av_buffersink_get_frame(c->sink,out);
    if(ret>=0) {
        /* Overlay returns the main frame with its properties already carried.
         * Copying them again appends duplicate side data (SEI, captions, ...). */
        av_frame_unref(frame);av_frame_move_ref(frame,out);
    }
    av_frame_free(&out);
done:
    return ret;
}
/* Mutex held. Control never settles or discards an owner-held claim. */
static Asset *fail_claim_locked(NakiWatermark *wm,int error,int cancelled) {
    if(!wm->claim.present)return NULL;
    NakiOperationHandle operation=wm->claim.operation;uint64_t revision=wm->claim.revision;
    char reason[192];
    if(cancelled)snprintf(reason,sizeof(reason),"owner stopped before application");
    else av_strerror(error<0?error:AVERROR(ENODATA),reason,sizeof(reason));
    operation_state_locked(wm,operation,cancelled?NAKI_OP_CANCELLED:NAKI_OP_FAILED,1,error,reason);
    operation_event_locked(wm,operation,cancelled?"update_cancelled":"update_failed");
    if(!cancelled)wm->failed=revision;
    if(revision==wm->accepted) {
        wm->desired_state=cancelled?NAKI_OP_CANCELLED:NAKI_OP_FAILED;
        snprintf(wm->error,sizeof(wm->error),"%s",reason);
    }
    Asset *discard=wm->claim.asset;memset(&wm->claim,0,sizeof(wm->claim));if(discard)wm->disposing++;
    pthread_cond_broadcast(&wm->changed);return discard;
}
void naki_watermark_owner_finish(NakiWatermark *wm,int error,int cancelled) {
    if(!wm)return;
    pthread_mutex_lock(&wm->mutex);Asset *discard=fail_claim_locked(wm,error,cancelled);
    pthread_mutex_unlock(&wm->mutex);dispose_generation(wm,discard,discard!=NULL);
}
static int watermark_frame(NakiWatermark *wm,NakiWatermarkCompositor **ptr,AVFrame *frame,AVRational tb,
                           int64_t source_us,const NakiFrameIdentity *identity,uint64_t *revision,NakiOperationHandle *lease) {
    if(lease)*lease=0;
    pthread_mutex_lock(&wm->mutex);
    if(wm->poisoned||(wm->operations&&!identity)) {
        int error=wm->poisoned?wm->poisoned:AVERROR(EINVAL);pthread_mutex_unlock(&wm->mutex);return error;
    }
    if((atomic_load(&wm->stop)||atomic_load(wm->cancel))&&!wm->claim.present) {pthread_mutex_unlock(&wm->mutex);return AVERROR(ECANCELED);}
    int ret=0;
    if(wm->ready&&wm->prepared==wm->accepted&&!wm->requested.enabled)ret=claim_candidate_locked(wm);
    int unsupported=wm->video_unsupported||unsupported_color(frame)||frame->width<=0||frame->height<=0||
        (uint64_t)frame->width*frame->height>WM_CANVAS_PIXELS_MAX;
    Asset *discard=NULL;
    if(unsupported) {
        wm->video_known=wm->video_unsupported=1;
        int enabled=wm->claim.present?wm->claim.config.enabled:wm->active_config.enabled;
        if(enabled)ret=AVERROR(ENOSYS);
        else {discard=reject_unsupported_update(wm);if(discard)wm->disposing++;}
    }
    if(!ret&&!wm->claim.present&&wm->ready&&wm->prepared==wm->accepted)ret=claim_candidate_locked(wm);
    if(ret<0) {
        Asset *failed=fail_claim_locked(wm,ret,0);pthread_mutex_unlock(&wm->mutex);
        dispose_generation(wm,discard,discard!=NULL);dispose_generation(wm,failed,failed!=NULL);return ret;
    }
    int claimed=wm->claim.present,optional_setup=claimed&&!wm->active&&!wm->required;
    Asset *asset=asset_ref(claimed?wm->claim.asset:wm->active);
    NakiWatermarkConfig config=claimed?wm->claim.config:wm->active_config;
    uint64_t rev=claimed?wm->claim.revision:wm->active_revision;
    NakiOperationHandle operation=claimed?wm->claim.operation:wm->active_operation,held=0;
    int64_t activation=asset==wm->active&&wm->activation_set?wm->activation_us:source_us;
    if(!claimed&&operation) {
        ret=naki_operations_frame_acquire(wm->operations,operation);
        if(!ret)held=operation;else wm->poisoned=AVERROR_INVALIDDATA;
    }
    pthread_mutex_unlock(&wm->mutex);dispose_generation(wm,discard,discard!=NULL);
    if(ret>=0&&asset&&!*ptr)ret=compositor_open(ptr,frame,tb);
    if(ret<0&&optional_setup) {
        pthread_mutex_lock(&wm->mutex);discard=fail_claim_locked(wm,ret,0);
        uint64_t fallback=wm->active_revision;NakiOperationHandle previous=wm->active_operation;
        ret=previous?naki_operations_frame_acquire(wm->operations,previous):0;
        if(!ret) {
            wm->applied=fallback;wm->applied_pts=source_us;*revision=fallback;if(lease)*lease=previous;
        } else wm->poisoned=AVERROR_INVALIDDATA;
        pthread_mutex_unlock(&wm->mutex);asset_free(asset);dispose_generation(wm,discard,discard!=NULL);return ret;
    }
    if(ret>=0)ret=compose_asset(*ptr,frame,tb,asset,config,activation,source_us);
    pthread_mutex_lock(&wm->mutex);Asset *old=NULL,*failed=NULL;
    if(ret<0) {
        if(claimed)failed=fail_claim_locked(wm,ret,0);
    } else if(claimed) {
        if(!wm->claim.present||wm->claim.revision!=rev)ret=wm->poisoned=AVERROR_INVALIDDATA;
        else if(operation&&(ret=naki_operations_applied(wm->operations,operation,identity,1,clock_us()))<0)wm->poisoned=AVERROR_INVALIDDATA;
        else {
            NakiOperationHandle previous=wm->active_operation;old=wm->active;
            wm->active=wm->claim.asset;wm->claim.asset=NULL;wm->active_config=config;wm->active_revision=rev;wm->active_operation=operation;
            if(wm->accepted==rev)wm->desired_state=NAKI_OP_APPLIED;
            wm->activation_us=activation;wm->activation_set=asset!=NULL;memset(&wm->claim,0,sizeof(wm->claim));
            if(old)wm->disposing++;
            if(operation) {
                held=operation;
                if(naki_operations_pin(wm->operations,operation,NAKI_OP_PIN_EFFECTIVE,0))wm->poisoned=AVERROR_INVALIDDATA;
                if(wm->desired_operation==operation)wm->desired_state=NAKI_OP_APPLIED;
                operation_event_locked(wm,operation,"update_applied");
                if((atomic_load(&wm->stop)||atomic_load(wm->cancel))&&naki_operations_retire(wm->operations,operation,clock_us()))wm->poisoned=AVERROR_INVALIDDATA;
            }
            if(previous&&previous!=operation) {
                if(naki_operations_retire(wm->operations,previous,clock_us())||naki_operations_pin(wm->operations,previous,0,NAKI_OP_PIN_EFFECTIVE))wm->poisoned=AVERROR_INVALIDDATA;
            }
            pthread_cond_broadcast(&wm->changed);
        }
    }
    if(wm->poisoned&&ret>=0)ret=wm->poisoned;
    if(ret<0&&claimed&&wm->claim.present&&!failed)failed=fail_claim_locked(wm,ret,0);
    if(ret<0&&held) {
        if(naki_operations_frame_release(wm->operations,held,clock_us()))wm->poisoned=AVERROR_INVALIDDATA;
        held=0;
    }
    if(ret>=0) {
        if(wm->applied!=rev) {wm->applied=rev;wm->applied_pts=source_us;}
        *revision=rev;if(lease)*lease=held;
    }
    pthread_mutex_unlock(&wm->mutex);asset_free(asset);
    dispose_generation(wm,old,old!=NULL);dispose_generation(wm,failed,failed!=NULL);return ret;
}
int naki_watermark_frame(NakiWatermark *wm,NakiWatermarkCompositor **ptr,AVFrame *frame,AVRational tb,int64_t source_us,uint64_t *revision) {
    return watermark_frame(wm,ptr,frame,tb,source_us,NULL,revision,NULL);
}
int naki_watermark_frame_tracked(NakiWatermark *wm,NakiWatermarkCompositor **ptr,AVFrame *frame,AVRational tb,
                                const NakiFrameIdentity *identity,uint64_t *revision,NakiOperationHandle *lease) {
    if(!identity||!lease)return AVERROR(EINVAL);
    return watermark_frame(wm,ptr,frame,tb,identity->source_us,identity,revision,lease);
}
int naki_watermark_release_frame(NakiWatermark *wm,NakiOperationHandle lease) {
    return lease?naki_operations_frame_release(wm->operations,lease,clock_us()):0;
}
int naki_watermark_written(NakiWatermark *wm,NakiOperationHandle lease,const NakiFrameIdentity *frame,int64_t written_us) {
    if(!lease)return 0;
    pthread_mutex_lock(&wm->mutex);int first=0;
    int ret=naki_operations_written_once(wm->operations,lease,frame,written_us,&first);
    if(ret<0)wm->poisoned=AVERROR_INVALIDDATA;
    else if(first)operation_event_locked(wm,lease,"update_first_written");
    pthread_mutex_unlock(&wm->mutex);return ret;
}
