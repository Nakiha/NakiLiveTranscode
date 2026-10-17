#ifndef NAKI_WATERMARK_CONFIG_H
#define NAKI_WATERMARK_CONFIG_H
#include <stddef.h>
#include <stdint.h>
#include <yyjson.h>
typedef struct NakiWatermarkConfig {
    char uri[2048];
    int enabled, required, load_timeout_ms, startup_timeout_ms;
    int width, height, x, y, anchor, stretch;
    double opacity;
} NakiWatermarkConfig;
/* NULL is the omitted, disabled initial configuration. All groups are strict. */
int naki_watermark_config(yyjson_val *value, NakiWatermarkConfig *out, char *error, size_t cap);
int naki_watermark_update(yyjson_val *root, NakiWatermarkConfig *out, uint64_t *revision, char *error, size_t cap);
#endif
