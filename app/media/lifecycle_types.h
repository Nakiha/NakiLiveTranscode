#ifndef NAKI_LIFECYCLE_TYPES_H
#define NAKI_LIFECYCLE_TYPES_H
/* Shared meanings only. Each component owns its transition implementation. */
typedef enum NakiHolderPhase {NAKI_HOLDER_ABSENT,NAKI_HOLDER_LIVE,NAKI_HOLDER_REAPED} NakiHolderPhase;
typedef enum NakiStopPolicy {NAKI_STOP_RUN,NAKI_STOP_DRAIN,NAKI_STOP_ABORT} NakiStopPolicy;
typedef enum NakiOutcome {NAKI_OUTCOME_HEALTHY,NAKI_OUTCOME_FAILED} NakiOutcome;
#endif
