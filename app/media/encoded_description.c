#include "encoded_description.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <limits.h>
struct NakiEncodedDescription {
    atomic_uint references;
    AVCodecParameters *parameters;
    AVRational time_base;
};
NakiEncodedDescription *naki_encoded_seal(const AVCodecContext *encoder) {
    if(!encoder||encoder->time_base.num<=0||encoder->time_base.den<=0)return NULL;
    NakiEncodedDescription *d=calloc(1,sizeof(*d));if(!d)return NULL;
    d->parameters=avcodec_parameters_alloc();
    if(!d->parameters||avcodec_parameters_from_context(d->parameters,encoder)<0) {
        avcodec_parameters_free(&d->parameters);free(d);return NULL;
    }
    d->time_base=encoder->time_base;atomic_init(&d->references,1);return d;
}
NakiEncodedDescription *naki_encoded_ref(NakiEncodedDescription *d) {
    if(d) {unsigned refs=atomic_fetch_add_explicit(&d->references,1,memory_order_relaxed);if(!refs||refs==UINT_MAX)abort();}
    return d;
}
void naki_encoded_unref(NakiEncodedDescription **slot) {
    NakiEncodedDescription *d=*slot;*slot=NULL;if(!d)return;
    unsigned refs=atomic_fetch_sub_explicit(&d->references,1,memory_order_acq_rel);if(!refs)abort();
    if(refs==1) {avcodec_parameters_free(&d->parameters);free(d);}
}
NakiEncodedInfo naki_encoded_info(const NakiEncodedDescription *d) {
    return d?(NakiEncodedInfo){d->time_base,d->parameters->extradata,d->parameters->extradata_size}:(NakiEncodedInfo){0};
}
int naki_encoded_parameters(const NakiEncodedDescription *d,AVCodecParameters *parameters) {
    return d?avcodec_parameters_copy(parameters,d->parameters):AVERROR(EINVAL);
}
