// nano64.c - Simple text editor for AscentOS (FIXED ARROW KEYS - NO MORE JUMPING)
#include <stdint.h>
#include <stddef.h>
#include "commands64.h"
#include "nano64.h"
#include "../fs/files64.h"

// External functions
extern void clear_screen64(void);
extern void set_position64(size_t row, size_t col);
extern void putchar64(char c, uint8_t color);
extern void print_str64(const char* str, uint8_t color);

// Editor state
static EditorState editor;

// Initialize editor
void nano_init(void) {
    editor.cursor_x = 0;
    editor.cursor_y = 0;
    editor.scroll_offset = 0;
    editor.line_count = 0;
    editor.modified = 0;
    editor.filename[0] = '\0';
    
    for (int i = 0; i < MAX_EDITOR_LINES; i++) {
        editor.lines[i][0] = '\0';
    }
}

// Load file into editor
int nano_load_file(const char* filename) {
    str_cpy(editor.filename, filename);
    
    const EmbeddedFile64* file = fs_get_file64(filename);
    if (!file) {
        // New file
        editor.line_count = 1;
        editor.lines[0][0] = '\0';
        return 1;
    }
    
    // Parse file content into lines
    const char* content = file->content;
    int line_idx = 0;
    int col = 0;
    
    while (*content && line_idx < MAX_EDITOR_LINES) {
        if (*content == '\n') {
            editor.lines[line_idx][col] = '\0';
            line_idx++;
            col = 0;
        } else if (col < MAX_LINE_LENGTH - 1) {
            editor.lines[line_idx][col++] = *content;
        }
        content++;
    }
    
    if (col > 0 || line_idx == 0) {
        editor.lines[line_idx][col] = '\0';
        line_idx++;
    }
    
    editor.line_count = line_idx;
    if (editor.line_count == 0) {
        editor.line_count = 1;
        editor.lines[0][0] = '\0';
    }
    
    return 1;
}

// Save file
int nano_save_file(void) {
    if (editor.filename[0] == '\0') {
        return 0;
    }
    
    // Build content string
    static char content_buffer[MAX_EDITOR_LINES * MAX_LINE_LENGTH];
    content_buffer[0] = '\0';
    
    for (int i = 0; i < editor.line_count; i++) {
        str_concat(content_buffer, editor.lines[i]);
        if (i < editor.line_count - 1) {
            str_concat(content_buffer, "\n");
        }
    }
    
    // Check if file exists, create if not
    const EmbeddedFile64* file = fs_get_file64(editor.filename);
    if (!file) {
        fs_touch_file64(editor.filename);
    }
    
    int success = fs_write_file64(editor.filename, content_buffer);
    if (success) {
        editor.modified = 0;
    }
    
    return success;
}

// Draw status bar
static void draw_status_bar(void) {
    set_position64(EDITOR_HEIGHT, 0);
    
    // Top border
    for (int i = 0; i < 80; i++) {
        putchar64('-', VGA_WHITE);
    }
    
    set_position64(EDITOR_HEIGHT + 1, 0);
    
    char status[128];
    str_cpy(status, " File: ");
    str_concat(status, editor.filename[0] ? editor.filename : "[New File]");
    
    if (editor.modified) {
        str_concat(status, " [Modified]");
    }
    
    print_str64(status, VGA_CYAN);
    
    // Line info
    set_position64(EDITOR_HEIGHT + 1, 55);
    char line_info[32];
    str_cpy(line_info, "Line ");
    char num_str[16];
    int_to_str(editor.cursor_y + 1, num_str);
    str_concat(line_info, num_str);
    str_concat(line_info, "/");
    int_to_str(editor.line_count, num_str);
    str_concat(line_info, num_str);
    print_str64(line_info, VGA_YELLOW);
}

// Draw help bar
static void draw_help_bar(void) {
    set_position64(EDITOR_HEIGHT + 2, 0);
    print_str64(" ^S Save  ^Q Quit  ^K Cut Line  Arrow Keys Move", VGA_GREEN);
}

// Draw editor content
static void nano_draw_screen(void) {
    // Clear screen first to prevent artifacts
    for (int screen_row = 0; screen_row < EDITOR_HEIGHT; screen_row++) {
        set_position64((size_t)screen_row, 0);
        for (int i = 0; i < 80; i++) {
            putchar64(' ', VGA_WHITE);
        }
    }
    
    // Draw visible lines
    for (int screen_row = 0; screen_row < EDITOR_HEIGHT; screen_row++) {
        int file_row = screen_row + editor.scroll_offset;
        
        set_position64((size_t)screen_row, 0);
        
        if (file_row < editor.line_count) {
            // Draw line number
            char line_num[8];
            int_to_str(file_row + 1, line_num);
            
            // Pad line number
            int num_len = str_len(line_num);
            for (int i = 0; i < 4 - num_len; i++) {
                putchar64(' ', VGA_DARK_GRAY);
            }
            print_str64(line_num, VGA_DARK_GRAY);
            putchar64('|', VGA_DARK_GRAY);
            putchar64(' ', VGA_WHITE);
            
            // Draw line content
            const char* line = editor.lines[file_row];
            int line_len = str_len(line);
            for (int i = 0; i < 73; i++) {
                if (i < line_len) {
                    putchar64(line[i], VGA_WHITE);
                } else {
                    putchar64(' ', VGA_WHITE);
                }
            }
        } else {
            // Empty line with tilde
            putchar64('~', VGA_CYAN);
            for (int i = 1; i < 80; i++) {
                putchar64(' ', VGA_WHITE);
            }
        }
    }
    
    draw_status_bar();
    draw_help_bar();
}

// Insert character at cursor
static void nano_insert_char(char c) {
    if (editor.cursor_y >= editor.line_count) {
        return;
    }
    
    char* line = editor.lines[editor.cursor_y];
    int len = str_len(line);
    
    if (len >= MAX_LINE_LENGTH - 1) {
        return;
    }
    
    // Shift characters right
    for (int i = len; i >= editor.cursor_x; i--) {
        line[i + 1] = line[i];
    }
    
    line[editor.cursor_x] = c;
    editor.cursor_x++;
    editor.modified = 1;
}

// Delete character at cursor
static void nano_delete_char(void) {
    if (editor.cursor_y >= editor.line_count) {
        return;
    }
    
    char* line = editor.lines[editor.cursor_y];
    int len = str_len(line);
    
    if (editor.cursor_x == 0) {
        // Join with previous line
        if (editor.cursor_y > 0) {
            char* prev_line = editor.lines[editor.cursor_y - 1];
            int prev_len = str_len(prev_line);
            
            if (prev_len + len < MAX_LINE_LENGTH - 1) {
                str_concat(prev_line, line);
                
                // Shift lines up
                for (int i = editor.cursor_y; i < editor.line_count - 1; i++) {
                    str_cpy(editor.lines[i], editor.lines[i + 1]);
                }
                editor.line_count--;
                editor.cursor_y--;
                editor.cursor_x = prev_len;
                editor.modified = 1;
            }
        }
    } else if (editor.cursor_x > 0) {
        // Delete character before cursor
        for (int i = editor.cursor_x - 1; i < len; i++) {
            line[i] = line[i + 1];
        }
        editor.cursor_x--;
        editor.modified = 1;
    }
}

// Insert new line
static void nano_insert_newline(void) {
    if (editor.line_count >= MAX_EDITOR_LINES) {
        return;
    }
    
    char* current_line = editor.lines[editor.cursor_y];
    
    // Shift lines down
    for (int i = editor.line_count; i > editor.cursor_y + 1; i--) {
        str_cpy(editor.lines[i], editor.lines[i - 1]);
    }
    
    // Split current line
    char* new_line = editor.lines[editor.cursor_y + 1];
    int i = 0;
    for (int j = editor.cursor_x; current_line[j]; j++) {
        new_line[i++] = current_line[j];
    }
    new_line[i] = '\0';
    
    current_line[editor.cursor_x] = '\0';
    
    editor.line_count++;
    editor.cursor_y++;
    editor.cursor_x = 0;
    editor.modified = 1;
}

// Delete current line
static void nano_delete_line(void) {
    if (editor.line_count <= 1) {
        editor.lines[0][0] = '\0';
        editor.cursor_x = 0;
        editor.modified = 1;
        return;
    }
    
    // Shift lines up
    for (int i = editor.cursor_y; i < editor.line_count - 1; i++) {
        str_cpy(editor.lines[i], editor.lines[i + 1]);
    }
    editor.line_count--;
    
    if (editor.cursor_y >= editor.line_count) {
        editor.cursor_y = editor.line_count - 1;
    }
    
    // Adjust cursor x
    int line_len = str_len(editor.lines[editor.cursor_y]);
    if (editor.cursor_x > line_len) {
        editor.cursor_x = line_len;
    }
    
    editor.modified = 1;
}

// Move cursor
static void nano_move_cursor(int dx, int dy) {
    if (dy != 0) {
        int new_y = editor.cursor_y + dy;
        if (new_y >= 0 && new_y < editor.line_count) {
            editor.cursor_y = new_y;
            
            // Adjust x position
            int line_len = str_len(editor.lines[editor.cursor_y]);
            if (editor.cursor_x > line_len) {
                editor.cursor_x = line_len;
            }
            
            // Adjust scroll
            if (editor.cursor_y < editor.scroll_offset) {
                editor.scroll_offset = editor.cursor_y;
            } else if (editor.cursor_y >= editor.scroll_offset + EDITOR_HEIGHT) {
                editor.scroll_offset = editor.cursor_y - EDITOR_HEIGHT + 1;
            }
        }
    }
    
    if (dx != 0) {
        int line_len = str_len(editor.lines[editor.cursor_y]);
        int new_x = editor.cursor_x + dx;
        
        if (new_x >= 0 && new_x <= line_len) {
            editor.cursor_x = new_x;
        } else if (new_x < 0 && editor.cursor_y > 0) {
            // Move to end of previous line
            editor.cursor_y--;
            editor.cursor_x = str_len(editor.lines[editor.cursor_y]);
            
            if (editor.cursor_y < editor.scroll_offset) {
                editor.scroll_offset = editor.cursor_y;
            }
        } else if (new_x > line_len && editor.cursor_y < editor.line_count - 1) {
            // Move to start of next line
            editor.cursor_y++;
            editor.cursor_x = 0;
            
            if (editor.cursor_y >= editor.scroll_offset + EDITOR_HEIGHT) {
                editor.scroll_offset = editor.cursor_y - EDITOR_HEIGHT + 1;
            }
        }
    }
}

// Handle arrow keys ONLY (called from keyboard handler)
void nano_handle_arrow(uint8_t scancode) {
    // Only accept proper arrow key scancodes (with E0 prefix from keyboard handler)
    if (scancode == 0x48) { // Up
        nano_move_cursor(0, -1);
    } else if (scancode == 0x50) { // Down
        nano_move_cursor(0, 1);
    } else if (scancode == 0x4B) { // Left
        nano_move_cursor(-1, 0);
    } else if (scancode == 0x4D) { // Right
        nano_move_cursor(1, 0);
    }
}

// Legacy function for compatibility (if needed elsewhere)
int nano_handle_key(uint8_t scancode) {
    // Arrow keys
    if (scancode == 0x48) { // Up
        nano_move_cursor(0, -1);
        return NANO_CONTINUE;
    }
    if (scancode == 0x50) { // Down
        nano_move_cursor(0, 1);
        return NANO_CONTINUE;
    }
    if (scancode == 0x4B) { // Left
        nano_move_cursor(-1, 0);
        return NANO_CONTINUE;
    }
    if (scancode == 0x4D) { // Right
        nano_move_cursor(1, 0);
        return NANO_CONTINUE;
    }
    
    return NANO_CONTINUE;
}

// Handle character input
void nano_handle_char(char c) {
    if (c == '\n') {
        nano_insert_newline();
    } else if (c == '\b') {
        nano_delete_char();
    } else if (c >= 32 && c <= 126) {
        nano_insert_char(c);
    }
}

// Update cursor position on screen
void nano_update_cursor(void) {
    int screen_y = editor.cursor_y - editor.scroll_offset;
    if (screen_y >= 0 && screen_y < EDITOR_HEIGHT) {
        set_position64((size_t)screen_y, (size_t)(6 + editor.cursor_x));
    }
}

// Main editor loop (called from command)
int nano_run(const char* filename) {
    nano_init();
    
    if (filename && str_len(filename) > 0) {
        nano_load_file(filename);
    } else {
        editor.line_count = 1;
        editor.lines[0][0] = '\0';
    }
    
    // Clear screen completely before drawing
    clear_screen64();
    
    // Small delay to ensure screen is cleared
    for (volatile int i = 0; i < 1000000; i++);
    
    // Draw everything
    nano_draw_screen();
    
    // Update cursor last
    nano_update_cursor();
    
    return 1; // Return to command handler
}

// Get editor state (for external use)
EditorState* nano_get_state(void) {
    return &editor;
}

// Redraw screen (for external refresh)
void nano_redraw(void) {
    nano_draw_screen();
    nano_update_cursor();
}