#ifndef NETWORK64_H
#define NETWORK64_H

#include <stdint.h>
#include <stdbool.h>

// ============================================================================
// MAC ADDRESS
// ============================================================================

typedef struct {
    uint8_t bytes[6];
} MACAddress;

// ============================================================================
// NETWORK CARD TYPES
// ============================================================================

typedef enum {
    NIC_TYPE_UNKNOWN = 0,
    NIC_TYPE_RTL8139,      // Realtek RTL8139
    NIC_TYPE_E1000,        // Intel E1000
    NIC_TYPE_PCNET,        // AMD PCnet
    NIC_TYPE_VIRTIO        // VirtIO Network
} NetworkCardType;

// ============================================================================
// NETWORK CARD STRUCTURE
// ============================================================================

typedef struct {
    NetworkCardType type;
    MACAddress mac;
    uint16_t io_base;
    uint32_t mem_base;
    uint8_t irq;
    bool initialized;
    bool link_up;
    
    // Statistics
    uint64_t packets_sent;
    uint64_t packets_received;
    uint64_t bytes_sent;
    uint64_t bytes_received;
    uint64_t errors;
} NetworkCard;

// ============================================================================
// IP ADDRESS
// ============================================================================

typedef struct {
    uint8_t bytes[4];
} IPv4Address;

// ============================================================================
// NETWORK CONFIGURATION
// ============================================================================

typedef struct {
    IPv4Address ip;
    IPv4Address subnet;
    IPv4Address gateway;
    IPv4Address dns;
    bool dhcp_enabled;
} NetworkConfig;

// ============================================================================
// PACKET BUFFER
// ============================================================================

#define MAX_PACKET_SIZE 1518
#define RX_BUFFER_COUNT 16
#define TX_BUFFER_COUNT 16

typedef struct {
    uint8_t data[MAX_PACKET_SIZE];
    uint16_t length;
    bool in_use;
} PacketBuffer;

// ============================================================================
// NETWORK MANAGER
// ============================================================================

typedef struct {
    NetworkCard card;
    NetworkConfig config;
    
    PacketBuffer rx_buffers[RX_BUFFER_COUNT];
    PacketBuffer tx_buffers[TX_BUFFER_COUNT];
    
    uint8_t rx_index;
    uint8_t tx_index;
} NetworkManager;

// ============================================================================
// FUNCTION PROTOTYPES
// ============================================================================

// Initialization
void network_init(void);
bool network_detect_card(void);

// MAC Address
void network_get_mac(MACAddress* mac);
void network_set_mac(const MACAddress* mac);
void mac_to_string(const MACAddress* mac, char* str);
bool mac_from_string(const char* str, MACAddress* mac);
bool mac_is_broadcast(const MACAddress* mac);
bool mac_is_multicast(const MACAddress* mac);
bool mac_equals(const MACAddress* a, const MACAddress* b);

// Network Configuration
void network_get_config(NetworkConfig* config);
void network_set_config(const NetworkConfig* config);
void ip_to_string(const IPv4Address* ip, char* str);
bool ip_from_string(const char* str, IPv4Address* ip);

// Link Status
bool network_is_initialized(void);
bool network_link_up(void);

// Packet Operations
bool network_send_packet(const uint8_t* data, uint16_t length);
int network_receive_packet(uint8_t* buffer, uint16_t max_length);

// Statistics
void network_get_stats(uint64_t* sent, uint64_t* received, uint64_t* errors);
void network_reset_stats(void);

// Card-specific functions
const char* network_get_card_type_string(void);
bool network_rtl8139_init(NetworkCard* card);
bool network_e1000_init(NetworkCard* card);

#endif // NETWORK64_H