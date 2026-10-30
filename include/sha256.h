#ifndef NEO_SHA256_H
#define NEO_SHA256_H

#include <nitekernel.h>

#define SHA256_DIGEST_SIZE 32

struct sha256_ctx {
    u32 state[8];
    u64 bitlen;
    u8  buffer[64];
    u32 buflen;
};

void sha256_init(struct sha256_ctx *ctx);
void sha256_update(struct sha256_ctx *ctx, const void *data, usize len);
void sha256_final(struct sha256_ctx *ctx, u8 out[SHA256_DIGEST_SIZE]);
void sha256(const void *data, usize len, u8 out[SHA256_DIGEST_SIZE]);
void sha256_hex(const u8 digest[SHA256_DIGEST_SIZE], char out[65]);
int  sha256_hex_equal(const u8 digest[SHA256_DIGEST_SIZE], const char *hex);

#endif
