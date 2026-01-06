#ifndef UDP64_H
#define UDP64_H

#include <stdint.h>
#include <stdbool.h>
#include "network64.h"

// ============================================================================
// UDP HEADER STRUCTURE
// ============================================================================

typedef struct {
    uint16_t src_port;      // Source port
    uint16_t dst_port;      // Destination port
    uint16_t length;        // Length (header + data)
    uint16_t checksum;      // Checksum
} __attribute__((packed)) UDPHeader;

// ============================================================================
// UDP PACKET
// ============================================================================

#define UDP_MAX_DATA_SIZE 1472  // MTU 1500 - IP header 20 - UDP header 8

typedef struct {
    IPv4Address src_ip;
    IPv4Address dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
    uint8_t data[UDP_MAX_DATA_SIZE];
    uint16_t data_length;
} UDPPacket;

// ============================================================================
// UDP SOCKET
// ============================================================================

#define MAX_UDP_SOCKETS 16
#define UDP_RX_QUEUE_SIZE 8

typedef enum {
    UDP_SOCKET_CLOSED = 0,
    UDP_SOCKET_BOUND,
    UDP_SOCKET_CONNECTED
} UDPSocketState;

typedef struct {
    UDPSocketState state;
    uint16_t local_port;
    IPv4Address remote_ip;
    uint16_t remote_port;
    
    // Receive queue
    UDPPacket rx_queue[UDP_RX_QUEUE_SIZE];
    uint8_t rx_head;
    uint8_t rx_tail;
    uint8_t rx_count;
    
    // Statistics
    uint64_t packets_sent;
    uint64_t packets_received;
    uint64_t bytes_sent;
    uint64_t bytes_received;
} UDPSocket;

// ============================================================================
// UDP MANAGER
// ============================================================================

typedef struct {
    UDPSocket sockets[MAX_UDP_SOCKETS];
    uint16_t next_ephemeral_port;  // For automatic port assignment
    
    // Global statistics
    uint64_t total_packets_sent;
    uint64_t total_packets_received;
    uint64_t total_errors;
} UDPManager;

// ============================================================================
// FUNCTION PROTOTYPES
// ============================================================================

// Initialization
void udp_init(void);

// Socket operations
int udp_socket_create(void);                                    // Returns socket ID or -1
bool udp_socket_bind(int socket_id, uint16_t port);            // Bind to local port
bool udp_socket_connect(int socket_id, IPv4Address* ip, uint16_t port);  // Connect to remote
bool udp_socket_close(int socket_id);

// Send/Receive
int udp_send(int socket_id, const uint8_t* data, uint16_t length);
int udp_sendto(int socket_id, IPv4Address* dst_ip, uint16_t dst_port, 
               const uint8_t* data, uint16_t length);
int udp_recv(int socket_id, uint8_t* buffer, uint16_t max_length, 
             IPv4Address* src_ip, uint16_t* src_port);

// Simple send/receive (no socket required)
bool udp_send_packet(IPv4Address* dst_ip, uint16_t dst_port, uint16_t src_port,
                     const uint8_t* data, uint16_t length);
                     
// Packet processing (called by network layer)
void udp_handle_packet(IPv4Address* src_ip, const uint8_t* packet, uint16_t length);

// Utilities
uint16_t udp_allocate_port(void);
bool udp_is_port_in_use(uint16_t port);
UDPSocket* udp_get_socket(int socket_id);

// Statistics
void udp_get_stats(uint64_t* sent, uint64_t* received, uint64_t* errors);
void udp_reset_stats(void);

// Socket info
void udp_get_socket_info(int socket_id, char* buffer, int max_length);
int udp_get_active_sockets(void);

#endif // UDP64_H