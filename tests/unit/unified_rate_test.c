#include "media/unified_rate.h"
#include "media/codec_progress.h"
#include <assert.h>
#include <stdio.h>
static NakiLiveRate model;
static NakiUnifiedRate u;
static void init(void) {memset(&model,0,sizeof(model));naki_unified_init(&u,&model,1100000,0,960*540,32,33333,1000000);}
static uint64_t admit(int index,double activity) {uint64_t seq;assert(!naki_unified_admit(&u,1000000+index*33333,1000000+index,activity,&seq));return seq;}
static void group(uint64_t ids[4]) {for(int i=0;i<4;i++)ids[i]=admit(i,1);int kinds[]={2,2,2,1};assert(!naki_unified_plan(&u,ids,kinds,4));}
typedef struct Fake {int result[4],send_count,receive_count,advance;uint64_t done;} Fake;
static int send_fake(void *opaque,const void *frame) {(void)frame;Fake *f=opaque;return f->result[f->send_count++];}
static int receive_fake(void *opaque) {Fake *f=opaque;f->receive_count++;f->done+=f->advance;return 0;}
static uint64_t progress_fake(void *opaque) {return ((Fake *)opaque)->done;}
static void null_tests(void) {
    Fake f={.result={-EAGAIN,0},.advance=1};NakiCodecProgress io={&f,send_fake,receive_fake,progress_fake};int sent=0;
    assert(!naki_codec_submit(&io,NULL,-EAGAIN,&sent)&&sent&&f.send_count==2&&f.receive_count==1);
    assert(!naki_codec_submit(&io,NULL,-EAGAIN,&sent)&&f.send_count==2);
    f=(Fake){.result={-EIO}};sent=0;assert(naki_codec_submit(&io,NULL,-EAGAIN,&sent)==-EIO&&!sent);
    f=(Fake){.result={-EAGAIN},.advance=0};sent=0;assert(naki_codec_submit(&io,NULL,-EAGAIN,&sent)==-EDEADLK&&!sent);
    f=(Fake){.result={-EAGAIN,-12345},.advance=1};sent=0;assert(naki_codec_submit(&io,NULL,-EAGAIN,&sent)==-12345&&!sent);
}
static void bias_tests(void) {
    int a,b;int64_t pa,pb;
    assert(!naki_unified_request_qp(32.1,200000,180000,0,0,&a,&pa));
    assert(!naki_unified_request_qp(32.1,200000,180000,0,NAKI_UNIFIED_MATCH_BIAS,&b,&pb));
    assert(a==33&&b==34&&pb<pa);
    assert(naki_unified_request_qp(32,200000,180000,0,NAN,&b,&pb)<0);
    assert(naki_unified_request_qp(32,200000,180000,0,-.1,&b,&pb)<0);
    assert(naki_unified_request_qp(32,200000,180000,0,1.1,&b,&pb)<0);
    uint64_t ids[4];init();assert(u.qp_bias==0);group(ids);int64_t quotas[4];
    for(int i=0;i<4;i++)quotas[i]=naki_unified_find(&u,ids[i])->quota;
    init();u.qp_bias=NAKI_UNIFIED_MATCH_BIAS;group(ids);
    for(int i=0;i<4;i++)assert(naki_unified_find(&u,ids[i])->quota==quotas[i]);
    NakiUnifiedItem *f=naki_unified_find(&u,ids[0]);NakiRateDecision d=f->decision;
    assert(!naki_unified_try(&u,ids[0])&&!naki_unified_accept(&u,ids[0]));
    int64_t bits=10000;double prior=model.coefficient[2];
    double observed=bits*exp2((d.qp-32)/6.)/(u.pixels*d.complexity);
    double expected=.8*prior+.2*naki_rate_clip(prior*.25,prior*4,observed);
    assert(!naki_unified_actual(&u,ids[0],bits,2,933334));
    assert(fabs(model.coefficient[2]-expected)<1e-12); /* final requested QP normalization */
    int kinds[3]={2,2,1};double after=model.coefficient[2];
    assert(after!=prior&&!naki_unified_plan(&u,ids+1,kinds,3));
    assert(naki_unified_find(&u,ids[1])->decision.model_coefficient==after);
}
int main(void) {
    uint64_t ids[4];int kind=3;init();uint64_t seq=admit(0,1);
    assert(!naki_unified_plan(&u,&seq,&kind,1));NakiUnifiedItem *f=naki_unified_find(&u,seq);int64_t q=f->quota;
    assert(u.C==q&&!u.D&&model.pending_bits==q);
    assert(!naki_unified_try(&u,seq));NakiUnifiedItem frozen=*f;
    assert(naki_unified_plan(&u,&seq,&kind,1)<0&&!memcmp(&frozen,f,sizeof(frozen)));
    assert(!naki_unified_accept(&u,seq)&&!u.C&&u.D==q);
    assert(naki_unified_accept(&u,seq)<0&&u.accepted==1&&u.D==q);
    assert(!naki_unified_actual(&u,seq,q+123,0,933334)&&u.D==q+123);
    assert(!naki_unified_can_write(&u,seq,933334));
    int64_t before=1100000-u.D;
    assert(!naki_unified_written(&u,seq,933334,1000000));
    assert(1100000-model.history[0].bits-u.D==before&&!u.C&&!u.D&&!model.pending_bits);
    assert(u.absolute_credit==(1100000-q-123)*1000000LL);
    init();group(ids);int64_t quotas[4];for(int i=0;i<4;i++)quotas[i]=naki_unified_find(&u,ids[i])->quota;
    uint64_t extra=0;assert(naki_unified_admit(&u,1200000,1000001,1,&extra)<0&&u.admitted==4);
    for(int i=0;i<2;i++) {assert(!naki_unified_try(&u,ids[i]));assert(!naki_unified_accept(&u,ids[i]));}
    int64_t accepted=u.D;assert(!naki_unified_cancel(&u)&&!u.C&&u.D==accepted&&model.pending_bits==accepted);
    assert(u.accepted==2&&!u.written&&!u.feedback);
    init();for(int i=0;i<4;i++)ids[i]=admit(i,.25);int kinds[]={2,2,2,1};assert(!naki_unified_plan(&u,ids,kinds,4));
    for(int i=0;i<4;i++)assert(naki_unified_find(&u,ids[i])->quota==quotas[i]);
    init();for(int i=0;i<4;i++)ids[i]=admit(i,4);assert(!naki_unified_plan(&u,ids,kinds,4));
    for(int i=0;i<4;i++)assert(naki_unified_find(&u,ids[i])->quota==quotas[i]);
    init();seq=admit(0,1);model.count=1;model.history[0]=(NakiRateEntry){900000,1000000,1020000};
    assert(!naki_unified_plan(&u,&seq,&kind,1)&&u.margin_used&&u.margin_uses==1);
    assert(u.minimum>u.soft_room&&u.minimum<=u.real_room&&u.C<=u.real_room);
    init();seq=admit(0,1);model.count=1;model.history[0]=(NakiRateEntry){900000,1000000,1099900};
    assert(naki_unified_plan(&u,&seq,&kind,1)<0&&!strcmp(u.reason,"MODEL_PREFLIGHT_FAIL_REAL")&&!u.C&&!u.D);
    init();NakiUnifiedSource source={1900000,1000000,800};assert(!naki_unified_source(&u,&source));
    assert(!naki_unified_clock(&u,1950000)&&u.source_rate==800&&u.source_credit==40000000);
    u.source_credit=-100000000;assert(!naki_unified_clock(&u,2000000)&&!u.source_rate&&u.source_credit==-60000000);
    assert(!naki_unified_clock(&u,2100000)&&u.source_credit==-60000000);
    assert(!naki_unified_source(&u,&source)&&!u.source_count); /* original expiry, no resurrection */
    init();assert(naki_unified_admit(&u,1000000,1000000,NAN,&seq)<0&&!u.admitted);
    null_tests();bias_tests();puts("unified_rate_test: PASS; ledger, gate, margin, source, NULL, zero/fixed bias, identical quotas, final-QP dynamic feedback");return 0;
}
