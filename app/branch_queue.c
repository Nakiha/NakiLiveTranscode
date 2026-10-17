#define _POSIX_C_SOURCE 200809L
#include "branch_queue.h"
#include <errno.h>
#include <string.h>
#include <libavutil/time.h>
void naki_branch_item_free(NakiBranchItem *i) {
    av_frame_free(&i->frame);av_packet_free(&i->packet);avcodec_parameters_free(&i->parameters);
    *i=(NakiBranchItem){0};
}
static int charge(uint64_t *total,uint64_t size) {
    if(size>UINT64_MAX-*total)return AVERROR(ERANGE);
    *total+=size;return 0;
}
static int charge_metadata(uint64_t *bytes,const AVDictionary *metadata) {
    const AVDictionaryEntry *entry=NULL;
    while((entry=av_dict_get(metadata,"",entry,AV_DICT_IGNORE_SUFFIX))) {
        int ret=charge(bytes,sizeof(*entry));if(ret<0)return ret;
        if((ret=charge(bytes,strlen(entry->key)+1))<0||(ret=charge(bytes,strlen(entry->value)+1))<0)return ret;
    }
    return 0;
}
int naki_branch_item_charge(NakiBranchItem *i) {
    if(!i||!!i->frame==!!i->packet||(!i->frame&&!i->parameters)||
       (i->frame&&i->parameters)||i->ingress_us<0||!i->media_us)return AVERROR(EINVAL);
    uint64_t bytes=sizeof(*i);int ret;
    if(i->frame) {
        const AVFrame *f=i->frame;
        if(!f->buf[0])return AVERROR(EINVAL); /* Must retain refcounted pixels. */
        if((ret=charge(&bytes,sizeof(*f)))<0)return ret;
        for(int n=0;n<AV_NUM_DATA_POINTERS;n++)if(f->buf[n]&&(ret=charge(&bytes,f->buf[n]->size))<0)return ret;
        for(int n=0;n<f->nb_extended_buf;n++)if((ret=charge(&bytes,f->extended_buf[n]->size))<0)return ret;
        if(f->opaque_ref&&(ret=charge(&bytes,f->opaque_ref->size))<0)return ret;
        if((ret=charge_metadata(&bytes,f->metadata))<0)return ret;
        for(int n=0;n<f->nb_side_data;n++) {
            const AVFrameSideData *s=f->side_data[n];
            if((ret=charge(&bytes,s->buf?s->buf->size:s->size))<0||
               (ret=charge(&bytes,sizeof(*s)))<0||(ret=charge_metadata(&bytes,s->metadata))<0)return ret;
        }
    } else {
        const AVPacket *p=i->packet;const AVCodecParameters *c=i->parameters;
        if(!p->buf||p->size<0||c->extradata_size<0)return AVERROR(EINVAL);
        if((ret=charge(&bytes,sizeof(*p)+sizeof(*c)))<0||(ret=charge(&bytes,p->buf->size))<0||
           (ret=charge(&bytes,(uint64_t)c->extradata_size+AV_INPUT_BUFFER_PADDING_SIZE))<0)return ret;
        if(c->ch_layout.order==AV_CHANNEL_ORDER_CUSTOM) {
            if(c->ch_layout.nb_channels<0||(!c->ch_layout.u.map&&c->ch_layout.nb_channels))return AVERROR(EINVAL);
            uint64_t channels=(uint64_t)c->ch_layout.nb_channels;
            if(channels>UINT64_MAX/sizeof(AVChannelCustom))return AVERROR(ERANGE);
            if((ret=charge(&bytes,channels*sizeof(AVChannelCustom)))<0)return ret;
        }
        if(p->opaque_ref&&(ret=charge(&bytes,p->opaque_ref->size))<0)return ret;
        for(int n=0;n<p->side_data_elems;n++)if((ret=charge(&bytes,p->side_data[n].size+sizeof(p->side_data[n])))<0)return ret;
        for(int n=0;n<c->nb_coded_side_data;n++)if((ret=charge(&bytes,c->coded_side_data[n].size+sizeof(c->coded_side_data[n])))<0)return ret;
    }
    i->charged_bytes=bytes;return 0;
}
int naki_branch_queue_init(NakiBranchQueue *q,unsigned capacity,uint64_t bytes,int64_t media,int64_t lag) {
    if(!q||!capacity||capacity>NAKI_BRANCH_QUEUE_MAX||!bytes||media<=0||lag<=0)return AVERROR(EINVAL);
    *q=(NakiBranchQueue){.capacity=capacity,.byte_limit=bytes,.media_limit_us=media,.lag_limit_us=lag};
    int ret=pthread_mutex_init(&q->mutex,NULL);if(ret)return AVERROR(ret);
    pthread_condattr_t attr;ret=pthread_condattr_init(&attr);
    if(!ret) {ret=pthread_condattr_setclock(&attr,CLOCK_MONOTONIC);if(!ret)ret=pthread_cond_init(&q->changed,&attr);pthread_condattr_destroy(&attr);}
    if(ret)pthread_mutex_destroy(&q->mutex);
    return ret?AVERROR(ret):0;
}
static int push(NakiBranchQueue *q,NakiBranchItem *i,int64_t now,int64_t grace) {
    int ret=naki_branch_item_charge(i);if(ret<0)return ret;
    pthread_mutex_lock(&q->mutex);
    if(q->aborted)ret=AVERROR(ECANCELED);
    else if(q->eof)ret=AVERROR_EOF;
    else if(q->admission_deadline_us&&now>=q->admission_deadline_us) {q->admission_timeouts++;ret=AVERROR(ETIMEDOUT);}
    else if(i->charged_bytes>q->byte_limit||i->media_us>(uint64_t)q->media_limit_us)ret=AVERROR(EFBIG);
    else {
        int64_t ingress=i->ingress_us;
        for(unsigned n=0;n<q->count;n++) {
            NakiBranchItem *p=&q->items[(q->head+n)%q->capacity];
            if(p->ingress_us<ingress)ingress=p->ingress_us;
        }
        if(now<ingress)ret=AVERROR(EINVAL);
        else if((uint64_t)now-(uint64_t)ingress>(uint64_t)q->lag_limit_us)ret=AVERROR(ETIMEDOUT);
        else if(q->count==q->capacity||i->charged_bytes>q->byte_limit-q->bytes||
                i->media_us>(uint64_t)q->media_limit_us-q->media_us) {
            ret=AVERROR(EAGAIN);
            if(grace) {
                if(!q->admission_deadline_us)q->admission_deadline_us=now+grace;
                q->admission_retries++;
            }
        }
        else {
            q->items[(q->head+q->count)%q->capacity]=*i;q->count++;q->bytes+=i->charged_bytes;q->media_us+=i->media_us;*i=(NakiBranchItem){0};
            if(q->count>q->high_water)q->high_water=q->count;
            if(q->bytes>q->high_bytes)q->high_bytes=q->bytes;
            pthread_cond_signal(&q->changed);ret=0;
        }
    }
    pthread_mutex_unlock(&q->mutex);return ret;
}
int naki_branch_queue_push(NakiBranchQueue *q,NakiBranchItem *i,int64_t now) {
    return push(q,i,now,0);
}
int naki_branch_queue_admit(NakiBranchQueue *q,NakiBranchItem *i,int64_t now,int64_t grace) {
    if(grace<1||grace>1000000||now<0||now>INT64_MAX-grace)return AVERROR(EINVAL);
    return push(q,i,now,grace);
}
int naki_branch_admit_many(NakiBranchOffer *offers,unsigned count,int64_t grace,
    int (*check)(void *),void *opaque) {
    if(!offers||!count||count>4||grace<1||grace>1000000)return AVERROR(EINVAL);
    for(unsigned i=0;i<count;i++)
        if(offers[i].queue&&offers[i].result==AVERROR(EAGAIN)&&!offers[i].item)return AVERROR(EINVAL);
    for(;;) {
        int ret=check?check(opaque):0;
        if(ret<0) {
            for(unsigned i=0;i<count;i++)if(offers[i].queue&&offers[i].result==AVERROR(EAGAIN))offers[i].result=ret;
            return ret;
        }
        int pending=0;
        for(unsigned i=0;i<count;i++)if(offers[i].queue&&offers[i].result==AVERROR(EAGAIN)) {
            offers[i].result=naki_branch_queue_admit(offers[i].queue,offers[i].item,av_gettime_relative(),grace);
            pending+=offers[i].result==AVERROR(EAGAIN);
        }
        if(!pending)return 0;
        av_usleep(1000);
    }
}
int naki_branch_queue_pop(NakiBranchQueue *q,NakiBranchItem *i,int64_t deadline) {
    if(!i||i->frame||i->packet||i->parameters||deadline<0)return AVERROR(EINVAL);
    pthread_mutex_lock(&q->mutex);int ret=0;
    while(!q->count&&!q->eof&&!q->aborted) {
        if(!deadline) {ret=AVERROR(EAGAIN);break;}
        struct timespec ts={deadline/1000000,(deadline%1000000)*1000};
        int wait=pthread_cond_timedwait(&q->changed,&q->mutex,&ts);
        if(wait) {ret=AVERROR(wait);break;}
    }
    if(q->aborted)ret=AVERROR(ECANCELED);
    else if(!ret&&!q->count)ret=AVERROR_EOF;
    else if(!ret) {
        *i=q->items[q->head];q->items[q->head]=(NakiBranchItem){0};q->head=(q->head+1)%q->capacity;
        q->count--;q->bytes-=i->charged_bytes;q->media_us-=i->media_us;
        if(q->admission_deadline_us&&q->count<=(q->capacity-1)/2&&q->bytes<=(q->byte_limit-1)/2&&
           q->media_us<=((uint64_t)q->media_limit_us-1)/2&&av_gettime_relative()<q->admission_deadline_us)
            q->admission_deadline_us=0;
    }
    pthread_mutex_unlock(&q->mutex);return ret;
}
int naki_branch_queue_check(NakiBranchQueue *q,int64_t now) {
    pthread_mutex_lock(&q->mutex);int ret=q->aborted?AVERROR(ECANCELED):0;
    if(!ret&&q->admission_deadline_us&&now>=q->admission_deadline_us)ret=AVERROR(ETIMEDOUT);
    for(unsigned n=0;!ret&&n<q->count;n++) {
        int64_t ingress=q->items[(q->head+n)%q->capacity].ingress_us;
        if(now<ingress)ret=AVERROR(EINVAL);
        else if((uint64_t)now-(uint64_t)ingress>(uint64_t)q->lag_limit_us)ret=AVERROR(ETIMEDOUT);
    }
    pthread_mutex_unlock(&q->mutex);return ret;
}
void naki_branch_queue_finish(NakiBranchQueue *q) {
    pthread_mutex_lock(&q->mutex);q->eof=1;pthread_cond_broadcast(&q->changed);pthread_mutex_unlock(&q->mutex);
}
void naki_branch_queue_abort(NakiBranchQueue *q) {
    NakiBranchItem retired[NAKI_BRANCH_QUEUE_MAX];unsigned count;
    pthread_mutex_lock(&q->mutex);q->aborted=1;count=q->count;
    for(unsigned n=0;n<count;n++) {unsigned index=(q->head+n)%q->capacity;retired[n]=q->items[index];q->items[index]=(NakiBranchItem){0};}
    q->count=0;q->bytes=0;q->media_us=0;pthread_cond_broadcast(&q->changed);pthread_mutex_unlock(&q->mutex);
    for(unsigned n=0;n<count;n++)naki_branch_item_free(&retired[n]);
}
void naki_branch_queue_destroy(NakiBranchQueue *q) {
    /* Owners and producer must already have joined. */
    naki_branch_queue_abort(q);pthread_cond_destroy(&q->changed);pthread_mutex_destroy(&q->mutex);
}
