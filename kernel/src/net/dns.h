#ifndef NET_DNS_H
#define NET_DNS_H

#include <stdint.h>
#include <stdbool.h>

#define DNS_PORT 53

typedef struct __attribute__((packed)) {
    uint16_t id;
    uint16_t flags;
    uint16_t q_count;
    uint16_t ans_count;
    uint16_t auth_count;
    uint16_t add_count;
} dns_header_t;

// Synchronously resolves an A record. Returns 0 on success, -1 on timeout/failure.
// Uses UDP port internally and polls for response.
int dns_resolve_A_record(const char *domain, uint32_t *out_ip);

#endif
