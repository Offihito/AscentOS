#ifndef NET_ICMP_H
#define NET_ICMP_H

#include <stdint.h>

#define ICMP_TYPE_ECHO_REPLY   0
#define ICMP_TYPE_ECHO_REQUEST 8

typedef struct __attribute__((packed)) {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t sequence;
} icmp_header_t;

// Handle an incoming ICMP packet
void icmp_handle_packet(const uint8_t *data, uint16_t len, uint32_t src_ip);

// Send an ICMP Echo Request (Ping)
int icmp_send_echo(uint32_t dst_ip, uint16_t id, uint16_t sequence, const void *data, uint16_t len);

#endif
