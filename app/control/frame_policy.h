#ifndef NAKI_FRAME_POLICY_H
#define NAKI_FRAME_POLICY_H
#include <stdint.h>

#define NAKI_OVERLOAD_LEVELS 3
#define NAKI_INFLIGHT_CAPACITY 512
enum { NAKI_TS_LEGACY, NAKI_TS_SOURCE_ALL };
enum { NAKI_INTRA_LEGACY, NAKI_INTRA_SOURCE, NAKI_INTRA_GRID, NAKI_INTRA_SOURCE_MAX };
enum { NAKI_I_FIRST=1, NAKI_I_SOURCE=2, NAKI_I_GRID=4, NAKI_I_MAX=8 };
enum { NAKI_TRACK_PACKET=1, NAKI_TRACK_FRAME=2 };
typedef struct NakiOverloadLevel { int enter_ms, sustain_ms, keep_one_in; } NakiOverloadLevel;
typedef struct NakiPolicyConfig {
    int timestamps, intra, interval_ms, overload_levels;
    NakiOverloadLevel levels[NAKI_OVERLOAD_LEVELS];
    int recover_below_ms, recover_sustain_ms, max_degraded_ms;
} NakiPolicyConfig;
typedef struct NakiTimeline {
    int have_pts;
    int64_t raw_last, last, offset;
    uint64_t wraps;
} NakiTimeline;
typedef struct NakiIntraState { int have_pts; int64_t anchor_us, last_us, grid; } NakiIntraState;
typedef struct NakiOverloadState {
    int level, recovering, failed;
    int64_t entered_us, recover_us, candidate_us[NAKI_OVERLOAD_LEVELS];
    uint64_t escalations, recoveries;
} NakiOverloadState;
typedef struct NakiInFlightEntry { int kind; uint64_t id; int64_t ingress_us; } NakiInFlightEntry;
typedef struct NakiInFlight { NakiInFlightEntry entries[NAKI_INFLIGHT_CAPACITY]; } NakiInFlight;

/* Strict display order, with only an identifiable modulo clock wrap repaired.
 * INT64_MIN means missing. Positive source gaps are retained without a limit. */
int naki_timeline_resolve(NakiTimeline *state, int64_t raw, int wrap_bits, int64_t *unwrapped);
int naki_intra_decide(const NakiPolicyConfig *config, const NakiIntraState *state,
                      int64_t pts_us, int source_i);
void naki_intra_accept(const NakiPolicyConfig *config, NakiIntraState *state,
                       int64_t pts_us, int reason);
void naki_overload_init(NakiOverloadState *state);
void naki_overload_tick(const NakiPolicyConfig *config, NakiOverloadState *state,
                        int64_t now_us, int64_t oldest_age_us);
int naki_frame_keep(const NakiPolicyConfig *config, int level, uint64_t frame_id, int protected_picture);
/* Caller serializes these short bounded operations; no I/O or allocation. */
int naki_inflight_add(NakiInFlight *state, int kind, uint64_t id, int64_t ingress_us);
int naki_inflight_remove(NakiInFlight *state, int kind, uint64_t id);
int64_t naki_inflight_age(const NakiInFlight *state, int64_t now_us);
#endif
