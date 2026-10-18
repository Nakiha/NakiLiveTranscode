#ifndef NAKI_INTENT_H
#define NAKI_INTENT_H
#include <stddef.h>
#include <yyjson.h>
/* Validate finite bounded JSON, including duplicate and embedded-NUL rejection. */
int naki_intent_validate(yyjson_val *value,char *error,size_t cap);
int naki_intent_fingerprint(yyjson_val *value,char out[65],char *error,size_t cap);
int naki_json_id(yyjson_val *value,size_t maximum);
#endif
