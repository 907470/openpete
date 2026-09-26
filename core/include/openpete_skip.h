// core/include/openpete_skip.h — plumbing shared by the games' skip modules
// (--skip-to-level, --skip-to-cutscene, --skip-to-area release blocks).

#ifndef OPENPETE_SKIP_H
#define OPENPETE_SKIP_H

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Return the skip fail-safe: VBLs from a skip's first release check to its forced release.
 *
 * A skip release block that sees no playable edge by this deadline
 * releases anyway and logs its "fail-safe release (no playable edge)" line.
 * 4000, or OPENPETE_SKIP_FAILSAFE_VBL when set positive (the skip-target
 * harness's negative control sets 1). The variable is read once.
 * @return The fail-safe in VBLs, at least 1. */
unsigned openpete_skip_failsafe_vbls(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_SKIP_H */
