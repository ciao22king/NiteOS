#include <sha256.h>
#include <string.h>

static const u32 K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(struct sha256_ctx *ctx, const u8 *p)
{
    u32 w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((u32)p[i * 4] << 24) | ((u32)p[i * 4 + 1] << 16) |
               ((u32)p[i * 4 + 2] << 8) | (u32)p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        u32 s0 = ROTR(w[i - 15], 7) ^ ROTR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        u32 s1 = ROTR(w[i - 2], 17) ^ ROTR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    u32 a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
    u32 e = ctx->state[4], f = ctx->state[5], g = ctx->state[6], h = ctx->state[7];

    for (int i = 0; i < 64; i++) {
        u32 S1 = ROTR(e, 6) ^ ROTR(e, 11) ^ ROTR(e, 25);
        u32 ch = (e & f) ^ (~e & g);
        u32 t1 = h + S1 + ch + K[i] + w[i];
        u32 S0 = ROTR(a, 2) ^ ROTR(a, 13) ^ ROTR(a, 22);
        u32 maj = (a & b) ^ (a & c) ^ (b & c);
        u32 t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

void sha256_init(struct sha256_ctx *ctx)
{
    ctx->state[0] = 0x6a09e667; ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372; ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f; ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab; ctx->state[7] = 0x5be0cd19;
    ctx->bitlen = 0;
    ctx->buflen = 0;
}

void sha256_update(struct sha256_ctx *ctx, const void *data, usize len)
{
    const u8 *p = data;
    ctx->bitlen += (u64)len * 8;
    while (len) {
        u32 take = 64 - ctx->buflen;
        if (take > len) take = (u32)len;
        memcpy(ctx->buffer + ctx->buflen, p, take);
        ctx->buflen += take;
        p += take;
        len -= take;
        if (ctx->buflen == 64) {
            sha256_block(ctx, ctx->buffer);
            ctx->buflen = 0;
        }
    }
}

void sha256_final(struct sha256_ctx *ctx, u8 out[SHA256_DIGEST_SIZE])
{
    u64 bitlen = ctx->bitlen;
    u8 pad = 0x80;
    sha256_update(ctx, &pad, 1);
    u8 zero = 0;
    while (ctx->buflen != 56) sha256_update(ctx, &zero, 1);
    u8 lenbytes[8];
    for (int i = 0; i < 8; i++) lenbytes[i] = (u8)(bitlen >> (56 - i * 8));
    /* Bypass bitlen accounting for the length field itself. */
    memcpy(ctx->buffer + ctx->buflen, lenbytes, 8);
    ctx->buflen = 64;
    sha256_block(ctx, ctx->buffer);

    for (int i = 0; i < 8; i++) {
        out[i * 4] = (u8)(ctx->state[i] >> 24);
        out[i * 4 + 1] = (u8)(ctx->state[i] >> 16);
        out[i * 4 + 2] = (u8)(ctx->state[i] >> 8);
        out[i * 4 + 3] = (u8)(ctx->state[i]);
    }
}

void sha256(const void *data, usize len, u8 out[SHA256_DIGEST_SIZE])
{
    struct sha256_ctx ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, out);
}

static const char hexdigits[] = "0123456789abcdef";

void sha256_hex(const u8 digest[SHA256_DIGEST_SIZE], char out[65])
{
    for (int i = 0; i < SHA256_DIGEST_SIZE; i++) {
        out[i * 2] = hexdigits[digest[i] >> 4];
        out[i * 2 + 1] = hexdigits[digest[i] & 0x0F];
    }
    out[64] = '\0';
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int sha256_hex_equal(const u8 digest[SHA256_DIGEST_SIZE], const char *hex)
{
    for (int i = 0; i < SHA256_DIGEST_SIZE; i++) {
        int hi = hexval(hex[i * 2]);
        int lo = hexval(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return 0;
        if ((u8)((hi << 4) | lo) != digest[i]) return 0;
    }
    return 1;
}
