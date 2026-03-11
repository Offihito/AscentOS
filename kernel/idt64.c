// idt64.c — AscentOS 64-bit IDT Kurulumu + PIC Yönetimi
//
// keyboard_unified.c içinden ayrıştırıldı.
// Sorumluluklar:
//   - IDT tablo ve IDTR yönetimi
//   - CPU exception handler (ISR 0-31) kayıtları
//   - PIC 8259A yeniden haritalama ve IRQ mask yönetimi
//   - IRQ handler'larının IDT'ye bağlanması (timer, klavye, mouse, ağ)
//   - PIT 1000 Hz timer ayarı
//
// Bağımlılıklar:
//   - interrupts64.asm  : isr_keyboard, isr_timer, isr_mouse, isr_net,
//                         isr0-isr31, isr8_df, load_idt64
//   - boot64_unified.asm: kernel_tss (df_stack_top IST1 kurulumu için)

#include <stdint.h>
#include "idt64.h"

// ============================================================================
// I/O Port Yardımcıları
// ============================================================================
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// ============================================================================
// Seri port çıktısı (kernel64.c'den)
// ============================================================================
extern void serial_print(const char* s);

// ============================================================================
// IDT Tablosu ve IDTR
// ============================================================================
static struct idt_entry idt[256];
static struct idt_ptr   idtr;

// ============================================================================
// task.h — kernel_tss (tss_t) doğru tipte buradan gelir
// ============================================================================
#include "task.h"

// ============================================================================
// ASM Sembolleri — interrupts64.asm
// ============================================================================
extern void isr_keyboard(void);
extern void isr_timer(void);
extern void isr_mouse(void);
extern void isr_net(void);     // IRQ11 → RTL8139 ağ kartı

// CPU Exception handler'lar (ISR 0-31)
extern void isr0(void);  extern void isr1(void);  extern void isr2(void);
extern void isr3(void);  extern void isr4(void);  extern void isr5(void);
extern void isr6(void);  extern void isr7(void);  extern void isr8_df(void);
extern void isr9(void);  extern void isr10(void); extern void isr11(void);
extern void isr12(void); extern void isr13(void); extern void isr14(void);
extern void isr15(void); extern void isr16(void); extern void isr17(void);
extern void isr18(void); extern void isr19(void); extern void isr20(void);
extern void isr21(void); extern void isr22(void); extern void isr23(void);
extern void isr24(void); extern void isr25(void); extern void isr26(void);
extern void isr27(void); extern void isr28(void); extern void isr29(void);
extern void isr30(void); extern void isr31(void);

// #DF IST1 stack tepesi + IDTR yükleyici (interrupts64.asm / boot64_unified.asm)
extern uint8_t df_stack_top[];
extern void    load_idt64(struct idt_ptr* ptr);


// ============================================================================
// Dahili Yardımcılar
// ============================================================================

// Tek bir IDT girişini yaz (dahili, static versiyon)
static void idt_write(int n, uint64_t h, uint16_t sel, uint8_t attr) {
    idt[n].offset_low  = (uint16_t)(h & 0xFFFF);
    idt[n].selector    = sel;
    idt[n].ist         = 0;
    idt[n].type_attr   = attr;
    idt[n].offset_mid  = (uint16_t)((h >> 16) & 0xFFFF);
    idt[n].offset_high = (uint32_t)((h >> 32) & 0xFFFFFFFF);
    idt[n].reserved    = 0;
}

// PIC 8259A çiftini yeniden haritala:
//   Master IRQ 0-7  → INT 0x20-0x27
//   Slave  IRQ 8-15 → INT 0x28-0x2F
// Başlangıçta tüm IRQ'lar maskelenir; irq_enable() ile tek tek açılır.
static void pic_remap(void) {
    // ICW1: başlatma komutu (cascade, ICW4 bekleniyor)
    outb(0x20, 0x11);
    outb(0xA0, 0x11);
    // ICW2: vektör ofseti
    outb(0x21, 0x20);  // master → INT 0x20
    outb(0xA1, 0x28);  // slave  → INT 0x28
    // ICW3: cascade bağlantısı (master pin2, slave kimlik=2)
    outb(0x21, 0x04);
    outb(0xA1, 0x02);
    // ICW4: 8086 modu
    outb(0x21, 0x01);
    outb(0xA1, 0x01);
    // Tüm IRQ'ları maskele; init_interrupts64() irq_enable() ile açar
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);
}

// ============================================================================
// Genel API Implementasyonu
// ============================================================================

// Dış erişim için IDT girişi yaz (idt64.h'de ilan edildi)
void idt_set_entry(int n, uint64_t handler, uint16_t selector, uint8_t attr) {
    idt_write(n, handler, selector, attr);
}

// IRQ hattını etkinleştir (mask bit'ini 0 yap)
void idt_irq_enable(uint8_t irq) {
    uint16_t port = (irq < 8) ? 0x21 : 0xA1;
    uint8_t  bit  = (irq < 8) ? irq : (irq - 8);
    outb(port, inb(port) & ~(1 << bit));
}

// IRQ hattını devre dışı bırak (mask bit'ini 1 yap)
void idt_irq_disable(uint8_t irq) {
    uint16_t port = (irq < 8) ? 0x21 : 0xA1;
    uint8_t  bit  = (irq < 8) ? irq : (irq - 8);
    outb(port, inb(port) | (1 << bit));
}

// ============================================================================
// init_interrupts64 — Ana kurulum fonksiyonu
// kernel_main() tarafından çağrılır
// ============================================================================
void init_interrupts64(void) {
    serial_print("[IDT] Initializing Interrupt Descriptor Table...\n");

    // ── Tüm 256 girişi sıfırla ───────────────────────────────────────────────
    for (int i = 0; i < 256; i++) idt_write(i, 0, 0, 0);
    serial_print("[IDT] 256 entries cleared\n");

    // ── CPU Exception Handler'ları (INT 0-31) ────────────────────────────────
    // Boş bırakılırsa CPU exception → triple fault zinciri oluşur.
    idt_write(0,  (uint64_t)isr0,  0x08, 0x8E); // #DE Divide Error
    idt_write(1,  (uint64_t)isr1,  0x08, 0x8E); // #DB Debug
    idt_write(2,  (uint64_t)isr2,  0x08, 0x8E); // #NMI
    idt_write(3,  (uint64_t)isr3,  0x08, 0x8E); // #BP Breakpoint
    idt_write(4,  (uint64_t)isr4,  0x08, 0x8E); // #OF Overflow
    idt_write(5,  (uint64_t)isr5,  0x08, 0x8E); // #BR Bound Range
    idt_write(6,  (uint64_t)isr6,  0x08, 0x8E); // #UD Invalid Opcode
    idt_write(7,  (uint64_t)isr7,  0x08, 0x8E); // #NM Device Not Available

    // #DF Double Fault — IST1 (ayrı temiz stack)
    // RSP bozukken normal stack kullanılırsa → ikinci fault → triple fault.
    // IST=1: CPU TSS.IST1'deki df_stack_top'a geçer, bozuk RSP'den bağımsız.
    idt_write(8, (uint64_t)isr8_df, 0x08, 0x8E);
    idt[8].ist = 1; // IST1 → TSS offset+36 = df_stack_top

    idt_write(9,  (uint64_t)isr9,  0x08, 0x8E); // Coprocessor Overrun
    idt_write(10, (uint64_t)isr10, 0x08, 0x8E); // #TS Invalid TSS
    idt_write(11, (uint64_t)isr11, 0x08, 0x8E); // #NP Segment Not Present
    idt_write(12, (uint64_t)isr12, 0x08, 0x8E); // #SS Stack Fault
    idt_write(13, (uint64_t)isr13, 0x08, 0x8E); // #GP General Protection
    idt_write(14, (uint64_t)isr14, 0x08, 0x8E); // #PF Page Fault
    idt_write(15, (uint64_t)isr15, 0x08, 0x8E); // Reserved
    idt_write(16, (uint64_t)isr16, 0x08, 0x8E); // #MF x87 FP Error
    idt_write(17, (uint64_t)isr17, 0x08, 0x8E); // #AC Alignment Check
    idt_write(18, (uint64_t)isr18, 0x08, 0x8E); // #MC Machine Check
    idt_write(19, (uint64_t)isr19, 0x08, 0x8E); // #XF SIMD FP
    idt_write(20, (uint64_t)isr20, 0x08, 0x8E); // #VE Virtualization
    idt_write(21, (uint64_t)isr21, 0x08, 0x8E); // #CP Control Protection
    idt_write(22, (uint64_t)isr22, 0x08, 0x8E);
    idt_write(23, (uint64_t)isr23, 0x08, 0x8E);
    idt_write(24, (uint64_t)isr24, 0x08, 0x8E);
    idt_write(25, (uint64_t)isr25, 0x08, 0x8E);
    idt_write(26, (uint64_t)isr26, 0x08, 0x8E);
    idt_write(27, (uint64_t)isr27, 0x08, 0x8E);
    idt_write(28, (uint64_t)isr28, 0x08, 0x8E);
    idt_write(29, (uint64_t)isr29, 0x08, 0x8E);
    idt_write(30, (uint64_t)isr30, 0x08, 0x8E); // #SX Security
    idt_write(31, (uint64_t)isr31, 0x08, 0x8E);
    serial_print("[IDT] Exception handlers registered (INT 0-31)\n");

    // ── TSS.IST1 = df_stack_top ───────────────────────────────────────────────
    // Intel SDM tss_t layout: +36. bayt = IST1 (8 bayt, little-endian)
    *((uint64_t*)((uint8_t*)&kernel_tss + 36)) = (uint64_t)df_stack_top;
    serial_print("[IDT] IST1 (#DF stack) configured\n");

    // ── PIC 8259A yeniden haritalama ─────────────────────────────────────────
    pic_remap();
    serial_print("[IDT] PIC 8259A remapped — Master:0x20 Slave:0x28\n");

    // ── IRQ Handler'larını IDT'ye bağla ─────────────────────────────────────
    idt_write(32, (uint64_t)isr_timer,    0x08, 0x8E); // IRQ0  Timer
    idt_write(33, (uint64_t)isr_keyboard, 0x08, 0x8E); // IRQ1  Klavye
    idt_write(43, (uint64_t)isr_net,      0x08, 0x8E); // IRQ11 RTL8139 (0x20+11=0x2B)
    idt_write(44, (uint64_t)isr_mouse,    0x08, 0x8E); // IRQ12 Mouse
    serial_print("[IDT] IRQ handlers: Timer(32) KB(33) Net(43) Mouse(44)\n");

    // ── IDTR'yi yükle ────────────────────────────────────────────────────────
    idtr.limit = sizeof(idt) - 1;
    idtr.base  = (uint64_t)&idt;
    load_idt64(&idtr);
    serial_print("[IDT] IDTR loaded (256 entries, 4096 bytes)\n");

    // ── IRQ mask'leri aç ─────────────────────────────────────────────────────
    idt_irq_enable(0);   // Timer
    idt_irq_enable(1);   // Klavye
    idt_irq_enable(2);   // Cascade (slave PIC için zorunlu)
    idt_irq_enable(11);  // RTL8139 ağ kartı
    idt_irq_enable(12);  // Mouse
    serial_print("[IDT] IRQ lines unmasked: 0(Timer) 1(KB) 2(Cascade) 11(Net) 12(Mouse)\n");

    // ── PIT 1000 Hz Timer ────────────────────────────────────────────────────
    // Kanal 0, Mode 3 (kare dalga), 1193182 / 1000 ≈ 1193 bölen
    uint32_t div = 1193182 / 1000;
    outb(0x43, 0x36);               // Kontrol: kanal 0, lobyte/hibyte, mode 3
    outb(0x40, (uint8_t)(div & 0xFF));
    outb(0x40, (uint8_t)((div >> 8) & 0xFF));
    serial_print("[IDT] PIT configured: 1000 Hz (div=1193)\n");

    // NOT: STI burada YAPILMIYOR.
    // Timer IRQ 1ms'de bir ateşlenir; task sistemi hazır olmadan
    // scheduler_tick() → NULL dereference → #GP.
    // STI, kernel_main içinde task_init + scheduler_init bittikten sonra yapılır.
    serial_print("[IDT] Ready — STI deferred until scheduler init\n");
}