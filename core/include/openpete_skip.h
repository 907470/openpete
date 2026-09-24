// core/include/openpete_skip.h — plumbing shared by the games' skip modules
// (--skip-to-level, --skip-to-cutscene, --skip-to-area release blocks).

#ifndef OPENPETE_SKIP_H
#define OPENPETE_SKIP_H

#ifdef __cplusplus
extern "C" {
#endif

/* The skip release blocks' fail-safe: VBLs from a skip's first check to its
 * "fail-safe release (no playable edge)" line. 4000, or
 * OPENPETE_SKIP_FAILSAFE_VBL when set positive (the skip-target harness's
 * negative control sets 1). Read once. */
unsigned openpete_skip_failsafe_vbls(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_SKIP_H */
