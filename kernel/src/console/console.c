#include "console.h"
#include "fb/framebuffer.h"
#include "font/font.h"
#include <stdint.h>
#include <stddef.h>
#include "../drivers/serial.h"
#include "lock/spinlock.h"

static spinlock_t console_lock = SPINLOCK_INIT;

#define BG_COLOR   0x001E1E2E
#define FG_COLOR   0x00CDD6F4

static uint32_t cursor_x;
static uint32_t cursor_y;
static uint32_t max_cols;
static uint32_t max_rows;
static bool cursor_visible = false;

static void scroll_up(void) {
    uint32_t fb_w = fb_get_width();
    uint32_t fb_h = fb_get_height();
    uint32_t pitch = fb_get_pitch();
    void *base = fb_get_base();

    uint32_t move_height = fb_h - FONT_HEIGHT;
    uint64_t bytes_to_copy = move_height * pitch;

    // Shift pixels up by one full text row using a forward copy loop.
    // Since dst < src, forward copying prevents overlap corruption.
    uint8_t *dst = (uint8_t *)base;
    uint8_t *src = (uint8_t *)base + (FONT_HEIGHT * pitch);
    for (uint64_t i = 0; i < bytes_to_copy; i++) {
        dst[i] = src[i];
    }

    // Erase the bottom row
    fb_fill_rect(0, fb_h - FONT_HEIGHT, fb_w, FONT_HEIGHT, BG_COLOR);

    // Pull cursor back onto the valid screen
    if (cursor_y > 0) {
        cursor_y--;
    }
    
    if (cursor_visible) {
        console_update_cursor(true);
    }
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

static void console_putchar_unlocked(char c) {
    bool was_visible = cursor_visible;
    if (was_visible) {
        console_update_cursor(false);
    }

    if (c == '\n') {
        cursor_x = 0;
        cursor_y++;
        if (cursor_y >= max_rows) {
            scroll_up();
        }
        if (was_visible) console_update_cursor(true);
        return;
    }

    if (c == '\r') {
        cursor_x = 0;
        if (was_visible) console_update_cursor(true);
        return;
    }
    
    if (c == '\b') {
        if (cursor_x > 0) {
            cursor_x--;
        } else if (cursor_y > 0) {
            cursor_y--;
            cursor_x = max_cols - 1;
        }
        draw_char(' ', cursor_x, cursor_y); 
        if (was_visible) console_update_cursor(true);
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
        if (was_visible) console_update_cursor(true);
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
    
    if (was_visible) {
        console_update_cursor(true);
    }
}

void console_putchar(char c) {
    spinlock_acquire(&console_lock);
    console_putchar_unlocked(c);
    spinlock_release(&console_lock);
}

void console_update_cursor(bool visible) {
    cursor_visible = visible;
    if (visible) {
        uint32_t px = cursor_x * FONT_WIDTH;
        uint32_t py = cursor_y * FONT_HEIGHT;
        for (uint32_t y = FONT_HEIGHT - 3; y < FONT_HEIGHT; y++) {
            for (uint32_t x = 0; x < FONT_WIDTH; x++) {
                fb_put_pixel(px + x, py + y, FG_COLOR);
            }
        }
    } else {
        draw_char(' ', cursor_x, cursor_y);
    }
}

void console_puts(const char *s) {
    spinlock_acquire(&console_lock);
    while (*s) {
        console_putchar_unlocked(*s++);
    }
    spinlock_release(&console_lock);
}

void console_clear(void) {
    spinlock_acquire(&console_lock);
    fb_clear(BG_COLOR);
    cursor_x = 0;
    cursor_y = 0;
    spinlock_release(&console_lock);
}
