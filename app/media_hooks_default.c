#include "media_hooks.h"

int naki_media_hooks_open(const NakiMediaJobInfo *info, NakiMediaHooks *hooks) {
    (void)info;
    *hooks=(NakiMediaHooks){.api_version=NAKI_MEDIA_HOOK_API};
    return 0;
}
