// keyboard_stub.c - Dummy keyboard handler for GUI mode
// GUI mode'da keyboard'a ihtiyacımız yok ama interrupt handler tanımlı olmalı

#include <stdint.h>

// Port I/O
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// Keyboard interrupt handler (sadece klavye buffer'ını temizler)
void keyboard_handler64(void) {
    // Klavye buffer'ından veriyi oku (yoksa interrupt tekrarlanır)
    inb(0x60);
    
    // İşlem yok - GUI mode'da klavye kullanmıyoruz
}