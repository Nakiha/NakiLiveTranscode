#ifndef NAKI_LOGGER_H
#define NAKI_LOGGER_H
#include <stddef.h>
#include <stdint.h>
#include <yyjson.h>
typedef struct Logger Logger;
typedef struct TraceSink TraceSink;
typedef enum LoggerCategory {LOGGER_EVENT,LOGGER_FRAME_TRACE} LoggerCategory;
typedef struct LoggerOptions {
    unsigned queue_capacity, rotate_seconds;
    uint64_t rotate_bytes;
    /* Optional nonblocking transport for stdout records; called only by logger thread. */
    int (*stdout_write)(void *opaque,const char *text,size_t size);
    void *stdout_opaque;
    char event_prefix[4096],role[16],instance_id[33];
    unsigned event_rotate_seconds,flush_interval_ms;
    uint64_t event_rotate_bytes;
} LoggerOptions;
typedef struct LoggerStats {
    uint64_t dropped_frames, dropped_status, io_errors, written, rotations;
    unsigned queued, high_water, capacity;
    uint64_t event_sequence;
} LoggerStats;
/* Immutable while the logger is alive; empty means stdout-only. */
const char *logger_event_prefix(const Logger *logger);
Logger *logger_start(const LoggerOptions *options);
void logger_stop(Logger *logger);
TraceSink *logger_trace_open(Logger *logger, const char *prefix);
void logger_trace_finish(Logger *logger, TraceSink *sink);
void logger_trace_release(Logger *logger, TraceSink *sink);
/* Takes ownership of doc, including on rejection. Never waits for I/O.
 * Reservations preserve order across concurrent preparation. Queue counts include
 * preparing records; bounded preparation overflow uses the normal loss counters.
 * Do not start submissions with a released sink or after starting logger_stop.
 * Stop waits for already reserved preparations; callers must let them complete. */
void logger_submit(Logger *logger, TraceSink *sink, yyjson_mut_doc *doc);
void logger_submit_record(Logger *logger,LoggerCategory category,TraceSink *sink,yyjson_mut_doc *doc);
void logger_get_stats(Logger *logger, LoggerStats *out);
#endif
