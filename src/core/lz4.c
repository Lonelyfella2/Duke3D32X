/* lz4.c - LZ4 block decoder (matches tools/lz4enc.py). */
#include "lz4.h"

int lz4_decode(const void *srcv, uint32_t srclen, uint8_t *dst, uint32_t dstlen)
{
    const uint8_t *src = (const uint8_t *)srcv;
    const uint8_t *send = src + srclen;
    uint8_t *d = dst, *dend = dst + dstlen;
    while (src < send) {
        uint32_t tok = *src++;
        uint32_t lit = tok >> 4;
        if (lit == 15) {
            uint32_t b;
            do { b = *src++; lit += b; } while (b == 255);
        }
        if (d + lit > dend) return -1;
        while (lit--) *d++ = *src++;
        if (src >= send) break;
        uint32_t off = src[0] | ((uint32_t)src[1] << 8);
        src += 2;
        uint32_t ml = tok & 15;
        if (ml == 15) {
            uint32_t b;
            do { b = *src++; ml += b; } while (b == 255);
        }
        ml += 4;
        const uint8_t *m = d - off;
        if (m < dst || d + ml > dend) return -1;
        while (ml--) *d++ = *m++;
    }
    return (int)(d - dst);
}
