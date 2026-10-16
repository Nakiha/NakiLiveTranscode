#ifndef NAKI_NETSTATS_H
#define NAKI_NETSTATS_H
#include <stdatomic.h>
#include <stdint.h>
#include <yyjson.h>
#include <libavformat/network_observer.h>
typedef struct NetworkCounter {
    atomic_uint_fast64_t calls, bytes, errors, would_block, eof, background_calls;
    atomic_uint_fast64_t sum_us, max_us, syscall_us, last_success_us;
} NetworkCounter;
typedef struct NetworkStats { NetworkCounter total[4], window[4]; } NetworkStats;
void network_stats_reset(NetworkStats *stats);
void network_observe(void *opaque, const AVNetworkObservation *event);
yyjson_mut_val *network_stats_json(yyjson_mut_doc *doc, NetworkStats *stats, int reset_window);
#endif
