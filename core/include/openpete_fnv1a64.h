#ifndef OPENPETE_FNV1A64_H
#define OPENPETE_FNV1A64_H

/* FNV1a-64 — used by the determinism harness to hash the
 * gameplay-state regions per frame. Header-only so the unit test
 * can verify the implementation against published vectors without
 * pulling the whole runtime. See
 * notes/plans/openpete-determinism-enforcement.md Phase 1. */

#include <stddef.h>
#include <stdint.h>

#define OPENPETE_FNV1A64_OFFSET 0xcbf29ce484222325ULL
#define OPENPETE_FNV1A64_PRIME  0x00000100000001B3ULL

static inline uint64_t openpete_fnv1a64_init(void) {
    return OPENPETE_FNV1A64_OFFSET;
}

static inline uint64_t openpete_fnv1a64_update(uint64_t h,
                                               const void* data,
                                               size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= OPENPETE_FNV1A64_PRIME;
    }
    return h;
}

static inline uint64_t openpete_fnv1a64(const void* data, size_t len) {
    return openpete_fnv1a64_update(openpete_fnv1a64_init(), data, len);
}

#endif /* OPENPETE_FNV1A64_H */
