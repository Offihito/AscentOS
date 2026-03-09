// pcspk.h - PC Speaker Driver for AscentOS 64-bit
// Donanım: PIT Channel 2 (0x42) + Port 0x61 speaker gate
#ifndef PCSPK_H
#define PCSPK_H

#include <stdint.h>

// ============================================================
// TEMEL FONKSİYONLAR
// ============================================================

// Belirtilen frekansta (Hz) speaker'ı çal — bloklamaz, süresiz çalar
void pcspk_play(uint32_t frequency_hz);

// Speaker'ı sustur
void pcspk_stop(void);

// Belirtilen frekansta, ms kadar çal (busy-wait — basit kullanım için)
// Scheduler yokken veya boot sırasında kullanmak için uygundur
void pcspk_beep(uint32_t frequency_hz, uint32_t duration_ms);

// ============================================================
// HAZIR NOTALAR (frekans sabitleri, Hz)
// ============================================================

#define NOTE_C4   262
#define NOTE_D4   294
#define NOTE_E4   330
#define NOTE_F4   349
#define NOTE_G4   392
#define NOTE_A4   440
#define NOTE_B4   494
#define NOTE_C5   523
#define NOTE_D5   587
#define NOTE_E5   659
#define NOTE_F5   698
#define NOTE_G5   784
#define NOTE_A5   880

// ============================================================
// HAZIR MELODILER
// ============================================================

// Kısa sistem beep'i (hata/uyarı)
void pcspk_system_beep(void);

// Boot melodisi (AscentOS başlarken)
void pcspk_boot_melody(void);

#endif // PCSPK_H