/* openpete_str.h — small string helpers with no libc portability traps.
 *
 * Why this exists rather than <strings.h>: the tree carries more than a
 * dozen headers of its own named strings.h: one per Spyro overlay under
 * games/spyro-1/generated/rewritten, plus PsyCross's psx/strings.h.
 * Any translation unit compiled with one of those directories on its
 * include path resolves #include <strings.h> to a game header, so the
 * POSIX declaration never arrives and strcasecmp compiles as an implicit
 * int(). Windows hides it behind a different spelling again. */

#ifndef OPENPETE_STR_H
#define OPENPETE_STR_H

#ifdef __cplusplus
extern "C" {
#endif

/* ASCII case-insensitive compare, locale-independent by construction.
 * Callers compare file names and extensions, where locale-sensitive
 * folding would be wrong rather than merely surprising. */
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
