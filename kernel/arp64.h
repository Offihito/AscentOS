#ifndef ARP64_H
#define ARP64_H

#include <stdint.h>
#include <stdbool.h>
#include "network64.h"

// ============================================================================
// ARP CONSTANTS
// ============================================================================

#define ARP_HARDWARE_ETHERNET  0x0001
#define ARP_PROTOCOL_IPV4      0x0800

#define ARP_OP_REQUEST  1
#define ARP_OP_REPLY    2

#define ARP_CACHE_SIZE  32
#define ARP_CACHE_TIMEOUT  300  // seconds (5 minutes)

// ============================================================================
// ARP PACKET STRUCTURE
// ============================================================================

// ARP Header (28 bytes for Ethernet + IPv4)
typedef struct __attribute__((packed)) {
    uint16_t hardware_type;      // 1 = Ethernet
    uint16_t protocol_type;      // 0x0800 = IPv4
    uint8_t  hardware_size;      // 6 = MAC address length
    uint8_t  protocol_size;      // 4 = IPv4 address length
    uint16_t opcode;             // 1 = request, 2 = reply
    
    // Sender
    uint8_t  sender_mac[6];
    uint8_t  sender_ip[4];
    
    // Target
    uint8_t  target_mac[6];
    uint8_t  target_ip[4];
} ARPPacket;

// ============================================================================
// ETHERNET FRAME - Use from icmp64.h to avoid duplication
// ============================================================================

// Include icmp64.h for EthernetHeader definition
#include "icmp64.h"

// ARP-specific Ethertype (add to icmp64.h if not present)
#ifndef ETHERTYPE_ARP
#define ETHERTYPE_ARP   0x0806
#endif

// Complete Ethernet + ARP packet
typedef struct __attribute__((packed)) {
    EthernetHeader eth_header;
    ARPPacket arp_packet;
} EthernetARPFrame;

// ============================================================================
// ARP CACHE ENTRY
// ============================================================================

typedef struct {
    IPv4Address ip;
    MACAddress mac;
    uint32_t timestamp;          // When was this entry added
    bool valid;
    bool static_entry;           // Static entries don't expire
} ARPCacheEntry;

// ============================================================================
// ARP CACHE
// ============================================================================

typedef struct {
    ARPCacheEntry entries[ARP_CACHE_SIZE];
    uint32_t entry_count;
    uint32_t current_time;       // Simple time counter
} ARPCache;

// ============================================================================
// FUNCTION PROTOTYPES
// ============================================================================

// Initialization
void arp_init(void);

// ARP Operations
bool arp_send_request(const IPv4Address* target_ip);
bool arp_send_reply(const IPv4Address* target_ip, const MACAddress* target_mac,
                   const IPv4Address* sender_ip, const MACAddress* sender_mac);
bool arp_resolve(const IPv4Address* ip, MACAddress* mac);

// ARP Packet Handling
void arp_handle_packet(const uint8_t* packet_data, uint16_t length);
void arp_process_request(const ARPPacket* arp);
void arp_process_reply(const ARPPacket* arp);

// ARP Cache Management
bool arp_cache_add(const IPv4Address* ip, const MACAddress* mac, bool is_static);
bool arp_cache_remove(const IPv4Address* ip);
bool arp_cache_lookup(const IPv4Address* ip, MACAddress* mac);
void arp_cache_clear(void);
void arp_cache_update_time(void);
void arp_cache_expire_old_entries(void);

// ARP Cache Display
int arp_cache_get_entries(ARPCacheEntry* entries, int max_entries);
void arp_cache_print(void);

// Utility Functions
uint16_t network_htons(uint16_t hostshort);
uint32_t network_htonl(uint32_t hostlong);

#endif // ARP64_H