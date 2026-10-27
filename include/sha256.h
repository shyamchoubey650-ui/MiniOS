#ifndef SHA256_H
#define SHA256_H
/* SHA-256 (FIPS 180-4), freestanding. Used by the user manager to store salted, iterated
 * password hashes. Pure functions, no kernel dependencies (unit-tested on the host). */
#include "include/types.h"

#define SHA256_DIGEST 32
typedef struct { uint32_t h[8]; uint64_t bits; uint8_t block[64]; uint32_t used; } sha256_ctx_t;

void sha256_init(sha256_ctx_t *c);
void sha256_update(sha256_ctx_t *c, const void *data, size_t len);
void sha256_final(sha256_ctx_t *c, uint8_t out[SHA256_DIGEST]);
void sha256(const void *data, size_t len, uint8_t out[SHA256_DIGEST]);   /* one-shot */
#endif
