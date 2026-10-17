#define _GNU_SOURCE
#include "watermark.h"
#include "crash.h"
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
        while(!wm->pending&&!atomic_load(&wm->stop))pthread_cond_wait(&wm->changed,&wm->mutex);
        if(atomic_load(&wm->stop)) {pthread_mutex_unlock(&wm->mutex);break;}
        NakiWatermarkConfig c=wm->requested;uint64_t rev=wm->accepted;wm->pending=0;
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
            if(c.enabled&&wm->video_known&&wm->video_unsupported)ret=AVERROR(ENOSYS);
            if(ret>=0) {
                discard=wm->candidate;wm->candidate=a;a=NULL;wm->prepared=rev;wm->ready=1;wm->ready_us=clock_us();
                *wm->error=0;
            } else {
                wm->failed=rev;av_strerror(ret,wm->error,sizeof(wm->error));
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
    /* The initial configuration is the compatibility baseline until a prepared
     * replacement takes effect. Disabled configurations require no resource. */
    if(!c->enabled) {wm->active_revision=wm->prepared=1;wm->ready=1;wm->ready_us=clock_us();}
    wm->startup_deadline=accepted_us+(int64_t)c->startup_timeout_ms*1000;wm->applied_pts=AV_NOPTS_VALUE;
    atomic_init(&wm->wanted,1);atomic_init(&wm->stop,0);
    if(pthread_create(&wm->loader,NULL,loader_main,wm)) {pthread_cond_destroy(&wm->changed);pthread_mutex_destroy(&wm->mutex);free(wm);return NULL;}
    return wm;
}
int naki_watermark_request(NakiWatermark *wm,const NakiWatermarkConfig *c,uint64_t revision,char *e,size_t n) {
    pthread_mutex_lock(&wm->mutex);const char *error=NULL;
    if(atomic_load(&wm->stop)||atomic_load(wm->cancel))error="watermark: job is closing";
    else if(c->enabled&&wm->video_unsupported)error="watermark: HDR or oversized canvas is unsupported";
    else if(revision!=wm->accepted+1)error="watermark: revision must equal accepted_revision + 1";
    else if(c->required!=wm->required)error="watermark: startup.required is immutable";
    else if(wm->required&&!c->enabled)error="watermark: required watermark cannot be cleared";
    if(error) {snprintf(e,n,"%s",error);pthread_mutex_unlock(&wm->mutex);return -1;}
    wm->requested=*c;wm->accepted=revision;wm->pending=c->enabled;wm->ready=!c->enabled;*wm->error=0;
    if(!c->enabled) {wm->prepared=revision;wm->ready_us=clock_us();}
    /* Old candidate is not active; freeing here cannot affect the media owner. */
    Asset *discard=wm->candidate;wm->candidate=NULL;
    atomic_store(&wm->wanted,revision);pthread_cond_broadcast(&wm->changed);pthread_mutex_unlock(&wm->mutex);
    asset_free(discard);return 0;
}
/* Mutex held; only the media owner (or startup gate) activates a generation. */
static Asset *activate_candidate_locked(NakiWatermark *wm) {
    Asset *old=wm->active;wm->active=wm->candidate;wm->candidate=NULL;
    if(wm->active!=old)wm->activation_set=0;
    wm->active_config=wm->requested;wm->active_revision=wm->prepared;wm->ready=0;
    return old;
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
    Asset *old=NULL;
    if(!ret) {
        wm->startup_done=1;
        /* Retain the initial required resource before opening the media source.
         * A concurrent hot update can replace its candidate but cannot remove
         * this startup generation and expose an unwatermarked first frame. */
        if(wm->required&&wm->ready) {
            old=activate_candidate_locked(wm);
        }
    }
    pthread_mutex_unlock(&wm->mutex);asset_free(old);return ret;
}
void naki_watermark_stop(NakiWatermark *wm) {
    if(!wm)return;
    atomic_store(&wm->stop,1);pthread_mutex_lock(&wm->mutex);pthread_cond_broadcast(&wm->changed);pthread_mutex_unlock(&wm->mutex);
}
void naki_watermark_close(NakiWatermark **ptr) {
    NakiWatermark *wm=*ptr;if(!wm)return;
    naki_watermark_stop(wm);pthread_join(wm->loader,NULL);asset_free(wm->active);asset_free(wm->candidate);
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
    yyjson_mut_obj_add_str(doc,v,"state",wm->failed==wm->accepted?"failed":wm->applied==wm->accepted?"applied":wm->ready?"prepared":"preparing");
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
    yyjson_mut_obj_add_val(doc,yyjson_mut_doc_get_root(doc),"watermark",v);pthread_mutex_unlock(&wm->mutex);
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
    atomic_store(&wm->wanted,0); /* Cancel an in-progress obsolete preparation. */
    return discard;
}
int naki_watermark_video(NakiWatermark *wm,const AVFrame *input,int width,int height) {
    pthread_mutex_lock(&wm->mutex);
    /* Reserve a prepared close at this boundary before inspecting the first
     * video. A later pending enable cannot undo that effective configuration. */
    Asset *old=NULL;
    if(wm->ready&&wm->prepared==wm->accepted&&!wm->requested.enabled)old=activate_candidate_locked(wm);
    wm->video_known=1;
    wm->video_unsupported=unsupported_color(input)||
        width<=0||height<=0||(uint64_t)width*height>WM_CANVAS_PIXELS_MAX;
    int ret=wm->video_unsupported&&wm->active_config.enabled?AVERROR(ENOSYS):0;
    Asset *discard=wm->video_unsupported&&!ret?reject_unsupported_update(wm):NULL;
    pthread_mutex_unlock(&wm->mutex);asset_free(old);asset_free(discard);return ret;
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
int naki_watermark_frame(NakiWatermark *wm,NakiWatermarkCompositor **ptr,AVFrame *frame,
                         AVRational tb,int64_t source_us,uint64_t *revision) {
    pthread_mutex_lock(&wm->mutex);Asset *old=NULL,*closed=NULL;
    if(wm->ready&&wm->prepared==wm->accepted&&!wm->requested.enabled)closed=activate_candidate_locked(wm);
    if(wm->video_unsupported||unsupported_color(frame)||frame->width<=0||frame->height<=0||
       (uint64_t)frame->width*frame->height>WM_CANVAS_PIXELS_MAX) {
        wm->video_known=1;wm->video_unsupported=1;
        if(wm->active_config.enabled) {pthread_mutex_unlock(&wm->mutex);asset_free(closed);return AVERROR(ENOSYS);}
        Asset *discard=reject_unsupported_update(wm);
        uint64_t rev=wm->active_revision;
        if(wm->applied!=rev) {wm->applied=rev;wm->applied_pts=source_us;}
        *revision=rev;pthread_mutex_unlock(&wm->mutex);asset_free(closed);asset_free(discard);return 0;
    }
    if(!*ptr&&(wm->active||(wm->ready&&wm->candidate))) {
        uint64_t preparing=wm->prepared;
        pthread_mutex_unlock(&wm->mutex);
        int setup=compositor_open(ptr,frame,tb);
        pthread_mutex_lock(&wm->mutex);
        if(setup<0) {
            /* The optional first hot enable must not destroy an otherwise
             * healthy bare stream if its new compositor cannot be prepared. */
            if(wm->active) {pthread_mutex_unlock(&wm->mutex);asset_free(closed);return setup;}
            Asset *discard=NULL;
            if(wm->ready&&wm->prepared==preparing) {
                discard=wm->candidate;wm->candidate=NULL;wm->ready=0;wm->failed=preparing;
                av_strerror(setup,wm->error,sizeof(wm->error));
            }
            *revision=wm->applied;pthread_mutex_unlock(&wm->mutex);asset_free(closed);asset_free(discard);return 0;
        }
    }
    if(wm->ready&&wm->prepared==wm->accepted) {
        /* A close was already reserved above; only an enabled candidate can
         * remain here, and compositor preparation has succeeded. */
        old=activate_candidate_locked(wm);
        /* applied is committed only after this frame was composed successfully. */
    }
    Asset *a=asset_ref(wm->active);NakiWatermarkConfig config=wm->active_config;
    uint64_t rev=wm->active_revision;
    if(a&&!wm->activation_set) {wm->activation_us=source_us;wm->activation_set=1;}
    int64_t activation=wm->activation_us;
    pthread_mutex_unlock(&wm->mutex);asset_free(closed);asset_free(old);
    int ret=0;
    if(!a)goto applied;
    if(!*ptr) {ret=AVERROR_INVALIDDATA;goto done;}
    NakiWatermarkCompositor *c=*ptr;
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
    av_frame_free(&out);if(ret<0)goto done;
applied:
    pthread_mutex_lock(&wm->mutex);
    if(wm->applied!=rev) {wm->applied=rev;wm->applied_pts=source_us;}
    pthread_mutex_unlock(&wm->mutex);*revision=rev;
done:
    asset_free(a);return ret;
}
