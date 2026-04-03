#include "acpi/acpi.h"
#include "console/console.h"
#include "mm/pmm.h"
#include "lib/string.h"
#include <limine.h>

// ── Internal state ───────────────────────────────────────────────────────────
static struct limine_rsdp_response *rsdp_response = NULL;
static struct acpi_rsdp *rsdp = NULL;
static struct acpi_sdt_header *root_sdt = NULL;
static bool use_xsdt = false;

// ── Parsed MADT data ────────────────────────────────────────────────────────
static uint32_t lapic_base_address = 0;
static uint32_t ioapic_address     = 0;
static uint32_t ioapic_gsi         = 0;

#define MAX_ISOS 24
static struct {
    uint8_t  irq_source;
    uint32_t gsi;
    uint16_t flags;
} iso_entries[MAX_ISOS];
static uint32_t iso_count = 0;

// CPU APIC IDs discovered from MADT Local APIC entries
#define MAX_CPUS_ACPI 64
static uint8_t cpu_apic_ids[MAX_CPUS_ACPI];
static uint32_t cpu_count = 0;

// ── Helpers ──────────────────────────────────────────────────────────────────

static void print_uint32(uint32_t num) {
    if (num == 0) { console_putchar('0'); return; }
    char buf[10];
    int i = 0;
    while (num > 0) { buf[i++] = '0' + (num % 10); num /= 10; }
    while (i > 0) { console_putchar(buf[--i]); }
}

static void print_hex32(uint32_t num) {
    const char *hex = "0123456789ABCDEF";
    for (int i = 28; i >= 0; i -= 4) {
        console_putchar(hex[(num >> i) & 0xF]);
    }
}

static bool acpi_validate_checksum(void *table, size_t length) {
    uint8_t *bytes = (uint8_t *)table;
    uint8_t sum = 0;
    for (size_t i = 0; i < length; i++) {
        sum += bytes[i];
    }
    return sum == 0;
}

// ── Table lookup ─────────────────────────────────────────────────────────────

void *acpi_find_table(const char *signature) {
    if (!root_sdt) return NULL;

    size_t entries = (root_sdt->length - sizeof(struct acpi_sdt_header));
    
    if (use_xsdt) {
        entries /= 8;
        struct acpi_xsdt *xsdt = (struct acpi_xsdt *)root_sdt;
        for (size_t i = 0; i < entries; i++) {
            struct acpi_sdt_header *header = (struct acpi_sdt_header *)(xsdt->pointers[i] + pmm_get_hhdm_offset());
            if (strncmp(header->signature, signature, 4) == 0) {
                if (acpi_validate_checksum(header, header->length)) {
                    return header;
                }
            }
        }
    } else {
        entries /= 4;
        struct acpi_rsdt *rsdt = (struct acpi_rsdt *)root_sdt;
        for (size_t i = 0; i < entries; i++) {
            struct acpi_sdt_header *header = (struct acpi_sdt_header *)((uint64_t)rsdt->pointers[i] + pmm_get_hhdm_offset());
            if (strncmp(header->signature, signature, 4) == 0) {
                if (acpi_validate_checksum(header, header->length)) {
                    return header;
                }
            }
        }
    }

    return NULL;
}

// ── MADT Data Accessors ─────────────────────────────────────────────────────

uint32_t acpi_get_lapic_base(void)      { return lapic_base_address; }
uint32_t acpi_get_ioapic_base(void)     { return ioapic_address; }
uint32_t acpi_get_ioapic_gsi_base(void) { return ioapic_gsi; }

bool acpi_get_irq_override(uint8_t irq, uint32_t *gsi, uint16_t *flags) {
    for (uint32_t i = 0; i < iso_count; i++) {
        if (iso_entries[i].irq_source == irq) {
            if (gsi)   *gsi   = iso_entries[i].gsi;
            if (flags) *flags = iso_entries[i].flags;
            return true;
        }
    }
    return false;
}

uint32_t acpi_get_cpu_count(void)         { return cpu_count; }
const uint8_t *acpi_get_cpu_apic_ids(void) { return cpu_apic_ids; }

// ── Initialization ──────────────────────────────────────────────────────────

void acpi_init(struct limine_rsdp_response *response) {
    rsdp_response = response;
    console_puts("[INFO] Initializing ACPI...\n");

    if (!rsdp_response || !rsdp_response->address) {
        console_puts("[ERR] ACPI RSDP not provided by bootloader!\n");
        return;
    }

    rsdp = (struct acpi_rsdp *)rsdp_response->address;

    if (rsdp->revision >= 2) {
        struct acpi_rsdp_ext *rsdp_ext = (struct acpi_rsdp_ext *)rsdp;
        if (acpi_validate_checksum(rsdp_ext, rsdp_ext->length)) {
            use_xsdt = true;
            root_sdt = (struct acpi_sdt_header *)((uint64_t)rsdp_ext->xsdt_address + pmm_get_hhdm_offset());
            console_puts("[OK] ACPI 2.0+ (XSDT) detected.\n");
        } else {
            console_puts("[WARN] XSDT Checksum invalid!\n");
        }
    }

    if (!use_xsdt) {
        if (acpi_validate_checksum(rsdp, 20)) {
            root_sdt = (struct acpi_sdt_header *)((uint64_t)rsdp->rsdt_address + pmm_get_hhdm_offset());
            console_puts("[OK] ACPI 1.0 (RSDT) detected.\n");
        } else {
            console_puts("[ERR] RSDT Checksum invalid!\n");
            return;
        }
    }

    if (!acpi_validate_checksum(root_sdt, root_sdt->length)) {
        console_puts("[ERR] Root SDT Checksum invalid!\n");
        return;
    }

    // ── Parse the MADT ──────────────────────────────────────────────────
    struct acpi_madt *madt = (struct acpi_madt *)acpi_find_table("APIC");
    if (!madt) {
        console_puts("[ERR] No MADT (APIC Table) found.\n");
        return;
    }

    console_puts("[OK] MADT table found.\n");

    // Save the Local APIC physical base from the MADT header
    lapic_base_address = madt->local_apic_address;
    console_puts("     Local APIC Base: 0x");
    print_hex32(lapic_base_address);
    console_puts("\n");

    uint32_t core_count = 0;
    uint8_t *entries = madt->entries;
    uint8_t *end = (uint8_t *)madt + madt->header.length;
    
    while (entries < end) {
        struct acpi_madt_entry_header *header = (struct acpi_madt_entry_header *)entries;

        switch (header->type) {
        case 0: { // ── Local APIC ────────────────────────────────────────
            struct acpi_madt_local_apic *lapic = (struct acpi_madt_local_apic *)entries;
            if (lapic->flags & 1) {
                if (core_count < MAX_CPUS_ACPI) {
                    cpu_apic_ids[core_count] = lapic->apic_id;
                }
                core_count++;
            }
            break;
        }
        case 1: { // ── I/O APIC ─────────────────────────────────────────
            struct acpi_madt_ioapic *io = (struct acpi_madt_ioapic *)entries;
            // Store the first I/O APIC we find
            if (ioapic_address == 0) {
                ioapic_address = io->ioapic_address;
                ioapic_gsi     = io->gsi_base;
                console_puts("     I/O APIC Addr:  0x");
                print_hex32(ioapic_address);
                console_puts(", GSI Base: ");
                print_uint32(ioapic_gsi);
                console_puts("\n");
            }
            break;
        }
        case 2: { // ── Interrupt Source Override ─────────────────────────
            struct acpi_madt_iso *iso = (struct acpi_madt_iso *)entries;
            if (iso_count < MAX_ISOS) {
                iso_entries[iso_count].irq_source = iso->irq_source;
                iso_entries[iso_count].gsi        = iso->gsi;
                iso_entries[iso_count].flags      = iso->flags;
                iso_count++;
                console_puts("     ISO: IRQ");
                print_uint32(iso->irq_source);
                console_puts(" -> GSI ");
                print_uint32(iso->gsi);
                console_puts(" (flags=0x");
                print_hex32(iso->flags);
                console_puts(")\n");
            }
            break;
        }
        default:
            break;
        }

        if (header->length == 0) break; // prevent infinite loop on corrupt table
        entries += header->length;
    }

    cpu_count = core_count;

    console_puts("     CPU Cores active: ");
    print_uint32(core_count);
    console_puts("\n");
}
