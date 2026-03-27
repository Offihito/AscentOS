// ============================================================================
// apic.c — Local APIC (LAPIC) ve I/O APIC sürücüsü
//
// Mimari notları:
//   - LAPIC: 0xFEE00000 fiziksel → 0xFFFFFFFF_FEE00000 sanal
//     (KERNEL_VMA = 0xFFFFFFFF80000000; phys_to_virt(0xFEE00000) = aynı)
//   - IOAPIC: 0xFEC00000 fiziksel → 0xFFFFFFFF_FEC00000 sanal
//   - Her ikisi için vmm_map_page() ile PRESENT|WRITE|PWT|PCD (0x1B) kullanılır
//     (PWT/PCD bitleri MMIO için CPU önbelleğini devre dışı bırakır)
//   - Timer kalibrasyonu: PIT kanal-2 (~10ms penceresi), bölen = 16
//   - PIC devre dışı bırakma: apic_disable_pic() ile tüm IRQ'lar maskelenir
//
// Bağımlılıklar:
//   vmm64.c  : vmm_map_page()
//   idt64.c  : idt_set_entry()
//   vga      : println64, print_str64
//   asm      : isr_apic_timer, isr_apic_spurious (interrupts64.asm)
// ============================================================================

#include <stdint.h>
#include "apic.h"

// ---------------------------------------------------------------------------
// Dış semboller
// ---------------------------------------------------------------------------
extern void serial_print(const char* s);
extern void serial_write(char c);
extern void println64(const char* str, uint8_t color);
extern void print_str64(const char* str, uint8_t color);

extern int vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags);

// IDT: APIC'e özgü vektörleri kaydet
extern void idt_set_entry(int n, uint64_t handler, uint16_t selector, uint8_t attr);

// interrupts64.asm'dan APIC ISR stub'ları
extern void isr_apic_timer(void);
extern void isr_apic_spurious(void);

// ---------------------------------------------------------------------------
// VGA renk kodları
// ---------------------------------------------------------------------------
#define VGA_WHITE  0x0F
#define VGA_GREEN  0x0A
#define VGA_YELLOW 0x0E
#define VGA_RED    0x0C
#define VGA_CYAN   0x03
#define VGA_GRAY   0x08

// ---------------------------------------------------------------------------
// Sayfa eşleme bayrakları (vmm_map_page için)
//   Bit 0 : PRESENT
//   Bit 1 : WRITE
//   Bit 3 : PWT  (Write-Through — MMIO önbellek kaçınması)
//   Bit 4 : PCD  (Cache Disable — MMIO için zorunlu)
// ---------------------------------------------------------------------------
#define VMM_FLAGS_MMIO  0x1Bu   // PRESENT | WRITE | PWT | PCD

// ---------------------------------------------------------------------------
// APIC MMIO sanal adresleri
//
// Not:
// Kernel boot aşamasında higher-half 2MB huge-page map kullanıyor.
// Bu yüzden LAPIC_VIRT=0xFFFFFFFFFEE00000 gibi adreslerde 4KB remap denemesi
// vmm_map_page() içinde PAGE_SIZE_2M çakışmasına takılıp başarısız olabilir.
//
// Kernel şu an lower-half identity-map'i koruduğu için APIC MMIO'yu doğrudan
// fiziksel adresinden erişiyoruz (virt == phys). Böylece yanlış fiziksel
// sayfadan okuma (ID/version=0) problemi çözülüyor.
// ---------------------------------------------------------------------------
#define LAPIC_VIRT   LAPIC_PHYS_DEFAULT
#define IOAPIC_VIRT  IOAPIC_PHYS_DEFAULT

// ---------------------------------------------------------------------------
// Modül durumu
// ---------------------------------------------------------------------------
static uint64_t g_lapic_virt    = 0;
static uint64_t g_ioapic_virt   = 0;
static uint64_t g_lapic_phys    = 0;
static uint64_t g_ioapic_phys   = IOAPIC_PHYS_DEFAULT;

static int       g_detected     = 0;
static int       g_initialized  = 0;
static int       g_pic_disabled = 0;
static uint32_t  g_ticks_per_ms = 0;   // PIT kalibrasyonu: LAPIC ticks / ms
static int       g_timer_hz     = 0;

// ============================================================================
// Yardımcı: uint32'yi serial porta yaz
// ============================================================================
static void serial_u32(uint32_t v) {
    if (!v) { serial_write('0'); return; }
    char buf[12]; int n = 0;
    while (v) { buf[n++] = '0' + (v % 10); v /= 10; }
    while (n--) serial_write(buf[n]);
}

static void serial_hex8(uint32_t v) {
    const char* h = "0123456789ABCDEF";
    serial_write('0'); serial_write('x');
    for (int i = 7; i >= 0; i--)
        serial_write(h[(v >> (i * 4)) & 0xF]);
}

// ============================================================================
// CPUID yardımcısı
// ============================================================================
static void do_cpuid(uint32_t leaf,
                     uint32_t* eax, uint32_t* ebx,
                     uint32_t* ecx, uint32_t* edx) {
    __asm__ volatile (
        "cpuid"
        : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
        : "a"(leaf)
        : "memory"
    );
}

// ============================================================================
// LAPIC MMIO yardımcıları
// ============================================================================
static inline uint32_t lapic_read(uint32_t reg) {
    return *(volatile uint32_t*)(g_lapic_virt + reg);
}

static inline void lapic_write(uint32_t reg, uint32_t val) {
    *(volatile uint32_t*)(g_lapic_virt + reg) = val;
    // Yazmanın tamamlandığını garantilemek için bir okuma yap
    (void)*(volatile uint32_t*)(g_lapic_virt + LAPIC_ID);
}

// ============================================================================
// I/O APIC MMIO yardımcıları
// ============================================================================
static inline uint32_t ioapic_read_reg(uint8_t reg) {
    *(volatile uint32_t*)(g_ioapic_virt + IOAPIC_REGSEL) = reg;
    return *(volatile uint32_t*)(g_ioapic_virt + IOAPIC_IOWIN);
}

static inline void ioapic_write_reg(uint8_t reg, uint32_t val) {
    *(volatile uint32_t*)(g_ioapic_virt + IOAPIC_REGSEL) = reg;
    *(volatile uint32_t*)(g_ioapic_virt + IOAPIC_IOWIN)  = val;
}

// Route legacy ISA IRQs to existing PIC-era IDT vectors so drivers keep working:
//   IRQ0->32 (timer), IRQ1->33 (keyboard), IRQ5->37 (SB16), IRQ11->43 (RTL8139), IRQ12->44 (mouse)
static void ioapic_route_legacy_irqs(void) {
    if (!g_initialized || !g_ioapic_virt || !g_lapic_virt) return;

    uint8_t lapic_id = (uint8_t)(lapic_read(LAPIC_ID) >> 24);

    ioapic_set_irq(0,  32, lapic_id, IOAPIC_DELIV_FIXED);
    ioapic_set_irq(1,  33, lapic_id, IOAPIC_DELIV_FIXED);
    ioapic_set_irq(5,  37, lapic_id, IOAPIC_DELIV_FIXED);
    ioapic_set_irq(11, 43, lapic_id, IOAPIC_DELIV_FIXED);
    ioapic_set_irq(12, 44, lapic_id, IOAPIC_DELIV_FIXED);

    ioapic_unmask_irq(0);
    ioapic_unmask_irq(1);
    ioapic_unmask_irq(5);
    ioapic_unmask_irq(11);
    ioapic_unmask_irq(12);

    serial_print("[IOAPIC] Legacy IRQ routing enabled (0,1,5,11,12)\n");
}

// ============================================================================
// I/O portları (PIC + PIT işlemleri için)
// ============================================================================
static inline void port_out(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" :: "a"(val), "Nd"(port));
}

static inline uint8_t port_in(uint16_t port) {
    uint8_t r;
    __asm__ volatile ("inb %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}

// ============================================================================
// apic_detect — CPUID bit-9 (EDX) ile LAPIC varlığı kontrolü
// ============================================================================
int apic_detect(void) {
    uint32_t eax, ebx, ecx, edx;
    do_cpuid(1, &eax, &ebx, &ecx, &edx);
    g_detected = (edx >> 9) & 1;
    return g_detected;
}

// ============================================================================
// apic_disable_pic — 8259A PIC'in tüm IRQ'larını maskele
// ============================================================================
void apic_disable_pic(void) {
    // If APIC is active, route common legacy IRQ lines through IOAPIC first.
    // This prevents losing keyboard/timer/network/mouse after PIC masking.
    if (g_initialized) {
        ioapic_route_legacy_irqs();
    }

    // Master PIC: port 0x21, Slave PIC: port 0xA1
    // 0xFF = tüm bitler 1 = tüm IRQ'lar maskelendi
    port_out(0x21, 0xFF);
    port_out(0xA1, 0xFF);
    g_pic_disabled = 1;
    serial_print("[APIC] 8259A PIC: all IRQs masked.\n");
}

// ============================================================================
// lapic_init — LAPIC'i eşle ve etkinleştir
// ============================================================================
void lapic_init(void) {
    // 1. IA32_APIC_BASE MSR'dan fiziksel taban oku
    uint64_t msr_val = rdmsr(MSR_APIC_BASE);
    g_lapic_phys     = msr_val & 0xFFFFF000ULL;

    if (g_lapic_phys == 0) {
        // Fallback (nadiren, %APIC_BASE boşsa)
        g_lapic_phys = LAPIC_PHYS_DEFAULT;
        msr_val = (msr_val & ~0xFFFFF000ULL) | g_lapic_phys;
    }

    // X2APIC modundaysak, MMIO için xAPIC'e çevir
    if (msr_val & APIC_BASE_X2APIC_EN) {
        serial_print("[LAPIC] x2APIC detected, switching to xAPIC MMIO mode...\n");
        msr_val &= ~APIC_BASE_X2APIC_EN;
    }

    // 2. Global APIC etkinleştirme bitini koru / set et
    wrmsr(MSR_APIC_BASE, msr_val | APIC_BASE_GLOBAL_EN);

    // 3. LAPIC MMIO erişim adresini seç ve map dene (identity-map fallback)
    g_lapic_virt = LAPIC_VIRT;
    if (vmm_map_page(g_lapic_virt, g_lapic_phys, VMM_FLAGS_MMIO) != 0) {
        // 2MB huge-page çakışması durumunda identity-map zaten aktif.
        // Bu nedenle burada hatayla çıkmak yerine mevcut map'i kullanıyoruz.
        serial_print("[LAPIC] vmm_map_page skipped (existing large-page map)\n");
    }

    // 4. ESR'yi iki kez sıfırla (hata birikimini temizle)
    lapic_write(LAPIC_ESR, 0);
    lapic_write(LAPIC_ESR, 0);

    // 5. Spurious Interrupt Vector Register:
    //    bit 8 = yazılım etkinleştirme, düşük 8 bit = vektör 0xFF
    lapic_write(LAPIC_SVR, LAPIC_SVR_SW_ENABLE | APIC_SPURIOUS_VECTOR);

    // 6. LVT girişlerini ayarla:
    //    - timer/thermal/perf/error: masked
    //    - LINT0: ExtINT (virtual wire, legacy PIC IRQ forwarding)
    //    - LINT1: NMI
    //
    // LINT0/LINT1'i maskelemek, APIC açık + PIC aktif senaryoda klavye gibi
    // PIC IRQ'larının CPU'ya hiç ulaşmamasına neden olabilir.
    lapic_write(LAPIC_LVT_TIMER,   LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_THERMAL, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_PERF,    LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_LINT0,   LAPIC_LVT_DM_EXTINT);
    lapic_write(LAPIC_LVT_LINT1,   LAPIC_LVT_DM_NMI);
    lapic_write(LAPIC_LVT_ERROR,   LAPIC_LVT_MASKED);

    // 7. Task Priority Register = 0 (tüm kesmeler geçebilir)
    lapic_write(LAPIC_TPR, 0);

    // 8. Spurious ISR'ı IDT'ye kaydet
    idt_set_entry(APIC_SPURIOUS_VECTOR,
                  (uint64_t)isr_apic_spurious, 0x08, 0x8E);

    serial_print("[LAPIC] Initialized  phys=");
    serial_hex8((uint32_t)g_lapic_phys);
    serial_print("  ID=");
    serial_u32(lapic_read(LAPIC_ID) >> 24);
    serial_print("  VER=");
    serial_u32(lapic_read(LAPIC_VER) & 0xFF);
    serial_write('\n');
}

// ============================================================================
// ioapic_init — I/O APIC'i eşle, tüm redirection girişlerini maskele
// ============================================================================
void ioapic_init(void) {
    // I/O APIC MMIO erişim adresini seç ve map dene (identity-map fallback)
    g_ioapic_virt = IOAPIC_VIRT;
    if (vmm_map_page(g_ioapic_virt, g_ioapic_phys, VMM_FLAGS_MMIO) != 0) {
        serial_print("[IOAPIC] vmm_map_page skipped (existing large-page map)\n");
    }

    // Versiyon yazmacından maks. redirection giriş sayısını oku
    uint32_t ver       = ioapic_read_reg(IOAPIC_REG_VER);
    uint32_t max_redir = (ver >> 16) & 0xFF;

    // Tüm IOREDTBL girişlerini maskele (maskeleme biti + düşük vektör 0x20)
    for (uint32_t i = 0; i <= max_redir; i++) {
        ioapic_write_reg((uint8_t)IOAPIC_REDTBL_LO(i),
                         IOAPIC_REDIR_MASKED | 0x20u);
        ioapic_write_reg((uint8_t)IOAPIC_REDTBL_HI(i), 0);
    }

    serial_print("[IOAPIC] Initialized  phys=");
    serial_hex8((uint32_t)g_ioapic_phys);
    serial_print("  max_redir=");
    serial_u32(max_redir);
    serial_write('\n');
}

// ============================================================================
// lapic_eoi — LAPIC'e End-Of-Interrupt sinyali gönder
// Her LAPIC kaynaklı kesme işleyicisinin sonunda çağrılmalıdır.
// (PIC EOI'nın (outb 0x20) yerini alır)
// ============================================================================
void lapic_eoi(void) {
    // EOI yazmacına herhangi bir değer yazmak yeterli
    *(volatile uint32_t*)(g_lapic_virt + LAPIC_EOI_REG) = 0;
}

// ============================================================================
// lapic_timer_calibrate — PIT kanal-2 ile LAPIC ticks/ms ölçümü
//
// PIT: 1.193182 MHz; ~10 ms için bölen = 11932
// LAPIC timer: başlangıç değeri 0xFFFFFFFF, bölen = 16
// Ölçüm sonrası: elapsed = 0xFFFFFFFF - kalan_sayac
// ============================================================================
uint32_t lapic_timer_calibrate(void) {
    const uint16_t PIT_DIV = 11932;   // ~10 ms @ 1.193182 MHz

    // LAPIC timer'ı bölen-16, maks. başlangıç değeriyle başlat
    lapic_write(LAPIC_TIMER_DIV,  LAPIC_DIV_BY_16);
    lapic_write(LAPIC_TIMER_INIT, 0xFFFFFFFFu);

    // PIT kanal-2 kapısını aç (port 0x61 bit0=1, bit1=0: PC hoparlörü kapat)
    uint8_t old_gate = port_in(0x61);
    port_out(0x61, (old_gate & ~0x02u) | 0x01u);

    // PIT kanal-2: mode 0 (terminal count), ikili, PIT_DIV bölen
    port_out(0x43, 0xB0u);                         // CH2 | mode0 | binary | LSB+MSB
    port_out(0x42, (uint8_t)(PIT_DIV & 0xFFu));    // LSB
    port_out(0x42, (uint8_t)(PIT_DIV >> 8));        // MSB

    // Port 0x61 bit-5 = PIT CH2 OUT; 1 olunca sayaç bitti (~10 ms)
    while (!(port_in(0x61) & 0x20u))
        __asm__ volatile ("pause");

    // Kalan sayacı oku
    uint32_t remaining = lapic_read(LAPIC_TIMER_CUR);

    // PIT kapısını geri al
    port_out(0x61, old_gate);

    // Geçen ticks (10 ms'de)
    uint32_t elapsed_10ms = 0xFFFFFFFFu - remaining;

    // ticks/ms (bölen-16 ile)
    g_ticks_per_ms = elapsed_10ms / 10u;
    if (g_ticks_per_ms == 0) g_ticks_per_ms = 1u;

    serial_print("[LAPIC] Timer calibrated: ");
    serial_u32(g_ticks_per_ms);
    serial_print(" ticks/ms (div=16)\n");

    return g_ticks_per_ms;
}

// ============================================================================
// lapic_timer_init — LAPIC timer'ı periyodik modda başlat
// ============================================================================
void lapic_timer_init(uint32_t hz) {
    if (!g_initialized || hz == 0) return;

    // Kalibrasyon yapılmamışsa çalıştır
    if (g_ticks_per_ms == 0) lapic_timer_calibrate();

    // Bir kesme aralığındaki ticks sayısı
    uint32_t ticks_per_irq = (g_ticks_per_ms * 1000u) / hz;
    if (ticks_per_irq == 0) ticks_per_irq = 1u;

    // IDT vektörünü kaydet (isr_apic_timer: interrupts64.asm)
    idt_set_entry(APIC_TIMER_VECTOR,
                  (uint64_t)isr_apic_timer, 0x08, 0x8E);

    // Timer'ı yapılandır: bölen-16, periyodik mod, vektör 0x40
    lapic_write(LAPIC_TIMER_DIV, LAPIC_DIV_BY_16);
    lapic_write(LAPIC_LVT_TIMER, LAPIC_TIMER_PERIODIC | APIC_TIMER_VECTOR);
    lapic_write(LAPIC_TIMER_INIT, ticks_per_irq);

    g_timer_hz = (int)hz;

    serial_print("[LAPIC] Timer started: ");
    serial_u32(hz);
    serial_print(" Hz  ticks/irq=");
    serial_u32(ticks_per_irq);
    serial_write('\n');
}

// ============================================================================
// lapic_timer_stop — LAPIC timer'ı durdur
// ============================================================================
void lapic_timer_stop(void) {
    if (!g_initialized) return;
    lapic_write(LAPIC_LVT_TIMER,   LAPIC_LVT_MASKED);
    lapic_write(LAPIC_TIMER_INIT,  0);
    g_timer_hz = 0;
    serial_print("[LAPIC] Timer stopped.\n");
}

// ============================================================================
// ioapic_set_irq — IRQ hattını IDT vektörüne yönlendir
// ============================================================================
void ioapic_set_irq(uint8_t irq, uint8_t vector,
                    uint8_t dest_id, uint32_t flags) {
    // 64-bit redirection girişi: yüksek 32-bit = hedef LAPIC ID (bit 56-63)
    uint64_t entry = (uint64_t)vector
                   | (uint64_t)flags
                   | ((uint64_t)dest_id << 56);

    ioapic_write_reg((uint8_t)IOAPIC_REDTBL_LO(irq),
                     (uint32_t)(entry & 0xFFFFFFFFu));
    ioapic_write_reg((uint8_t)IOAPIC_REDTBL_HI(irq),
                     (uint32_t)(entry >> 32));
}

// ============================================================================
// ioapic_mask_irq / ioapic_unmask_irq
// ============================================================================
void ioapic_mask_irq(uint8_t irq) {
    uint32_t lo = ioapic_read_reg((uint8_t)IOAPIC_REDTBL_LO(irq));
    ioapic_write_reg((uint8_t)IOAPIC_REDTBL_LO(irq), lo | IOAPIC_REDIR_MASKED);
}

void ioapic_unmask_irq(uint8_t irq) {
    uint32_t lo = ioapic_read_reg((uint8_t)IOAPIC_REDTBL_LO(irq));
    ioapic_write_reg((uint8_t)IOAPIC_REDTBL_LO(irq),
                     lo & ~IOAPIC_REDIR_MASKED);
}

// ============================================================================
// apic_init — ana başlatma
// ============================================================================
void apic_init(void) {
    if (!apic_detect()) {
        serial_print("[APIC] Not present (CPUID:EDX bit-9=0).\n");
        return;
    }
    lapic_init();
    ioapic_init();
    g_initialized = 1;
    serial_print("[APIC] Ready (LAPIC + IOAPIC).\n");
}

// ============================================================================
// apic_is_initialized
// ============================================================================
int apic_is_initialized(void) { return g_initialized; }
int apic_pic_is_disabled(void) { return g_pic_disabled; }

// ============================================================================
// apic_get_info — APICInfo doldurucu
// ============================================================================
void apic_get_info(APICInfo* out) {
    out->detected          = g_detected;
    out->initialized       = g_initialized;
    out->lapic_phys_base   = g_lapic_phys;
    out->ioapic_phys_base  = g_ioapic_phys;
    out->pic_disabled      = g_pic_disabled;
    out->timer_hz          = g_timer_hz;
    out->timer_ticks_per_ms = g_ticks_per_ms;

    if (g_initialized && g_lapic_virt) {
        uint32_t ver         = lapic_read(LAPIC_VER);
        out->lapic_id        = lapic_read(LAPIC_ID) >> 24;
        out->lapic_version   = ver & 0xFFu;
        out->lapic_max_lvt   = (ver >> 16) & 0xFFu;
    } else {
        out->lapic_id = out->lapic_version = out->lapic_max_lvt = 0;
    }

    if (g_initialized && g_ioapic_virt) {
        uint32_t ver           = ioapic_read_reg(IOAPIC_REG_VER);
        out->ioapic_id         = ioapic_read_reg(IOAPIC_REG_ID) >> 24;
        out->ioapic_version    = ver & 0xFFu;
        out->ioapic_max_redir  = (ver >> 16) & 0xFFu;
    } else {
        out->ioapic_id = out->ioapic_version = out->ioapic_max_redir = 0;
    }
}