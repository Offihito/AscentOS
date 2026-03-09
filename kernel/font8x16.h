// font8x16.h — Dahili 8×16 bitmap font (PC Screen Font, ASCII 0x20–0x7F)
// Her karakter 16 byte = 16 satır × 8 bit.
// vesa64.c bu başlığı include ederek font verisine erişir.

#pragma once
#include <stdint.h>

// 96 karakter (0x20 ' ' → 0x7F DEL)
// Kullanım: font8x16[ch - 0x20][satır]
extern const uint8_t font8x16[96][16];