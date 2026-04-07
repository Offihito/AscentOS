#include "console.h"
#include "../apic/lapic_timer.h"
#include "../drivers/input/keyboard.h"
#include "../drivers/serial.h"
#include "fb/framebuffer.h"
#include "font/font.h"
#include "lib/string.h"
#include "lock/spinlock.h"
#include <stddef.h>
#include <stdint.h>

static spinlock_t console_lock = SPINLOCK_INIT;
static void console_set_cursor_visible_unlocked(bool visible);
static void console_refresh_cursor_unlocked(void);
static uint32_t console_history_row(uint32_t screen_row);
static void console_process_escape_sequence(void);
static void console_clear_line_from_cursor(void);
static void draw_char_colored(char c, uint32_t col, uint32_t row, uint32_t fg, uint32_t bg);
static void draw_history_char(uint32_t col, uint32_t row);

static bool terminal_escape = false;
static char terminal_escape_buffer[32];
static size_t terminal_escape_len = 0;

#define BG_COLOR 0x00000000
#define FG_COLOR 0x00CDD6F4

// ── ANSI color palette (Catppuccin Mocha) ───────────────────────────────────
static uint32_t ansi_colors_normal[8] = {
    0x00181825, // 0 black   (surface0)
    0x00F38BA8, // 1 red
    0x00A6E3A1, // 2 green
    0x00F9E2AF, // 3 yellow
    0x0089B4FA, // 4 blue
    0x00CBA4F7, // 5 magenta
    0x0094E2D5, // 6 cyan
    0x00CDD6F4, // 7 white
};

static uint32_t ansi_colors_bright[8] = {
    0x00313244, // 0 bright black  (surface1)
    0x00F38BA8, // 1 bright red
    0x00A6E3A1, // 2 bright green
    0x00F9E2AF, // 3 bright yellow
    0x0089B4FA, // 4 bright blue
    0x00CBA4F7, // 5 bright magenta
    0x0094E2D5, // 6 bright cyan
    0x00FFFFFF, // 7 bright white
};

// ── Current SGR color state ──────────────────────────────────────────────────
static uint32_t current_fg = FG_COLOR;
static uint32_t current_bg = BG_COLOR;

typedef struct {
  char c;
  uint32_t fg;
  uint32_t bg;
} console_char_t;

#define HISTORY_MAX 1000
#define COLS_MAX 256

static console_char_t history[HISTORY_MAX][COLS_MAX];
static uint32_t history_write_row = 0;
static uint32_t view_scroll_offset = 0;

static uint32_t cursor_x;
static uint32_t cursor_y;
static uint32_t max_cols;
static uint32_t max_rows;
static bool cursor_logical_visible = false;
static bool cursor_phys_on = false;
static uint64_t last_blink_ms = 0;

static void console_redraw(void) {
  fb_set_backbuffer_mode(true);
  fb_clear(BG_COLOR);

  uint32_t end_row = history_write_row - view_scroll_offset;
  uint32_t start_row =
      (end_row >= max_rows - 1) ? (end_row - (max_rows - 1)) : 0;

  for (uint32_t y = 0; y < max_rows; y++) {
    uint32_t h_row = start_row + y;
    if (h_row > end_row)
      break;

    for (uint32_t x = 0; x < max_cols; x++) {
      console_char_t *ch = &history[h_row % HISTORY_MAX][x];
      uint32_t px = x * FONT_WIDTH;
      uint32_t py = y * FONT_HEIGHT;

      if (ch->c != 0) {
        const uint8_t *glyph = font_get_glyph(ch->c);
        for (uint32_t gy = 0; gy < FONT_HEIGHT; gy++) {
          uint8_t bits = glyph[gy];
          for (uint32_t gx = 0; gx < FONT_WIDTH; gx++) {
            uint32_t color = (bits & (0x80 >> gx)) ? ch->fg : ch->bg;
            fb_put_pixel(px + gx, py + gy, color);
          }
        }
      } else if (ch->bg != BG_COLOR) {
        // Cell is blank but has a non-default background (e.g. reverse video
        // status bar)
        fb_fill_rect(px, py, FONT_WIDTH, FONT_HEIGHT, ch->bg);
      }
    }
  }

  if (view_scroll_offset == 0 && cursor_logical_visible && cursor_phys_on) {
    uint32_t px = cursor_x * FONT_WIDTH;
    uint32_t py = cursor_y * FONT_HEIGHT;
    for (uint32_t y = FONT_HEIGHT - 3; y < FONT_HEIGHT; y++) {
      for (uint32_t x = 0; x < FONT_WIDTH; x++) {
        fb_put_pixel(px + x, py + y, FG_COLOR);
      }
    }
  }

  fb_swap_buffer();
  fb_set_backbuffer_mode(false);
}

void console_scroll_view(int delta) {
  spinlock_acquire(&console_lock);

  int new_offset = (int)view_scroll_offset + delta;
  if (new_offset < 0)
    new_offset = 0;

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

  memcpy(dst, src, bytes_to_copy);

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

static uint32_t console_history_row(uint32_t screen_row) {
  if (history_write_row >= max_rows - 1) {
    return (history_write_row - (max_rows - 1) + screen_row) % HISTORY_MAX;
  }
  return screen_row % HISTORY_MAX;
}

static void console_clear_line_from_cursor(void) {
  uint32_t row = console_history_row(cursor_y);
  for (uint32_t x = cursor_x; x < max_cols; x++) {
    console_char_t *cell = &history[row][x];
    cell->c = 0;
    cell->fg = FG_COLOR;
    cell->bg = BG_COLOR;
    if (view_scroll_offset == 0) {
      fb_fill_rect(x * FONT_WIDTH, cursor_y * FONT_HEIGHT, FONT_WIDTH,
                   FONT_HEIGHT, BG_COLOR);
    }
  }
}

// ── SGR escape sequence handler ─────────────────────────────────────────────
static void console_process_escape_sequence(void) {
  if (terminal_escape_len == 0) return;
  if (terminal_escape_buffer[0] != '[') return;

  bool question = false;
  int params[16] = {0};
  int param_count = 1;
  char *p = terminal_escape_buffer + 1;

  if (*p == '?') {
    question = true;
    p++;
  }

  while (*p && (*p < '@' || *p > '~')) {
    if (*p >= '0' && *p <= '9') {
      params[param_count - 1] = params[param_count - 1] * 10 + (*p - '0');
    } else if (*p == ';') {
      if (param_count < 16) param_count++;
    }
    p++;
  }

  char final = *p;
  int value  = params[0];
  int value2 = (param_count > 1) ? params[1] : 0;

  switch (final) {
  // ── Cursor movement ────────────────────────────────────────────────────────
  case 'A':
    if (value == 0) value = 1;
    cursor_y = (cursor_y >= (uint32_t)value) ? cursor_y - value : 0;
    break;
  case 'B':
    cursor_y += value ? value : 1;
    if (cursor_y >= max_rows) cursor_y = max_rows - 1;
    break;
  case 'C':
    cursor_x += value ? value : 1;
    if (cursor_x >= max_cols) cursor_x = max_cols - 1;
    break;
  case 'D':
    if (value == 0) value = 1;
    cursor_x = (cursor_x >= (uint32_t)value) ? cursor_x - value : 0;
    break;
  case 'H':
  case 'f':
    cursor_y = (value  > 0) ? (uint32_t)(value  - 1) : 0;
    cursor_x = (value2 > 0) ? (uint32_t)(value2 - 1) : 0;
    if (cursor_y >= max_rows) cursor_y = max_rows - 1;
    if (cursor_x >= max_cols) cursor_x = max_cols - 1;
    break;

  // ── Erase ─────────────────────────────────────────────────────────────────
  case 'J':
    if (value == 2) {
      fb_clear(BG_COLOR);
      memset(history, 0, sizeof(history));
      cursor_x = 0;
      cursor_y = 0;
      view_scroll_offset = 0;
      history_write_row = 0;
      cursor_logical_visible = false;
      cursor_phys_on = false;
    }
    break;
  case 'K':
    console_clear_line_from_cursor();
    break;

  // ── Cursor visibility ──────────────────────────────────────────────────────
  case 'h':
    if (question && value == 25)
      console_set_cursor_visible_unlocked(true);
    break;
  case 'l':
    if (question && value == 25)
      console_set_cursor_visible_unlocked(false);
    break;

  // ── Cursor position report ─────────────────────────────────────────────────
  case 'n':
    if (!question && value == 6) {
      char resp[32];
      int len = 0;
      char temp[16];
      uint32_t row = cursor_y + 1;
      uint32_t col = cursor_x + 1;
      resp[len++] = 0x1B;
      resp[len++] = '[';
      if (row == 0) {
        resp[len++] = '0';
      } else {
        int digits = 0;
        uint32_t v = row;
        while (v > 0) { temp[digits++] = '0' + (v % 10); v /= 10; }
        while (digits--) resp[len++] = temp[digits];
      }
      resp[len++] = ';';
      if (col == 0) {
        resp[len++] = '0';
      } else {
        int digits = 0;
        uint32_t v = col;
        while (v > 0) { temp[digits++] = '0' + (v % 10); v /= 10; }
        while (digits--) resp[len++] = temp[digits];
      }
      resp[len++] = 'R';
      keyboard_push_bytes(resp, (uint32_t)len);
    }
    break;

  // ── SGR — Select Graphic Rendition (colors + attributes) ──────────────────
  case 'm': {
    // Bare ESC[m is equivalent to ESC[0m — full reset
    if (param_count == 1 && params[0] == 0) {
      current_fg = FG_COLOR;
      current_bg = BG_COLOR;
      break;
    }

    for (int pi = 0; pi < param_count; pi++) {
      int code = params[pi];

      if (code == 0) {
        // Reset all attributes
        current_fg = FG_COLOR;
        current_bg = BG_COLOR;
      } else if (code == 1) {
        // Bold — use bright variant of current fg if it is a named color.
        // For simplicity we leave fg unchanged (still visible).
      } else if (code == 22) {
        // Normal intensity — no-op for now
      } else if (code == 7) {
        // Reverse video — swap fg and bg
        uint32_t tmp = current_fg;
        current_fg = current_bg;
        current_bg = tmp;
      } else if (code == 27) {
        // Reverse off — reset to defaults
        current_fg = FG_COLOR;
        current_bg = BG_COLOR;
      } else if (code >= 30 && code <= 37) {
        current_fg = ansi_colors_normal[code - 30];
      } else if (code == 38) {
        // 256-color or truecolor fg — consume extra params
        if (pi + 1 < param_count && params[pi + 1] == 5 && pi + 2 < param_count) {
          // ESC[38;5;Nm — 256-color: map index to our palette for 0-15, else grey
          int idx = params[pi + 2];
          if (idx >= 0 && idx <= 7)       current_fg = ansi_colors_normal[idx];
          else if (idx >= 8 && idx <= 15) current_fg = ansi_colors_bright[idx - 8];
          else                            current_fg = FG_COLOR;
          pi += 2;
        } else if (pi + 1 < param_count && params[pi + 1] == 2 && pi + 4 < param_count) {
          // ESC[38;2;R;G;Bm — truecolor
          uint8_t r = (uint8_t)params[pi + 2];
          uint8_t g = (uint8_t)params[pi + 3];
          uint8_t b = (uint8_t)params[pi + 4];
          current_fg = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
          pi += 4;
        }
      } else if (code == 39) {
        current_fg = FG_COLOR; // default fg
      } else if (code >= 40 && code <= 47) {
        current_bg = ansi_colors_normal[code - 40];
      } else if (code == 48) {
        // 256-color or truecolor bg
        if (pi + 1 < param_count && params[pi + 1] == 5 && pi + 2 < param_count) {
          int idx = params[pi + 2];
          if (idx >= 0 && idx <= 7)       current_bg = ansi_colors_normal[idx];
          else if (idx >= 8 && idx <= 15) current_bg = ansi_colors_bright[idx - 8];
          else                            current_bg = BG_COLOR;
          pi += 2;
        } else if (pi + 1 < param_count && params[pi + 1] == 2 && pi + 4 < param_count) {
          uint8_t r = (uint8_t)params[pi + 2];
          uint8_t g = (uint8_t)params[pi + 3];
          uint8_t b = (uint8_t)params[pi + 4];
          current_bg = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
          pi += 4;
        }
      } else if (code == 49) {
        current_bg = BG_COLOR; // default bg
      } else if (code >= 90 && code <= 97) {
        current_fg = ansi_colors_bright[code - 90];
      } else if (code >= 100 && code <= 107) {
        current_bg = ansi_colors_bright[code - 100];
      }
    }
    break;
  }

  default:
    break;
  }
}

// ── Character drawing helpers ────────────────────────────────────────────────

static void draw_char_colored(char c, uint32_t col, uint32_t row,
                              uint32_t fg, uint32_t bg) {
  const uint8_t *glyph = font_get_glyph(c);
  uint32_t px = col * FONT_WIDTH;
  uint32_t py = row * FONT_HEIGHT;

  for (uint32_t y = 0; y < FONT_HEIGHT; y++) {
    uint8_t bits = glyph[y];
    for (uint32_t x = 0; x < FONT_WIDTH; x++) {
      uint32_t color = (bits & (0x80 >> x)) ? fg : bg;
      fb_put_pixel(px + x, py + y, color);
    }
  }
}

static void draw_history_char(uint32_t col, uint32_t row) {
  uint32_t history_row = console_history_row(row);
  console_char_t *ch = &history[history_row][col];

  uint32_t px = col * FONT_WIDTH;
  uint32_t py = row * FONT_HEIGHT;

  if (ch->c == 0) {
    fb_fill_rect(px, py, FONT_WIDTH, FONT_HEIGHT, ch->bg);
    return;
  }

  draw_char_colored(ch->c, col, row, ch->fg, ch->bg);
}

// ── Core putchar (must be called with console_lock held) ─────────────────────
static void console_putchar_unlocked(char c) {
  if (terminal_escape) {
    if (terminal_escape_len < sizeof(terminal_escape_buffer) - 1) {
      terminal_escape_buffer[terminal_escape_len++] = c;
    }
    // Finalise on any character in the range @-~ except '[' (which is the
    // introducer for CSI sequences that still need more chars).
    if (c >= '@' && c <= '~' && !(terminal_escape_len == 1 && c == '[')) {
      console_process_escape_sequence();
      terminal_escape = false;
      terminal_escape_len = 0;
    }
    return;
  }

  if (c == 0x1B) {
    terminal_escape = true;
    terminal_escape_len = 0;
    return;
  }

  serial_putchar(c);

  if (c == '\n') {
    cursor_x = 0;
    cursor_y++;
    history_write_row++;
    // Clear the new history line
    uint32_t row = console_history_row(cursor_y);
    for (uint32_t i = 0; i < COLS_MAX; i++) {
      history[row][i].c   = 0;
      history[row][i].fg  = FG_COLOR;
      history[row][i].bg  = BG_COLOR;
    }

    if (view_scroll_offset == 0 && cursor_y < max_rows) {
      fb_fill_rect(0, cursor_y * FONT_HEIGHT, fb_get_width(), FONT_HEIGHT,
                   BG_COLOR);
    }

    if (cursor_y >= max_rows) {
      if (view_scroll_offset == 0) {
        scroll_up();
        uint32_t new_row = history_write_row % HISTORY_MAX;
        for (uint32_t i = 0; i < COLS_MAX; i++) {
          history[new_row][i].c   = 0;
          history[new_row][i].fg  = FG_COLOR;
          history[new_row][i].bg  = BG_COLOR;
        }
      } else {
        cursor_y--;
        view_scroll_offset++;
        if (view_scroll_offset >= HISTORY_MAX - max_rows)
          view_scroll_offset = HISTORY_MAX - max_rows;
      }
    }
    return;
  }

  if (c == '\r') {
    cursor_x = 0;
    return;
  }

  if (c == '\b') {
    if (cursor_x > 0) {
      cursor_x--;
    } else if (cursor_y > 0) {
      cursor_y--;
      cursor_x = max_cols - 1;
    }

    uint32_t row = console_history_row(cursor_y);
    history[row][cursor_x].c   = ' ';
    history[row][cursor_x].fg  = FG_COLOR;
    history[row][cursor_x].bg  = BG_COLOR;

    if (view_scroll_offset == 0) {
      fb_fill_rect(cursor_x * FONT_WIDTH, cursor_y * FONT_HEIGHT, FONT_WIDTH,
                   FONT_HEIGHT, BG_COLOR);
    }
    return;
  }

  if (c == '\t') {
    for (int i = 0; i < 4; i++)
      console_putchar_unlocked(' ');
    return;
  }

  // Standard printable character — store with current SGR colors
  if (cursor_x < COLS_MAX) {
    uint32_t row = console_history_row(cursor_y);
    history[row][cursor_x].c   = c;
    history[row][cursor_x].fg  = current_fg;
    history[row][cursor_x].bg  = current_bg;
  }

  if (view_scroll_offset == 0) {
    draw_char_colored(c, cursor_x, cursor_y, current_fg, current_bg);
  }

  cursor_x++;
  if (cursor_x >= max_cols) {
    cursor_x = 0;
    cursor_y++;
    history_write_row++;
    uint32_t new_row = history_write_row % HISTORY_MAX;
    for (uint32_t i = 0; i < COLS_MAX; i++) {
      history[new_row][i].c   = 0;
      history[new_row][i].fg  = FG_COLOR;
      history[new_row][i].bg  = BG_COLOR;
    }

    if (view_scroll_offset == 0 && cursor_y < max_rows) {
      fb_fill_rect(0, cursor_y * FONT_HEIGHT, fb_get_width(), FONT_HEIGHT,
                   BG_COLOR);
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
}

// ── Public API ───────────────────────────────────────────────────────────────

void console_putchar(char c) {
  spinlock_acquire(&console_lock);
  console_putchar_unlocked(c);
  spinlock_release(&console_lock);
}

// Batch-aware puts: writes all characters then does ONE framebuffer blit.
// This is the main anti-flicker fix — kilo sends one large write() per frame
// so everything lands in one swap.
void console_puts(const char *s) {
  spinlock_acquire(&console_lock);

  bool was_visible = cursor_logical_visible;
  if (was_visible && view_scroll_offset == 0)
    console_set_cursor_visible_unlocked(false);

  while (*s)
    console_putchar_unlocked(*s++);

  if (was_visible && view_scroll_offset == 0)
    console_set_cursor_visible_unlocked(true);

  spinlock_release(&console_lock);
}

// Batch write used by the VFS console node and fd 1/2 in sys_write.
// Renders all characters then swaps the backbuffer exactly once.
void console_write_batch(const char *buf, size_t len) {
  spinlock_acquire(&console_lock);

  fb_set_backbuffer_mode(true);

  // Don't toggle cursor visibility - just skip drawing it and restore after
  bool cursor_was_on = cursor_phys_on;
  cursor_phys_on = false;

  for (size_t i = 0; i < len; i++)
    console_putchar_unlocked(buf[i]);

  // Restore cursor state and draw it once at the end
  cursor_phys_on = cursor_was_on;
  if (cursor_logical_visible && cursor_phys_on && view_scroll_offset == 0) {
    uint32_t px = cursor_x * FONT_WIDTH;
    uint32_t py = cursor_y * FONT_HEIGHT;
    for (uint32_t y = FONT_HEIGHT - 3; y < FONT_HEIGHT; y++)
      for (uint32_t x = 0; x < FONT_WIDTH; x++)
        fb_put_pixel(px + x, py + y, FG_COLOR);
  }

  fb_swap_buffer();
  fb_set_backbuffer_mode(false);

  spinlock_release(&console_lock);
}

static void console_set_cursor_visible_unlocked(bool visible) {
  cursor_logical_visible = visible;
  if (visible) {
    cursor_phys_on = true;
    last_blink_ms = lapic_timer_get_ms();

    uint32_t px = cursor_x * FONT_WIDTH;
    uint32_t py = cursor_y * FONT_HEIGHT;
    for (uint32_t y = FONT_HEIGHT - 3; y < FONT_HEIGHT; y++)
      for (uint32_t x = 0; x < FONT_WIDTH; x++)
        fb_put_pixel(px + x, py + y, FG_COLOR);
  } else {
    cursor_phys_on = false;
    draw_history_char(cursor_x, cursor_y);
  }
}

void console_set_cursor_visible(bool visible) {
  spinlock_acquire(&console_lock);
  console_set_cursor_visible_unlocked(visible);
  spinlock_release(&console_lock);
}

static void console_refresh_cursor_unlocked(void) {
  if (!cursor_logical_visible || view_scroll_offset != 0)
    return;

  uint64_t now = lapic_timer_get_ms();
  if (now - last_blink_ms < 500)
    return;

  last_blink_ms = now;
  cursor_phys_on = !cursor_phys_on;

  if (cursor_phys_on) {
    uint32_t px = cursor_x * FONT_WIDTH;
    uint32_t py = cursor_y * FONT_HEIGHT;
    for (uint32_t y = FONT_HEIGHT - 3; y < FONT_HEIGHT; y++)
      for (uint32_t x = 0; x < FONT_WIDTH; x++)
        fb_put_pixel(px + x, py + y, FG_COLOR);
  } else {
    draw_history_char(cursor_x, cursor_y);
  }
}

void console_refresh_cursor(void) {
  spinlock_acquire(&console_lock);
  console_refresh_cursor_unlocked();
  spinlock_release(&console_lock);
}

void console_clear(void) {
  spinlock_acquire(&console_lock);
  fb_clear(BG_COLOR);
  memset(history, 0, sizeof(history));
  cursor_x = 0;
  cursor_y = 0;
  view_scroll_offset = 0;
  history_write_row = 0;
  cursor_logical_visible = false;
  cursor_phys_on = false;
  current_fg = FG_COLOR;
  current_bg = BG_COLOR;
  spinlock_release(&console_lock);
}