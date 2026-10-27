#include "media/rendition_runtime_internal.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
/* These must not be reached by an unclaimed action or a closing session. The
 * positive ready/attached path is covered by the real lifecycle integration. */
int pipeline_reader_epoch_ready(Pipeline *p,int64_t generation){(void)p;(void)generation;abort();}
NakiFrameIdentity pipeline_frame_identity(Pipeline *p,const Stamp *s,int64_t pts,AVRational tb){(void)p;(void)s;(void)pts;(void)tb;abort();}
int main(void){
    OutputConfig spec={.io_timeout_ms=10000,.watermark={.enabled=1,.required=1,.startup_timeout_ms=30000}};
    assert(naki_rendition_prepare_deadline(&spec,100)==42000100);
    spec.watermark.required=0;assert(naki_rendition_prepare_deadline(&spec,100)==12000100);
    Job *j=calloc(1,sizeof(*j));Pipeline *p=calloc(1,sizeof(*p));NakiRenditions *m=calloc(1,sizeof(*m));assert(j&&p&&m);
    j->renditions=m;p->job=j;m->job=j;m->source_generation=1;
    m->slots[0].pid=1;m->slots[0].prepared=1;m->slots[0].prepare_source_generation=1;
    m->transition.operation=1;m->transition.slot=0;m->transition.revision=2;
    for(int action=NAKI_RENDITION_ADD;action<=NAKI_RENDITION_RESTART;action++){
        m->transition.action=action;m->transition.started=0;
        assert(!naki_rendition_attach(p,NULL));assert(!m->slots[0].attached&&m->transition.operation==1);
    }
    m->transition.action=NAKI_RENDITION_REMOVE;m->transition.started=1;
    assert(!naki_rendition_attach(p,NULL));
    m->transition.action=NAKI_RENDITION_ADD;m->finishing=1;
    assert(!naki_rendition_attach(p,NULL));m->finishing=0;atomic_store(&j->lifecycle.cancel,1);
    assert(!naki_rendition_attach(p,NULL));assert(!m->slots[0].attached);
    free(m);free(p);free(j);puts("PASS reserved remove/add/restart cannot attach before owner preparation; stop/EOF cannot attach");return 0;
}
