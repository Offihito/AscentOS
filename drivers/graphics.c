// graphics.c - Graphics Output Protocol Abstraction Layer
// UEFI GOP + VESA Framebuffer destekler
// Bootloader'dan kernel'e geçişte frame buffer bilgisini alır

#include "graphics.h"
#include "../kernel/font8x16.h"
#include <stddef.h>

// ============================================================================
// Global State
// ============================================================================
gfx_info_t gfx_state = {
    .mode = GFX_MODE_UNKNOWN,
    .pixel_format = GFX_PIXEL_ARGB8888,
    .width = 0,
    .height = 0,
    .pitch = 0,
    .bpp = 0,
    .fb_addr = 0,
    .fb_size = 0,
};

// ============================================================================
// Helper Functions
// ============================================================================

static void* memset_gfx(void* dest, int val, uint64_t n) {
    uint8_t* d = (uint8_t*)dest;
    while (n--) *d++ = (uint8_t)val;
    return dest;
}

static void serial_print(const char* str);
extern void serial_print(const char* str);

static void int_to_str_gfx(int num, char* str) {
    if (num == 0) { str[0] = '0'; str[1] = '\0'; return; }
    int neg = (num < 0);
    if (neg) num = -num;
    char buf[32], *p = buf + 31;
    *p = '\0';
    while (num > 0) { *(--p) = '0' + (num % 10); num /= 10; }
    if (neg) *(--p) = '-';
    int i = 0;
    while (p[i]) { str[i] = p[i]; i++; }
    str[i] = '\0';
}

static void uint64_to_string_gfx(uint64_t num, char* str) {
    if (num == 0) { str[0] = '0'; str[1] = '\0'; return; }
    char buf[32], *p = buf + 31;
    *p = '\0';
    while (num > 0) { *(--p) = "0123456789ABCDEF"[num % 16]; num /= 16; }
    int i = 0;
    while (p[i]) { str[i] = p[i]; i++; }
    str[i] = '\0';
}

// ============================================================================
// Initialization
// ============================================================================

void gfx_set_gop_framebuffer(uint64_t fb_addr, uint32_t width, uint32_t height,
                               uint32_t pitch, gfx_pixel_format_t fmt)
{
    gfx_state.mode = GFX_MODE_GOP;
    gfx_state.fb_addr = fb_addr;
    gfx_state.width = width;
    gfx_state.height = height;
    gfx_state.pitch = pitch;
    gfx_state.bpp = 32;  // GOP genellikle 32-bit
    gfx_state.pixel_format = fmt;
    gfx_state.fb_size = (uint64_t)pitch * height;

    char buf[32];
    serial_print("[GFX] GOP Framebuffer set: ");
    uint64_to_string_gfx(fb_addr, buf);
    serial_print(buf);
    serial_print(" (");
    int_to_str_gfx(width, buf); serial_print(buf);
    serial_print("x");
    int_to_str_gfx(height, buf); serial_print(buf);
    serial_print(", ");
    int_to_str_gfx(pitch, buf); serial_print(buf);
    serial_print(" bytes/line)\n");
}

void gfx_set_vesa_framebuffer(uint64_t fb_addr, uint32_t width, uint32_t height,
                                uint32_t pitch, uint8_t bpp)
{
    gfx_state.mode = GFX_MODE_VESA;
    gfx_state.fb_addr = fb_addr;
    gfx_state.width = width;
    gfx_state.height = height;
    gfx_state.pitch = pitch;
    gfx_state.bpp = bpp;
    gfx_state.pixel_format = GFX_PIXEL_ARGB8888;
    gfx_state.fb_size = (uint64_t)pitch * height;

    char buf[32];
    serial_print("[GFX] VESA Framebuffer set: ");
    uint64_to_string_gfx(fb_addr, buf);
    serial_print(buf);
    serial_print(" (");
    int_to_str_gfx(width, buf); serial_print(buf);
    serial_print("x");
    int_to_str_gfx(height, buf); serial_print(buf);
    serial_print(", ");
    int_to_str_gfx(bpp, buf); serial_print(buf);
    serial_print("bpp)\n");
}

void gfx_init(void)
{
    if (gfx_state.mode == GFX_MODE_UNKNOWN) {
        serial_print("[GFX] ERROR: No framebuffer configured!\n");
        return;
    }

    serial_print("[GFX] Graphics initialized - ");
    const char* mode_str = (gfx_state.mode == GFX_MODE_GOP) ? "GOP" : "VESA";
    serial_print(mode_str);
    serial_print("\n");

    // Framebuffer'ı black ile doldur
    gfx_fill(0xFF000000);
}

// ============================================================================
// Color Conversion
// ============================================================================

uint32_t gfx_vga_to_argb(uint8_t vga_color)
{
    static const uint32_t vga_palette[16] = {
        0xFF000000,  // 0  Siyah
        0xFF0000AA,  // 1  Mavi
        0xFF00AA00,  // 2  Yeşil
        0xFF00AAAA,  // 3  Cyan
        0xFFAA0000,  // 4  Kırmızı
        0xFFAA00AA,  // 5  Magenta
        0xFFAA5500,  // 6  Kahverengi
        0xFFAAAAAA,  // 7  Açık gri
        0xFF555555,  // 8  Koyu gri
        0xFF5555FF,  // 9  Açık mavi
        0xFF55FF55,  // 10 Açık yeşil
        0xFF55FFFF,  // 11 Açık cyan
        0xFFFF5555,  // 12 Açık kırmızı
        0xFFFF55FF,  // 13 Açık magenta
        0xFFFFFF55,  // 14 Sarı
        0xFFFFFFFF,  // 15 Beyaz
    };
    return vga_palette[vga_color & 0x0F];
}

uint32_t gfx_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return 0xFF000000 | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

uint32_t gfx_argb(uint8_t a, uint8_t r, uint8_t g, uint8_t b)
{
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

// ============================================================================
// Drawing Functions
// ============================================================================

static void put_pixel_raw(uint32_t x, uint32_t y, uint32_t color)
{
    if (!gfx_state.fb_addr || x >= gfx_state.width || y >= gfx_state.height)
        return;

    uint64_t offset = (uint64_t)y * gfx_state.pitch + (uint64_t)x * 4;
    uint32_t* pixel = (uint32_t*)(gfx_state.fb_addr + offset);
    *pixel = color;
}

void gfx_fill(uint32_t color)
{
    if (!gfx_state.fb_addr) return;

    uint32_t* fb = (uint32_t*)gfx_state.fb_addr;
    uint32_t total_pixels = gfx_state.width * gfx_state.height;

    for (uint32_t i = 0; i < total_pixels; i++)
        fb[i] = color;
}

void gfx_put_pixel(uint32_t x, uint32_t y, uint32_t color)
{
    put_pixel_raw(x, y, color);
}

void gfx_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color)
{
    for (uint32_t yy = y; yy < y + h && yy < gfx_state.height; yy++) {
        for (uint32_t xx = x; xx < x + w && xx < gfx_state.width; xx++) {
            put_pixel_raw(xx, yy, color);
        }
    }
}

void gfx_draw_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color)
{
    // Top and bottom
    for (uint32_t xx = x; xx < x + w && xx < gfx_state.width; xx++) {
        put_pixel_raw(xx, y, color);
        put_pixel_raw(xx, y + h - 1, color);
    }
    // Left and right
    for (uint32_t yy = y; yy < y + h && yy < gfx_state.height; yy++) {
        put_pixel_raw(x, yy, color);
        put_pixel_raw(x + w - 1, yy, color);
    }
}

void gfx_draw_hline(uint32_t x, uint32_t y, uint32_t w, uint32_t color)
{
    for (uint32_t xx = x; xx < x + w && xx < gfx_state.width; xx++) {
        put_pixel_raw(xx, y, color);
    }
}

void gfx_draw_vline(uint32_t x, uint32_t y, uint32_t h, uint32_t color)
{
    for (uint32_t yy = y; yy < y + h && yy < gfx_state.height; yy++) {
        put_pixel_raw(x, yy, color);
    }
}

void gfx_put_char(uint32_t x, uint32_t y, char c, uint32_t fg, uint32_t bg)
{
    // ASCII 32-127 → index 0-95
    unsigned idx = (unsigned char)c;
    if (idx < 32 || idx >= 128) {
        idx = 32;  // Fallback to space character
    }
    idx -= 32;

    // Font bitmap: her karakter 16 satır, her satır 1 byte (8 pixel)
    extern const uint8_t font8x16[96][16];
    const uint8_t* glyph = font8x16[idx];

    for (int row = 0; row < 16; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            uint32_t color = (bits & (0x80 >> col)) ? fg : bg;
            put_pixel_raw(x + col, y + row, color);
        }
    }
}

void gfx_put_string(uint32_t x, uint32_t y, const char* str, uint32_t fg, uint32_t bg)
{
    if (!str) return;
    
    uint32_t curr_x = x;
    for (int i = 0; str[i]; i++) {
        gfx_put_char(curr_x, y, str[i], fg, bg);
        curr_x += 8;
        if (curr_x + 8 > gfx_state.width) {
            curr_x = x;
            y += 16;
            if (y + 16 > gfx_state.height) break;
        }
    }
}

// ============================================================================
// Query Functions
// ============================================================================

gfx_info_t* gfx_get_info(void) {
    return &gfx_state;
}

uint32_t gfx_get_width(void) {
    return gfx_state.width;
}

uint32_t gfx_get_height(void) {
    return gfx_state.height;
}

gfx_mode_t gfx_get_mode(void) {
    return gfx_state.mode;
}
