// 64-bit Keyboard Driver with User-based Prompt + E0 Prefix Handling

#include "commands64.h"
#include "nano64.h"
#include "accounts64.h"
#include <stddef.h>

// Forward declarations
void putchar64(char c, uint8_t color);
void print_str64(const char* str, uint8_t color);
void println64(const char* str, uint8_t color);
void set_position64(size_t row, size_t col);

// External functions
extern void clear_screen64(void);

// I/O
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

// IDT structures
struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct idt_entry idt[256];
static struct idt_ptr idtr;

// External ISRs
extern void isr_keyboard(void);
extern void load_idt64(struct idt_ptr* ptr);

// Keyboard state
static char input_buffer[256];
static int buffer_pos = 0;
static int shift_pressed = 0;
static int caps_lock = 0;
static int ctrl_pressed = 0;
static int extended_key = 0;

// Scancode to ASCII conversion tables
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

// Show command prompt with user info
void show_prompt64(void) {
    const char* username = accounts_get_current_username();
    UserLevel level = accounts_get_current_level();
    
    // Color based on permission level
    uint8_t prompt_color = VGA_GREEN;
    if (level == USER_LEVEL_ROOT) {
        prompt_color = 0x0C;  // Red for root
    } else if (level == USER_LEVEL_ADMIN) {
        prompt_color = 0x0D;  // Magenta for admin
    } else if (level == USER_LEVEL_USER) {
        prompt_color = VGA_CYAN;  // Cyan for user
    } else {
        prompt_color = 0x08;  // Gray for guest
    }
    
    print_str64(username, prompt_color);
    
    // Prompt symbol based on level
    if (level >= USER_LEVEL_ADMIN) {
        print_str64("# ", 0x0C);  // Red # for admin/root
    } else {
        print_str64("$ ", prompt_color);  // $ for user/guest
    }
}

// Scancode to char conversion
char scancode_to_char(uint8_t scancode) {
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

// IDT entry setup
void set_idt_entry(int num, uint64_t handler, uint16_t selector, 
                   uint8_t type_attr, uint8_t ist) {
    idt[num].offset_low = handler & 0xFFFF;
    idt[num].offset_mid = (handler >> 16) & 0xFFFF;
    idt[num].offset_high = (handler >> 32) & 0xFFFFFFFF;
    idt[num].selector = selector;
    idt[num].ist = ist;
    idt[num].type_attr = type_attr;
    idt[num].reserved = 0;
}

// PIC remapping
void remap_pic(void) {
    outb(0x20, 0x11);
    outb(0x21, 0x20);
    outb(0x21, 0x04);
    outb(0x21, 0x01);
    
    outb(0xA0, 0x11);
    outb(0xA1, 0x28);
    outb(0xA1, 0x02);
    outb(0xA1, 0x01);
    
    outb(0x21, 0xFD);
    outb(0xA1, 0xFF);
}

// Initialize interrupts
void init_interrupts64(void) {
    for (int i = 0; i < 256; i++) {
        set_idt_entry(i, 0, 0, 0, 0);
    }
    
    set_idt_entry(0x21, (uint64_t)isr_keyboard, 0x08, 0x8E, 0);
    
    remap_pic();
    
    idtr.limit = sizeof(idt) - 1;
    idtr.base = (uint64_t)&idt;
    load_idt64(&idtr);
    
    __asm__ volatile ("sti");
}

// Process command
void process_command64(const char* cmd) {
    if (cmd[0] == '\0') {
        println64("", VGA_WHITE);
        show_prompt64();
        return;
    }
    
    putchar64('\n', VGA_WHITE);
    
    // Special commands
    if (cmd[0] == 'c' && cmd[1] == 'l' && cmd[2] == 'e' && cmd[3] == 'a' && cmd[4] == 'r' && cmd[5] == '\0') {
        clear_screen64();
        println64("AscentOS 64-bit ready!", VGA_GREEN);
        println64("", VGA_WHITE);
        show_prompt64();
        return;
    }
    
    if (cmd[0] == 'r' && cmd[1] == 'e' && cmd[2] == 'b' && cmd[3] == 'o' && cmd[4] == 'o' && cmd[5] == 't' && cmd[6] == '\0') {
        println64("Rebooting system...", VGA_YELLOW);
        
        uint8_t good = 0x02;
        while (good & 0x02) {
            good = inb(0x64);
        }
        outb(0x64, 0xFE);
        
        __asm__ volatile ("cli");
        struct idt_ptr null_idt = {0, 0};
        __asm__ volatile ("lidt %0" : : "m"(null_idt));
        __asm__ volatile ("int $0x00");
        
        while(1) {
            __asm__ volatile ("hlt");
        }
    }
    
    // Normal commands
    CommandOutput output;
    int success = execute_command64(cmd, &output);
    
    if (success) {
        for (int i = 0; i < output.line_count; i++) {
            println64(output.lines[i], output.colors[i]);
        }
    }
    
    println64("", VGA_WHITE);
    show_prompt64();
}

// Keyboard interrupt handler
void keyboard_handler64(void) {
    uint8_t scancode = inb(0x60);
    
    if (scancode == 0xE0) {
        extended_key = 1;
        outb(0x20, 0x20);
        return;
    }
    
    // NANO MODE
    if (is_nano_mode()) {
        if (extended_key) {
            int handled = 0;
            
            if (scancode == 0x48) {
                nano_handle_arrow(0x48);
                nano_redraw();
                handled = 1;
            } else if (scancode == 0x50) {
                nano_handle_arrow(0x50);
                nano_redraw();
                handled = 1;
            } else if (scancode == 0x4B) {
                nano_handle_arrow(0x4B);
                nano_redraw();
                handled = 1;
            } else if (scancode == 0x4D) {
                nano_handle_arrow(0x4D);
                nano_redraw();
                handled = 1;
            }
            
            extended_key = 0;
            
            if (handled) {
                outb(0x20, 0x20);
                return;
            }
        }
        
        if (!extended_key && (scancode == 0x48 || scancode == 0x50 || 
                              scancode == 0x4B || scancode == 0x4D)) {
            outb(0x20, 0x20);
            return;
        }
        
        extended_key = 0;
        
        if (scancode == 0x1D) {
            ctrl_pressed = 1;
            outb(0x20, 0x20);
            return;
        }
        if (scancode == 0x9D) {
            ctrl_pressed = 0;
            outb(0x20, 0x20);
            return;
        }
        
        if (ctrl_pressed) {
            int result = NANO_CONTINUE;
            
            if (scancode == 0x1F) {
                result = NANO_SAVE;
            } else if (scancode == 0x10) {
                result = NANO_QUIT;
            } else if (scancode == 0x25) {
                EditorState* state = nano_get_state();
                if (state->line_count > 1) {
                    for (int i = state->cursor_y; i < state->line_count - 1; i++) {
                        for (int j = 0; j < MAX_LINE_LENGTH; j++) {
                            state->lines[i][j] = state->lines[i + 1][j];
                        }
                    }
                    state->line_count--;
                    if (state->cursor_y >= state->line_count) {
                        state->cursor_y = state->line_count - 1;
                    }
                } else {
                    state->lines[0][0] = '\0';
                }
                state->cursor_x = 0;
                state->modified = 1;
                nano_redraw();
                outb(0x20, 0x20);
                return;
            }
            
            if (result == NANO_SAVE) {
                if (nano_save_file()) {
                    set_position64(23, 0);
                    print_str64("[ File saved successfully! Press any key... ]                  ", 0x0A);
                    for (volatile int i = 0; i < 15000000; i++);
                } else {
                    set_position64(23, 0);
                    print_str64("[ ERROR: Could not save file! ]                               ", 0x0C);
                    for (volatile int i = 0; i < 15000000; i++);
                }
                nano_redraw();
                outb(0x20, 0x20);
                return;
            }
            
            if (result == NANO_QUIT) {
                EditorState* state = nano_get_state();
                if (state->modified) {
                    set_position64(23, 0);
                    print_str64("[ Modified! Save (Ctrl+S) or quit again to discard ]          ", 0x0E);
                    state->modified = 0;
                    for (volatile int i = 0; i < 20000000; i++);
                    nano_redraw();
                } else {
                    set_nano_mode(0);
                    clear_screen64();
                    println64("", VGA_WHITE);
                    println64("Exited nano editor", 0x0A);
                    println64("", VGA_WHITE);
                    show_prompt64();
                }
                outb(0x20, 0x20);
                return;
            }
        }
        
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
        
        if (scancode == 0x3A) {
            caps_lock = !caps_lock;
            outb(0x20, 0x20);
            return;
        }
        
        if (scancode & 0x80) {
            outb(0x20, 0x20);
            return;
        }
        
        char c = scancode_to_char(scancode);
        if (c) {
            nano_handle_char(c);
            nano_redraw();
        }
        
        outb(0x20, 0x20);
        return;
    }
    
    // NORMAL MODE
    if (extended_key) {
        extended_key = 0;
        outb(0x20, 0x20);
        return;
    }
    
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
    
    if (scancode == 0x3A) {
        caps_lock = !caps_lock;
        outb(0x20, 0x20);
        return;
    }
    
    if (scancode & 0x80) {
        outb(0x20, 0x20);
        return;
    }
    
    char c = scancode_to_char(scancode);
    
    if (c == 0) {
        outb(0x20, 0x20);
        return;
    }
    
    if (c == '\n') {
        input_buffer[buffer_pos] = '\0';
        process_command64(input_buffer);
        buffer_pos = 0;
        outb(0x20, 0x20);
        return;
    }
    
    if (c == '\b') {
        if (buffer_pos > 0) {
            buffer_pos--;
            putchar64('\b', VGA_WHITE);
        }
        outb(0x20, 0x20);
        return;
    }
    
    if (buffer_pos < 255) {
        input_buffer[buffer_pos++] = c;
        putchar64(c, VGA_WHITE);
    }
    
    outb(0x20, 0x20);
}

// Initialize keyboard
void init_keyboard64(void) {
    buffer_pos = 0;
    shift_pressed = 0;
    caps_lock = 0;
    ctrl_pressed = 0;
    extended_key = 0;
}