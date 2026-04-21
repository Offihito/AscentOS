#ifndef DRIVERS_NET_RTL8139_H
#define DRIVERS_NET_RTL8139_H

#include <stdbool.h>
#include <stdint.h>

// Initialize the RTL8139 NIC: PCI probe, reset, RX/TX ring setup, IRQ install.
// Prints status to console. Safe to call if no RTL8139 is present (just logs a warning).
void rtl8139_init(void);

// Returns pointer to the 6-byte MAC address, or NULL if NIC not initialized.
const uint8_t *rtl8139_get_mac(void);

// Returns true if the NIC link is detected as up.
bool rtl8139_link_up(void);

// Returns true if the RTL8139 was successfully initialized.
bool rtl8139_is_present(void);

// Send a raw Ethernet frame. Returns 0 on success, -1 on error.
int rtl8139_send(const void *data, uint16_t len);

// Get the I/O base port of the NIC (for diagnostic display).
uint16_t rtl8139_get_iobase(void);

// Get the IRQ line assigned to the NIC.
uint8_t rtl8139_get_irq(void);

// Poll the NIC for pending packets (polled-mode fallback).
// Returns true if packets were found and enqueued.
bool rtl8139_poll(void);

#endif
