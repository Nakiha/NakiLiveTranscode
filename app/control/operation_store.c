#include "operation_store.h"
#include "identity.h"
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum Delivery {DELIVERY_NOT_READY,DELIVERY_PENDING,DELIVERY_WRITTEN,DELIVERY_NOT_WRITTEN,DELIVERY_UNCERTAIN} Delivery;
typedef struct Operation {
    uint32_t generation;
    int used,error_code,retired,outcome_unknown;
    char component[33],action[33],destination_id[33];
    uint64_t remote_version;
    NakiOperationKind kind;NakiOperationState state;Delivery delivery;
    char job_id[64],operation_id[129],output_id[33],fingerprint[65],origin_request_id[129],reason[192];
    uint64_t expected_revision,revision,frame_references,result_version;
    unsigned pins;
    int64_t accepted_us,preparing_us,prepared_us,applying_us,applied_us,first_written_us,terminal_us,retention_us;
    NakiFrameIdentity applied,first_written;
} Operation;
struct NakiOperations {
    pthread_mutex_t mutex;
    Operation *records;
    unsigned update_capacity,capacity;
    int64_t retention_us;
    uint64_t result_version;
};
/* A job has one store for its whole worker lifetime. Version every observable
 * mutation under its mutex, including pins/leases after terminal delivery. */
static void changed(NakiOperations *s,Operation *r) {
    if(s->result_version==UINT64_MAX)abort();
    r->result_version=++s->result_version;
}
static const char *states[]={"accepted","preparing","prepared","applying","applied","failed","superseded","cancelled","stopping","stopped"};
static const char *kinds[]={"create","update","stop","topology"};
static const char *deliveries[]={"not_ready","pending","first_written","not_written","uncertain"};
static yyjson_mut_val *record_json(yyjson_mut_doc *document,const Operation *record);
static int valid_frame(const NakiFrameIdentity *frame,int written) {
    return frame&&frame->frame_id&&frame->source_tb_num>0&&frame->source_tb_den>0&&
        frame->source_pts!=INT64_MIN&&frame->source_us!=INT64_MIN&&(!written||(frame->output_tb_num>0&&frame->output_tb_den>0));
}
size_t naki_operations_record_bytes(void) {return sizeof(Operation);}
static int terminal(const Operation *record) {
    return record->state==NAKI_OP_FAILED||record->state==NAKI_OP_SUPERSEDED||record->state==NAKI_OP_CANCELLED||record->state==NAKI_OP_STOPPED||
        (record->state==NAKI_OP_APPLIED&&(record->delivery==DELIVERY_WRITTEN||record->delivery==DELIVERY_NOT_WRITTEN||record->delivery==DELIVERY_UNCERTAIN));
}
static int evictable(const NakiOperations *s,const Operation *r,int64_t now) {
    return r->used&&!r->pins&&!r->frame_references&&terminal(r)&&r->terminal_us&&now-r->terminal_us>=s->retention_us;
}
static NakiOperationHandle handle(unsigned index,const Operation *r) {return ((uint64_t)r->generation<<32)|(index+1);}
static Operation *get(NakiOperations *s,NakiOperationHandle id) {
    uint32_t slot=(uint32_t)id,generation=(uint32_t)(id>>32);
    if(!slot||slot>s->capacity)return NULL;
    Operation *r=&s->records[slot-1];return r->used&&r->generation==generation?r:NULL;
}
static void close_delivery(Operation *r,int64_t now) {
    if(r->state==NAKI_OP_APPLIED&&r->retired&&!r->frame_references&&!(r->pins&NAKI_OP_PIN_CLAIMED)&&r->delivery==DELIVERY_PENDING) {
        r->delivery=DELIVERY_NOT_WRITTEN;snprintf(r->reason,sizeof(r->reason),"revision retired and all media references closed without successful write");
    }
    if(terminal(r)&&!r->terminal_us)r->terminal_us=now;
}
NakiOperations *naki_operations_open(unsigned updates,size_t bytes,int64_t retention) {
    if(!updates||updates>256||retention<0||bytes<sizeof(NakiOperations)+3*sizeof(Operation))return NULL;
    size_t affordable=(bytes-sizeof(NakiOperations))/sizeof(Operation);
    if(affordable<updates+2)updates=(unsigned)affordable-2;
    NakiOperations *s=calloc(1,sizeof(*s));if(!s)return NULL;
    s->capacity=updates+2;s->update_capacity=updates;s->retention_us=retention;
    s->records=calloc(s->capacity,sizeof(*s->records));
    if(!s->records||pthread_mutex_init(&s->mutex,NULL)) {free(s->records);free(s);return NULL;}
    return s;
}
void naki_operations_close(NakiOperations **pointer) {
    NakiOperations *s=*pointer;if(!s)return;
    for(unsigned i=0;i<s->capacity;i++)if(s->records[i].frame_references)abort();
    pthread_mutex_destroy(&s->mutex);free(s->records);free(s);*pointer=NULL;
}
int naki_operations_lookup(NakiOperations *s,const char *job,const char *operation,NakiOperationHandle *id) {
    int result=-ENOENT;pthread_mutex_lock(&s->mutex);
    for(unsigned i=0;i<s->capacity;i++)if(s->records[i].used&&!strcmp(s->records[i].job_id,job)&&!strcmp(s->records[i].operation_id,operation)) {*id=handle(i,&s->records[i]);result=0;break;}
    pthread_mutex_unlock(&s->mutex);return result;
}
int naki_operations_replay(NakiOperations *s,const NakiOperationIntent *in,yyjson_mut_doc **document) {
    Operation copy;int result=-ENOENT;*document=NULL;pthread_mutex_lock(&s->mutex);
    for(unsigned i=0;i<s->capacity;i++) {
        Operation *r=&s->records[i];
        if(r->used&&!strcmp(r->job_id,in->job_id)&&!strcmp(r->operation_id,in->operation_id)) {
            result=r->kind==in->kind&&!strcmp(r->fingerprint,in->fingerprint)?0:-EEXIST;
            if(!result)copy=*r;
            break;
        }
    }
    pthread_mutex_unlock(&s->mutex);
    if(!result) {
        *document=yyjson_mut_doc_new(NULL);if(!*document)return -ENOMEM;
        yyjson_mut_doc_set_root(*document,record_json(*document,&copy));
    }
    return result;
}
int naki_operations_reserve(NakiOperations *s,const NakiOperationIntent *in,int64_t now,NakiOperationHandle *id) {
    if(!s||in->kind<NAKI_OP_CREATE||in->kind>NAKI_OP_TOPOLOGY||!naki_id_valid(in->job_id,63)||!naki_id_valid(in->operation_id,128)||
       !naki_id_valid(in->origin_request_id,128)||!in->fingerprint||strlen(in->fingerprint)!=64||
       ((in->kind==NAKI_OP_UPDATE||in->kind==NAKI_OP_TOPOLOGY)&&(!naki_id_valid(in->output_id,32)||!in->revision)))return -EINVAL;
    if((in->component&&!naki_id_valid(in->component,32))||(in->action&&!naki_id_valid(in->action,32))||(in->destination_id&&!naki_id_valid(in->destination_id,32)))return -EINVAL;
    pthread_mutex_lock(&s->mutex);
    for(unsigned i=0;i<s->capacity;i++) {
        Operation *r=&s->records[i];
        if(r->used&&!strcmp(r->job_id,in->job_id)&&!strcmp(r->operation_id,in->operation_id)) {
            int result=r->kind==in->kind&&!strcmp(r->fingerprint,in->fingerprint)?0:-EEXIST;
            if(!result)*id=handle(i,r);
            pthread_mutex_unlock(&s->mutex);return result;
        }
    }
    int update=in->kind==NAKI_OP_UPDATE||in->kind==NAKI_OP_TOPOLOGY;
    unsigned first=update?0:s->update_capacity+(in->kind==NAKI_OP_STOP),last=update?s->update_capacity:first+1;
    int result=-ENOSPC;
    for(unsigned i=first;i<last;i++) {
        Operation *r=&s->records[i];if(r->used&&!evictable(s,r,now))continue;
        if(r->generation==UINT32_MAX)continue;
        uint32_t generation=r->generation+1;memset(r,0,sizeof(*r));r->generation=generation;r->used=1;r->kind=in->kind;r->state=NAKI_OP_ACCEPTED;
        strcpy(r->job_id,in->job_id);strcpy(r->operation_id,in->operation_id);strcpy(r->fingerprint,in->fingerprint);strcpy(r->origin_request_id,in->origin_request_id);
        if(in->output_id)snprintf(r->output_id,sizeof(r->output_id),"%s",in->output_id);
        snprintf(r->component,sizeof(r->component),"%s",in->component?in->component:"watermark");
        if(in->action)strcpy(r->action,in->action);
        if(in->destination_id)strcpy(r->destination_id,in->destination_id);
        r->revision=in->revision;r->expected_revision=in->expected_revision;r->accepted_us=now;r->retention_us=s->retention_us;changed(s,r);*id=handle(i,r);result=1;break;
    }
    pthread_mutex_unlock(&s->mutex);return result;
}
int naki_operations_pin(NakiOperations *s,NakiOperationHandle id,unsigned set,unsigned clear) {
    pthread_mutex_lock(&s->mutex);Operation *r=get(s,id);int result=r?0:-ENOENT;
    if(r&&r->pins!=((r->pins&~clear)|set)) {r->pins=(r->pins&~clear)|set;changed(s,r);}
    pthread_mutex_unlock(&s->mutex);return result;
}
static int allowed(const Operation *r,NakiOperationState next,int owner) {
    if(r->state==next)return 1;
    if(r->kind==NAKI_OP_CREATE)return 0;
    if(r->kind==NAKI_OP_STOP)return (r->state==NAKI_OP_ACCEPTED&&next==NAKI_OP_STOPPING)||
        (r->state==NAKI_OP_STOPPING&&(next==NAKI_OP_STOPPED||next==NAKI_OP_FAILED));
    if(r->state==NAKI_OP_APPLYING)return owner&&(next==NAKI_OP_FAILED||next==NAKI_OP_CANCELLED);
    if(r->state==NAKI_OP_ACCEPTED||r->state==NAKI_OP_PREPARING||r->state==NAKI_OP_PREPARED) {
        if(next==NAKI_OP_FAILED||next==NAKI_OP_SUPERSEDED||next==NAKI_OP_CANCELLED)return 1;
        return (r->state==NAKI_OP_ACCEPTED&&next==NAKI_OP_PREPARING)||(r->state==NAKI_OP_PREPARING&&next==NAKI_OP_PREPARED)||
            (r->state==NAKI_OP_PREPARED&&next==NAKI_OP_APPLYING&&owner);
    }
    return 0;
}
int naki_operations_transition(NakiOperations *s,NakiOperationHandle id,NakiOperationState next,int owner,int error,const char *reason,int64_t now) {
    pthread_mutex_lock(&s->mutex);Operation *r=get(s,id);int result=-ENOENT;
    if(r) {
        result=next>=NAKI_OP_ACCEPTED&&next<=NAKI_OP_STOPPED&&allowed(r,next,owner)?0:-EPERM;
        if(!result&&r->state!=next) {
            r->state=next;r->error_code=error;if(reason)snprintf(r->reason,sizeof(r->reason),"%s",reason);
            if(next==NAKI_OP_PREPARING)r->preparing_us=now;
            if(next==NAKI_OP_PREPARED)r->prepared_us=now;
            if(next==NAKI_OP_APPLYING) {r->applying_us=now;r->pins|=NAKI_OP_PIN_CLAIMED;}
            if(next==NAKI_OP_FAILED||next==NAKI_OP_CANCELLED)r->pins&=~NAKI_OP_PIN_CLAIMED;
            close_delivery(r,now);changed(s,r);
        }
    }
    pthread_mutex_unlock(&s->mutex);return result;
}
int naki_operations_applied(NakiOperations *s,NakiOperationHandle id,const NakiFrameIdentity *frame,int lease,int64_t now) {
    pthread_mutex_lock(&s->mutex);Operation *r=get(s,id);int result=-ENOENT;
    if(r) {
        result=(r->kind==NAKI_OP_UPDATE||r->kind==NAKI_OP_TOPOLOGY)&&r->state==NAKI_OP_APPLYING&&(valid_frame(frame,0)||(r->kind==NAKI_OP_TOPOLOGY&&frame&&!frame->frame_id))?0:-EPERM;
        if(!result) {r->state=NAKI_OP_APPLIED;r->applied=*frame;r->applied_us=now;r->pins&=~NAKI_OP_PIN_CLAIMED;r->delivery=DELIVERY_PENDING;if(lease)r->frame_references++;close_delivery(r,now);changed(s,r);}
    }
    pthread_mutex_unlock(&s->mutex);return result;
}
int naki_operations_frame_acquire(NakiOperations *s,NakiOperationHandle id) {
    pthread_mutex_lock(&s->mutex);Operation *r=get(s,id);int result=r&&r->state==NAKI_OP_APPLIED&&!r->retired&&r->frame_references<UINT64_MAX?0:-EINVAL;
    if(!result) {r->frame_references++;changed(s,r);}
    pthread_mutex_unlock(&s->mutex);return result;
}
int naki_operations_frame_release(NakiOperations *s,NakiOperationHandle id,int64_t now) {
    pthread_mutex_lock(&s->mutex);Operation *r=get(s,id);int result=r&&r->frame_references?0:-EINVAL;
    if(!result) {r->frame_references--;close_delivery(r,now);changed(s,r);}pthread_mutex_unlock(&s->mutex);return result;
}
int naki_operations_written_once(NakiOperations *s,NakiOperationHandle id,const NakiFrameIdentity *frame,int64_t now,int *first) {
    if(first)*first=0;
    pthread_mutex_lock(&s->mutex);Operation *r=get(s,id);int result=r&&r->state==NAKI_OP_APPLIED&&r->frame_references&&valid_frame(frame,1)&&now>=r->applied_us?0:-EINVAL;
    if(!result&&r->delivery!=DELIVERY_WRITTEN) {r->first_written=*frame;r->first_written_us=now;r->delivery=DELIVERY_WRITTEN;close_delivery(r,now);changed(s,r);if(first)*first=1;}
    pthread_mutex_unlock(&s->mutex);return result;
}
int naki_operations_written(NakiOperations *s,NakiOperationHandle id,const NakiFrameIdentity *frame,int64_t now) {
    return naki_operations_written_once(s,id,frame,now,NULL);
}
int naki_operations_retire(NakiOperations *s,NakiOperationHandle id,int64_t now) {
    pthread_mutex_lock(&s->mutex);Operation *r=get(s,id);int result=r?0:-ENOENT;
    if(r&&!r->retired) {r->retired=1;close_delivery(r,now);changed(s,r);}pthread_mutex_unlock(&s->mutex);return result;
}
void naki_operations_cancel_unclaimed(NakiOperations *s,int64_t now) {
    pthread_mutex_lock(&s->mutex);
    for(unsigned i=0;i<s->capacity;i++) {
        Operation *r=&s->records[i];if(!r->used||(r->kind!=NAKI_OP_UPDATE&&r->kind!=NAKI_OP_TOPOLOGY))continue;
        if(r->state==NAKI_OP_ACCEPTED||r->state==NAKI_OP_PREPARING||r->state==NAKI_OP_PREPARED) {r->state=NAKI_OP_CANCELLED;strcpy(r->reason,"job stopping before owner claim");}
        r->retired=1;close_delivery(r,now);changed(s,r);
    }
    pthread_mutex_unlock(&s->mutex);
}
static yyjson_mut_val *frame_json(yyjson_mut_doc *d,const NakiFrameIdentity *f) {
    yyjson_mut_val *v=yyjson_mut_obj(d);yyjson_mut_obj_add_uint(d,v,"frame_id",f->frame_id);yyjson_mut_obj_add_sint(d,v,"source_pts",f->source_pts);
    yyjson_mut_obj_add_sint(d,v,"source_us",f->source_us);yyjson_mut_obj_add_sint(d,v,"output_pts",f->output_pts);
    yyjson_mut_obj_add_int(d,v,"source_tb_num",f->source_tb_num);yyjson_mut_obj_add_int(d,v,"source_tb_den",f->source_tb_den);
    yyjson_mut_obj_add_int(d,v,"output_tb_num",f->output_tb_num);yyjson_mut_obj_add_int(d,v,"output_tb_den",f->output_tb_den);return v;
}
static yyjson_mut_val *record_json(yyjson_mut_doc *d,const Operation *r) {
    yyjson_mut_val *v=yyjson_mut_obj(d);yyjson_mut_obj_add_str(d,v,"kind",kinds[r->kind]);yyjson_mut_obj_add_str(d,v,"state",states[r->state]);
    yyjson_mut_obj_add_strcpy(d,v,"job_id",r->job_id);yyjson_mut_obj_add_strcpy(d,v,"operation_id",r->operation_id);yyjson_mut_obj_add_strcpy(d,v,"origin_request_id",r->origin_request_id);
    yyjson_mut_obj_add_strcpy(d,v,"intent_fingerprint",r->fingerprint);yyjson_mut_obj_add_bool(d,v,"outcome_known",!r->outcome_unknown);
    yyjson_mut_obj_add_str(d,v,"result_source","worker");yyjson_mut_obj_add_uint(d,v,"retention_pins",r->pins);
    yyjson_mut_obj_add_uint(d,v,"result_version",r->result_version);
    yyjson_mut_obj_add_bool(d,v,"retired",r->retired);yyjson_mut_obj_add_sint(d,v,"minimum_terminal_retention_s",(r->retention_us+999999)/1000000);
    /* No wall-clock expiry promise for pinned or in-flight records. */
    yyjson_mut_obj_add_null(d,v,"result_expires_at");
    if(terminal(r)&&!r->pins&&!r->frame_references&&r->terminal_us)yyjson_mut_obj_add_sint(d,v,"result_expires_monotonic_us",r->terminal_us+r->retention_us);
    else yyjson_mut_obj_add_null(d,v,"result_expires_monotonic_us");
    yyjson_mut_obj_add_int(d,v,"error_code",r->error_code);yyjson_mut_obj_add_strcpy(d,v,"reason",r->reason);
    yyjson_mut_obj_add_sint(d,v,"accepted_us",r->accepted_us);yyjson_mut_obj_add_sint(d,v,"preparing_us",r->preparing_us);yyjson_mut_obj_add_sint(d,v,"prepared_us",r->prepared_us);
    yyjson_mut_obj_add_sint(d,v,"applying_us",r->applying_us);yyjson_mut_obj_add_sint(d,v,"applied_us",r->applied_us);yyjson_mut_obj_add_sint(d,v,"terminal_us",r->terminal_us);
    if(r->kind==NAKI_OP_UPDATE||r->kind==NAKI_OP_TOPOLOGY) {
        if(r->kind==NAKI_OP_TOPOLOGY){const char *phase=states[r->state];if(r->state==NAKI_OP_APPLIED)phase=!strcmp(r->action,"remove_rendition")?"removed":r->delivery==DELIVERY_WRITTEN?"active":"attached";yyjson_mut_obj_add_str(d,v,"phase",phase);}
        yyjson_mut_val *target=yyjson_mut_obj(d),*delivery=yyjson_mut_obj(d);
        yyjson_mut_obj_add_strcpy(d,target,"output_id",r->output_id);yyjson_mut_obj_add_strcpy(d,target,"component",r->component);
        if(*r->destination_id)yyjson_mut_obj_add_strcpy(d,target,"destination_id",r->destination_id);
        if(*r->action)yyjson_mut_obj_add_strcpy(d,v,"action",r->action);
        yyjson_mut_obj_add_val(d,v,"target",target);
        yyjson_mut_obj_add_uint(d,v,"expected_revision",r->expected_revision);yyjson_mut_obj_add_uint(d,v,"accepted_revision",r->revision);
        yyjson_mut_obj_add_str(d,delivery,"state",deliveries[r->delivery]);yyjson_mut_obj_add_uint(d,delivery,"inflight_frame_leases",r->frame_references);
        if(r->state==NAKI_OP_APPLIED&&r->applied.frame_id)yyjson_mut_obj_add_val(d,v,"applied_checkpoint",frame_json(d,&r->applied));else yyjson_mut_obj_add_null(d,v,"applied_checkpoint");
        if(r->delivery==DELIVERY_WRITTEN) {yyjson_mut_obj_add_val(d,delivery,"first_written",frame_json(d,&r->first_written));yyjson_mut_obj_add_sint(d,delivery,"written_us",r->first_written_us);}
        else yyjson_mut_obj_add_null(d,delivery,"first_written");
        yyjson_mut_obj_add_val(d,v,"delivery",delivery);
    }
    return v;
}
yyjson_mut_doc *naki_operations_get(NakiOperations *s,NakiOperationHandle id) {
    Operation copy;pthread_mutex_lock(&s->mutex);Operation *r=get(s,id);if(r)copy=*r;pthread_mutex_unlock(&s->mutex);if(!r)return NULL;
    yyjson_mut_doc *d=yyjson_mut_doc_new(NULL);if(d)yyjson_mut_doc_set_root(d,record_json(d,&copy));return d;
}
yyjson_mut_doc *naki_operations_page(NakiOperations *s,unsigned cursor,unsigned maximum,unsigned *next) {
    if(!maximum||maximum>8||cursor>s->capacity)return NULL;
    Operation copies[8];unsigned count=0;pthread_mutex_lock(&s->mutex);
    while(cursor<s->capacity&&count<maximum) {if(s->records[cursor].used)copies[count++]=s->records[cursor];cursor++;}
    *next=cursor<s->capacity?cursor:0;pthread_mutex_unlock(&s->mutex);
    yyjson_mut_doc *d=yyjson_mut_doc_new(NULL);if(!d)return NULL;yyjson_mut_val *root=yyjson_mut_obj(d),*records=yyjson_mut_arr(d);yyjson_mut_doc_set_root(d,root);
    for(unsigned i=0;i<count;i++)yyjson_mut_arr_add_val(records,record_json(d,&copies[i]));
    yyjson_mut_obj_add_val(d,root,"operations",records);yyjson_mut_obj_add_uint(d,root,"next_cursor",*next);return d;
}

static int read_frame(yyjson_val *v,NakiFrameIdentity *f,int written) {
    if(!yyjson_is_obj(v))return -EINVAL;
    *f=(NakiFrameIdentity){.frame_id=yyjson_get_uint(yyjson_obj_get(v,"frame_id")),
        .source_pts=yyjson_get_sint(yyjson_obj_get(v,"source_pts")),.source_us=yyjson_get_sint(yyjson_obj_get(v,"source_us")),
        .output_pts=yyjson_get_sint(yyjson_obj_get(v,"output_pts")),
        .source_tb_num=yyjson_get_int(yyjson_obj_get(v,"source_tb_num")),.source_tb_den=yyjson_get_int(yyjson_obj_get(v,"source_tb_den")),
        .output_tb_num=yyjson_get_int(yyjson_obj_get(v,"output_tb_num")),.output_tb_den=yyjson_get_int(yyjson_obj_get(v,"output_tb_den"))};
    return valid_frame(f,written)?0:-EINVAL;
}
int naki_operations_observe(NakiOperations *s,NakiOperationHandle id,yyjson_val *v,int64_t now) {
    const char *state=yyjson_get_str(yyjson_obj_get(v,"state")),*operation=yyjson_get_str(yyjson_obj_get(v,"operation_id")),*job=yyjson_get_str(yyjson_obj_get(v,"job_id")),*fp=yyjson_get_str(yyjson_obj_get(v,"intent_fingerprint"));
    if(!state||!operation||!job||!fp)return -EINVAL;
    int next=-1,delivery=-1;for(unsigned i=0;i<sizeof(states)/sizeof(*states);i++)if(!strcmp(state,states[i]))next=(int)i;
    yyjson_val *dv=yyjson_obj_get(v,"delivery");const char *ds=yyjson_get_str(yyjson_obj_get(dv,"state"));if(ds)for(unsigned i=0;i<sizeof(deliveries)/sizeof(*deliveries);i++)if(!strcmp(ds,deliveries[i]))delivery=(int)i;
    uint64_t version=yyjson_get_uint(yyjson_obj_get(v,"result_version")),refs=yyjson_get_uint(yyjson_obj_get(dv,"inflight_frame_leases"));
    if(next<0||delivery<0||!version||refs>128)return -EINVAL;
    NakiFrameIdentity applied={0},written={0};
    const char *kind=yyjson_get_str(yyjson_obj_get(v,"kind"));int topology=kind&&!strcmp(kind,"topology");
    if(next==NAKI_OP_APPLIED&&read_frame(yyjson_obj_get(v,"applied_checkpoint"),&applied,0)&&!(topology&&yyjson_is_null(yyjson_obj_get(v,"applied_checkpoint"))))return -EINVAL;
    if(delivery==DELIVERY_WRITTEN&&read_frame(yyjson_obj_get(dv,"first_written"),&written,1))return -EINVAL;
    pthread_mutex_lock(&s->mutex);Operation *r=get(s,id);int ret=-ESTALE;
    if(r&&!strcmp(operation,r->operation_id)&&!strcmp(job,r->job_id)&&!strcmp(fp,r->fingerprint)&&(r->kind==NAKI_OP_UPDATE||r->kind==NAKI_OP_TOPOLOGY)) {
        ret=0;if(version>r->remote_version) {
            r->remote_version=version;r->state=next;r->delivery=delivery;r->frame_references=refs;r->applied=applied;r->first_written=written;
            r->pins=(unsigned)yyjson_get_uint(yyjson_obj_get(v,"retention_pins"));r->retired=yyjson_get_bool(yyjson_obj_get(v,"retired"));
            r->error_code=yyjson_get_int(yyjson_obj_get(v,"error_code"));const char *why=yyjson_get_str(yyjson_obj_get(v,"reason"));if(why)snprintf(r->reason,sizeof(r->reason),"%s",why);
#define T(k) r->k=yyjson_get_sint(yyjson_obj_get(v,#k))
            T(preparing_us);T(prepared_us);T(applying_us);T(applied_us);T(terminal_us);
#undef T
            r->first_written_us=yyjson_get_sint(yyjson_obj_get(dv,"written_us"));close_delivery(r,now);changed(s,r);
        }
    }
    pthread_mutex_unlock(&s->mutex);return ret;
}
int naki_operations_holder_exited(NakiOperations *s,NakiOperationHandle id,int64_t now) {
    pthread_mutex_lock(&s->mutex);Operation *r=get(s,id);int ret=r?0:-ENOENT;
    if(r) {
        r->frame_references=0;r->pins=0;r->retired=1;
        if(r->state==NAKI_OP_APPLIED) {if(r->delivery!=DELIVERY_WRITTEN)r->delivery=DELIVERY_UNCERTAIN;}
        else if(!terminal(r)){r->state=NAKI_OP_FAILED;r->outcome_unknown=1;r->delivery=DELIVERY_UNCERTAIN;}
        snprintf(r->reason,sizeof(r->reason),"rendition holder exited; previously confirmed delivery is retained; missing acknowledgements are uncertain");
        close_delivery(r,now);changed(s,r);
    }
    pthread_mutex_unlock(&s->mutex);return ret;
}
