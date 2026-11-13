#ifndef LZ4_H
#define LZ4_H
#include <stdint.h>
int lz4_decode(const void *src, uint32_t srclen, uint8_t *dst, uint32_t dstlen);
#endif
