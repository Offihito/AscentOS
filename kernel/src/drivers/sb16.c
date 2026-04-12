#include "sb16.h"
#include "../console/console.h"
#include "../console/klog.h"
#include "../io/io.h"
#include "../dma/dma.h"
#include "../apic/ioapic.h"
#include "../apic/lapic.h"
#include "../acpi/acpi.h"
#include "../cpu/isr.h"
#include "../fs/vfs.h"
#include "../mm/pmm.h"
#include "../mm/heap.h"
#include "../lib/string.h"
#include "../sched/sched.h"
#include "../fb/framebuffer.h"

#define SB16_BASE 0x220
#define MIXER_REG (SB16_BASE + 0x4)
#define MIXER_DATA (SB16_BASE + 0x5)
#define DSP_RESET (SB16_BASE + 0x6)
#define DSP_READ  (SB16_BASE + 0xA)
#define DSP_WRITE (SB16_BASE + 0xC)
#define DSP_READ_STATUS (SB16_BASE + 0xE)
#define DSP_INT_ACK_8 (SB16_BASE + 0xE)
#define DSP_INT_ACK_16 (SB16_BASE + 0xF)

volatile uint32_t sb16_irq_fired = 0;
static bool sb16_present = false;
static bool sb16_active_16bit = false;

// ── OSS device state (defaults match /dev/dsp standard) ─────────────────────
static uint32_t dsp_sample_rate = 8000;
static uint8_t  dsp_channels    = 1;
static uint8_t  dsp_bits        = 8;

// ── DMA buffer (64KB-aligned within ISA 16MB) ───────────────────────────────
static uint8_t *sb16_dma_buf  = 0;
static uint64_t sb16_dma_phys = 0;

// ── DSP low-level I/O ───────────────────────────────────────────────────────

static void dsp_write(uint8_t value) {
    while (inb(DSP_WRITE) & 0x80) {
        io_wait();
    }
    outb(DSP_WRITE, value);
}

static void sb16_mixer_write(uint8_t reg, uint8_t val) {
    outb(MIXER_REG, reg);
    outb(MIXER_DATA, val);
}

static uint8_t dsp_read(void) {
    while (!(inb(DSP_READ_STATUS) & 0x80)) {
        io_wait();
    }
    return inb(DSP_READ);
}

// ── ISR ─────────────────────────────────────────────────────────────────────

static void sb16_isr(struct registers *regs) {
    (void)regs;
    if (sb16_active_16bit) {
        inb(DSP_INT_ACK_16);
    } else {
        inb(DSP_INT_ACK_8);
    }
    
    // Read Mixer IRQ status to clear interrupt (critical for hardware)
    outb(MIXER_REG, 0x82);
    (void)inb(MIXER_DATA);

    sb16_irq_fired = 1;
}

// ── DMA buffer allocation ───────────────────────────────────────────────────

static int ensure_dma_buffer(void) {
    if (sb16_dma_buf) return 0;
    // Allocate 32 pages (128KB) to guarantee a 64KB-aligned block in ISA range
    uint64_t raw_phys = (uint64_t)pmm_alloc_blocks(32);
    if (!raw_phys) return -1;
    sb16_dma_phys = (raw_phys + 65535) & ~65535ULL;
    sb16_dma_buf = (uint8_t *)(sb16_dma_phys + pmm_get_hhdm_offset());
    return 0;
}

// ── Hardware initialization ─────────────────────────────────────────────────

void sb16_init(void) {
    // Reset DSP
    outb(DSP_RESET, 1);
    for (int i = 0; i < 10; i++) io_wait();
    outb(DSP_RESET, 0);

    // Wait for ready
    uint32_t timeout = 10000;
    while (timeout > 0) {
        if ((inb(DSP_READ_STATUS) & 0x80) && (inb(DSP_READ) == 0xAA)) {
            break;
        }
        io_wait();
        timeout--;
    }

    if (timeout == 0) {
        console_puts("[WARN] SB16 Sound Card not detected.\n");
        return;
    }

    // Get DSP version
    dsp_write(0xE1);
    uint8_t major = dsp_read();
    uint8_t minor = dsp_read();

    console_puts("[INFO] SB16 Sound Card detected (DSP v");
    char c1 = '0' + (major % 10);
    console_putchar((major > 9) ? '1' : c1);
    if (major > 9) console_putchar('0' + (major % 10));
    console_putchar('.');
    console_putchar('0' + (minor / 10));
    console_putchar('0' + (minor % 10));
    console_puts(").\n");

    // Route IRQ 5 via APIC
    uint8_t irq = 5;
    uint8_t vector = 32 + irq;
    uint32_t gsi = irq;
    uint16_t flags = 0;
    acpi_get_irq_override(irq, &gsi, &flags);

    register_interrupt_handler(vector, sb16_isr);
    ioapic_route_irq((uint8_t)gsi, vector, (uint8_t)lapic_get_id(), flags);

    // Mute auxiliary inputs to eliminate analog background sizzle
    sb16_mixer_write(0x22, 0xFF); // Master Vol L/R
    sb16_mixer_write(0x04, 0xFF); // Voice/DAC Vol L/R
    sb16_mixer_write(0x0A, 0x00); // Mic Mute
    sb16_mixer_write(0x26, 0x00); // CD Audio Mute
    sb16_mixer_write(0x2E, 0x00); // Line-In Mute

    sb16_present = true;
}

// ── Play a single DMA chunk (internal) ──────────────────────────────────────

void sb16_play_chunk(uint32_t phys_addr, uint32_t length, uint16_t sample_rate, uint8_t channels, uint8_t bits) {
    if (!sb16_present) return;

    sb16_irq_fired = 0;

    static int last_bits = -1;
    static uint16_t last_rate = 0;

    bool same_config = (bits == last_bits && sample_rate == last_rate);

    if (!same_config) {
        // Stop current transfer before changing rate
        if (sb16_irq_fired == 0) { // If a transfer was potentially active
            dsp_write(bits == 16 ? 0xD5 : 0xD0); // Pause/Stop commands
        }
        
        // SB16 expects: Command 0x41, High Byte, Low Byte
        dsp_write(0x41);
        dsp_write((sample_rate >> 8) & 0xFF);
        dsp_write(sample_rate & 0xFF);
        
        last_bits = bits;
        last_rate = sample_rate;
    }

    uint8_t mode = 0;
    if (channels == 2) mode |= 0x20;

    uint16_t dsp_count;
    if (bits == 16) {
        mode |= 0x10; // Signed PCM
        if (channels == 2) dsp_count = (length / 4) - 1;
        else dsp_count = (length / 2) - 1;
    } else {
        if (channels == 2) dsp_count = (length / 2) - 1;
        else dsp_count = length - 1;
    }

    if (bits == 16) {
        mode |= 0x10;
        sb16_active_16bit = true;
        dma_start(5, DMA_MODE_SINGLE | DMA_MODE_READ, phys_addr, length);
        dsp_write(0xB0); // 16-bit Single-cycle
        dsp_write(mode);
        dsp_write(dsp_count & 0xFF);
        dsp_write((dsp_count >> 8) & 0xFF);
    } else {
        sb16_active_16bit = false;
        dma_start(1, DMA_MODE_SINGLE | DMA_MODE_READ, phys_addr, length);
        dsp_write(0xC0); // 8-bit Single-cycle
        dsp_write(mode);
        dsp_write(dsp_count & 0xFF);
        dsp_write((dsp_count >> 8) & 0xFF);
    }
}

// ── OSS /dev/dsp format control ─────────────────────────────────────────────

void sb16_set_format(uint32_t rate, uint8_t channels, uint8_t bits) {
    if (rate >= 4000 && rate <= 48000) dsp_sample_rate = rate;
    if (channels == 1 || channels == 2) dsp_channels = channels;
    if (bits == 8 || bits == 16) dsp_bits = bits;
}

uint32_t sb16_get_sample_rate(void) { return dsp_sample_rate; }
uint8_t  sb16_get_channels(void)    { return dsp_channels; }
uint8_t  sb16_get_bits(void)        { return dsp_bits; }

// ── VFS write callback for /dev/dsp ─────────────────────────────────────────

static uint32_t dsp_vfs_write(struct vfs_node *node, uint32_t offset,
                               uint32_t size, uint8_t *buffer) {
    (void)node; (void)offset;
    if (!sb16_present || !buffer || size == 0) return 0;
    if (ensure_dma_buffer() != 0) return 0;

    uint32_t written = 0;
    while (written < size) {
        uint32_t chunk = size - written;
        if (chunk > 8192) chunk = 8192; // Match 8KB reference chunk size

        memcpy(sb16_dma_buf, buffer + written, chunk);
        sb16_play_chunk((uint32_t)sb16_dma_phys, chunk,
                        (uint16_t)dsp_sample_rate, dsp_channels, dsp_bits);

        // Block until DMA transfer completes
        while (!sb16_irq_fired) {
            sched_yield();
        }

        written += chunk;
    }
    return written;
}

// ── VFS registration ────────────────────────────────────────────────────────

void sb16_register_vfs(void) {
    if (!sb16_present) return;

    vfs_node_t *node = kmalloc(sizeof(vfs_node_t));
    if (!node) return;

    memset(node, 0, sizeof(vfs_node_t));
    strcpy(node->name, "dsp");
    node->flags  = FS_CHARDEV;
    node->mask   = 0666;
    node->length = 0;
    node->write  = dsp_vfs_write;

    fb_register_device_node("dsp", node);
}
