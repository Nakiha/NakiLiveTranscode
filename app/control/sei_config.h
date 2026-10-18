#ifndef NAKI_SEI_CONFIG_H
#define NAKI_SEI_CONFIG_H
#include <stddef.h>
#include <stdint.h>
#include <yyjson.h>
#define NAKI_SEI_TYPES 64
#define NAKI_SEI_UNION_TYPES (4 * NAKI_SEI_TYPES)
typedef struct NakiSeiConfig { unsigned count; uint32_t types[NAKI_SEI_UNION_TYPES]; } NakiSeiConfig;
int naki_sei_config(yyjson_val *value,NakiSeiConfig *out,char *error,size_t cap);
int naki_sei_union(NakiSeiConfig *out,const NakiSeiConfig *add);
int naki_sei_selected(const NakiSeiConfig *config,uint32_t type);

#endif
