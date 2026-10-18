#ifndef NAKI_COMPONENT_UPDATE_H
#define NAKI_COMPONENT_UPDATE_H
#include "watermark_config.h"
enum NakiWatermarkPresence {
    NAKI_PATCH_ENABLED=1U<<0,NAKI_PATCH_URI=1U<<1,NAKI_PATCH_LOAD_TIMEOUT=1U<<2,
    NAKI_PATCH_X=1U<<3,NAKI_PATCH_Y=1U<<4,NAKI_PATCH_WIDTH=1U<<5,NAKI_PATCH_HEIGHT=1U<<6,
    NAKI_PATCH_ANCHOR=1U<<7,NAKI_PATCH_FIT=1U<<8,NAKI_PATCH_OPACITY=1U<<9
};
typedef struct NakiWatermarkPatch {unsigned present;NakiWatermarkConfig values;} NakiWatermarkPatch;
typedef struct NakiUpdateRequest {
    char operation_id[129],output_id[33],fingerprint[65];
    uint64_t expected_revision;
    NakiWatermarkPatch patch;
} NakiUpdateRequest;
int naki_update_parse(yyjson_val *request,const char *job_id,NakiUpdateRequest *out,char *error,size_t capacity);
int naki_watermark_patch_apply(const NakiWatermarkConfig *desired,const NakiWatermarkPatch *patch,
                             NakiWatermarkConfig *merged,char *error,size_t capacity);
yyjson_mut_val *naki_watermark_config_json(yyjson_mut_doc *document,const NakiWatermarkConfig *config);
int naki_update_from_legacy(const NakiWatermarkConfig *config,uint64_t revision,const char *job_id,const char *output_id,
                            const char *operation_id,NakiUpdateRequest *out,char *error,size_t capacity);
#endif
