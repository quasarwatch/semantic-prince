/*
 * sha256.h - self-contained SHA-256 (FIPS 180-4).
 */

#ifndef SHA256_H
#define SHA256_H

#include <stdint.h>
#include <stddef.h>

typedef struct
{
  uint32_t h[8];
  uint8_t  buf[64];
  uint64_t len;         /* total message length in bytes */
  size_t   buflen;      /* bytes pending in buf */
} sha256_ctx;

void sha256_init(sha256_ctx *c);
void sha256_update(sha256_ctx *c, const void *data, size_t len);
void sha256_final(sha256_ctx *c, uint8_t out[32]);

/* helper: hex digest of a buffer, out must hold 65 bytes */
void sha256_hex(const void *data, size_t len, char out[65]);

#endif
