// core/include/openpete_build_id.h — what this build calls itself.
//
// One source for the version string: `git describe --tags --always --dirty`
// at configure time, overridden by OPENPETE_DIST_VERSION, which is also what
// names the packaged artifact. The name on disk and the name on screen are
// therefore the same string.
//
// "Tagged" means the tree was exactly a release when it was configured: the
// describe resolved to a tag with no commits and no local modifications on
// top, or a version was handed in explicitly. An untagged build is a build
// somebody has to be able to identify from a screenshot, which is why the
// version line stays on screen for its whole session.

#ifndef OPENPETE_BUILD_ID_H
#define OPENPETE_BUILD_ID_H

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The version string this build shows on screen and names its artifact with.
 *
 * OPENPETE_DIST_VERSION when configure was given one, else the configure-time
 * `git describe` output; "unknown" when the configure step had no git.
 * @return a static string, never NULL */
const char *openpete_build_version(void);

/** @brief Whether the tree was exactly a release when it was configured.
 *
 * Set at configure time: an explicit OPENPETE_DIST_VERSION, or a
 * `git describe --tags --exact-match` that resolves on a tree without the
 * `-dirty` suffix. An untagged build keeps its version line on screen for the
 * whole session.
 * @return 1 for a release build, 0 otherwise */
int openpete_build_tagged(void);

/** @brief Whether the recomp module is linked the way the distribution presets link it.
 *
 * True for OPENPETE_RECOMP_LINKAGE=shared, the linkage every distributed
 * binary is built with, or an explicit -DOPENPETE_BUILD_SHARED_RECOMP=1. A
 * static-linkage binary shows no version line.
 * @return 1 for a shared-recomp build, 0 otherwise */
int openpete_build_shared_recomp(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_BUILD_ID_H */
