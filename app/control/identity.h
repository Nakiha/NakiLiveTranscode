#ifndef NAKI_IDENTITY_H
#define NAKI_IDENTITY_H
#include <stddef.h>
/* IDs are opaque ASCII tokens, never paths. UUID-sized entropy, no PID/time fallback. */
int naki_id_valid(const char *value,size_t maximum);
int naki_id_generate(char out[33]);
/* Initialize once before starting threads; immutable for this process life. */
int naki_process_init(void);
const char *naki_process_id(void);
#endif
