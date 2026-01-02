// keyboard_gui.c - GUI mode keyboard handler with terminal support
#include <stdint.h>
#include <stddef.h>

// Forward declarations
typedef struct Terminal Terminal;
extern void terminal_handle_key(Terminal* term, char c);
extern void terminal_draw_incremental(Terminal* term);

// Port I/O
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

// Keyboard state
static int shift_pressed = 0;
static int caps_lock = 0;

// ASCII conversion tables
static const char scancode_to_ascii[128] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0,    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0,    '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*', 0, ' '
};

static const char scancode_to_ascii_shift[128] = {
    0,  27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0,    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0,    '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
    '*', 0, ' '
};

// Global terminal pointer (set by kernel)
static Terminal* active_terminal = NULL;

void keyboard_set_terminal(Terminal* term) {
    active_terminal = term;
}

// Convert scancode to character
static char scancode_to_char(uint8_t scancode) {
    if (scancode >= 128) return 0;
    
    char c;
    if (shift_pressed) {
        c = scancode_to_ascii_shift[scancode];
    } else {
        c = scancode_to_ascii[scancode];
        if (caps_lock && c >= 'a' && c <= 'z') {
            c = c - 'a' + 'A';
        }
    }
    return c;
}

// Keyboard interrupt handler
void keyboard_handler64(void) {
    uint8_t scancode = inb(0x60);
    
    // Handle shift keys
    if (scancode == 0x2A || scancode == 0x36) {
        shift_pressed = 1;
        outb(0x20, 0x20);
        return;
    }
    if (scancode == 0xAA || scancode == 0xB6) {
        shift_pressed = 0;
        outb(0x20, 0x20);
        return;
    }
    
    // Handle caps lock
    if (scancode == 0x3A) {
        caps_lock = !caps_lock;
        outb(0x20, 0x20);
        return;
    }
    
    // Skip release codes
    if (scancode & 0x80) {
        outb(0x20, 0x20);
        return;
    }
    
    // Convert to ASCII
    char c = scancode_to_char(scancode);
    
    if (c != 0 && active_terminal) {
        terminal_handle_key(active_terminal, c);
        terminal_draw_incremental(active_terminal);
    }
    
    // EOI to PIC
    outb(0x20, 0x20);
}