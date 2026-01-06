#include "network64.h"
#include <stddef.h>

// External print function
extern void println64(const char* str, uint8_t color);

// ============================================================================
// PCI ACCESS
// ============================================================================

static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile ("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    __asm__ volatile ("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    __asm__ volatile ("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// ============================================================================
// PCI CONFIGURATION
// ============================================================================

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

static uint32_t pci_read_config(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t address = (uint32_t)((bus << 16) | (slot << 11) | 
                                  (func << 8) | (offset & 0xFC) | 0x80000000);
    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

static void pci_write_config(uint8_t bus, uint8_t slot, uint8_t func, 
                            uint8_t offset, uint32_t value) {
    uint32_t address = (uint32_t)((bus << 16) | (slot << 11) | 
                                  (func << 8) | (offset & 0xFC) | 0x80000000);
    outl(PCI_CONFIG_ADDRESS, address);
    outl(PCI_CONFIG_DATA, value);
}

// ============================================================================
// GLOBAL NETWORK MANAGER
// ============================================================================

static NetworkManager net_manager;

// ============================================================================
// STRING UTILITIES
// ============================================================================

static int str_len(const char* str) {
    int len = 0;
    while (str[len]) len++;
    return len;
}

static void memset_net(void* ptr, int value, size_t num) {
    uint8_t* p = (uint8_t*)ptr;
    for (size_t i = 0; i < num; i++) {
        p[i] = (uint8_t)value;
    }
}

// ============================================================================
// MAC ADDRESS FUNCTIONS
// ============================================================================

void mac_to_string(const MACAddress* mac, char* str) {
    const char hex[] = "0123456789ABCDEF";
    for (int i = 0; i < 6; i++) {
        str[i * 3] = hex[(mac->bytes[i] >> 4) & 0xF];
        str[i * 3 + 1] = hex[mac->bytes[i] & 0xF];
        if (i < 5) str[i * 3 + 2] = ':';
    }
    str[17] = '\0';
}

bool mac_from_string(const char* str, MACAddress* mac) {
    if (str_len(str) != 17) return false;
    
    for (int i = 0; i < 6; i++) {
        uint8_t high = 0, low = 0;
        char c = str[i * 3];
        
        if (c >= '0' && c <= '9') high = c - '0';
        else if (c >= 'A' && c <= 'F') high = c - 'A' + 10;
        else if (c >= 'a' && c <= 'f') high = c - 'a' + 10;
        else return false;
        
        c = str[i * 3 + 1];
        if (c >= '0' && c <= '9') low = c - '0';
        else if (c >= 'A' && c <= 'F') low = c - 'A' + 10;
        else if (c >= 'a' && c <= 'f') low = c - 'a' + 10;
        else return false;
        
        mac->bytes[i] = (high << 4) | low;
        
        if (i < 5 && str[i * 3 + 2] != ':') return false;
    }
    
    return true;
}

bool mac_is_broadcast(const MACAddress* mac) {
    for (int i = 0; i < 6; i++) {
        if (mac->bytes[i] != 0xFF) return false;
    }
    return true;
}

bool mac_is_multicast(const MACAddress* mac) {
    return (mac->bytes[0] & 0x01) != 0;
}

bool mac_equals(const MACAddress* a, const MACAddress* b) {
    for (int i = 0; i < 6; i++) {
        if (a->bytes[i] != b->bytes[i]) return false;
    }
    return true;
}

// ============================================================================
// IP ADDRESS FUNCTIONS
// ============================================================================

void ip_to_string(const IPv4Address* ip, char* str) {
    char temp[4];
    str[0] = '\0';
    
    for (int i = 0; i < 4; i++) {
        int val = ip->bytes[i];
        int pos = 0;
        
        if (val == 0) {
            temp[pos++] = '0';
        } else {
            int div = 100;
            bool started = false;
            while (div > 0) {
                int digit = val / div;
                if (digit > 0 || started) {
                    temp[pos++] = '0' + digit;
                    started = true;
                }
                val %= div;
                div /= 10;
            }
        }
        temp[pos] = '\0';
        
        // Append to result
        int j = 0;
        while (str[j]) j++;
        for (int k = 0; temp[k]; k++) {
            str[j++] = temp[k];
        }
        
        if (i < 3) str[j++] = '.';
        str[j] = '\0';
    }
}

bool ip_from_string(const char* str, IPv4Address* ip) {
    int byte_index = 0;
    int current = 0;
    
    for (int i = 0; str[i]; i++) {
        if (str[i] == '.') {
            if (current > 255 || byte_index >= 4) return false;
            ip->bytes[byte_index++] = (uint8_t)current;
            current = 0;
        } else if (str[i] >= '0' && str[i] <= '9') {
            current = current * 10 + (str[i] - '0');
        } else {
            return false;
        }
    }
    
    if (byte_index != 3 || current > 255) return false;
    ip->bytes[byte_index] = (uint8_t)current;
    
    return true;
}

// ============================================================================
// RTL8139 DRIVER
// ============================================================================

#define RTL8139_VENDOR_ID 0x10EC
#define RTL8139_DEVICE_ID 0x8139

// RTL8139 Registers
#define RTL8139_IDR0       0x00  // MAC address
#define RTL8139_MAR0       0x08  // Multicast filter
#define RTL8139_RBSTART    0x30  // Receive buffer start
#define RTL8139_CMD        0x37  // Command register
#define RTL8139_IMR        0x3C  // Interrupt mask
#define RTL8139_ISR        0x3E  // Interrupt status
#define RTL8139_RCR        0x44  // Receive config
#define RTL8139_CONFIG1    0x52

// Commands
#define RTL8139_CMD_RESET  0x10
#define RTL8139_CMD_RX_EN  0x08
#define RTL8139_CMD_TX_EN  0x04

bool network_rtl8139_init(NetworkCard* card) {
    // Scan PCI for RTL8139
    for (uint8_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            uint32_t vendor_device = pci_read_config(bus, slot, 0, 0);
            uint16_t vendor = vendor_device & 0xFFFF;
            uint16_t device = (vendor_device >> 16) & 0xFFFF;
            
            if (vendor == RTL8139_VENDOR_ID && device == RTL8139_DEVICE_ID) {
                // Found RTL8139!
                card->type = NIC_TYPE_RTL8139;
                
                // Read I/O base address
                uint32_t bar0 = pci_read_config(bus, slot, 0, 0x10);
                card->io_base = bar0 & 0xFFFC;
                
                // Enable bus mastering
                uint32_t cmd_reg = pci_read_config(bus, slot, 0, 0x04);
                cmd_reg |= 0x04;  // Bus master enable
                pci_write_config(bus, slot, 0, 0x04, cmd_reg);
                
                // Power on
                outb(card->io_base + RTL8139_CONFIG1, 0x00);
                
                // Software reset
                outb(card->io_base + RTL8139_CMD, RTL8139_CMD_RESET);
                while ((inb(card->io_base + RTL8139_CMD) & RTL8139_CMD_RESET) != 0);
                
                // Read MAC address
                for (int i = 0; i < 6; i++) {
                    card->mac.bytes[i] = inb(card->io_base + RTL8139_IDR0 + i);
                }
                
                // Set receive buffer (dummy for now)
                outl(card->io_base + RTL8139_RBSTART, 0x00);
                
                // Enable receive and transmit
                outb(card->io_base + RTL8139_CMD, 
                     RTL8139_CMD_RX_EN | RTL8139_CMD_TX_EN);
                
                // Configure receive: accept all packets
                outl(card->io_base + RTL8139_RCR, 0x0000000F);
                
                card->initialized = true;
                card->link_up = true;
                
                return true;
            }
        }
    }
    
    return false;
}

// ============================================================================
// INTEL E1000 DRIVER (Basic)
// ============================================================================

#define E1000_VENDOR_ID 0x8086
#define E1000_DEVICE_ID 0x100E

bool network_e1000_init(NetworkCard* card) {
    // Scan PCI for E1000
    for (uint8_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            uint32_t vendor_device = pci_read_config(bus, slot, 0, 0);
            uint16_t vendor = vendor_device & 0xFFFF;
            uint16_t device = (vendor_device >> 16) & 0xFFFF;
            
            if (vendor == E1000_VENDOR_ID && device == E1000_DEVICE_ID) {
                card->type = NIC_TYPE_E1000;
                
                // Read memory base address
                uint32_t bar0 = pci_read_config(bus, slot, 0, 0x10);
                card->mem_base = bar0 & 0xFFFFFFF0;
                
                // Generate a default MAC
                card->mac.bytes[0] = 0x52;
                card->mac.bytes[1] = 0x54;
                card->mac.bytes[2] = 0x00;
                card->mac.bytes[3] = 0x12;
                card->mac.bytes[4] = 0x34;
                card->mac.bytes[5] = 0x56;
                
                card->initialized = true;
                card->link_up = true;
                
                return true;
            }
        }
    }
    
    return false;
}

// ============================================================================
// NETWORK INITIALIZATION
// ============================================================================

void network_init(void) {
    memset_net(&net_manager, 0, sizeof(NetworkManager));
    
    // Try to detect network card
    if (!network_detect_card()) {
        // No card found, use dummy MAC
        net_manager.card.mac.bytes[0] = 0x52;
        net_manager.card.mac.bytes[1] = 0x54;
        net_manager.card.mac.bytes[2] = 0x00;
        net_manager.card.mac.bytes[3] = 0xDE;
        net_manager.card.mac.bytes[4] = 0xAD;
        net_manager.card.mac.bytes[5] = 0xBE;
    }
    
    // Set default network config
    net_manager.config.dhcp_enabled = true;
    
    // Default IP: 10.0.2.15 (QEMU default)
    net_manager.config.ip.bytes[0] = 10;
    net_manager.config.ip.bytes[1] = 0;
    net_manager.config.ip.bytes[2] = 2;
    net_manager.config.ip.bytes[3] = 15;
    
    // Subnet: 255.255.255.0
    net_manager.config.subnet.bytes[0] = 255;
    net_manager.config.subnet.bytes[1] = 255;
    net_manager.config.subnet.bytes[2] = 255;
    net_manager.config.subnet.bytes[3] = 0;
    
    // Gateway: 10.0.2.2
    net_manager.config.gateway.bytes[0] = 10;
    net_manager.config.gateway.bytes[1] = 0;
    net_manager.config.gateway.bytes[2] = 2;
    net_manager.config.gateway.bytes[3] = 2;
    
    // DNS: 8.8.8.8
    net_manager.config.dns.bytes[0] = 8;
    net_manager.config.dns.bytes[1] = 8;
    net_manager.config.dns.bytes[2] = 8;
    net_manager.config.dns.bytes[3] = 8;
}

bool network_detect_card(void) {
    // Try RTL8139 first
    if (network_rtl8139_init(&net_manager.card)) {
        return true;
    }
    
    // Try E1000
    if (network_e1000_init(&net_manager.card)) {
        return true;
    }
    
    return false;
}

// ============================================================================
// NETWORK STATUS
// ============================================================================

bool network_is_initialized(void) {
    return net_manager.card.initialized;
}

bool network_link_up(void) {
    return net_manager.card.link_up;
}

const char* network_get_card_type_string(void) {
    switch (net_manager.card.type) {
        case NIC_TYPE_RTL8139: return "Realtek RTL8139";
        case NIC_TYPE_E1000:   return "Intel E1000";
        case NIC_TYPE_PCNET:   return "AMD PCnet";
        case NIC_TYPE_VIRTIO:  return "VirtIO Network";
        default:               return "Unknown/No Card";
    }
}

// ============================================================================
// NETWORK CONFIGURATION
// ============================================================================

void network_get_mac(MACAddress* mac) {
    for (int i = 0; i < 6; i++) {
        mac->bytes[i] = net_manager.card.mac.bytes[i];
    }
}

void network_set_mac(const MACAddress* mac) {
    for (int i = 0; i < 6; i++) {
        net_manager.card.mac.bytes[i] = mac->bytes[i];
    }
}

void network_get_config(NetworkConfig* config) {
    *config = net_manager.config;
}

void network_set_config(const NetworkConfig* config) {
    net_manager.config = *config;
}

// ============================================================================
// STATISTICS
// ============================================================================

void network_get_stats(uint64_t* sent, uint64_t* received, uint64_t* errors) {
    *sent = net_manager.card.packets_sent;
    *received = net_manager.card.packets_received;
    *errors = net_manager.card.errors;
}

void network_reset_stats(void) {
    net_manager.card.packets_sent = 0;
    net_manager.card.packets_received = 0;
    net_manager.card.bytes_sent = 0;
    net_manager.card.bytes_received = 0;
    net_manager.card.errors = 0;
}

// ============================================================================
// PACKET OPERATIONS (Stubs for now)
// ============================================================================

bool network_send_packet(const uint8_t* data, uint16_t length) {
    (void)data;
    (void)length;
    
    if (!net_manager.card.initialized) return false;
    
    // TODO: Implement actual packet sending
    net_manager.card.packets_sent++;
    net_manager.card.bytes_sent += length;
    
    return true;
}

int network_receive_packet(uint8_t* buffer, uint16_t max_length) {
    (void)buffer;
    (void)max_length;
    
    if (!net_manager.card.initialized) return -1;
    
    // TODO: Implement actual packet receiving
    
    return 0;  // No packets
}