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

void openpete_sha256_init(openpete_sha256_ctx* c);
void openpete_sha256_update(openpete_sha256_ctx* c, const void* data, size_t len);
void openpete_sha256_final(openpete_sha256_ctx* c, uint8_t out[32]);

/* Hex-encode a finished digest (lowercase, NUL-terminated, 65 bytes). */
void openpete_sha256_hex(const uint8_t digest[32], char out[65]);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_SHA256_H */
