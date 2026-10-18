#include "metrics.h"
void metric_add(Metric *metric, double ms, double slow_ms) {
    metric->count++; metric->sum_ms += ms;
    if (ms > metric->max_ms) metric->max_ms = ms;
    if(ms>=slow_ms) metric->slow_count++;
    int bin=ms<200 ? (int)ms : ms<1000 ? 200+(int)((ms-200)/20) : ms<16000 ? 240+(int)((ms-1000)/1000) : 255;
    if(bin<0)bin=0;
    metric->bins[bin]++;
}
