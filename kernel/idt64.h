// idt64.h — AscentOS 64-bit IDT (Interrupt Descriptor Table) API
// Bu dosyayı #include eden modüller IDT kurulumunu ve PIC yönetimini kullanabilir.

#ifndef IDT64_H
#define IDT64_H

#include <stdint.h>

// ============================================================================
// IDT Giriş Yapısı
// ============================================================================
struct idt_entry {
    uint16_t offset_low;   // Handler adresinin [15:0]
    uint16_t selector;     // Kernel code segment (0x08)
    uint8_t  ist;          // IST alanı (0 = normal stack, 1 = IST1 #DF için)
    uint8_t  type_attr;    // Tip + DPL + Present bitleri (0x8E = interrupt gate)
    uint16_t offset_mid;   // Handler adresinin [31:16]
    uint32_t offset_high;  // Handler adresinin [63:32]
    uint32_t reserved;     // Sıfır olmalı
} __attribute__((packed));

// ============================================================================
// IDTR (IDT Register) Yapısı
// ============================================================================
struct idt_ptr {
    uint16_t limit;   // IDT boyutu - 1
    uint64_t base;    // IDT'nin lineer adresi
} __attribute__((packed));

// ============================================================================
// Genel API
// ============================================================================

// IDT'yi sıfırla, exception/IRQ gate'lerini kur, PIC'i yeniden haritala,
// Timer'ı ayarla ve STI ile interrupt'ları etkinleştir.
// kernel64.c → kernel_main() içinde init_keyboard64()'dan ÖNCE çağrılır.
void init_interrupts64(void);

// Tek bir IDT girişini programatik olarak ayarla.
// n        : vektör numarası (0–255)
// handler  : 64-bit handler fonksiyonunun adresi
// selector : segment seçici (kernel code = 0x08)
// attr     : tip/DPL/P bitleri (interrupt gate = 0x8E)
void idt_set_entry(int n, uint64_t handler, uint16_t selector, uint8_t attr);

// Belirli bir IRQ hattını PIC üzerinden etkinleştir (mask'i kaldır).
// irq: 0-7 → master PIC, 8-15 → slave PIC
void idt_irq_enable(uint8_t irq);

// Belirli bir IRQ hattını PIC üzerinden devre dışı bırak (mask'i koy).
void idt_irq_disable(uint8_t irq);

#endif // IDT64_H