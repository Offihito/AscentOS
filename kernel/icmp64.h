#ifndef ICMP64_H
#define ICMP64_H

#include <stdint.h>
#include <stdbool.h>
#include "network64.h"
#include <stddef.h>

// ============================================================================
// ICMP MESSAGE TYPES
// ============================================================================

#define ICMP_ECHO_REPLY      0
#define ICMP_DEST_UNREACHABLE 3
#define ICMP_SOURCE_QUENCH   4
#define ICMP_REDIRECT        5
#define ICMP_ECHO_REQUEST    8
#define ICMP_TIME_EXCEEDED   11
#define ICMP_PARAMETER_PROBLEM 12
#define ICMP_TIMESTAMP       13
#define ICMP_TIMESTAMP_REPLY 14
#define ICMP_INFO_REQUEST    15
#define ICMP_INFO_REPLY      16

// ============================================================================
// ICMP HEADER STRUCTURE
// ============================================================================

typedef struct __attribute__((packed)) {
    uint8_t  type;          // ICMP type
    uint8_t  code;          // ICMP code
    uint16_t checksum;      // Checksum
    uint16_t identifier;    // Identifier (for echo)
    uint16_t sequence;      // Sequence number (for echo)
} ICMPHeader;

// ============================================================================
// IP HEADER STRUCTURE (for ICMP)
// ============================================================================

typedef struct __attribute__((packed)) {
    uint8_t  version_ihl;   // Version (4 bits) + IHL (4 bits)
    uint8_t  tos;           // Type of Service
    uint16_t total_length;  // Total length
    uint16_t id;            // Identification
    uint16_t flags_offset;  // Flags (3 bits) + Fragment offset (13 bits)
    uint8_t  ttl;           // Time to Live
    uint8_t  protocol;      // Protocol (1 = ICMP)
    uint16_t checksum;      // Header checksum
    uint32_t source_ip;     // Source IP address
    uint32_t dest_ip;       // Destination IP address
} IPv4Header;

#define IP_PROTOCOL_ICMP 1
#define IP_VERSION_4     4
#define IP_IHL_DEFAULT   5  // 5 * 4 = 20 bytes

// ============================================================================
// ETHERNET HEADER STRUCTURE
// ============================================================================

typedef struct __attribute__((packed)) {
    uint8_t  dest_mac[6];   // Destination MAC
    uint8_t  src_mac[6];    // Source MAC
    uint16_t ethertype;     // EtherType (0x0800 = IPv4)
} EthernetHeader;

#define ETHERTYPE_IPV4 0x0800
#define ETHERTYPE_ARP  0x0806

// ============================================================================
// PING STRUCTURE
// ============================================================================

#define PING_DATA_SIZE 32
#define PING_TIMEOUT_MS 5000

typedef struct {
    IPv4Address target;
    uint16_t sequence;
    uint16_t identifier;
    uint32_t send_time;      // Timestamp when sent
    bool waiting_reply;
    uint8_t ttl;
    uint32_t rtt_ms;         // Round-trip time in milliseconds
} PingRequest;

// ============================================================================
// PING STATISTICS
// ============================================================================

typedef struct {
    uint32_t sent;
    uint32_t received;
    uint32_t lost;
    uint32_t min_rtt;
    uint32_t max_rtt;
    uint32_t avg_rtt;
} PingStats;

// ============================================================================
// FUNCTION PROTOTYPES
// ============================================================================

// ICMP Initialization
void icmp_init(void);

// Checksum calculation
uint16_t icmp_checksum(const void* data, size_t length);
uint16_t ip_checksum(const void* data, size_t length);

// Ping operations
bool icmp_send_echo_request(const IPv4Address* target, uint16_t sequence);
bool icmp_process_echo_reply(const uint8_t* packet, uint16_t length);

// High-level ping interface
bool ping_host(const IPv4Address* target, uint8_t count, PingStats* stats);
void ping_print_stats(const PingStats* stats, const IPv4Address* target);

// Packet building
void icmp_build_echo_packet(uint8_t* buffer, const IPv4Address* target, 
                           uint16_t sequence, uint16_t* total_length);

// Time utilities
uint32_t get_ticks_ms(void);
void delay_ms(uint32_t ms);

// Packet reception
void icmp_register_handler(void);
void icmp_handle_packet(const uint8_t* packet, uint16_t length);

#endif // ICMP64_H