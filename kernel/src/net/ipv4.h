#ifndef NET_IPV4_H
#define NET_IPV4_H

#include <stdint.h>

#define PROTO_ICMP 1
#define PROTO_UDP  17
#define PROTO_TCP  6

typedef struct __attribute__((packed)) {
    uint8_t  version_ihl;   // Version (4) and IHL (Header Length)
    uint8_t  tos;           // Type of Service
    uint16_t length;        // Total Length
    uint16_t id;            // Identification
    uint16_t flags_offset;  // Flags (3 bits) and Fragment Offset (13 bits)
    uint8_t  ttl;           // Time to Live
    uint8_t  protocol;      // Protocol (ICMP, UDP, TCP)
    uint16_t checksum;      // Header Checksum
    uint32_t src_ip;        // Source IP Address
    uint32_t dst_ip;        // Destination IP Address
} ipv4_header_t;

// Handle an incoming IPv4 packet
void ipv4_handle_packet(const uint8_t *data, uint16_t len);

// Send an IPv4 packet
int ipv4_send_packet(uint32_t dst_ip, uint8_t protocol, const void *data, uint16_t len);

#endif
