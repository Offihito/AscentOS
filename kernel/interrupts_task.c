// interrupts_task.c - Interrupt Initialization for Task-Based Keyboard
// Keyboard is now a task, not an interrupt handler

#include <stdint.h>

// ============================================================================
// IDT STRUCTURES
// ============================================================================

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct idt_entry idt[256];
static struct idt_ptr idtr;

// External assembly functions
extern void load_idt64(struct idt_ptr* ptr);
extern void isr_timer(void);

#ifdef GUI_MODE
extern void isr_mouse(void);
#endif

extern void serial_print(const char* str);

// ============================================================================
// I/O PORT OPERATIONS
// ============================================================================

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

// ============================================================================
// IDT SETUP
// ============================================================================

void set_idt_entry(int num, uint64_t handler, uint16_t selector, 
                   uint8_t type_attr, uint8_t ist) {
    idt[num].offset_low = handler & 0xFFFF;
    idt[num].offset_mid = (handler >> 16) & 0xFFFF;
    idt[num].offset_high = (handler >> 32) & 0xFFFFFFFF;
    idt[num].selector = selector;
    idt[num].ist = ist;
    idt[num].type_attr = type_attr;
    idt[num].reserved = 0;
}

// ============================================================================
// PIC REMAPPING
// ============================================================================

void remap_pic(void) {
    // ICW1 - Initialize
    outb(0x20, 0x11);
    outb(0x21, 0x20);  // IRQ0-7 -> INT 32-39
    outb(0x21, 0x04);
    outb(0x21, 0x01);
    
    outb(0xA0, 0x11);
    outb(0xA1, 0x28);  // IRQ8-15 -> INT 40-47
    outb(0xA1, 0x02);
    outb(0xA1, 0x01);
    
    // Mask settings:
    // - IRQ0 (timer) = ENABLED (bit 0 = 0)
    // - IRQ1 (keyboard) = DISABLED (bit 1 = 1) - Keyboard is now a task!
    // - IRQ12 (mouse) = depends on mode
    
    #ifdef GUI_MODE
    // Enable timer (IRQ0) only
    // 0xFE = 11111110 (only bit 0 is 0 = timer enabled)
    outb(0x21, 0xFE);
    
    // Enable mouse on slave PIC (IRQ12 = bit 4 on slave)
    // 0xEF = 11101111 (bit 4 is 0 = mouse enabled)
    outb(0xA1, 0xEF);
    #else
    // TEXT MODE - Enable timer (IRQ0) only
    // 0xFE = 11111110 (only bit 0 is 0 = timer enabled)
    outb(0x21, 0xFE);
    outb(0xA1, 0xFF);  // Disable all on slave PIC
    #endif
}

// ============================================================================
// INTERRUPT INITIALIZATION
// ============================================================================

void init_interrupts64(void) {
    serial_print("[INTERRUPTS] Initializing interrupts (task-based keyboard)...\n");
    
    // Clear IDT
    for (int i = 0; i < 256; i++) {
        set_idt_entry(i, 0, 0, 0, 0);
    }
    
    // Timer interrupt (IRQ0 -> INT 0x20 = 32)
    set_idt_entry(0x20, (uint64_t)isr_timer, 0x08, 0x8E, 0);
    serial_print("[INTERRUPTS] Timer interrupt (IRQ0) registered\n");
    
    // KEYBOARD INTERRUPT IS NOT SET UP - Keyboard is now a task!
    serial_print("[INTERRUPTS] Keyboard interrupt DISABLED (using task-based polling)\n");
    
    #ifdef GUI_MODE
    // Mouse interrupt (IRQ12 -> INT 0x2C = 44) - GUI mode only
    set_idt_entry(0x2C, (uint64_t)isr_mouse, 0x08, 0x8E, 0);
    serial_print("[INTERRUPTS] Mouse interrupt (IRQ12) registered\n");
    #endif
    
    // Remap PIC
    remap_pic();
    serial_print("[INTERRUPTS] PIC remapped\n");
    
    // Load IDT
    idtr.limit = sizeof(idt) - 1;
    idtr.base = (uint64_t)&idt;
    load_idt64(&idtr);
    serial_print("[INTERRUPTS] IDT loaded\n");
    
    // Configure PIT (Programmable Interval Timer) for 1000 Hz
    uint32_t divisor = 1193182 / 1000;  // 1000 Hz = 1ms per tick
    outb(0x43, 0x36);  // Channel 0, mode 3 (square wave)
    outb(0x40, divisor & 0xFF);
    outb(0x40, (divisor >> 8) & 0xFF);
    serial_print("[INTERRUPTS] PIT configured for 1000 Hz\n");
    
    // Enable interrupts
    __asm__ volatile ("sti");
    serial_print("[INTERRUPTS] Interrupts enabled\n");
}