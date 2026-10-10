/* openpete_str.h - small string helpers with no libc portability traps.
 *
 * Why this exists rather than <strings.h>: PsyCross carries a header of its
 * own named strings.h (psx/strings.h). Any translation unit compiled with
 * that directory on its include path resolves #include <strings.h> to it,
 * so the POSIX declaration never arrives and strcasecmp compiles as an
 * implicit int(). Windows hides it behind a different spelling again. */

#ifndef OPENPETE_STR_H
#define OPENPETE_STR_H

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Compare two NUL-terminated strings with ASCII case folding, independent of the locale.
 *
 * Only 'A'..'Z' fold to lower case; other bytes compare as unsigned char.
 * Callers compare directory entry names and mod names, whose matching and
 * order must not change with the host locale.
 * @param a first string.
 * @param b second string.
 * @return negative, zero or positive as a folds before, equal to or after b. */
static inline int op_strcasecmp(const char* a, const char* b) {
    unsigned char ca, cb;
    for (;;) {
        ca = (unsigned char)*a++;
        cb = (unsigned char)*b++;
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb - 'A' + 'a');
        if (ca != cb) return (int)ca - (int)cb;
        if (!ca) return 0;
    }
}

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_STR_H */
