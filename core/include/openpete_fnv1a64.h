#ifndef OPENPETE_FNV1A64_H
#define OPENPETE_FNV1A64_H

/* FNV1a-64 — used by the determinism harness to hash the
 * gameplay-state regions per frame. Header-only so the unit test
 * can verify the implementation against published vectors without
 * pulling the whole runtime. */

#include <stddef.h>
#include <stdint.h>

#define OPENPETE_FNV1A64_OFFSET 0xcbf29ce484222325ULL
#define OPENPETE_FNV1A64_PRIME  0x00000100000001B3ULL

/** @brief Return the FNV-1a 64-bit offset basis, the hash of no bytes.
 *
 * @return OPENPETE_FNV1A64_OFFSET. */
static inline uint64_t openpete_fnv1a64_init(void) {
    return OPENPETE_FNV1A64_OFFSET;
}

/** @brief Fold len bytes into the running FNV-1a 64-bit hash h.
 *
 * For each byte, XORs it into h and multiplies by OPENPETE_FNV1A64_PRIME
 * modulo 2^64. Chained calls over consecutive pieces equal one call over
 * their concatenation.
 * @param h Running hash, from openpete_fnv1a64_init or a previous update.
 * @param data Bytes to fold; may be NULL when len is 0.
 * @param len Byte count.
 * @return The updated hash. */
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

/** @brief Return the FNV-1a 64-bit hash of len bytes.
 *
 * @param data Bytes to hash; may be NULL when len is 0.
 * @param len Byte count.
 * @return The hash. */
static inline uint64_t openpete_fnv1a64(const void* data, size_t len) {
    return openpete_fnv1a64_update(openpete_fnv1a64_init(), data, len);
}

#endif /* OPENPETE_FNV1A64_H */
