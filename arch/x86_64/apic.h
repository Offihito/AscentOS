#ifndef APIC_H
#define APIC_H

// ============================================================================
// apic.h — Local APIC (LAPIC) + I/O APIC driver header
//
// Desteklenen özellikler:
//   - CPUID bit-9 ile APIC varlığı kontrolü
//   - IA32_APIC_BASE MSR üzerinden fiziksel taban okuma
//   - LAPIC MMIO map + enable (SVR bit-8)
//   - LAPIC timer: PIT ch2 kalibrasyonu, periodic mod, maskeleme
//   - I/O APIC: MMIO map, IOREDTBL yönetimi (mask / unmask / route)
//   - 8259A PIC maskesi (APIC devreye girdikten sonra)
//   - EOI: lapic_eoi() — timer/ağ ISR'larından çağrılır
//
// Kullanım sırası:
//   1. apic_detect()          — CPUID ile varlık kontrolü
//   2. apic_init()            — LAPIC + IOAPIC başlat
//   3. lapic_timer_init(hz)   — LAPIC timer başlat (PIT'in yerini alır)
//   4. apic_disable_pic()     — 8259A'yı maskele (isteğe bağlı)
// ============================================================================

#include <stdint.h>

// ============================================================================
// Varsayılan fiziksel taban adresleri
// ============================================================================
#define LAPIC_PHYS_DEFAULT   0xFEE00000ULL   // Local APIC MMIO
#define IOAPIC_PHYS_DEFAULT  0xFEC00000ULL   // I/O APIC MMIO

// ============================================================================
// LAPIC yazmaç ofsetleri (LAPIC taban adresine göre)
// ============================================================================
#define LAPIC_ID             0x020   // APIC ID
#define LAPIC_VER            0x030   // Version
#define LAPIC_TPR            0x080   // Task Priority Register
#define LAPIC_APR            0x090   // Arbitration Priority Register
#define LAPIC_PPR            0x0A0   // Processor Priority Register
#define LAPIC_EOI_REG        0x0B0   // End-Of-Interrupt (yazma: herhangi bir değer)
#define LAPIC_LDR            0x0D0   // Logical Destination Register
#define LAPIC_DFR            0x0E0   // Destination Format Register
#define LAPIC_SVR            0x0F0   // Spurious Interrupt Vector Register
#define LAPIC_ISR0           0x100   // In-Service Register 0 (8 x 32-bit)
#define LAPIC_TMR0           0x180   // Trigger Mode Register 0
#define LAPIC_IRR0           0x200   // Interrupt Request Register 0
#define LAPIC_ESR            0x280   // Error Status Register
#define LAPIC_ICR_LO         0x300   // Interrupt Command (düşük 32 bit)
#define LAPIC_ICR_HI         0x310   // Interrupt Command (yüksek 32 bit)
#define LAPIC_LVT_TIMER      0x320   // LVT Timer
#define LAPIC_LVT_THERMAL    0x330   // LVT Thermal Sensor
#define LAPIC_LVT_PERF       0x340   // LVT Perf Mon Counter
#define LAPIC_LVT_LINT0      0x350   // LVT LINT0
#define LAPIC_LVT_LINT1      0x360   // LVT LINT1
#define LAPIC_LVT_ERROR      0x370   // LVT Error
#define LAPIC_TIMER_INIT     0x380   // Initial Count Register
#define LAPIC_TIMER_CUR      0x390   // Current Count Register
#define LAPIC_TIMER_DIV      0x3E0   // Divide Configuration Register

// SVR bayrakları
#define LAPIC_SVR_SW_ENABLE  (1u << 8)    // Yazılım etkinleştirme
#define LAPIC_SVR_FOCUS_DIS  (1u << 9)    // Focus işlemciyi devre dışı bırak

// LVT genel bayraklar
#define LAPIC_LVT_MASKED     (1u << 16)   // Girişi maskele

// LVT delivery mode bits (10:8)
#define LAPIC_LVT_DM_FIXED   (0u << 8)
#define LAPIC_LVT_DM_NMI     (4u << 8)
#define LAPIC_LVT_DM_EXTINT  (7u << 8)

// Timer mod bayrakları (LVT_TIMER yazmaç bit 17-18)
#define LAPIC_TIMER_ONESHOT  (0u << 17)   // Tek atış
#define LAPIC_TIMER_PERIODIC (1u << 17)   // Periyodik
#define LAPIC_TIMER_TSC_DDL  (2u << 17)   // TSC-deadline (x2APIC gerektirir)

// Timer bölen kodları (LAPIC_TIMER_DIV yazmaç)
#define LAPIC_DIV_BY_2       0x00
#define LAPIC_DIV_BY_4       0x01
#define LAPIC_DIV_BY_8       0x02
#define LAPIC_DIV_BY_16      0x03
#define LAPIC_DIV_BY_32      0x08
#define LAPIC_DIV_BY_64      0x09
#define LAPIC_DIV_BY_128     0x0A
#define LAPIC_DIV_BY_1       0x0B

// ============================================================================
// I/O APIC yazmaç ofsetleri (IOAPIC taban adresine göre)
// ============================================================================
#define IOAPIC_REGSEL        0x00    // Yazmaç seçici (indeks)
#define IOAPIC_IOWIN         0x10    // Veri penceresi

// Dolaylı yazmaçlar (REGSEL üzerinden seçilir)
#define IOAPIC_REG_ID        0x00    // I/O APIC ID
#define IOAPIC_REG_VER       0x01    // Versiyon + maks. redirection girişi
#define IOAPIC_REG_ARB       0x02    // Arbitration ID
// IOREDTBL[n]: n numaralı redirection girişi
//   Low  32-bit: reg 0x10 + 2*n
//   High 32-bit: reg 0x11 + 2*n
#define IOAPIC_REDTBL_LO(n)  (0x10u + 2u * (n))
#define IOAPIC_REDTBL_HI(n)  (0x11u + 2u * (n))

// IOREDTBL bayrakları
#define IOAPIC_REDIR_MASKED     (1u << 16)   // Bu IRQ'yu maskele
#define IOAPIC_REDIR_LEVEL      (1u << 15)   // Level-triggered (varsayılan: edge)
#define IOAPIC_REDIR_ACTLOW     (1u << 13)   // Aktif-düşük (varsayılan: aktif-yüksek)
#define IOAPIC_REDIR_LOGICAL    (1u << 11)   // Hedef: mantıksal (varsayılan: fiziksel)

// Dağıtım modları (IOREDTBL bit 10:8)
#define IOAPIC_DELIV_FIXED      (0u << 8)
#define IOAPIC_DELIV_LOWEST_PRI (1u << 8)
#define IOAPIC_DELIV_SMI        (2u << 8)
#define IOAPIC_DELIV_NMI        (4u << 8)
#define IOAPIC_DELIV_INIT       (5u << 8)
#define IOAPIC_DELIV_EXTINT     (7u << 8)

// ============================================================================
// MSR tanımlamaları
// ============================================================================
#define MSR_APIC_BASE        0x1B
#define APIC_BASE_BSP        (1u << 8)    // Bootstrap Processor bayrağı
#define APIC_BASE_X2APIC_EN  (1u << 10)  // x2APIC modu
#define APIC_BASE_GLOBAL_EN  (1u << 11)  // Global APIC etkinleştirme

// ============================================================================
// IDT vektör tahsisleri (APIC'e özel, PIC vektörlerinden ayrı)
// ============================================================================
#define APIC_TIMER_VECTOR     0x40   // LAPIC timer → INT 64
#define APIC_SPURIOUS_VECTOR  0xFF   // Sahte kesme (LAPIC tarafından gönderilir)

// ============================================================================
// Durum yapısı
// ============================================================================
typedef struct {
    int      detected;           // 1: CPUID bit-9 = APIC var
    int      initialized;        // 1: apic_init() çağrıldı
    uint64_t lapic_phys_base;    // LAPIC fiziksel adresi (MSR'dan okunur)
    uint64_t ioapic_phys_base;   // I/O APIC fiziksel adresi
    uint32_t lapic_id;           // LAPIC ID (mevcut çekirdek)
    uint32_t lapic_version;      // LAPIC versiyon kodu
    uint32_t lapic_max_lvt;      // Maksimum LVT girişi (0-based)
    uint32_t ioapic_id;          // I/O APIC ID
    uint32_t ioapic_version;     // I/O APIC versiyon kodu
    uint32_t ioapic_max_redir;   // Maksimum redirection girişi
    uint32_t timer_ticks_per_ms; // PIT kalibrasyonuyla bulunan LAPIC ticks/ms
    int      timer_hz;           // Ayarlanan timer frekansı (0 = durdu)
    int      pic_disabled;       // 1: 8259A PIC maskelendi
} APICInfo;

// ============================================================================
// MSR yardımcıları (inline)
// ============================================================================
static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t val) {
    __asm__ volatile ("wrmsr" : :
        "c"(msr),
        "a"((uint32_t)(val & 0xFFFFFFFF)),
        "d"((uint32_t)(val >> 32)));
}

// ============================================================================
// Genel API
// ============================================================================

// CPUID ile APIC varlığı kontrolü.
// Dönüş: 1 = var, 0 = yok
int  apic_detect(void);

// LAPIC + IOAPIC tam başlatma.
// apic_detect() başarısızsa işlem yapmaz.
void apic_init(void);

// LAPIC'i başlatır (VMM map + SVR enable + LVT maskele + TPR=0).
// Genellikle apic_init() tarafından çağrılır.
void lapic_init(void);

// I/O APIC'i başlatır (VMM map + tüm IOREDTBL maskele).
// Genellikle apic_init() tarafından çağrılır.
void ioapic_init(void);

// LAPIC'e EOI sinyali gönderir.
// Timer/ağ ISR'larının sonunda çağrılır (PIC EOI'nın yerine geçer).
void lapic_eoi(void);

// PIT ch2 kullanarak LAPIC timer'ı kalibre eder.
// Dönüş: ticks/ms (bölen 16'da).
uint32_t lapic_timer_calibrate(void);

// LAPIC timer'ı periyodik modda hz Hz'de başlatır.
// Önce lapic_timer_calibrate() çağrılmamışsa otomatik kalibrasyon yapar.
// IDT vektörü: APIC_TIMER_VECTOR (0x40).
void lapic_timer_init(uint32_t hz);

// LAPIC timer'ı durdurur (LVT_TIMER maskele + init_count = 0).
void lapic_timer_stop(void);

// I/O APIC'te bir IRQ hattını belirtilen vektöre yönlendirir.
//   irq     : I/O APIC pin numarası (0-23)
//   vector  : IDT vektörü
//   dest_id : hedef LAPIC ID (fiziksel modda)
//   flags   : IOAPIC_REDIR_* bayrakları
void ioapic_set_irq(uint8_t irq, uint8_t vector,
                    uint8_t dest_id, uint32_t flags);

// I/O APIC'te IRQ hattını maskele / maskesini kaldır.
void ioapic_mask_irq(uint8_t irq);
void ioapic_unmask_irq(uint8_t irq);

// 8259A PIC'i tamamen maskeler (tüm IRQ'lar).
// APIC devreye girdikten sonra çağrılır.
void apic_disable_pic(void);

// Durum bilgisini APICInfo yapısına doldurur.
void apic_get_info(APICInfo* out);

// 1: apic_init() başarıyla tamamlandı
int  apic_is_initialized(void);

// 1: PIC maskelendi, IRQ'lar APIC/IOAPIC üzerinden bekleniyor
int  apic_pic_is_disabled(void);

#endif /* APIC_H */