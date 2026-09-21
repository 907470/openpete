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

/* Never NULL; "unknown" when the configure step had no git. */
const char *openpete_build_version(void);

/* 1 = exactly a release, 0 = anything else. */
int openpete_build_tagged(void);

/* 1 when the recomp module is linked the way the distribution presets link
 * it (OPENPETE_RECOMP_LINKAGE=shared), which is what every binary that leaves
 * this machine is built from. A locally built static-linkage binary is not a
 * build anybody has to identify from a screenshot, and shows no version
 * line. */
int openpete_build_shared_recomp(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_BUILD_ID_H */
