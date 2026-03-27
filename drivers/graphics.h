#pragma once

#include <stdint.h>
#include <stddef.h>

// ============================================================================
// Graphics Output Protocol Abstraction Layer
// Hem UEFI GOP hem VESA/Multiboot2 destekler
// ============================================================================

typedef enum {
    GFX_MODE_UNKNOWN = 0,
    GFX_MODE_GOP,       // UEFI Graphics Output Protocol
    GFX_MODE_VESA,      // VESA Framebuffer (Multiboot2)
    GFX_MODE_TEXT,      // VGA Text Mode (fallback)
} gfx_mode_t;

typedef enum {
    GFX_PIXEL_ARGB8888 = 0,  // 32-bit ARGB (most common)
    GFX_PIXEL_RGBA8888 = 1,  // 32-bit RGBA
    GFX_PIXEL_BGRX8888 = 2,  // 32-bit BGRX
    GFX_PIXEL_RGB565   = 3,  // 16-bit RGB
} gfx_pixel_format_t;

typedef struct {
    gfx_mode_t mode;
    gfx_pixel_format_t pixel_format;
    
    uint32_t width;   // pixels
    uint32_t height;  // pixels
    uint32_t pitch;   // bytes per line
    uint32_t bpp;     // bits per pixel
    
    uint64_t fb_addr; // physical framebuffer address
    uint64_t fb_size; // framebuffer size in bytes
} gfx_info_t;

// ============================================================================
// Global Graphics State
// ============================================================================
extern gfx_info_t gfx_state;

// ============================================================================
// Initialization Functions
// UEFI Boot Services'ten çağrılır (loader stage'de)
// ============================================================================

// UEFI GOP framebuffer bilgisini kaydet
void gfx_set_gop_framebuffer(uint64_t fb_addr, uint32_t width, uint32_t height,
                               uint32_t pitch, gfx_pixel_format_t fmt);

// VESA/Multiboot2 framebuffer bilgisini kaydet
void gfx_set_vesa_framebuffer(uint64_t fb_addr, uint32_t width, uint32_t height,
                                uint32_t pitch, uint8_t bpp);

// Graphics sistemini başlat (kernel stage'de)
void gfx_init(void);

// ============================================================================
// Graphics Drawing Functions
// ============================================================================

// Framebuffer'ı verilen renkle doldur (32-bit ARGB)
void gfx_fill(uint32_t color);

// Pixel çiz
void gfx_put_pixel(uint32_t x, uint32_t y, uint32_t color);

// Filled rectangle çiz
void gfx_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);

// Rectangle outline çiz
void gfx_draw_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);

// Horizontal line çiz
void gfx_draw_hline(uint32_t x, uint32_t y, uint32_t w, uint32_t color);

// Vertical line çiz
void gfx_draw_vline(uint32_t x, uint32_t y, uint32_t h, uint32_t color);

// ============================================================================
// Text Drawing (with 8x16 font)
// ============================================================================

// Character çiz (8x16 font)
void gfx_put_char(uint32_t x, uint32_t y, char c, uint32_t fg, uint32_t bg);

// String çiz
void gfx_put_string(uint32_t x, uint32_t y, const char* str, uint32_t fg, uint32_t bg);

// ============================================================================
// Color Conversion Helper
// ============================================================================

// VGA renk indexini 32-bit ARGB'ye çevir
uint32_t gfx_vga_to_argb(uint8_t vga_color);

// RGB bileşenlerinden 32-bit renk oluştur
uint32_t gfx_rgb(uint8_t r, uint8_t g, uint8_t b);

// ARGB renk: alpha=0xFF (opaque)
uint32_t gfx_argb(uint8_t a, uint8_t r, uint8_t g, uint8_t b);

// ============================================================================
// Query Functions
// ============================================================================

gfx_info_t* gfx_get_info(void);

uint32_t gfx_get_width(void);
uint32_t gfx_get_height(void);
gfx_mode_t gfx_get_mode(void);
