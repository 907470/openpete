/* openpete_str.h - small string helpers with no libc portability traps.
 *
 * op_strcasecmp folds ASCII only, so its result does not depend on the
 * host locale the way strcasecmp's does, and it has one spelling on every
 * platform (MSVC names the libc function _stricmp).
 *
 * op_appendf appends formatted text to a fixed buffer and keeps the length
 * it tracks at what the buffer holds: snprintf returns the length the text
 * would have had, and a caller that adds that return to its offset walks
 * past the buffer on the first truncation. */

#ifndef OPENPETE_STR_H
#define OPENPETE_STR_H

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

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

/** @brief Append printf-formatted text at buf + *len, keeping *len at the bytes the buffer holds.
 *
 * The buffer stays NUL-terminated. On truncation the text is cut at
 * cap - 1 bytes and *len becomes cap - 1, so *len never exceeds what was
 * written and a later write(buf, *len) reads only initialized bytes. A
 * call on a full buffer, or with cap 0, writes nothing.
 * @param buf the buffer.
 * @param cap the buffer's size in bytes.
 * @param len the bytes already in buf (at most cap - 1); advanced by the append.
 * @param fmt the printf format.
 * @return 0 when the whole text fit, -1 when it was truncated or failed to format. */
static inline int op_appendf(char* buf, size_t cap, size_t* len, const char* fmt, ...) {
    if (!cap || *len >= cap - 1) {
        if (cap) *len = cap - 1;
        return -1;
    }
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf + *len, cap - *len, fmt, ap);
    va_end(ap);
    if (r < 0) {
        buf[*len] = '\0';
        return -1;
    }
    if ((size_t)r >= cap - *len) {
        *len = cap - 1;
        return -1;
    }
    *len += (size_t)r;
    return 0;
}

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_STR_H */
