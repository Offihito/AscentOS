// nano64.h - Text editor header
#ifndef NANO64_H
#define NANO64_H

#include <stdint.h>

// Forward declaration to avoid circular dependency
#ifndef MAX_LINE_LENGTH
#define MAX_LINE_LENGTH 128
#endif

#define MAX_EDITOR_LINES 200
#define EDITOR_HEIGHT 21  // Lines visible on screen

// Editor state
typedef struct {
    char lines[MAX_EDITOR_LINES][MAX_LINE_LENGTH];
    int line_count;
    int cursor_x;
    int cursor_y;
    int scroll_offset;
    char filename[64];
    int modified;
} EditorState;

// Return codes
#define NANO_CONTINUE 0
#define NANO_SAVE 1
#define NANO_QUIT 2

// Functions
void nano_init(void);
int nano_load_file(const char* filename);
int nano_save_file(void);
int nano_run(const char* filename);
void nano_handle_arrow(uint8_t scancode);  // Arrow key handler (E0 prefix required)
int nano_handle_key(uint8_t scancode);     // Legacy key handler (for compatibility)
void nano_handle_char(char c);
void nano_update_cursor(void);
void nano_redraw(void);
EditorState* nano_get_state(void);

// Mode check functions
int is_nano_mode(void);
void set_nano_mode(int mode);

#endif // NANO64_H