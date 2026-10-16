#ifndef NAKI_LOGGER_H
#define NAKI_LOGGER_H
#include <stddef.h>
#include <stdint.h>
#include <yyjson.h>
typedef struct Logger Logger;
typedef struct TraceSink TraceSink;
typedef struct LoggerOptions {
    unsigned queue_capacity, rotate_seconds;
    uint64_t rotate_bytes;
    /* Optional nonblocking transport for stdout records; called only by logger thread. */
    int (*stdout_write)(void *opaque,const char *text,size_t size);
    void *stdout_opaque;
} LoggerOptions;
typedef struct LoggerStats {
    uint64_t dropped_frames, dropped_status, io_errors, written, rotations;
    unsigned queued, high_water, capacity;
} LoggerStats;
Logger *logger_start(const LoggerOptions *options);
void logger_stop(Logger *logger);
TraceSink *logger_trace_open(Logger *logger, const char *prefix);
void logger_trace_finish(Logger *logger, TraceSink *sink);
void logger_trace_release(Logger *logger, TraceSink *sink);
/* Takes ownership of doc, including on queue overflow. Never waits for I/O. */
void logger_submit(Logger *logger, TraceSink *sink, yyjson_mut_doc *doc);
void logger_get_stats(Logger *logger, LoggerStats *out);
#endif
