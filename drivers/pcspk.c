#include "pcspk.h"

static inline void _outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t _inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// ============================================================
// PIT still
// ============================================================

#define PIT_BASE_FREQ   1193182UL 
#define PIT_CH2_DATA    0x42       
#define PIT_CMD         0x43        
#define PORT_KB_CTRL    0x61        

// ============================================================
// Basic Speaker Functions
// ============================================================

void pcspk_play(uint32_t frequency_hz) {
    if (frequency_hz == 0) {
        pcspk_stop();
        return;
    }


    uint32_t divisor = PIT_BASE_FREQ / frequency_hz;
    if (divisor > 0xFFFF) divisor = 0xFFFF;  
    if (divisor < 1)      divisor = 1;

    _outb(PIT_CMD, 0xB6);

    _outb(PIT_CH2_DATA, (uint8_t)(divisor & 0xFF));
    _outb(PIT_CH2_DATA, (uint8_t)((divisor >> 8) & 0xFF));


    uint8_t ctrl = _inb(PORT_KB_CTRL);
    _outb(PORT_KB_CTRL, ctrl | 0x03);
}

void pcspk_stop(void) {
    uint8_t ctrl = _inb(PORT_KB_CTRL);
    _outb(PORT_KB_CTRL, ctrl & ~0x03);
}


static inline uint64_t _rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}


#define CYCLES_PER_MS  1000000ULL

static void _delay_ms(uint32_t ms) {
    if (ms == 0) return;
    uint64_t start  = _rdtsc();
    uint64_t target = start + (uint64_t)ms * CYCLES_PER_MS;
    while (_rdtsc() < target) {
        __asm__ volatile ("pause");
    }
}


void pcspk_beep(uint32_t frequency_hz, uint32_t duration_ms) {
    pcspk_play(frequency_hz);
    _delay_ms(duration_ms);
    pcspk_stop();
    _delay_ms(10);   
}

// ============================================================
// Built in Melodies
// ============================================================

void pcspk_system_beep(void) {
    pcspk_beep(NOTE_A4, 300);   
}

void pcspk_boot_melody(void) {
    pcspk_beep(NOTE_C5, 180);  
    pcspk_beep(NOTE_E5, 180);   
    pcspk_beep(NOTE_G5, 350);  
}