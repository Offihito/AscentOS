// pcspk.c - PC Speaker Driver for AscentOS 64-bit
//
// DONANIM:
//   PIT (Programmable Interval Timer) — Intel 8253/8254
//   Base clock: 1.193182 MHz
//   Channel 2 (port 0x42) → PC Speaker'a bağlı
//   Port 0x61 bit 0: PIT Ch2 gate (1=aktif)
//   Port 0x61 bit 1: Speaker enable (1=ses çıkar)
//
// KULLANIM:
//   pcspk_play(440);   // 440 Hz, La notası, başlar
//   pcspk_stop();      // Keser
//   pcspk_beep(440, 500); // 440 Hz, 500ms, bloklar

#include "pcspk.h"

// ============================================================
// PORT ERİŞİMİ
// kernel64.c'de zaten tanımlı ama bu dosya bağımsız derlenir
// ============================================================

static inline void _outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t _inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// ============================================================
// PIT SABITI
// ============================================================

#define PIT_BASE_FREQ   1193182UL   // PIT'in temel clock frekansı (Hz)
#define PIT_CH2_DATA    0x42        // PIT Channel 2 data portu
#define PIT_CMD         0x43        // PIT komut registeri
#define PORT_KB_CTRL    0x61        // Klavye kontrol portu (speaker gate)

// ============================================================
// TEMEL SPEAKER FONKSİYONLARI
// ============================================================

// Belirtilen frekansta speaker'ı çalmaya başla (bloklamaz)
void pcspk_play(uint32_t frequency_hz) {
    if (frequency_hz == 0) {
        pcspk_stop();
        return;
    }

    // PIT bölen değeri hesapla
    // divisor = 1193182 / frequency
    uint32_t divisor = PIT_BASE_FREQ / frequency_hz;
    if (divisor > 0xFFFF) divisor = 0xFFFF;  // 16-bit sınır
    if (divisor < 1)      divisor = 1;

    // PIT Channel 2'yi kare dalga moduna ayarla
    // 0xB6 = 10110110b:
    //   bit 7-6: 10  = Channel 2 seç
    //   bit 5-4: 11  = LSB+MSB yükle
    //   bit 3-1: 011 = Mod 3 (kare dalga)
    //   bit 0:   0   = Binary sayma
    _outb(PIT_CMD, 0xB6);

    // Frekansı yükle (önce LSB, sonra MSB)
    _outb(PIT_CH2_DATA, (uint8_t)(divisor & 0xFF));
    _outb(PIT_CH2_DATA, (uint8_t)((divisor >> 8) & 0xFF));

    // Port 0x61: bit 0 ve 1'i set et → PIT gate aç + speaker bağla
    uint8_t ctrl = _inb(PORT_KB_CTRL);
    _outb(PORT_KB_CTRL, ctrl | 0x03);
}

// Speaker'ı sustur
void pcspk_stop(void) {
    // Port 0x61: bit 0 ve 1'i temizle → speaker kes
    uint8_t ctrl = _inb(PORT_KB_CTRL);
    _outb(PORT_KB_CTRL, ctrl & ~0x03);
}

// ============================================================
// GECIKME — RDTSC (CPU timestamp counter)
//
// rdtsc: CPU'nun boot'tan beri saydığı cycle sayısı.
// QEMU varsayılan CPU frekansı ~1 GHz civarı.
// 1ms ≈ 1_000_000 cycle (1 GHz için).
// Calibrasyon: ilk çağrıda ~10ms ölçüp cycle/ms hesapla.
// ============================================================

static inline uint64_t _rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

// QEMU'da tipik değer: 1_000_000 cycle/ms (1 GHz)
// Güvenli taraf: düşük tutarsak gecikme uzar (ses kesilir) değil uzar (ses devam eder)
// 800_000 → az bekleme, 1_500_000 → fazla bekleme; 1_000_000 iyi orta nokta
#define CYCLES_PER_MS  1000000ULL

static void _delay_ms(uint32_t ms) {
    if (ms == 0) return;
    uint64_t start  = _rdtsc();
    uint64_t target = start + (uint64_t)ms * CYCLES_PER_MS;
    while (_rdtsc() < target) {
        __asm__ volatile ("pause");
    }
}

// Belirtilen frekansta, ms kadar çal
// Her çağrı sonunda 10ms sessizlik: PulseAudio buffer temizlenir,
// notalar arası tıkırtı kaybolur.
void pcspk_beep(uint32_t frequency_hz, uint32_t duration_ms) {
    pcspk_play(frequency_hz);
    _delay_ms(duration_ms);
    pcspk_stop();
    _delay_ms(10);   // inter-note gap — tıkırtı önleyici
}

// ============================================================
// HAZIR MELODILER
// ============================================================

// Kısa sistem beep'i — hata, uyarı, dikkat için
void pcspk_system_beep(void) {
    pcspk_beep(NOTE_A4, 300);   // 300ms — PulseAudio'nun duyacağı minimum
}

// AscentOS boot melodisi — C Major arpej
void pcspk_boot_melody(void) {
    pcspk_beep(NOTE_C5, 180);   // Do
    pcspk_beep(NOTE_E5, 180);   // Mi
    pcspk_beep(NOTE_G5, 350);   // Sol — son nota uzun
}