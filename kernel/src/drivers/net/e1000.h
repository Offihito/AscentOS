#ifndef DRIVERS_NET_E1000_H
#define DRIVERS_NET_E1000_H

#include <stdbool.h>
#include <stdint.h>

// Initialize the Intel e1000 NIC: PCI probe, reset, RX/TX ring setup, IRQ install.
// Prints status to console. Safe to call if no e1000 is present (just logs a warning).
void e1000_init(void);

// Returns pointer to the 6-byte MAC address, or NULL if NIC not initialized.
const uint8_t *e1000_get_mac(void);

// Returns true if the NIC link is detected as up.
bool e1000_link_up(void);

// Returns true if the e1000 was successfully initialized.
bool e1000_is_present(void);

// Send a raw Ethernet frame. Returns 0 on success, -1 on error.
int e1000_send(const void *data, uint16_t len);

// Poll the NIC for pending packets (polled-mode fallback).
// Returns true if packets were found and enqueued.
bool e1000_poll(void);

#endif
