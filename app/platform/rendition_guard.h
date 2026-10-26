#ifndef NAKI_RENDITION_GUARD_H
#define NAKI_RENDITION_GUARD_H
/* Install after exec and collector setup, before authorizing a media mapping.
 * Kernel-enforced: allow codec threads, deny new processes, later exec and sendmsg/sendmmsg FD export.
 * The rendition runtime must not export pool FDs/mappings. This is a holder
 * lifetime boundary, not a sandbox against arbitrary malicious native code.
 * Failure is fatal to preparation; never silently run an unguarded holder. */
int naki_rendition_guard(void);
#endif
