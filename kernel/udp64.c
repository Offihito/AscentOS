#include "udp64.h"
#include "network64.h"
#include <stddef.h>

// External functions
extern void println64(const char* str, uint8_t color);
extern void serial_print(const char* str);

// ============================================================================
// GLOBAL UDP MANAGER
// ============================================================================

static UDPManager udp_manager;

// ============================================================================
// UTILITIES
// ============================================================================

static void memset_udp(void* ptr, int value, size_t num) {
    uint8_t* p = (uint8_t*)ptr;
    for (size_t i = 0; i < num; i++) {
        p[i] = (uint8_t)value;
    }
}

static void memcpy_udp(void* dest, const void* src, size_t num) {
    uint8_t* d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;
    for (size_t i = 0; i < num; i++) {
        d[i] = s[i];
    }
}

// Byte order conversion
static uint16_t htons(uint16_t hostshort) {
    return (hostshort >> 8) | (hostshort << 8);
}

static uint16_t ntohs(uint16_t netshort) {
    return (netshort >> 8) | (netshort << 8);
}

// ============================================================================
// CHECKSUM CALCULATION
// ============================================================================

static uint16_t udp_checksum(IPv4Address* src_ip, IPv4Address* dst_ip,
                             UDPHeader* udp_header, const uint8_t* data, uint16_t data_len) {
    uint32_t sum = 0;
    
    // Pseudo header
    for (int i = 0; i < 4; i++) {
        sum += src_ip->bytes[i];
        sum += dst_ip->bytes[i];
    }
    
    sum += 17;  // UDP protocol number
    sum += ntohs(udp_header->length);
    
    // UDP header (skip checksum field)
    sum += ntohs(udp_header->src_port);
    sum += ntohs(udp_header->dst_port);
    sum += ntohs(udp_header->length);
    
    // Data
    for (uint16_t i = 0; i < data_len; i += 2) {
        if (i + 1 < data_len) {
            sum += (data[i] << 8) | data[i + 1];
        } else {
            sum += data[i] << 8;
        }
    }
    
    // Fold 32-bit sum to 16 bits
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    
    return ~sum;
}

// ============================================================================
// IP LAYER (SIMPLIFIED)
// ============================================================================

#define IP_PROTOCOL_UDP 17

typedef struct {
    uint8_t version_ihl;      // Version (4 bits) + IHL (4 bits)
    uint8_t tos;              // Type of Service
    uint16_t total_length;    // Total length
    uint16_t identification;  // Identification
    uint16_t flags_fragment;  // Flags (3 bits) + Fragment offset (13 bits)
    uint8_t ttl;              // Time to Live
    uint8_t protocol;         // Protocol
    uint16_t checksum;        // Header checksum
    uint32_t src_ip;          // Source IP
    uint32_t dst_ip;          // Destination IP
} __attribute__((packed)) IPv4Header;

static uint16_t ip_checksum(IPv4Header* header) {
    uint32_t sum = 0;
    uint16_t* ptr = (uint16_t*)header;
    
    for (int i = 0; i < 10; i++) {  // IP header is 20 bytes = 10 words
        if (i != 5) {  // Skip checksum field
            sum += ntohs(ptr[i]);
        }
    }
    
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    
    return ~sum;
}

// ============================================================================
// INITIALIZATION
// ============================================================================

void udp_init(void) {
    memset_udp(&udp_manager, 0, sizeof(UDPManager));
    udp_manager.next_ephemeral_port = 49152;  // Start of ephemeral port range
    
    serial_print("UDP protocol initialized\n");
}

// ============================================================================
// SOCKET OPERATIONS
// ============================================================================

int udp_socket_create(void) {
    for (int i = 0; i < MAX_UDP_SOCKETS; i++) {
        if (udp_manager.sockets[i].state == UDP_SOCKET_CLOSED) {
            UDPSocket* sock = &udp_manager.sockets[i];
            memset_udp(sock, 0, sizeof(UDPSocket));
            sock->state = UDP_SOCKET_BOUND;  // Will be properly bound later
            sock->local_port = 0;  // Not bound yet
            return i;
        }
    }
    return -1;  // No free sockets
}

bool udp_socket_bind(int socket_id, uint16_t port) {
    if (socket_id < 0 || socket_id >= MAX_UDP_SOCKETS) return false;
    
    UDPSocket* sock = &udp_manager.sockets[socket_id];
    if (sock->state == UDP_SOCKET_CLOSED) return false;
    
    // Check if port is already in use
    if (port != 0 && udp_is_port_in_use(port)) return false;
    
    // If port is 0, allocate one
    if (port == 0) {
        port = udp_allocate_port();
        if (port == 0) return false;
    }
    
    sock->local_port = port;
    sock->state = UDP_SOCKET_BOUND;
    
    return true;
}

bool udp_socket_connect(int socket_id, IPv4Address* ip, uint16_t port) {
    if (socket_id < 0 || socket_id >= MAX_UDP_SOCKETS) return false;
    
    UDPSocket* sock = &udp_manager.sockets[socket_id];
    if (sock->state == UDP_SOCKET_CLOSED) return false;
    
    // If not bound, bind to ephemeral port
    if (sock->local_port == 0) {
        if (!udp_socket_bind(socket_id, 0)) return false;
    }
    
    for (int i = 0; i < 4; i++) {
        sock->remote_ip.bytes[i] = ip->bytes[i];
    }
    sock->remote_port = port;
    sock->state = UDP_SOCKET_CONNECTED;
    
    return true;
}

bool udp_socket_close(int socket_id) {
    if (socket_id < 0 || socket_id >= MAX_UDP_SOCKETS) return false;
    
    UDPSocket* sock = &udp_manager.sockets[socket_id];
    memset_udp(sock, 0, sizeof(UDPSocket));
    sock->state = UDP_SOCKET_CLOSED;
    
    return true;
}

// ============================================================================
// PORT MANAGEMENT
// ============================================================================

uint16_t udp_allocate_port(void) {
    uint16_t start_port = udp_manager.next_ephemeral_port;
    
    while (true) {
        if (!udp_is_port_in_use(udp_manager.next_ephemeral_port)) {
            uint16_t port = udp_manager.next_ephemeral_port;
            udp_manager.next_ephemeral_port++;
            if (udp_manager.next_ephemeral_port > 65535) {
                udp_manager.next_ephemeral_port = 49152;
            }
            return port;
        }
        
        udp_manager.next_ephemeral_port++;
        if (udp_manager.next_ephemeral_port > 65535) {
            udp_manager.next_ephemeral_port = 49152;
        }
        
        // Prevent infinite loop
        if (udp_manager.next_ephemeral_port == start_port) {
            return 0;  // No free ports
        }
    }
}

bool udp_is_port_in_use(uint16_t port) {
    for (int i = 0; i < MAX_UDP_SOCKETS; i++) {
        if (udp_manager.sockets[i].state != UDP_SOCKET_CLOSED &&
            udp_manager.sockets[i].local_port == port) {
            return true;
        }
    }
    return false;
}

UDPSocket* udp_get_socket(int socket_id) {
    if (socket_id < 0 || socket_id >= MAX_UDP_SOCKETS) return NULL;
    if (udp_manager.sockets[socket_id].state == UDP_SOCKET_CLOSED) return NULL;
    return &udp_manager.sockets[socket_id];
}

// ============================================================================
// SEND OPERATIONS
// ============================================================================

bool udp_send_packet(IPv4Address* dst_ip, uint16_t dst_port, uint16_t src_port,
                     const uint8_t* data, uint16_t length) {
    if (length > UDP_MAX_DATA_SIZE) return false;
    
    // Build packet: Ethernet + IP + UDP + Data
    uint8_t packet[1518];  // Max Ethernet frame
    int offset = 0;
    
    // Ethernet header (14 bytes)
    NetworkConfig config;
    network_get_config(&config);
    MACAddress src_mac, dst_mac;
    network_get_mac(&src_mac);
    
    // Use ARP to get destination MAC (simplified - assume broadcast for now)
    memset_udp(&dst_mac, 0xFF, 6);  // Broadcast
    
    // Destination MAC
    memcpy_udp(&packet[offset], &dst_mac, 6);
    offset += 6;
    
    // Source MAC
    memcpy_udp(&packet[offset], &src_mac, 6);
    offset += 6;
    
    // EtherType (IPv4 = 0x0800)
    packet[offset++] = 0x08;
    packet[offset++] = 0x00;
    
    // IP header (20 bytes)
    IPv4Header* ip_header = (IPv4Header*)&packet[offset];
    ip_header->version_ihl = 0x45;  // IPv4, 20 bytes header
    ip_header->tos = 0;
    ip_header->total_length = htons(20 + 8 + length);  // IP + UDP + data
    ip_header->identification = htons(1234);
    ip_header->flags_fragment = htons(0x4000);  // Don't fragment
    ip_header->ttl = 64;
    ip_header->protocol = IP_PROTOCOL_UDP;
    ip_header->checksum = 0;
    
    // Source and dest IP (network byte order)
    for (int i = 0; i < 4; i++) {
        ((uint8_t*)&ip_header->src_ip)[i] = config.ip.bytes[i];
        ((uint8_t*)&ip_header->dst_ip)[i] = dst_ip->bytes[i];
    }
    
    ip_header->checksum = htons(ip_checksum(ip_header));
    offset += 20;
    
    // UDP header (8 bytes)
    UDPHeader* udp_header = (UDPHeader*)&packet[offset];
    udp_header->src_port = htons(src_port);
    udp_header->dst_port = htons(dst_port);
    udp_header->length = htons(8 + length);
    udp_header->checksum = 0;  // Optional for IPv4
    
    // Calculate UDP checksum
    udp_header->checksum = htons(udp_checksum(&config.ip, dst_ip, udp_header, data, length));
    offset += 8;
    
    // Data
    memcpy_udp(&packet[offset], data, length);
    offset += length;
    
    // Send packet
    bool success = network_send_packet(packet, offset);
    
    if (success) {
        udp_manager.total_packets_sent++;
    } else {
        udp_manager.total_errors++;
    }
    
    return success;
}

int udp_send(int socket_id, const uint8_t* data, uint16_t length) {
    UDPSocket* sock = udp_get_socket(socket_id);
    if (!sock) return -1;
    if (sock->state != UDP_SOCKET_CONNECTED) return -1;
    
    if (udp_send_packet(&sock->remote_ip, sock->remote_port, sock->local_port, data, length)) {
        sock->packets_sent++;
        sock->bytes_sent += length;
        return length;
    }
    
    return -1;
}

int udp_sendto(int socket_id, IPv4Address* dst_ip, uint16_t dst_port,
               const uint8_t* data, uint16_t length) {
    UDPSocket* sock = udp_get_socket(socket_id);
    if (!sock) return -1;
    
    // If not bound, bind to ephemeral port
    if (sock->local_port == 0) {
        if (!udp_socket_bind(socket_id, 0)) return -1;
    }
    
    if (udp_send_packet(dst_ip, dst_port, sock->local_port, data, length)) {
        sock->packets_sent++;
        sock->bytes_sent += length;
        return length;
    }
    
    return -1;
}

// ============================================================================
// RECEIVE OPERATIONS
// ============================================================================

void udp_handle_packet(IPv4Address* src_ip, const uint8_t* packet, uint16_t length) {
    if (length < sizeof(UDPHeader)) {
        udp_manager.total_errors++;
        return;
    }
    
    UDPHeader* header = (UDPHeader*)packet;
    uint16_t src_port = ntohs(header->src_port);
    uint16_t dst_port = ntohs(header->dst_port);
    uint16_t udp_length = ntohs(header->length);
    
    if (udp_length < 8 || udp_length > length) {
        udp_manager.total_errors++;
        return;
    }
    
    uint16_t data_length = udp_length - 8;
    const uint8_t* data = packet + sizeof(UDPHeader);
    
    // Find socket listening on this port
    for (int i = 0; i < MAX_UDP_SOCKETS; i++) {
        UDPSocket* sock = &udp_manager.sockets[i];
        
        if (sock->state == UDP_SOCKET_CLOSED) continue;
        if (sock->local_port != dst_port) continue;
        
        // Check if connected socket matches source
        if (sock->state == UDP_SOCKET_CONNECTED) {
            bool match = true;
            for (int j = 0; j < 4; j++) {
                if (sock->remote_ip.bytes[j] != src_ip->bytes[j]) {
                    match = false;
                    break;
                }
            }
            if (!match || sock->remote_port != src_port) continue;
        }
        
        // Add to receive queue
        if (sock->rx_count < UDP_RX_QUEUE_SIZE) {
            UDPPacket* pkt = &sock->rx_queue[sock->rx_tail];
            
            for (int j = 0; j < 4; j++) {
                pkt->src_ip.bytes[j] = src_ip->bytes[j];
            }
            pkt->src_port = src_port;
            pkt->dst_port = dst_port;
            pkt->data_length = data_length < UDP_MAX_DATA_SIZE ? data_length : UDP_MAX_DATA_SIZE;
            memcpy_udp(pkt->data, data, pkt->data_length);
            
            sock->rx_tail = (sock->rx_tail + 1) % UDP_RX_QUEUE_SIZE;
            sock->rx_count++;
            
            sock->packets_received++;
            sock->bytes_received += data_length;
        }
        
        break;
    }
    
    udp_manager.total_packets_received++;
}

int udp_recv(int socket_id, uint8_t* buffer, uint16_t max_length,
             IPv4Address* src_ip, uint16_t* src_port) {
    UDPSocket* sock = udp_get_socket(socket_id);
    if (!sock) return -1;
    if (sock->rx_count == 0) return 0;  // No data
    
    UDPPacket* pkt = &sock->rx_queue[sock->rx_head];
    
    uint16_t copy_len = pkt->data_length < max_length ? pkt->data_length : max_length;
    memcpy_udp(buffer, pkt->data, copy_len);
    
    if (src_ip) {
        for (int i = 0; i < 4; i++) {
            src_ip->bytes[i] = pkt->src_ip.bytes[i];
        }
    }
    if (src_port) {
        *src_port = pkt->src_port;
    }
    
    sock->rx_head = (sock->rx_head + 1) % UDP_RX_QUEUE_SIZE;
    sock->rx_count--;
    
    return copy_len;
}

// ============================================================================
// STATISTICS
// ============================================================================

void udp_get_stats(uint64_t* sent, uint64_t* received, uint64_t* errors) {
    *sent = udp_manager.total_packets_sent;
    *received = udp_manager.total_packets_received;
    *errors = udp_manager.total_errors;
}

void udp_reset_stats(void) {
    udp_manager.total_packets_sent = 0;
    udp_manager.total_packets_received = 0;
    udp_manager.total_errors = 0;
    
    for (int i = 0; i < MAX_UDP_SOCKETS; i++) {
        UDPSocket* sock = &udp_manager.sockets[i];
        sock->packets_sent = 0;
        sock->packets_received = 0;
        sock->bytes_sent = 0;
        sock->bytes_received = 0;
    }
}

// ============================================================================
// SOCKET INFO
// ============================================================================

void udp_get_socket_info(int socket_id, char* buffer, int max_length) {
    UDPSocket* sock = udp_get_socket(socket_id);
    if (!sock) {
        buffer[0] = '\0';
        return;
    }
    
    const char* state_str = "UNKNOWN";
    switch (sock->state) {
        case UDP_SOCKET_CLOSED: state_str = "CLOSED"; break;
        case UDP_SOCKET_BOUND: state_str = "BOUND"; break;
        case UDP_SOCKET_CONNECTED: state_str = "CONNECTED"; break;
    }
    
    // Simple string building (avoiding sprintf)
    int pos = 0;
    
    // Socket ID
    buffer[pos++] = '0' + socket_id;
    buffer[pos++] = ' ';
    buffer[pos++] = ' ';
    
    // State
    for (const char* s = state_str; *s && pos < max_length - 1; s++) {
        buffer[pos++] = *s;
    }
    
    buffer[pos] = '\0';
}

int udp_get_active_sockets(void) {
    int count = 0;
    for (int i = 0; i < MAX_UDP_SOCKETS; i++) {
        if (udp_manager.sockets[i].state != UDP_SOCKET_CLOSED) {
            count++;
        }
    }
    return count;
}