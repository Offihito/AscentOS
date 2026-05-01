#ifndef DRIVERS_NET_NIC_H
#define DRIVERS_NET_NIC_H

/*
 * NIC Abstraction Layer
 *
 * Provides a uniform interface over the available NIC drivers (RTL8139, e1000).
 * The network stack calls these functions instead of driver-specific ones.
 * During init, we probe for each supported NIC and bind the first one found.
 */

#include <stdbool.h>
#include <stdint.h>

// Probe and initialize all supported NIC drivers; bind the first one found.
// Must be called after pci_init().
void nic_init(void);

// Returns the 6-byte MAC address of the active NIC, or NULL if none.
const uint8_t *nic_get_mac(void);

// Returns true if the active NIC's link is up.
bool nic_link_up(void);

// Returns true if any NIC was successfully initialized.
bool nic_is_present(void);

// Send a raw Ethernet frame through the active NIC.
// Returns 0 on success, -1 on error.
int nic_send(const void *data, uint16_t len);

// Poll the active NIC for pending packets (polled-mode fallback).
// Returns true if packets were found and enqueued.
bool nic_poll(void);

#endif
