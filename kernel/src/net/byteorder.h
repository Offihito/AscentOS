#ifndef NET_BYTEORDER_H
#define NET_BYTEORDER_H

#include <stdint.h>

// x86_64 is little-endian; network byte order is big-endian.
// GCC/Clang provide __builtin_bswap* which compile to single instructions.

static inline uint16_t htons(uint16_t x) { return __builtin_bswap16(x); }
static inline uint16_t ntohs(uint16_t x) { return __builtin_bswap16(x); }
static inline uint32_t htonl(uint32_t x) { return __builtin_bswap32(x); }
static inline uint32_t ntohl(uint32_t x) { return __builtin_bswap32(x); }

#endif
