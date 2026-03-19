#ifndef PCSPK_H
#define PCSPK_H

#include <stdint.h>

// ============================================================
// Basic Functions
// ============================================================

void pcspk_play(uint32_t frequency_hz);

void pcspk_stop(void);

void pcspk_beep(uint32_t frequency_hz, uint32_t duration_ms);

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

void pcspk_system_beep(void);

void pcspk_boot_melody(void);

#endif 