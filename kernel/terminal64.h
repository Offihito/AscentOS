// ============================================================================
// terminal64.h - Terminal Window for GUI Mode (Dynamic Resize Support - FIXED)
// ============================================================================
#ifndef TERMINAL64_H
#define TERMINAL64_H

#include <stdint.h>
#include <stdbool.h>
#include "gui64.h"

// Maksimum desteklenen boyutlar (buffer için sabit büyük bir alan ayırıyoruz)
#define MAX_TERM_COLS 200
#define MAX_TERM_ROWS 100
#define TERM_BUFFER_SIZE (MAX_TERM_COLS * MAX_TERM_ROWS)

typedef struct {
    Window window;

    // Buffer ve input line artık maksimum boyuta göre
    char buffer[TERM_BUFFER_SIZE];
    char input_line[MAX_TERM_COLS + 1];  // +1 for null terminator

    // Cursor konumu
    int cursor_x, cursor_y;

    // Input pozisyonu
    int input_pos;

    // Cursor görünürlüğü ve blink
    bool cursor_visible;
    uint32_t cursor_blink_counter;

    // Renkler
    Color text_color;
    Color cursor_color;

    // Görünür (visible) alan - pencere boyutuna göre dinamik olarak hesaplanır
    int visible_cols;
    int visible_rows;

    // Dragging support
    bool is_dragging;
    int drag_offset_x;
    int drag_offset_y;

    // Resizing support
    bool is_resizing;
    int resize_start_x;
    int resize_start_y;
    int resize_start_width;
    int resize_start_height;

    // Full redraw flag - resize/drag sonrası kullanılır
    bool needs_full_redraw;

} Terminal;

// Terminal functions
void terminal_init(Terminal* term, int x, int y, int width, int height);
void terminal_clear(Terminal* term);
void terminal_putchar(Terminal* term, char c);
void terminal_print(Terminal* term, const char* str);
void terminal_println(Terminal* term, const char* str);
void terminal_handle_key(Terminal* term, char key);
void terminal_draw(Terminal* term);
void terminal_draw_incremental(Terminal* term);
void terminal_show_prompt(Terminal* term);
void terminal_scroll(Terminal* term);
void terminal_draw_icon(int x, int y);
void terminal_println_colored(Terminal* term, const char* str, uint32_t color);
void terminal_print_colored(Terminal* term, const char* str, uint32_t color);

// Mouse interaction
bool terminal_handle_mouse_down(Terminal* term, int mouse_x, int mouse_y);
void terminal_handle_mouse_up(Terminal* term);
void terminal_handle_mouse_move(Terminal* term, int mouse_x, int mouse_y, 
                               int screen_width, int screen_height);
bool terminal_is_in_title_bar(Terminal* term, int mouse_x, int mouse_y);
bool terminal_is_in_resize_corner(Terminal* term, int mouse_x, int mouse_y);

#endif