#ifndef NET_NETIF_H
#define NET_NETIF_H

#include <stdbool.h>
#include <stdint.h>

// Build a uint32_t IPv4 address from 4 octets (host byte order)
#define IP4(a, b, c, d) \
    (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | \
     ((uint32_t)(c) << 8)  | (uint32_t)(d))

// Network interface descriptor
typedef struct netif {
    uint8_t  mac[6];       // Hardware (MAC) address
    uint32_t ip;           // Our IP address (host byte order)
    uint32_t gateway;      // Default gateway (host byte order)
    uint32_t netmask;      // Subnet mask    (host byte order)
    bool     up;           // Interface is configured and ready
} netif_t;

// Get the global network interface
netif_t *netif_get(void);

// Configure the interface with a static IP, gateway, and netmask
void netif_configure(uint32_t ip, uint32_t gateway, uint32_t netmask);

#endif
