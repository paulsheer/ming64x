#include "os/opensslsha1.h"
#include <string.h>

#define ROL32(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void
sha1_transform(uint32_t h[5], const uint8_t data[64])
{
    uint32_t w[80];
    uint32_t a, b, c, d, e, f, k, t;
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t)data[i * 4] << 24) |
               ((uint32_t)data[i * 4 + 1] << 16) |
               ((uint32_t)data[i * 4 + 2] << 8) |
               ((uint32_t)data[i * 4 + 3]);
    }
    for (i = 16; i < 80; i++)
        w[i] = ROL32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4];

    for (i = 0; i < 80; i++) {
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }
        t = ROL32(a, 5) + f + e + k + w[i];
        e = d; d = c; c = ROL32(b, 30); b = a; a = t;
    }

    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

int
SHA1_Init(SHA_CTX *c)
{
    c->h[0] = 0x67452301;
    c->h[1] = 0xEFCDAB89;
    c->h[2] = 0x98BADCFE;
    c->h[3] = 0x10325476;
    c->h[4] = 0xC3D2E1F0;
    c->count = 0;
    c->buf_idx = 0;
    return 1;
}

int
SHA1_Update(SHA_CTX *c, const void *data, size_t len)
{
    const uint8_t *p = data;
    size_t i;

    for (i = 0; i < len; i++) {
        c->buf[c->buf_idx++] = p[i];
        if (c->buf_idx == 64) {
            sha1_transform(c->h, c->buf);
            c->count += 512;
            c->buf_idx = 0;
        }
    }
    return 1;
}

int
SHA1_Final(unsigned char *md, SHA_CTX *c)
{
    uint64_t total_bits;
    int i;

    total_bits = c->count + (uint64_t)c->buf_idx * 8;

    c->buf[c->buf_idx++] = 0x80;
    if (c->buf_idx > 56) {
        memset(c->buf + c->buf_idx, 0, 64 - c->buf_idx);
        sha1_transform(c->h, c->buf);
        c->buf_idx = 0;
    }
    memset(c->buf + c->buf_idx, 0, 56 - c->buf_idx);

    for (i = 0; i < 8; i++)
        c->buf[56 + i] = (uint8_t)(total_bits >> (56 - i * 8));
    sha1_transform(c->h, c->buf);

    for (i = 0; i < 5; i++) {
        md[i * 4]     = (uint8_t)(c->h[i] >> 24);
        md[i * 4 + 1] = (uint8_t)(c->h[i] >> 16);
        md[i * 4 + 2] = (uint8_t)(c->h[i] >> 8);
        md[i * 4 + 3] = (uint8_t)(c->h[i]);
    }
    return 1;
}
