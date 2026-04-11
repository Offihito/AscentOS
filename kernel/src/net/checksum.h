#ifndef NET_CHECKSUM_H
#define NET_CHECKSUM_H

#include <stdint.h>
#include <stddef.h>

// Standard Internet Checksum (RFC 1071)
static inline uint16_t calculate_checksum(const void *data, size_t len) {
    uint32_t sum = 0;
    const uint16_t *buf = (const uint16_t *)data;

    while (len > 1) {
        sum += *buf++;
        len -= 2;
    }

    // Add remaining byte if length is odd
    if (len > 0) {
        sum += *(const uint8_t *)buf;
    }

    // Fold 32-bit sum into 16 bits
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return (uint16_t)(~sum);
}

#endif
