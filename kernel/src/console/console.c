#include "console.h"
#include "../drivers/serial.h"
#include "fb/framebuffer.h"
#include "font/font.h"
#include "lock/spinlock.h"
#include <stddef.h>
#include <stdint.h>

static spinlock_t console_lock = SPINLOCK_INIT;

#define BG_COLOR 0x001E1E2E
#define FG_COLOR 0x00CDD6F4

typedef struct {
  char c;
  uint32_t fg;
  uint32_t bg;
} console_char_t;

#define HISTORY_MAX 1000
#define COLS_MAX 256

static console_char_t history[HISTORY_MAX][COLS_MAX];
static uint32_t history_write_row =
    0; // The "logical" row index in history (circular)
static uint32_t view_scroll_offset =
    0; // 0 = at bottom, >0 = scrolled back N lines

static uint32_t cursor_x;
static uint32_t cursor_y;
static uint32_t max_cols;
static uint32_t max_rows;
static bool cursor_visible = false;

static void console_redraw(void) {
  fb_set_backbuffer_mode(true);
  fb_clear(BG_COLOR);

  // Calculate which row in history corresponds to the top of the screen
  uint32_t end_row = history_write_row - view_scroll_offset;
  uint32_t start_row =
      (end_row >= max_rows - 1) ? (end_row - (max_rows - 1)) : 0;

  for (uint32_t y = 0; y < max_rows; y++) {
    uint32_t h_row = start_row + y;
    if (h_row > end_row)
      break;

    for (uint32_t x = 0; x < max_cols; x++) {
      console_char_t *ch = &history[h_row % HISTORY_MAX][x];
      if (ch->c != 0) {
        const uint8_t *glyph = font_get_glyph(ch->c);
        uint32_t px = x * FONT_WIDTH;
        uint32_t py = y * FONT_HEIGHT;

        for (uint32_t gy = 0; gy < FONT_HEIGHT; gy++) {
          uint8_t bits = glyph[gy];
          for (uint32_t gx = 0; gx < FONT_WIDTH; gx++) {
            uint32_t color = (bits & (0x80 >> gx)) ? ch->fg : ch->bg;
            fb_put_pixel(px + gx, py + gy, color);
          }
        }
      }
    }
  }

  if (view_scroll_offset == 0 && cursor_visible) {
    console_update_cursor(true);
  }

  fb_swap_buffer();
  fb_set_backbuffer_mode(false);
}

void console_scroll_view(int delta) {
  spinlock_acquire(&console_lock);

  int new_offset = (int)view_scroll_offset + delta;
  if (new_offset < 0)
    new_offset = 0;

  // Don't scroll back further than HISTORY_MAX or history_write_row
  uint32_t max_scroll = (history_write_row > (max_rows - 1))
                            ? (history_write_row - (max_rows - 1))
                            : 0;
  if (max_scroll > HISTORY_MAX - max_rows)
    max_scroll = HISTORY_MAX - max_rows;

  if ((uint32_t)new_offset > max_scroll)
    new_offset = max_scroll;

  if ((uint32_t)new_offset != view_scroll_offset) {
    view_scroll_offset = (uint32_t)new_offset;
    console_redraw();
  }

  spinlock_release(&console_lock);
}

static void scroll_up(void) {
  uint32_t fb_w = fb_get_width();
  uint32_t fb_h = fb_get_height();
  uint32_t pitch = fb_get_pitch();
  void *base = fb_get_base();

  uint32_t move_height = fb_h - FONT_HEIGHT;
  uint64_t bytes_to_copy = move_height * pitch;

  uint8_t *dst = (uint8_t *)base;
  uint8_t *src = (uint8_t *)base + (FONT_HEIGHT * pitch);

  // Use a slightly faster copy if possible, though manual byte copy is safest
  // for now
  for (uint64_t i = 0; i < bytes_to_copy; i++) {
    dst[i] = src[i];
  }

  fb_fill_rect(0, fb_h - FONT_HEIGHT, fb_w, FONT_HEIGHT, BG_COLOR);

  if (cursor_y > 0) {
    cursor_y--;
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
  serial_putchar(c);
  bool was_visible = cursor_visible;

  if (was_visible && view_scroll_offset == 0) {
    console_update_cursor(false);
  }

  if (c == '\n') {
    cursor_x = 0;
    cursor_y++;
    history_write_row++;
    // Clear the new history line
    for (uint32_t i = 0; i < COLS_MAX; i++) {
      history[history_write_row % HISTORY_MAX][i].c = 0;
    }

    if (cursor_y >= max_rows) {
      if (view_scroll_offset == 0) {
        scroll_up();
      } else {
        cursor_y--;
        // If we are scrolled back, we still increment view_scroll_offset
        // so the user stays at the same logical position relative to the top
        view_scroll_offset++;
        if (view_scroll_offset >= HISTORY_MAX - max_rows)
          view_scroll_offset = HISTORY_MAX - max_rows;
      }
    }
    if (was_visible && view_scroll_offset == 0)
      console_update_cursor(true);
    return;
  }

  if (c == '\r') {
    cursor_x = 0;
    if (was_visible && view_scroll_offset == 0)
      console_update_cursor(true);
    return;
  }

  if (c == '\b') {
    if (cursor_x > 0) {
      cursor_x--;
    } else if (cursor_y > 0) {
      cursor_y--;
      cursor_x = max_cols - 1;
    }

    // Update history
    history[history_write_row % HISTORY_MAX][cursor_x].c = ' ';
    history[history_write_row % HISTORY_MAX][cursor_x].fg = FG_COLOR;
    history[history_write_row % HISTORY_MAX][cursor_x].bg = BG_COLOR;

    if (view_scroll_offset == 0) {
      draw_char(' ', cursor_x, cursor_y);
      if (was_visible)
        console_update_cursor(true);
    }
    return;
  }

  // Tab handling
  if (c == '\t') {
    for (int i = 0; i < 4; i++)
      console_putchar_unlocked(' ');
    return;
  }

  // Standard character
  if (cursor_x < COLS_MAX) {
    history[history_write_row % HISTORY_MAX][cursor_x].c = c;
    history[history_write_row % HISTORY_MAX][cursor_x].fg = FG_COLOR;
    history[history_write_row % HISTORY_MAX][cursor_x].bg = BG_COLOR;
  }

  if (view_scroll_offset == 0) {
    draw_char(c, cursor_x, cursor_y);
  }

  cursor_x++;
  if (cursor_x >= max_cols) {
    cursor_x = 0;
    cursor_y++;
    history_write_row++;
    // Clear history line
    for (uint32_t i = 0; i < COLS_MAX; i++) {
      history[history_write_row % HISTORY_MAX][i].c = 0;
    }

    if (cursor_y >= max_rows) {
      if (view_scroll_offset == 0) {
        scroll_up();
      } else {
        cursor_y--;
        view_scroll_offset++;
      }
    }
  }

  if (was_visible && view_scroll_offset == 0) {
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
