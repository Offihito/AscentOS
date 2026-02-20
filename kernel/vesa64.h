// vesa64.h - VESA Framebuffer Text Mode Driver
// VGA text mode yerine VESA framebuffer üzerinde çalışır
// Multiboot2'nin sağladığı framebuffer kullanılır

#pragma once
#include <stdint.h>
#include <stddef.h>

// ============================================================================
// Renkler (RGB)
// ============================================================================
typedef uint32_t VesaColor;

// Temel 16 renk paleti (VGA text mode uyumlu indeksler)
#define VESA_BLACK          0x00000000
#define VESA_BLUE           0x000000AA
#define VESA_GREEN          0x0000AA00
#define VESA_CYAN           0x0000AAAA
#define VESA_RED            0x00AA0000
#define VESA_MAGENTA        0x00AA00AA
#define VESA_BROWN          0x00AA5500
#define VESA_LIGHT_GRAY     0x00AAAAAA
#define VESA_DARK_GRAY      0x00555555
#define VESA_LIGHT_BLUE     0x005555FF
#define VESA_LIGHT_GREEN    0x0055FF55
#define VESA_LIGHT_CYAN     0x0055FFFF
#define VESA_LIGHT_RED      0x00FF5555
#define VESA_LIGHT_MAGENTA  0x00FF55FF
#define VESA_YELLOW         0x00FFFF55
#define VESA_WHITE          0x00FFFFFF

// VGA renk indeksinden RGB'ye çeviri (0-15 arası)
VesaColor vesa_color_from_vga(uint8_t vga_color_index);

// ============================================================================
// Font bilgisi
// ============================================================================
#define FONT_WIDTH   8    // Her karakter 8 pixel geniş
#define FONT_HEIGHT  16   // Her karakter 16 pixel yüksek

// ============================================================================
// Fonksiyon bildirimleri
// ============================================================================

// Başlatma
void init_vesa64(void);

// Ekranı temizle
void clear_screen64(void);

// Karakter yaz
void putchar64(char c, uint8_t color);

// String yaz
void print_str64(const char* str, uint8_t color);

// String + newline
void println64(const char* str, uint8_t color);

// Renk ayarla (fg/bg ayrı)
void set_color64(uint8_t fg, uint8_t bg);

// Cursor pozisyonu
void set_position64(size_t row, size_t col);
void get_position64(size_t* row, size_t* col);

// Ekran boyutu (karakter cinsinden)
void get_screen_size64(size_t* width, size_t* height);

// Scroll
void scroll_up(size_t lines);
void scroll_down(size_t lines);
void get_scroll_info64(size_t* buffer_lines, size_t* offset);

// Cursor güncelle (VESA modunda yazılımsal cursor)
void update_cursor64(void);

// VGA uyumluluk (eskiden kalan çağrıları kırmamak için stub)
void reset_to_standard_mode(void);
void set_extended_text_mode(void);