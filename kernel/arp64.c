#include "arp64.h"
#include "network64.h"
#include <stddef.h>

// External functions
extern void println64(const char* str, uint8_t color);
extern bool network_send_packet(const uint8_t* data, uint16_t length);

// ============================================================================
// GLOBAL ARP CACHE
// ============================================================================

static ARPCache arp_cache;

// ============================================================================
// STRING UTILITIES
// ============================================================================

static void memset_arp(void* ptr, int value, size_t num) {
    uint8_t* p = (uint8_t*)ptr;
    for (size_t i = 0; i < num; i++) {
        p[i] = (uint8_t)value;
    }
}

static void memcpy_arp(void* dest, const void* src, size_t num) {
    uint8_t* d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;
    for (size_t i = 0; i < num; i++) {
        d[i] = s[i];
    }
}

static bool ip_equals(const IPv4Address* a, const IPv4Address* b) {
    for (int i = 0; i < 4; i++) {
        if (a->bytes[i] != b->bytes[i]) return false;
    }
    return true;
}

// ============================================================================
// BYTE ORDER CONVERSION
// ============================================================================

uint16_t network_htons(uint16_t hostshort) {
    return ((hostshort & 0xFF) << 8) | ((hostshort >> 8) & 0xFF);
}

uint32_t network_htonl(uint32_t hostlong) {
    return ((hostlong & 0xFF) << 24) |
           ((hostlong & 0xFF00) << 8) |
           ((hostlong & 0xFF0000) >> 8) |
           ((hostlong >> 24) & 0xFF);
}

// ============================================================================
// ARP INITIALIZATION
// ============================================================================

void arp_init(void) {
    memset_arp(&arp_cache, 0, sizeof(ARPCache));
    arp_cache.current_time = 0;
    
    // Add gateway to ARP cache (static entry for QEMU default gateway)
    NetworkConfig config;
    network_get_config(&config);
    
    if (config.gateway.bytes[0] != 0 || config.gateway.bytes[1] != 0 ||
        config.gateway.bytes[2] != 0 || config.gateway.bytes[3] != 0) {
        
        // QEMU default gateway MAC (typically 52:54:00:12:34:56)
        MACAddress gateway_mac;
        gateway_mac.bytes[0] = 0x52;
        gateway_mac.bytes[1] = 0x54;
        gateway_mac.bytes[2] = 0x00;
        gateway_mac.bytes[3] = 0x12;
        gateway_mac.bytes[4] = 0x34;
        gateway_mac.bytes[5] = 0x56;
        
        arp_cache_add(&config.gateway, &gateway_mac, true);
    }
}

// ============================================================================
// ARP CACHE MANAGEMENT
// ============================================================================

bool arp_cache_add(const IPv4Address* ip, const MACAddress* mac, bool is_static) {
    // Check if entry already exists
    for (uint32_t i = 0; i < arp_cache.entry_count; i++) {
        if (arp_cache.entries[i].valid && ip_equals(&arp_cache.entries[i].ip, ip)) {
            // Update existing entry
            memcpy_arp(&arp_cache.entries[i].mac, mac, sizeof(MACAddress));
            arp_cache.entries[i].timestamp = arp_cache.current_time;
            arp_cache.entries[i].static_entry = is_static;
            return true;
        }
    }
    
    // Find free slot
    for (uint32_t i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!arp_cache.entries[i].valid) {
            memcpy_arp(&arp_cache.entries[i].ip, ip, sizeof(IPv4Address));
            memcpy_arp(&arp_cache.entries[i].mac, mac, sizeof(MACAddress));
            arp_cache.entries[i].timestamp = arp_cache.current_time;
            arp_cache.entries[i].valid = true;
            arp_cache.entries[i].static_entry = is_static;
            
            if (i >= arp_cache.entry_count) {
                arp_cache.entry_count = i + 1;
            }
            return true;
        }
    }
    
    return false;  // Cache full
}

bool arp_cache_remove(const IPv4Address* ip) {
    for (uint32_t i = 0; i < arp_cache.entry_count; i++) {
        if (arp_cache.entries[i].valid && ip_equals(&arp_cache.entries[i].ip, ip)) {
            if (arp_cache.entries[i].static_entry) {
                return false;  // Cannot remove static entries
            }
            arp_cache.entries[i].valid = false;
            return true;
        }
    }
    return false;
}

bool arp_cache_lookup(const IPv4Address* ip, MACAddress* mac) {
    for (uint32_t i = 0; i < arp_cache.entry_count; i++) {
        if (arp_cache.entries[i].valid && ip_equals(&arp_cache.entries[i].ip, ip)) {
            memcpy_arp(mac, &arp_cache.entries[i].mac, sizeof(MACAddress));
            return true;
        }
    }
    return false;
}

void arp_cache_clear(void) {
    for (uint32_t i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!arp_cache.entries[i].static_entry) {
            arp_cache.entries[i].valid = false;
        }
    }
    
    // Recount entries
    arp_cache.entry_count = 0;
    for (uint32_t i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache.entries[i].valid) {
            arp_cache.entry_count = i + 1;
        }
    }
}

void arp_cache_update_time(void) {
    arp_cache.current_time++;
}

void arp_cache_expire_old_entries(void) {
    for (uint32_t i = 0; i < arp_cache.entry_count; i++) {
        if (arp_cache.entries[i].valid && !arp_cache.entries[i].static_entry) {
            uint32_t age = arp_cache.current_time - arp_cache.entries[i].timestamp;
            if (age > ARP_CACHE_TIMEOUT) {
                arp_cache.entries[i].valid = false;
            }
        }
    }
}

int arp_cache_get_entries(ARPCacheEntry* entries, int max_entries) {
    int count = 0;
    for (uint32_t i = 0; i < arp_cache.entry_count && count < max_entries; i++) {
        if (arp_cache.entries[i].valid) {
            memcpy_arp(&entries[count], &arp_cache.entries[i], sizeof(ARPCacheEntry));
            count++;
        }
    }
    return count;
}

// ============================================================================
// ARP PACKET CONSTRUCTION
// ============================================================================

static void build_ethernet_header(EthernetHeader* eth, 
                                  const MACAddress* dest_mac,
                                  const MACAddress* src_mac,
                                  uint16_t ethertype) {
    memcpy_arp(eth->dest_mac, dest_mac->bytes, 6);
    memcpy_arp(eth->src_mac, src_mac->bytes, 6);
    eth->ethertype = network_htons(ethertype);
}

static void build_arp_packet(ARPPacket* arp,
                             uint16_t opcode,
                             const MACAddress* sender_mac,
                             const IPv4Address* sender_ip,
                             const MACAddress* target_mac,
                             const IPv4Address* target_ip) {
    arp->hardware_type = network_htons(ARP_HARDWARE_ETHERNET);
    arp->protocol_type = network_htons(ARP_PROTOCOL_IPV4);
    arp->hardware_size = 6;
    arp->protocol_size = 4;
    arp->opcode = network_htons(opcode);
    
    memcpy_arp(arp->sender_mac, sender_mac->bytes, 6);
    memcpy_arp(arp->sender_ip, sender_ip->bytes, 4);
    memcpy_arp(arp->target_mac, target_mac->bytes, 6);
    memcpy_arp(arp->target_ip, target_ip->bytes, 4);
}

// ============================================================================
// ARP REQUEST/REPLY
// ============================================================================

bool arp_send_request(const IPv4Address* target_ip) {
    if (!network_is_initialized()) {
        return false;
    }
    
    EthernetARPFrame frame;
    
    // Get our MAC and IP
    MACAddress our_mac;
    network_get_mac(&our_mac);
    
    NetworkConfig config;
    network_get_config(&config);
    
    // Broadcast MAC address
    MACAddress broadcast_mac;
    for (int i = 0; i < 6; i++) {
        broadcast_mac.bytes[i] = 0xFF;
    }
    
    // Zero MAC for target (we don't know it yet)
    MACAddress zero_mac;
    memset_arp(&zero_mac, 0, sizeof(MACAddress));
    
    // Build Ethernet header (broadcast)
    build_ethernet_header(&frame.eth_header, &broadcast_mac, &our_mac, ETHERTYPE_ARP);
    
    // Build ARP request
    build_arp_packet(&frame.arp_packet, ARP_OP_REQUEST,
                    &our_mac, &config.ip,
                    &zero_mac, target_ip);
    
    // Send the packet
    return network_send_packet((uint8_t*)&frame, sizeof(EthernetARPFrame));
}

bool arp_send_reply(const IPv4Address* target_ip, const MACAddress* target_mac,
                   const IPv4Address* sender_ip, const MACAddress* sender_mac) {
    if (!network_is_initialized()) {
        return false;
    }
    
    EthernetARPFrame frame;
    
    // Build Ethernet header (unicast to requester)
    build_ethernet_header(&frame.eth_header, target_mac, sender_mac, ETHERTYPE_ARP);
    
    // Build ARP reply
    build_arp_packet(&frame.arp_packet, ARP_OP_REPLY,
                    sender_mac, sender_ip,
                    target_mac, target_ip);
    
    // Send the packet
    return network_send_packet((uint8_t*)&frame, sizeof(EthernetARPFrame));
}

// ============================================================================
// ARP RESOLUTION
// ============================================================================

bool arp_resolve(const IPv4Address* ip, MACAddress* mac) {
    // First check cache
    if (arp_cache_lookup(ip, mac)) {
        return true;
    }
    
    // Not in cache, send ARP request
    arp_send_request(ip);
    
    // In a real implementation, we would wait for a reply here
    // For now, we'll just return false and rely on the cache being
    // populated when replies arrive
    
    return false;
}

// ============================================================================
// ARP PACKET HANDLING
// ============================================================================

void arp_handle_packet(const uint8_t* packet_data, uint16_t length) {
    if (length < sizeof(EthernetARPFrame)) {
        return;  // Packet too small
    }
    
    const EthernetARPFrame* frame = (const EthernetARPFrame*)packet_data;
    
    // Verify it's an ARP packet
    if (network_htons(frame->eth_header.ethertype) != ETHERTYPE_ARP) {
        return;
    }
    
    const ARPPacket* arp = &frame->arp_packet;
    
    // Verify ARP for Ethernet + IPv4
    if (network_htons(arp->hardware_type) != ARP_HARDWARE_ETHERNET ||
        network_htons(arp->protocol_type) != ARP_PROTOCOL_IPV4) {
        return;
    }
    
    uint16_t opcode = network_htons(arp->opcode);
    
    if (opcode == ARP_OP_REQUEST) {
        arp_process_request(arp);
    } else if (opcode == ARP_OP_REPLY) {
        arp_process_reply(arp);
    }
}

void arp_process_request(const ARPPacket* arp) {
    // Get our IP
    NetworkConfig config;
    network_get_config(&config);
    
    // Check if the request is for us
    IPv4Address target_ip;
    memcpy_arp(&target_ip, arp->target_ip, 4);
    
    if (ip_equals(&target_ip, &config.ip)) {
        // This ARP request is for us! Send a reply
        
        IPv4Address sender_ip;
        MACAddress sender_mac;
        
        memcpy_arp(&sender_ip, arp->sender_ip, 4);
        memcpy_arp(&sender_mac, arp->sender_mac, 6);
        
        // Add sender to our ARP cache
        arp_cache_add(&sender_ip, &sender_mac, false);
        
        // Send ARP reply
        MACAddress our_mac;
        network_get_mac(&our_mac);
        
        arp_send_reply(&sender_ip, &sender_mac, &config.ip, &our_mac);
    }
}

void arp_process_reply(const ARPPacket* arp) {
    // Extract sender's IP and MAC
    IPv4Address sender_ip;
    MACAddress sender_mac;
    
    memcpy_arp(&sender_ip, arp->sender_ip, 4);
    memcpy_arp(&sender_mac, arp->sender_mac, 6);
    
    // Add to cache
    arp_cache_add(&sender_ip, &sender_mac, false);
}

// ============================================================================
// ARP CACHE PRINTING (for debugging)
// ============================================================================

void arp_cache_print(void) {
    for (uint32_t i = 0; i < arp_cache.entry_count; i++) {
        if (arp_cache.entries[i].valid) {
            char ip_str[16];
            char mac_str[18];
            
            ip_to_string(&arp_cache.entries[i].ip, ip_str);
            mac_to_string(&arp_cache.entries[i].mac, mac_str);
            
            // Print using external function
            // This would need to be formatted properly
        }
    }
}