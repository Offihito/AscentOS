// keyboard_unified.c - Unified Keyboard Driver for Text and GUI modes
// Supports both text mode terminal and GUI mode (without terminal window)

#include <stdint.h>
#include <stddef.h>

// Conditional includes based on build mode
#ifndef GUI_MODE
    #include "../apps/commands64.h"
    #include "../apps/nano64.h"
#endif

// ============================================================================
// I/O PORT OPERATIONS
// ============================================================================

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

// ============================================================================
// IDT STRUCTURES (Text mode only)
// ============================================================================

#ifndef GUI_MODE
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

extern void isr_keyboard(void);
extern void load_idt64(struct idt_ptr* ptr);
#endif

// ============================================================================
// KEYBOARD STATE
// ============================================================================

static int shift_pressed = 0;
static int caps_lock = 0;

#ifndef GUI_MODE
static char input_buffer[256];
static int buffer_pos = 0;
static int ctrl_pressed = 0;
static int extended_key = 0;

// ── Userland klavye ring buffer ───────────────
// Userland task çalışıyorken tuşlar buraya gelir,
// sys_read() buradan okur.
#define KB_RING_SIZE 256
static volatile char     kb_ring[KB_RING_SIZE];
static volatile int      kb_ring_head = 0;  // yazma
static volatile int      kb_ring_tail = 0;  // okuma
static volatile int      kb_userland_mode = 0; // 1=userland aktif

void kb_set_userland_mode(int on) { kb_userland_mode = on; }
int  kb_userland_active(void)     { return kb_userland_mode; }

// Ring buffer'a karakter yaz (IRQ handler'dan çağrılır)
void kb_ring_push(char c) {
    int next = (kb_ring_head + 1) % KB_RING_SIZE;
    if (next != kb_ring_tail) {
        kb_ring[kb_ring_head] = c;
        kb_ring_head = next;
    }

}

// Ring buffer'dan karakter oku (sys_read'den çağrılır)
// Yoksa -1 döner
int kb_ring_pop(void) {
    if (kb_ring_head == kb_ring_tail) return -1;
    char c = kb_ring[kb_ring_tail];
    kb_ring_tail = (kb_ring_tail + 1) % KB_RING_SIZE;
    return (unsigned char)c;
}
#endif

// ============================================================================
// SCANCODE TO ASCII CONVERSION TABLES
// ============================================================================

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

// ============================================================================
// SCANCODE TO CHARACTER CONVERSION
// ============================================================================

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

// ============================================================================
// TEXT MODE FUNCTIONS
// ============================================================================

#ifndef GUI_MODE

// VGA color codes
#define VGA_WHITE  0x0F
#define VGA_GREEN  0x0A
#define VGA_CYAN   0x0B
#define VGA_YELLOW 0x0E

// External functions from kernel
extern void putchar64(char c, uint8_t color);
extern void print_str64(const char* str, uint8_t color);
extern void println64(const char* str, uint8_t color);
extern void set_position64(size_t row, size_t col);
extern void clear_screen64(void);

// Show command prompt (simplified without user info)
void show_prompt64(void) {
    print_str64("AscentOS", VGA_CYAN);
    print_str64("$ ", VGA_GREEN);
}

// Process command in text mode
void process_command64(const char* cmd) {
    if (cmd[0] == '\0') {
        println64("", VGA_WHITE);
        show_prompt64();
        return;
    }
    
    putchar64('\n', VGA_WHITE);
    
    // Special command: clear
    if (cmd[0] == 'c' && cmd[1] == 'l' && cmd[2] == 'e' && cmd[3] == 'a' && 
        cmd[4] == 'r' && cmd[5] == '\0') {
        clear_screen64();
        println64("AscentOS 64-bit ready!", VGA_GREEN);
        println64("", VGA_WHITE);
        show_prompt64();
        return;
    }
    
    // Special command: reboot
    if (cmd[0] == 'r' && cmd[1] == 'e' && cmd[2] == 'b' && cmd[3] == 'o' && 
        cmd[4] == 'o' && cmd[5] == 't' && cmd[6] == '\0') {
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
    
    // Execute normal commands
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
    
    // Enable IRQ0 (timer) and IRQ1 (keyboard)
    // 0xFC = 11111100 binary (bits 0 and 1 are 0 = enabled)
    outb(0x21, 0xFC);
    outb(0xA1, 0xFF);
}

// Initialize interrupts
void init_interrupts64(void) {
    for (int i = 0; i < 256; i++) {
        set_idt_entry(i, 0, 0, 0, 0);
    }
    
    // External timer interrupt handler
    extern void isr_timer(void);
    
    // Timer interrupt (IRQ0 -> INT 0x20 = 32)
    set_idt_entry(0x20, (uint64_t)isr_timer, 0x08, 0x8E, 0);
    
    // Keyboard interrupt (IRQ1 -> INT 0x21 = 33)
    set_idt_entry(0x21, (uint64_t)isr_keyboard, 0x08, 0x8E, 0);
    
    remap_pic();
    
    idtr.limit = sizeof(idt) - 1;
    idtr.base = (uint64_t)&idt;
    load_idt64(&idtr);
    
    // Configure PIT (Programmable Interval Timer) for 1000 Hz
    uint32_t divisor = 1193182 / 1000;  // 1000 Hz = 1ms per tick
    outb(0x43, 0x36);  // Channel 0, mode 3 (square wave)
    outb(0x40, divisor & 0xFF);
    outb(0x40, (divisor >> 8) & 0xFF);
    
    __asm__ volatile ("sti");
}

#endif

// ============================================================================
// KEYBOARD INTERRUPT HANDLER
// ============================================================================

void keyboard_handler64(void) {
    uint8_t scancode = inb(0x60);
    
#ifdef GUI_MODE
    // ========================================================================
    // GUI MODE HANDLER - Simple key handling without terminal
    // ========================================================================
    extern volatile int gui_request_new_window;
    
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
    
    // Convert to ASCII (for future use - maybe log to serial)
    char c = scancode_to_char(scancode);
    
    if (c == 'o' || c == 'O') {
        gui_request_new_window = 1;
    }
    
    outb(0x20, 0x20);
    
#else
    // ========================================================================
    // TEXT MODE HANDLER
    // ========================================================================
    
    // Handle E0 prefix for extended keys
    if (scancode == 0xE0) {
        extended_key = 1;
        outb(0x20, 0x20);
        return;
    }
    
    // NANO MODE HANDLING
    if (is_nano_mode()) {
        // Handle arrow keys with E0 prefix
        if (extended_key) {
            int handled = 0;
            
            if (scancode == 0x48) {      // Up arrow
                nano_handle_arrow(0x48);
                nano_redraw();
                handled = 1;
            } else if (scancode == 0x50) { // Down arrow
                nano_handle_arrow(0x50);
                nano_redraw();
                handled = 1;
            } else if (scancode == 0x4B) { // Left arrow
                nano_handle_arrow(0x4B);
                nano_redraw();
                handled = 1;
            } else if (scancode == 0x4D) { // Right arrow
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
        
        // Ignore arrow key scancodes without E0 prefix
        if (!extended_key && (scancode == 0x48 || scancode == 0x50 || 
                              scancode == 0x4B || scancode == 0x4D)) {
            outb(0x20, 0x20);
            return;
        }
        
        extended_key = 0;
        
        // Handle Ctrl key
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
        
        // Handle Ctrl+key combinations
        if (ctrl_pressed) {
            int result = NANO_CONTINUE;
            
            if (scancode == 0x1F) {        // Ctrl+S - Save
                result = NANO_SAVE;
            } else if (scancode == 0x10) {  // Ctrl+Q - Quit
                result = NANO_QUIT;
            } else if (scancode == 0x25) {  // Ctrl+K - Kill line
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
            
            // Handle save result
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
            
            // Handle quit result
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
        
        // Handle shift keys in nano mode
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
        
        // Handle caps lock in nano mode
        if (scancode == 0x3A) {
            caps_lock = !caps_lock;
            outb(0x20, 0x20);
            return;
        }
        
        // Skip release codes in nano mode
        if (scancode & 0x80) {
            outb(0x20, 0x20);
            return;
        }
        
        // Handle regular characters in nano mode
        char c = scancode_to_char(scancode);
        if (c) {
            nano_handle_char(c);
            nano_redraw();
        }
        
        outb(0x20, 0x20);
        return;
    }
    
    // NORMAL MODE HANDLING
    if (extended_key) {
        extended_key = 0;
        outb(0x20, 0x20);
        return;
    }
    
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
    
    if (c == 0) {
        outb(0x20, 0x20);
        return;
    }
    
    // Handle Enter key
    if (c == '\n') {
        if (kb_userland_mode) {
            // Userland aktif: newline ring'e gönder (karakterler zaten gönderildi)
            kb_ring_push('\n');
            putchar64('\n', VGA_WHITE);
            outb(0x20, 0x20);
            return;
        }

        input_buffer[buffer_pos] = '\0';
        extern void serial_print(const char* str);
        serial_print("[KEYBOARD] Enter pressed, command: ");
        serial_print(input_buffer);
        serial_print("\n");
        process_command64(input_buffer);
        buffer_pos = 0;
        outb(0x20, 0x20);
        return;
    }

    // Handle Backspace
    if (c == '\b') {
        if (kb_userland_mode) {
            // Userland: sadece ring'e gönder
            // VGA echo'yu calculator/readline halleder (buf boşsa basmaz)
            kb_ring_push('\b');
        } else {
            if (buffer_pos > 0) {
                buffer_pos--;
                putchar64('\b', VGA_WHITE);
            }
        }
        outb(0x20, 0x20);
        return;
    }

    // Add character to buffer
    if (buffer_pos < 255) {
        if (kb_userland_mode) {
            // Userland: direkt ring'e gönder + VGA echo
            kb_ring_push(c);
            putchar64(c, VGA_WHITE);  // kullanıcı yazdığını görsün
        } else {
            input_buffer[buffer_pos++] = c;
            putchar64(c, VGA_WHITE);
        }
    }
    
    outb(0x20, 0x20);
#endif
}

// ============================================================================
// INITIALIZATION
// ============================================================================

void init_keyboard64(void) {
#ifndef GUI_MODE
    buffer_pos = 0;
    ctrl_pressed = 0;
    extended_key = 0;
#endif
    shift_pressed = 0;
    caps_lock = 0;
}