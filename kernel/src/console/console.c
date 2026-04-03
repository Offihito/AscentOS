#include "console.h"
#include "fb/framebuffer.h"
#include "font/font.h"
#include <stdint.h>
#include <stddef.h>

#define BG_COLOR   0x001E1E2E
#define FG_COLOR   0x00CDD6F4

static uint32_t cursor_x;
static uint32_t cursor_y;
static uint32_t max_cols;
static uint32_t max_rows;

static void scroll_up(void) {
    uint32_t fb_w = fb_get_width();
    uint32_t fb_h = fb_get_height();
    uint32_t row_pixels = FONT_HEIGHT;

    volatile uint32_t *base = (volatile uint32_t *)((uint8_t *)0);

    // We need access to the raw fb pointer for memmove-like scroll.
    // Use framebuffer functions instead: redraw approach.
    // For simplicity, just clear screen and reset cursor.
    // A proper scroll would copy pixel rows, but requires raw fb access.
    // We'll implement a basic version using the fb API.

    (void)base;
    (void)fb_w;
    (void)fb_h;
    (void)row_pixels;

    // Simple approach: clear and reset
    fb_clear(BG_COLOR);
    cursor_x = 0;
    cursor_y = 0;
}

void console_init(struct limine_framebuffer *framebuffer) {
    fb_init(framebuffer);

    max_cols = fb_get_width() / FONT_WIDTH;
    max_rows = fb_get_height() / FONT_HEIGHT;
    cursor_x = 0;
    cursor_y = 0;

    fb_clear(BG_COLOR);
}

static void draw_char(char c, uint32_t col, uint32_t row) {
    const uint8_t *glyph = font_get_glyph(c);
    uint32_t px = col * FONT_WIDTH;
    uint32_t py = row * FONT_HEIGHT;

    for (uint32_t y = 0; y < FONT_HEIGHT; y++) {
        uint8_t bits = glyph[y];
        for (uint32_t x = 0; x < FONT_WIDTH; x++) {
            uint32_t color = (bits & (0x80 >> x)) ? FG_COLOR : BG_COLOR;
            fb_put_pixel(px + x, py + y, color);
        }
    }
}

void console_putchar(char c) {
    if (c == '\n') {
        cursor_x = 0;
        cursor_y++;
        if (cursor_y >= max_rows) {
            scroll_up();
        }
        return;
    }

    if (c == '\r') {
        cursor_x = 0;
        return;
    }

    if (c == '\t') {
        uint32_t tab_stop = (cursor_x + 4) & ~3u;
        cursor_x = tab_stop;
        if (cursor_x >= max_cols) {
            cursor_x = 0;
            cursor_y++;
            if (cursor_y >= max_rows) {
                scroll_up();
            }
        }
        return;
    }

    draw_char(c, cursor_x, cursor_y);
    cursor_x++;

    if (cursor_x >= max_cols) {
        cursor_x = 0;
        cursor_y++;
        if (cursor_y >= max_rows) {
            scroll_up();
        }
    }
}

void console_puts(const char *s) {
    while (*s) {
        console_putchar(*s++);
    }
}

void console_clear(void) {
    fb_clear(BG_COLOR);
    cursor_x = 0;
    cursor_y = 0;
}
