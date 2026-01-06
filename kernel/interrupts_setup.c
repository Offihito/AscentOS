// interrupts_setup.c - IDT Setup with Keyboard and Mouse Support
#include <stdint.h>

// Port I/O
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// IDT Entry
typedef struct {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t attributes;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} __attribute__((packed)) IDTEntry;

// IDT Pointer
typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) IDTPointer;

// IDT tablosu (256 entry)
static IDTEntry idt[256];
static IDTPointer idtp;

// Dış interrupt handler'lar
extern void isr_keyboard(void);
extern void isr_mouse(void);
extern void load_idt64(IDTPointer* ptr);
extern void isr_timer(void);
extern void isr_scheduler(void);

// IDT entry ayarla
static void idt_set_gate(uint8_t num, uint64_t handler, uint16_t sel, uint8_t flags) {
    idt[num].offset_low = handler & 0xFFFF;
    idt[num].selector = sel;
    idt[num].ist = 0;
    idt[num].attributes = flags;
    idt[num].offset_mid = (handler >> 16) & 0xFFFF;
    idt[num].offset_high = (handler >> 32) & 0xFFFFFFFF;
    idt[num].zero = 0;
}

// PIC'i yeniden programla
static void pic_remap(void) {
    // Master PIC
    outb(0x20, 0x11);
    outb(0x21, 0x20);  // IRQ0-7 -> INT 32-39
    outb(0x21, 0x04);
    outb(0x21, 0x01);
    
    // Slave PIC
    outb(0xA0, 0x11);
    outb(0xA1, 0x28);  // IRQ8-15 -> INT 40-47
    outb(0xA1, 0x02);
    outb(0xA1, 0x01);
    
    // Tüm IRQ'ları devre dışı bırak
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);
}

// IRQ'yu etkinleştir
static void irq_enable(uint8_t irq) {
    uint16_t port;
    uint8_t value;
    
    if (irq < 8) {
        port = 0x21;  // Master PIC
    } else {
        port = 0xA1;  // Slave PIC
        irq -= 8;
    }
    
    value = inb(port) & ~(1 << irq);
    outb(port, value);
}

void init_interrupts64(void) {
    // IDT'yi sıfırla
    for (int i = 0; i < 256; i++) {
        idt[i].offset_low = 0;
        idt[i].selector = 0;
        idt[i].ist = 0;
        idt[i].attributes = 0;
        idt[i].offset_mid = 0;
        idt[i].offset_high = 0;
        idt[i].zero = 0;
    }
    
    // PIC'i yeniden programla
    pic_remap();
    
    // Timer interrupt'ı ayarla (IRQ0 -> INT 32)
    idt_set_gate(32, (uint64_t)isr_timer, 0x08, 0x8E);
    
    // Keyboard interrupt'ı ayarla (IRQ1 -> INT 33)
    idt_set_gate(33, (uint64_t)isr_keyboard, 0x08, 0x8E);
    
    // Mouse interrupt'ı ayarla (IRQ12 -> INT 44)
    idt_set_gate(44, (uint64_t)isr_mouse, 0x08, 0x8E);
    
    // Scheduler interrupt (INT 0x80) - Software interrupt
    idt_set_gate(0x80, (uint64_t)isr_scheduler, 0x08, 0x8E);
    
    // IDT pointer'ı ayarla
    idtp.limit = sizeof(idt) - 1;
    idtp.base = (uint64_t)&idt;
    
    // IDT'yi yükle
    load_idt64(&idtp);
    
    // IRQ'ları etkinleştir
    irq_enable(0);   // Timer (CRITICAL for multitasking!)
    irq_enable(1);   // Keyboard
    irq_enable(12);  // Mouse (Slave PIC'te IRQ4)
    irq_enable(2);   // Slave PIC cascade
    
    // Timer'ı yapılandır (PIT - Programmable Interval Timer)
    // 1000 Hz (1ms per tick)
    uint32_t divisor = 1193182 / 1000;  // 1000 Hz
    outb(0x43, 0x36);  // Channel 0, mode 3 (square wave)
    outb(0x40, divisor & 0xFF);
    outb(0x40, (divisor >> 8) & 0xFF);
    
    // Interrupt'ları etkinleştir
    __asm__ volatile ("sti");
}
