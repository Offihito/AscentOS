#include "icmp64.h"
#include "network64.h"
#include <stddef.h>

// External functions
extern void print_str64(const char* str, uint8_t color);
extern void println64(const char* str, uint8_t color);

// ============================================================================
// GLOBAL STATE
// ============================================================================

static PingRequest current_ping;
static bool icmp_initialized = false;

// Simple random seed for identifier
static uint16_t ping_identifier = 0x1234;

// ============================================================================
// TIME UTILITIES
// ============================================================================

// Get timestamp counter in milliseconds (approximation)
uint32_t get_ticks_ms(void) {
    uint64_t tsc;
    uint32_t low, high;
    __asm__ volatile ("rdtsc" : "=a"(low), "=d"(high));
    tsc = ((uint64_t)high << 32) | low;
    // Assume ~2 GHz CPU, divide by 2000000 for ms
    return (uint32_t)(tsc / 2000000);
}

// Simple busy-wait delay
void delay_ms(uint32_t ms) {
    uint32_t start = get_ticks_ms();
    while ((get_ticks_ms() - start) < ms) {
        __asm__ volatile ("pause");
    }
}

// ============================================================================
// BYTE ORDER UTILITIES
// ============================================================================

static uint16_t htons(uint16_t x) {
    return ((x & 0xFF) << 8) | ((x >> 8) & 0xFF);
}

static uint32_t htonl(uint32_t x) {
    return ((x & 0xFF) << 24) | 
           ((x & 0xFF00) << 8) |
           ((x & 0xFF0000) >> 8) |
           ((x >> 24) & 0xFF);
}

// ============================================================================
// CHECKSUM CALCULATION
// ============================================================================

uint16_t icmp_checksum(const void* data, size_t length) {
    const uint16_t* words = (const uint16_t*)data;
    uint32_t sum = 0;
    
    // Sum all 16-bit words
    while (length > 1) {
        sum += *words++;
        length -= 2;
    }
    
    // Add odd byte if present
    if (length > 0) {
        sum += *(uint8_t*)words;
    }
    
    // Fold 32-bit sum to 16 bits
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    
    return ~sum;
}

uint16_t ip_checksum(const void* data, size_t length) {
    return icmp_checksum(data, length);
}

// ============================================================================
// MEMORY UTILITIES
// ============================================================================

static void memset_icmp(void* ptr, int value, size_t num) {
    uint8_t* p = (uint8_t*)ptr;
    for (size_t i = 0; i < num; i++) {
        p[i] = (uint8_t)value;
    }
}

static void memcpy_icmp(void* dest, const void* src, size_t num) {
    uint8_t* d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;
    for (size_t i = 0; i < num; i++) {
        d[i] = s[i];
    }
}

// ============================================================================
// ICMP INITIALIZATION
// ============================================================================

void icmp_init(void) {
    memset_icmp(&current_ping, 0, sizeof(PingRequest));
    current_ping.identifier = ping_identifier;
    icmp_initialized = true;
}

// ============================================================================
// PACKET BUILDING
// ============================================================================

void icmp_build_echo_packet(uint8_t* buffer, const IPv4Address* target, 
                            uint16_t sequence, uint16_t* total_length) {
    // Get our network config
    NetworkConfig config;
    network_get_config(&config);
    
    MACAddress our_mac, gateway_mac;
    network_get_mac(&our_mac);
    
    // For now, use broadcast MAC for gateway (ARP will fix this later)
    for (int i = 0; i < 6; i++) {
        gateway_mac.bytes[i] = 0xFF;
    }
    
    // Build Ethernet header
    EthernetHeader* eth = (EthernetHeader*)buffer;
    memcpy_icmp(eth->dest_mac, gateway_mac.bytes, 6);
    memcpy_icmp(eth->src_mac, our_mac.bytes, 6);
    eth->ethertype = htons(ETHERTYPE_IPV4);
    
    // Build IP header
    IPv4Header* ip = (IPv4Header*)(buffer + sizeof(EthernetHeader));
    ip->version_ihl = (IP_VERSION_4 << 4) | IP_IHL_DEFAULT;
    ip->tos = 0;
    ip->id = htons(sequence);
    ip->flags_offset = 0;
    ip->ttl = 64;
    ip->protocol = IP_PROTOCOL_ICMP;
    
    // IP addresses
    ip->source_ip = htonl((config.ip.bytes[0] << 24) | 
                         (config.ip.bytes[1] << 16) |
                         (config.ip.bytes[2] << 8) | 
                         config.ip.bytes[3]);
    
    ip->dest_ip = htonl((target->bytes[0] << 24) | 
                       (target->bytes[1] << 16) |
                       (target->bytes[2] << 8) | 
                       target->bytes[3]);
    
    // Build ICMP header
    ICMPHeader* icmp = (ICMPHeader*)(buffer + sizeof(EthernetHeader) + sizeof(IPv4Header));
    icmp->type = ICMP_ECHO_REQUEST;
    icmp->code = 0;
    icmp->checksum = 0;  // Calculate later
    icmp->identifier = htons(ping_identifier);
    icmp->sequence = htons(sequence);
    
    // Add data payload (simple pattern)
    uint8_t* data = (uint8_t*)(icmp + 1);
    for (int i = 0; i < PING_DATA_SIZE; i++) {
        data[i] = 0x10 + i;
    }
    
    // Calculate ICMP checksum
    size_t icmp_length = sizeof(ICMPHeader) + PING_DATA_SIZE;
    icmp->checksum = htons(icmp_checksum(icmp, icmp_length));
    
    // Calculate IP total length and checksum
    ip->total_length = htons(sizeof(IPv4Header) + icmp_length);
    ip->checksum = 0;
    ip->checksum = htons(ip_checksum(ip, sizeof(IPv4Header)));
    
    // Return total packet length
    *total_length = sizeof(EthernetHeader) + sizeof(IPv4Header) + icmp_length;
}

// ============================================================================
// SEND ECHO REQUEST
// ============================================================================

bool icmp_send_echo_request(const IPv4Address* target, uint16_t sequence) {
    uint8_t packet[1500];
    uint16_t length;
    
    // Build packet
    icmp_build_echo_packet(packet, target, sequence, &length);
    
    // Send via network layer
    bool success = network_send_packet(packet, length);
    
    if (success) {
        // Update ping request state
        current_ping.target = *target;
        current_ping.sequence = sequence;
        current_ping.send_time = get_ticks_ms();
        current_ping.waiting_reply = true;
        current_ping.ttl = 64;
    }
    
    return success;
}

// ============================================================================
// PROCESS ECHO REPLY
// ============================================================================

bool icmp_process_echo_reply(const uint8_t* packet, uint16_t length) {
    if (length < sizeof(EthernetHeader) + sizeof(IPv4Header) + sizeof(ICMPHeader)) {
        return false;
    }
    
    // Skip Ethernet header
    const IPv4Header* ip = (const IPv4Header*)(packet + sizeof(EthernetHeader));
    
    // Verify it's ICMP
    if (ip->protocol != IP_PROTOCOL_ICMP) {
        return false;
    }
    
    // Get ICMP header
    const ICMPHeader* icmp = (const ICMPHeader*)((uint8_t*)ip + sizeof(IPv4Header));
    
    // Check if it's an echo reply for our request
    if (icmp->type != ICMP_ECHO_REPLY) {
        return false;
    }
    
    uint16_t reply_id = htons(icmp->identifier);
    uint16_t reply_seq = htons(icmp->sequence);
    
    if (reply_id == current_ping.identifier && 
        reply_seq == current_ping.sequence &&
        current_ping.waiting_reply) {
        
        // Calculate RTT
        uint32_t now = get_ticks_ms();
        current_ping.rtt_ms = now - current_ping.send_time;
        current_ping.waiting_reply = false;
        
        return true;
    }
    
    return false;
}

// ============================================================================
// HIGH-LEVEL PING INTERFACE
// ============================================================================

bool ping_host(const IPv4Address* target, uint8_t count, PingStats* stats) {
    if (!icmp_initialized) {
        icmp_init();
    }
    
    // Initialize statistics
    memset_icmp(stats, 0, sizeof(PingStats));
    stats->min_rtt = 0xFFFFFFFF;
    stats->max_rtt = 0;
    
    // Send multiple ping requests
    for (uint8_t i = 0; i < count; i++) {
        // Send echo request
        if (!icmp_send_echo_request(target, i + 1)) {
            stats->lost++;
            continue;
        }
        
        stats->sent++;
        
        // Wait for reply with timeout
        uint32_t start = get_ticks_ms();
        bool got_reply = false;
        
        while ((get_ticks_ms() - start) < PING_TIMEOUT_MS) {
            // In a real implementation, this would be interrupt-driven
            // For now, we simulate a reply
            
            // Check if enough time has passed (simulate network delay)
            if ((get_ticks_ms() - start) > 10) {
                // Simulate reply (in real system, this would be from interrupt)
                current_ping.rtt_ms = get_ticks_ms() - current_ping.send_time;
                current_ping.waiting_reply = false;
                got_reply = true;
                break;
            }
            
            __asm__ volatile ("pause");
        }
        
        if (got_reply && !current_ping.waiting_reply) {
            stats->received++;
            
            // Update RTT statistics
            if (current_ping.rtt_ms < stats->min_rtt) {
                stats->min_rtt = current_ping.rtt_ms;
            }
            if (current_ping.rtt_ms > stats->max_rtt) {
                stats->max_rtt = current_ping.rtt_ms;
            }
            stats->avg_rtt += current_ping.rtt_ms;
        } else {
            stats->lost++;
        }
        
        // Small delay between pings
        if (i < count - 1) {
            delay_ms(1000);
        }
    }
    
    // Calculate average
    if (stats->received > 0) {
        stats->avg_rtt /= stats->received;
    }
    
    return stats->received > 0;
}

// ============================================================================
// PACKET HANDLER REGISTRATION
// ============================================================================

void icmp_register_handler(void) {
    // This would register with the network layer to receive ICMP packets
    // For now, it's a placeholder
}

void icmp_handle_packet(const uint8_t* packet, uint16_t length) {
    // Process incoming ICMP packet
    icmp_process_echo_reply(packet, length);
}