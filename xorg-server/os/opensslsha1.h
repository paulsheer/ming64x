#ifndef OS_OPENSSLSHA1_H
#define OS_OPENSSLSHA1_H

#include <stddef.h>
#include <stdint.h>

#define SHA_DIGEST_LENGTH 20

typedef struct {
    uint32_t h[5];
    uint64_t count;
    uint8_t buf[64];
    int buf_idx;
} SHA_CTX;

int SHA1_Init(SHA_CTX *c);
int SHA1_Update(SHA_CTX *c, const void *data, size_t len);
int SHA1_Final(unsigned char *md, SHA_CTX *c);

#endif
