#ifndef NAKI_TEST_PIPELINE_LIFECYCLE_H
#define NAKI_TEST_PIPELINE_LIFECYCLE_H
#include "media_hooks.h"
#include <yyjson.h>
void naki_test_lifecycle_open(const NakiMediaJobInfo *info);
void naki_test_lifecycle_close(void);
int naki_test_lifecycle_video(void);
void naki_test_lifecycle_snapshot(yyjson_mut_doc *doc);
#endif
