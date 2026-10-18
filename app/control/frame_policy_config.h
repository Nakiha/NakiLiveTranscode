#ifndef NAKI_FRAME_POLICY_CONFIG_H
#define NAKI_FRAME_POLICY_CONFIG_H
#include "frame_policy.h"
#include <stddef.h>
#include <yyjson.h>
int naki_policy_config_parse(yyjson_val *root, NakiPolicyConfig *config, char *error, size_t cap);
#endif
