#ifndef ACPI_H
#define ACPI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <limine.h>

// ── ACPI SDT Header ─────────────────────────────────────────────────────────
struct acpi_sdt_header {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

// ── ACPI 1.0 RSDP ───────────────────────────────────────────────────────────
struct acpi_rsdp {
    char signature[8]; // "RSD PTR "
    uint8_t checksum;
    char oem_id[6];
    uint8_t revision;
    uint32_t rsdt_address;
} __attribute__((packed));

// ── ACPI 2.0+ Extended RSDP ─────────────────────────────────────────────────
struct acpi_rsdp_ext {
    struct acpi_rsdp first_part;
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t extended_checksum;
    uint8_t reserved[3];
} __attribute__((packed));

// ── RSDT (32-bit pointers) ──────────────────────────────────────────────────
struct acpi_rsdt {
    struct acpi_sdt_header header;
    uint32_t pointers[]; 
} __attribute__((packed));

// ── XSDT (64-bit pointers) ──────────────────────────────────────────────────
struct acpi_xsdt {
    struct acpi_sdt_header header;
    uint64_t pointers[];
} __attribute__((packed));

// ── MADT (Multiple APIC Description Table) ──────────────────────────────────
struct acpi_madt {
    struct acpi_sdt_header header;
    uint32_t local_apic_address;
    uint32_t flags;
    uint8_t entries[];
} __attribute__((packed));

// ── MADT Entry Header ───────────────────────────────────────────────────────
struct acpi_madt_entry_header {
    uint8_t type;
    uint8_t length;
} __attribute__((packed));

// ── MADT Entry Type 0: Processor Local APIC ─────────────────────────────────
struct acpi_madt_local_apic {
    struct acpi_madt_entry_header header;
    uint8_t acpi_processor_id;
    uint8_t apic_id;
    uint32_t flags;
} __attribute__((packed));

// ── MADT Entry Type 1: I/O APIC ─────────────────────────────────────────────
struct acpi_madt_ioapic {
    struct acpi_madt_entry_header header;
    uint8_t ioapic_id;
    uint8_t reserved;
    uint32_t ioapic_address;
    uint32_t gsi_base;
} __attribute__((packed));

// ── ACPI MADT Entry Type 2: Interrupt Source Override ─────────────────────────────
struct acpi_madt_iso {
    struct acpi_madt_entry_header header;
    uint8_t bus_source;     // 0 = ISA
    uint8_t irq_source;     // ISA IRQ number
    uint32_t gsi;            // Global System Interrupt it maps to
    uint16_t flags;          // Polarity (bits 1:0) and Trigger (bits 3:2)
} __attribute__((packed));

// ── MCFG (Memory Mapped Configuration Space Base Address Description Table) ─────
struct acpi_mcfg_entry {
    uint64_t base_address;
    uint16_t pci_segment_group_number;
    uint8_t start_bus_number;
    uint8_t end_bus_number;
    uint32_t reserved;
} __attribute__((packed));

struct acpi_mcfg {
    struct acpi_sdt_header header;
    uint64_t reserved;
    struct acpi_mcfg_entry entries[];
} __attribute__((packed));

// ── ACPI Initialization & Table Lookup ───────────────────────────────────────
void acpi_init(struct limine_rsdp_response *response);
void *acpi_find_table(const char *signature);

// ── MADT Data Accessors (valid after acpi_init) ─────────────────────────────

// Returns the physical base address of the Local APIC from the MADT.
uint32_t acpi_get_lapic_base(void);

// Returns the physical base address of the first I/O APIC.
uint32_t acpi_get_ioapic_base(void);

// Returns the GSI base of the first I/O APIC.
uint32_t acpi_get_ioapic_gsi_base(void);

// Looks up an Interrupt Source Override for the given ISA IRQ.
// If found, *gsi and *flags are filled in and the function returns true.
bool acpi_get_irq_override(uint8_t irq, uint32_t *gsi, uint16_t *flags);

// Returns the number of enabled CPUs discovered in the MADT.
uint32_t acpi_get_cpu_count(void);

// Returns a pointer to the array of APIC IDs for all enabled CPUs.
const uint8_t *acpi_get_cpu_apic_ids(void);

// Returns the MCFG table if present, otherwise NULL.
struct acpi_mcfg *acpi_get_mcfg(void);

#endif
