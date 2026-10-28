#ifndef NAKI_DESTINATION_LIFECYCLE_H
#define NAKI_DESTINATION_LIFECYCLE_H
#include <stdatomic.h>
#include "lifecycle_types.h"
/* Writer execution is atomic; logical attachment is serialized by the
 * destination owner mutex. Neither readiness nor done authorizes reclamation. */
typedef struct NakiWriterLife {atomic_uint _private_writer;} NakiWriterLife;
typedef enum NakiWriterPhase {NAKI_WRITER_OPENING,NAKI_WRITER_READY,NAKI_WRITER_DONE} NakiWriterPhase;
typedef struct NakiWriterView {NakiWriterPhase phase;NakiStopPolicy stop;NakiOutcome outcome;} NakiWriterView;
void naki_writer_init(NakiWriterLife *life);
NakiWriterView naki_writer_view(const NakiWriterLife *life);
int naki_writer_ready(NakiWriterLife *life);
void naki_writer_stop(NakiWriterLife *life);
void naki_writer_fail(NakiWriterLife *life);
void naki_writer_finish(NakiWriterLife *life);
static inline int naki_writer_failed(const NakiWriterLife *l){return naki_writer_view(l).outcome==NAKI_OUTCOME_FAILED;}
static inline int naki_writer_done(const NakiWriterLife *l){return naki_writer_view(l).phase==NAKI_WRITER_DONE;}
static inline int naki_writer_stopping(const NakiWriterLife *l){return naki_writer_view(l).stop!=NAKI_STOP_RUN;}
static inline int naki_writer_is_ready(const NakiWriterLife *l){NakiWriterView v=naki_writer_view(l);return v.phase==NAKI_WRITER_READY&&v.outcome==NAKI_OUTCOME_HEALTHY;}
typedef enum NakiDestinationPhase {NAKI_DEST_EMPTY,NAKI_DEST_PREPARING,NAKI_DEST_WAIT_RAP,NAKI_DEST_ACTIVE,NAKI_DEST_REMOVING} NakiDestinationPhase;
typedef struct NakiDestinationLife {unsigned _private_destination;} NakiDestinationLife;
typedef struct NakiDestinationView {NakiDestinationPhase phase;NakiHolderPhase holder;} NakiDestinationView;
NakiDestinationView naki_dl_view(const NakiDestinationLife *life);
int naki_dl_reserve(NakiDestinationLife *life,int primary);
int naki_dl_started(NakiDestinationLife *life);
int naki_dl_ready(NakiDestinationLife *life,const NakiWriterLife *writer);
int naki_dl_attach(NakiDestinationLife *life,const NakiWriterLife *writer,int verified_idr,int operation_committed);
int naki_dl_remove(NakiDestinationLife *life);
int naki_dl_joined(NakiDestinationLife *life);
int naki_dl_clear(NakiDestinationLife *life);
static inline int naki_dl_active(const NakiDestinationLife *l){return naki_dl_view(l).phase==NAKI_DEST_ACTIVE;}
static inline int naki_dl_removed(const NakiDestinationLife *l){return naki_dl_view(l).phase==NAKI_DEST_REMOVING;}
static inline int naki_dl_live(const NakiDestinationLife *l){return naki_dl_view(l).holder==NAKI_HOLDER_LIVE;}
#endif
