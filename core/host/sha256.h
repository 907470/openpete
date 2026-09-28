// core/host/sha256.h — minimal SHA-256 (FIPS 180-4) for content
// addressing: disc-library ingest keys, retail-hash identification,
// release stamps. Not a crypto library — no HMAC, no side-channel
// hardening; identity hashing only.
#ifndef OPENPETE_SHA256_H
#define OPENPETE_SHA256_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t state[8];
    uint64_t total;
    uint8_t  buf[64];
    size_t   buflen;
} openpete_sha256_ctx;

/** @brief Reset a context to the SHA-256 initial state with no bytes hashed.
 * @param c Context to initialise. */
void openpete_sha256_init(openpete_sha256_ctx* c);

/** @brief Append bytes to the running hash.
 *
 * Whole 64-byte blocks are compressed as they complete; the remainder is
 * held in the context buffer. Calls may split the input at any byte.
 * @param c Context initialised by openpete_sha256_init.
 * @param data Input bytes.
 * @param len Byte count. */
void openpete_sha256_update(openpete_sha256_ctx* c, const void* data, size_t len);

/** @brief Pad the message, compress the last block and write the 32-byte digest.
 *
 * The padding is 0x80, zeros to 56 mod 64, then the 64-bit big-endian bit
 * length. The context is left in its padded state and must be
 * re-initialised before reuse.
 * @param c Context holding the hashed message.
 * @param out Receives the digest, big-endian. */
void openpete_sha256_final(openpete_sha256_ctx* c, uint8_t out[32]);

/** @brief Write a 32-byte digest as 64 lowercase hex digits and a NUL into out.
 *
 * @param digest Finished digest from openpete_sha256_final.
 * @param out Destination of 65 bytes. */
void openpete_sha256_hex(const uint8_t digest[32], char out[65]);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_SHA256_H */
