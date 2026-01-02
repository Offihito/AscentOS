// terminal64.c - Terminal Window with Full Dynamic Resize Support (FIXED)
#include "terminal64.h"
#include "gui64.h"
#include "commands_gui.h"
#include <stddef.h>

#define MAX_TERM_COLS 200
#define MAX_TERM_ROWS 100
#define TERM_BUFFER_SIZE (MAX_TERM_COLS * MAX_TERM_ROWS)

static void str_copy(char* dest, const char* src, size_t max) {
    size_t i;
    for (i = 0; i < max - 1 && src[i]; i++) {
        dest[i] = src[i];
    }
    dest[i] = '\0';
}

static void mem_set(void* dest, int val, size_t n) {
    uint8_t* d = (uint8_t*)dest;
    while (n--) *d++ = (uint8_t)val;
}

// Pencere boyutuna göre görünür kolon/satır sayısını hesapla
static void terminal_update_visible_size(Terminal* term) {
    const int char_width = 8;
    const int char_height = 8;
    
    term->visible_cols = (term->window.width - 16) / char_width;   // 8px sol + 8px sağ
    term->visible_rows = (term->window.height - 36) / char_height; // 28px title + 8px alt

    if (term->visible_cols < 40) term->visible_cols = 40;
    if (term->visible_rows < 10) term->visible_rows = 10;
    if (term->visible_cols > MAX_TERM_COLS) term->visible_cols = MAX_TERM_COLS;
    if (term->visible_rows > MAX_TERM_ROWS) term->visible_rows = MAX_TERM_ROWS;
}

void terminal_init(Terminal* term, int x, int y, int width, int height) {
    term->window.x = x;
    term->window.y = y;
    term->window.width = width;
    term->window.height = height;
    term->window.visible = true;
    term->window.border_color = RGB(30, 30, 35);
    term->window.bg_color = RGB(12, 12, 18);
    str_copy(term->window.title, "AscentOS Terminal", 64);
    
    term->cursor_x = 0;
    term->cursor_y = 0;
    term->input_pos = 0;
    term->cursor_visible = true;
    term->cursor_blink_counter = 0;
    term->text_color = RGB(0, 255, 0);
    term->cursor_color = RGB(255, 255, 255);
    
    term->is_dragging = false;
    term->is_resizing = false;
    term->needs_full_redraw = false;
    
    mem_set(term->buffer, ' ', TERM_BUFFER_SIZE);
    mem_set(term->input_line, 0, MAX_TERM_COLS + 1);

    terminal_update_visible_size(term);
    terminal_clear(term);
    terminal_show_prompt(term);
}

void terminal_clear(Terminal* term) {
    terminal_update_visible_size(term);
    for (int row = 0; row < term->visible_rows; row++) {
        for (int col = 0; col < term->visible_cols; col++) {
            term->buffer[row * MAX_TERM_COLS + col] = ' ';
        }
    }
    term->cursor_x = 0;
    term->cursor_y = 0;
}

void terminal_scroll(Terminal* term) {
    terminal_update_visible_size(term);
    int cols = term->visible_cols;
    int rows = term->visible_rows;
    
    for (int row = 0; row < rows - 1; row++) {
        for (int col = 0; col < cols; col++) {
            term->buffer[row * MAX_TERM_COLS + col] = 
                term->buffer[(row + 1) * MAX_TERM_COLS + col];
        }
    }
    for (int col = 0; col < cols; col++) {
        term->buffer[(rows - 1) * MAX_TERM_COLS + col] = ' ';
    }
}

void terminal_putchar(Terminal* term, char c) {
    if (c == '\n') {
        term->cursor_x = 0;
        term->cursor_y++;
        if (term->cursor_y >= term->visible_rows) {
            terminal_scroll(term);
            term->cursor_y = term->visible_rows - 1;
        }
        return;
    }
    
    if (c == '\r') {
        term->cursor_x = 0;
        return;
    }
    
    if (c == '\b') {
        if (term->cursor_x > 0) {
            term->cursor_x--;
            term->buffer[term->cursor_y * MAX_TERM_COLS + term->cursor_x] = ' ';
        }
        return;
    }
    
    if (term->cursor_x >= term->visible_cols) {
        term->cursor_x = 0;
        term->cursor_y++;
        if (term->cursor_y >= term->visible_rows) {
            terminal_scroll(term);
            term->cursor_y = term->visible_rows - 1;
        }
    }
    
    term->buffer[term->cursor_y * MAX_TERM_COLS + term->cursor_x] = c;
    term->cursor_x++;
}

void terminal_print(Terminal* term, const char* str) {
    while (*str) {
        terminal_putchar(term, *str++);
    }
}

void terminal_println(Terminal* term, const char* str) {
    terminal_print(term, str);
    terminal_putchar(term, '\n');
}

void terminal_show_prompt(Terminal* term) {
    terminal_print(term, "AscentOS> ");
}

void terminal_handle_key(Terminal* term, char key) {
    if (key == '\n' || key == '\r') {
        terminal_putchar(term, '\n');
        
        if (term->input_pos > 0) {
            // Komut işlemeden önce buffer'ı güvene al
            char cmd_backup[MAX_TERM_COLS + 1];
            str_copy(cmd_backup, term->input_line, MAX_TERM_COLS + 1);
            
            mem_set(term->input_line, 0, MAX_TERM_COLS + 1);
            term->input_pos = 0;
            
            // Komutu işle
            process_command(term, cmd_backup);
            
            // Komut sonrası tam çizim gerekir (filesystem işlemleri için)
            term->needs_full_redraw = true;
        }
        
        terminal_show_prompt(term);
        
        // Sadece komut çalıştırıldıysa tam redraw
        if (term->needs_full_redraw) {
            terminal_draw(term);
        } else {
            terminal_draw_incremental(term);
        }
    }
    else if (key == '\b') {
        if (term->input_pos > 0) {
            term->input_pos--;
            term->input_line[term->input_pos] = '\0';
            terminal_putchar(term, '\b');
            terminal_draw_incremental(term);
        }
    }
    else if (key >= 32 && key < 127 && term->input_pos < term->visible_cols - 1) {
        term->input_line[term->input_pos++] = key;
        terminal_putchar(term, key);
        terminal_draw_incremental(term);
    }
}

// Gerçek incremental redraw: sadece değişen satırları çizer
void terminal_draw_incremental(Terminal* term) {
    if (!term->window.visible) return;
    
    // Resize/drag sonrası full redraw gerekiyorsa onu yap
    if (term->needs_full_redraw) {
        terminal_draw(term);
        term->needs_full_redraw = false;
        return;
    }
    
    terminal_update_visible_size(term);

    const int start_x = term->window.x + 8;
    const int start_y = term->window.y + 32;
    const int char_width = 8;
    const int char_height = 8;

    // Değişebilecek satırlar: mevcut ve bir önceki
    int start_row = (term->cursor_y > 0) ? term->cursor_y - 1 : 0;
    int end_row = term->cursor_y;

    for (int row = start_row; row <= end_row && row < term->visible_rows; row++) {
        int line_y = start_y + row * char_height;

        // Satırı temizle
        gui_fill_rect(term->window.x + 4, line_y, 
                      term->window.width - 8, char_height, 
                      term->window.bg_color);

        // Karakterleri çiz
        for (int col = 0; col < term->visible_cols; col++) {
            char c = term->buffer[row * MAX_TERM_COLS + col];
            if (c != ' ' && c != 0) {
                gui_draw_char(start_x + col * char_width, line_y, 
                              c, term->text_color, 0);
            }
        }
    }

    // Cursor'ı çiz
    if (term->cursor_y < term->visible_rows && term->cursor_x < term->visible_cols) {
        int cx = start_x + term->cursor_x * char_width;
        int cy = start_y + term->cursor_y * char_height + 7;
        gui_fill_rect(cx, cy, char_width, 1, term->cursor_color);
    }
}

void terminal_draw(Terminal* term) {
    if (!term->window.visible) return;
    
    terminal_update_visible_size(term);
    
    gui_draw_window(&term->window);
    
    const int start_x = term->window.x + 8;
    const int start_y = term->window.y + 32;
    const int char_width = 8;
    const int char_height = 8;
    
    // İçerik alanını tamamen temizle
    gui_fill_rect(term->window.x + 4, term->window.y + 28,
                  term->window.width - 8, term->window.height - 32,
                  term->window.bg_color);
    
    // Tüm görünür karakterleri çiz
    for (int row = 0; row < term->visible_rows; row++) {
        for (int col = 0; col < term->visible_cols; col++) {
            char c = term->buffer[row * MAX_TERM_COLS + col];
            if (c != ' ' && c != 0) {
                int x = start_x + col * char_width;
                int y = start_y + row * char_height;
                gui_draw_char(x, y, c, term->text_color, 0);
            }
        }
    }
    
    // Cursor
    if (term->cursor_y < term->visible_rows && term->cursor_x < term->visible_cols) {
        int cx = start_x + term->cursor_x * char_width;
        int cy = start_y + term->cursor_y * char_height + 7;
        gui_fill_rect(cx, cy, char_width, 1, term->cursor_color);
    }
    
    // Resize handle
    const int handle_size = 12;
    int hx = term->window.x + term->window.width - handle_size;
    int hy = term->window.y + term->window.height - handle_size;
    Color hc = RGB(80, 80, 90);
    for (int i = 0; i < 3; i++) {
        gui_draw_line(hx + i * 4, hy + handle_size - 1,
                      hx + handle_size - 1, hy + i * 4, hc);
    }
    
    term->needs_full_redraw = false;
}

bool terminal_is_in_title_bar(Terminal* term, int mouse_x, int mouse_y) {
    return (mouse_x >= term->window.x && 
            mouse_x < term->window.x + term->window.width &&
            mouse_y >= term->window.y && 
            mouse_y < term->window.y + 28);
}

bool terminal_is_in_resize_corner(Terminal* term, int mouse_x, int mouse_y) {
    const int handle_size = 12;
    int hx = term->window.x + term->window.width - handle_size;
    int hy = term->window.y + term->window.height - handle_size;
    return (mouse_x >= hx && mouse_x < term->window.x + term->window.width &&
            mouse_y >= hy && mouse_y < term->window.y + term->window.height);
}

bool terminal_handle_mouse_down(Terminal* term, int mouse_x, int mouse_y) {
    if (!term->window.visible) return false;
    
    if (terminal_is_in_resize_corner(term, mouse_x, mouse_y)) {
        term->is_resizing = true;
        term->resize_start_x = mouse_x;
        term->resize_start_y = mouse_y;
        term->resize_start_width = term->window.width;
        term->resize_start_height = term->window.height;
        return true;
    }
    
    if (terminal_is_in_title_bar(term, mouse_x, mouse_y)) {
        term->is_dragging = true;
        term->drag_offset_x = mouse_x - term->window.x;
        term->drag_offset_y = mouse_y - term->window.y;
        return true;
    }
    
    return false;
}

void terminal_handle_mouse_up(Terminal* term) {
    bool was_resizing = term->is_resizing;
    
    term->is_dragging = false;
    term->is_resizing = false;
    
    // Sadece resize bittiyse ve boyut değiştiyse tam redraw gerekir
    if (was_resizing && term->needs_full_redraw) {
        terminal_draw(term);
        term->needs_full_redraw = false;
    }
}

void terminal_handle_mouse_move(Terminal* term, int mouse_x, int mouse_y, 
                               int screen_width, int screen_height) {
    if (term->is_resizing) {
        // Calculate new size
        int delta_x = mouse_x - term->resize_start_x;
        int delta_y = mouse_y - term->resize_start_y;
        
        int new_width = term->resize_start_width + delta_x;
        int new_height = term->resize_start_height + delta_y;
        
        // Minimum size constraints
        const int min_width = 300;
        const int min_height = 200;
        if (new_width < min_width) new_width = min_width;
        if (new_height < min_height) new_height = min_height;
        
        // Maximum size constraints (screen bounds)
        int max_width = screen_width - term->window.x - 10;
        int max_height = screen_height - term->window.y - 10;
        if (new_width > max_width) new_width = max_width;
        if (new_height > max_height) new_height = max_height;
        
        // Boyut değiştiğinde flag set et ama burada redraw yapma
        if (new_width != term->window.width || new_height != term->window.height) {
            term->window.width = new_width;
            term->window.height = new_height;
            // Flag'i set et, resize bitince kullanılacak
            term->needs_full_redraw = true;
        }
    }
    else if (term->is_dragging) {
        int new_x = mouse_x - term->drag_offset_x;
        int new_y = mouse_y - term->drag_offset_y;
        
        if (new_x < 0) new_x = 0;
        if (new_y < 0) new_y = 0;
        if (new_x + term->window.width > screen_width) {
            new_x = screen_width - term->window.width;
        }
        if (new_y + term->window.height > screen_height) {
            new_y = screen_height - term->window.height;
        }
        
        // Pozisyon değiştiğinde güncelle (flag gerekmez, kernel halleder)
        if (new_x != term->window.x || new_y != term->window.y) {
            term->window.x = new_x;
            term->window.y = new_y;
        }
    }
}

void terminal_print_colored(Terminal* term, const char* str, uint32_t color) {
    terminal_print(term, str);
}

void terminal_println_colored(Terminal* term, const char* str, uint32_t color) {
    terminal_println(term, str);
}

void terminal_draw_icon(int x, int y) {
    const Color border = RGB(50, 50, 60);
    const Color title_bar = RGB(30, 30, 35);
    const Color bg = RGB(12, 12, 18);
    const Color text = RGB(0, 255, 0);
    
    gui_fill_rect(x, y, 16, 16, border);
    gui_fill_rect(x + 1, y + 1, 14, 3, title_bar);
    gui_fill_rect(x + 1, y + 4, 14, 11, bg);
    
    gui_put_pixel(x + 3, y + 6, text);
    gui_put_pixel(x + 4, y + 6, text);
    gui_put_pixel(x + 4, y + 7, text);
    gui_put_pixel(x + 5, y + 7, text);
    gui_put_pixel(x + 4, y + 8, text);
    gui_put_pixel(x + 3, y + 8, text);
    
    gui_fill_rect(x + 7, y + 7, 2, 3, text);
    
    gui_put_pixel(x + 10, y + 7, text);
    gui_put_pixel(x + 12, y + 7, text);
}