#include "drivers/input/keyboard.h"
#include "drivers/input/scancode.h"
#include "../../cpu/isr.h"
#include "../../io/io.h"
#include "../../console/console.h"
#include "../../sched/sched.h"
#include <stdint.h>
#include <stdbool.h>

const char scancode_to_char[] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b', 
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\r',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 
    0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, 
    '*', 0, ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, KEY_UP, KEY_PGUP, 
    '-', KEY_LEFT, 0, KEY_RIGHT, '+', 0, KEY_DOWN, KEY_PGDN, 0, 0, 0, 0, 0, 0, 0, 0
};

const char scancode_to_char_shift[] = {
    0,  27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b', 
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\r',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 
    0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, 
    '*', 0, ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, KEY_UP, KEY_PGUP, 
    '-', KEY_LEFT, 0, KEY_RIGHT, '+', 0, KEY_DOWN, KEY_PGDN, 0, 0, 0, 0, 0, 0, 0, 0
};

#define KBD_BUFFER_SIZE 256
static char kbd_buffer[KBD_BUFFER_SIZE];
static volatile uint32_t kbd_head = 0;
static volatile uint32_t kbd_tail = 0;

// Scancode event buffer
#define SCANCODE_BUFFER_SIZE 256
static scancode_event_t scancode_buffer[SCANCODE_BUFFER_SIZE];
static volatile uint32_t scancode_head = 0;
static volatile uint32_t scancode_tail = 0;
static volatile bool scancode_mode_enabled = false;

static bool left_shift = false;
static bool right_shift = false;
static bool left_ctrl = false;
static bool right_ctrl = false;
static bool left_alt = false;
static bool right_alt = false;
static bool caps_lock = false;
static bool extended_scancode = false;

wait_queue_t keyboard_wait_queue;

static void ring_buffer_push(char c) {
    uint32_t next = (kbd_head + 1) % KBD_BUFFER_SIZE;
    if (next != kbd_tail) {
        kbd_buffer[kbd_head] = c;
        kbd_head = next;
        wait_queue_wake_all(&keyboard_wait_queue);
    }
}

static void scancode_buffer_push(uint8_t scancode, uint8_t is_extended, uint8_t is_release) {
    uint32_t next = (scancode_head + 1) % SCANCODE_BUFFER_SIZE;
    if (next != scancode_tail) {
        scancode_buffer[scancode_head].scancode = scancode;
        scancode_buffer[scancode_head].is_extended = is_extended;
        scancode_buffer[scancode_head].is_release = is_release;
        scancode_head = next;
        wait_queue_wake_all(&keyboard_wait_queue);
    }
}

void keyboard_push_bytes(const char *bytes, uint32_t len) {
    __asm__ volatile("cli");
    for (uint32_t i = 0; i < len; i++) {
        ring_buffer_push(bytes[i]);
    }
    __asm__ volatile("sti");
}

bool keyboard_has_char(void) {
    return kbd_head != kbd_tail;
}

char keyboard_get_char(void) {

    while (kbd_head == kbd_tail) {
        sched_yield();
    }
    char c = kbd_buffer[kbd_tail];
    kbd_tail = (kbd_tail + 1) % KBD_BUFFER_SIZE;
    return c;
}

// Scancode mode functions
bool keyboard_has_scancode(void) {
    return scancode_head != scancode_tail;
}

bool keyboard_get_scancode(scancode_event_t *event) {
    if (!event) return false;
    if (scancode_head == scancode_tail) return false;
    
    *event = scancode_buffer[scancode_tail];
    scancode_tail = (scancode_tail + 1) % SCANCODE_BUFFER_SIZE;
    return true;
}

void keyboard_set_scancode_mode(bool enabled) {
    __asm__ volatile("cli");
    scancode_mode_enabled = enabled;
    extended_scancode = false;  // Reset state to avoid corrupting next key
    __asm__ volatile("sti");
}

bool keyboard_is_scancode_mode(void) {
    return scancode_mode_enabled;
}

static void keyboard_callback(struct registers *regs) {
    (void)regs;
    uint8_t code = inb(0x60);
    if (code == 0) return;

    bool release = (code & 0x80) != 0;
    uint8_t scancode = code & 0x7F;

    // In scancode mode, capture all raw scancodes
    if (scancode_mode_enabled) {
        if (code == 0xE0) {
            extended_scancode = true;
            return;
        }

        // Push to buffer FIRST before any early returns
        if (extended_scancode) {
            extended_scancode = false;
            scancode_buffer_push(scancode, 1, release ? 1 : 0);
            // Track extended modifiers (right ctrl, right alt)
            if (scancode == 0x1D) { right_ctrl = !release; }
            else if (scancode == 0x38) { right_alt = !release; }
        } else {
            scancode_buffer_push(scancode, 0, release ? 1 : 0);
            // Track non-extended modifiers
            if (scancode == 0x1D) { left_ctrl = !release; }
            else if (scancode == 0x2A) { left_shift = !release; }
            else if (scancode == 0x36) { right_shift = !release; }
        }
        return;
    }

    if (code == 0xE0) {
        extended_scancode = true;
        return;
    }

    if (extended_scancode) {
        extended_scancode = false;

        if (scancode == 0x1D) {
            right_ctrl = !release;
            return;
        }

        if (release) {
            return; // Ignore extended key releases for normal keys
        }

        int c = 0;
        switch (scancode) {
            case 0x48: c = KEY_UP; break;
            case 0x50: c = KEY_DOWN; break;
            case 0x4B: c = KEY_LEFT; break;
            case 0x4D: c = KEY_RIGHT; break;
            case 0x49: c = KEY_PGUP; break;
            case 0x51: c = KEY_PGDN; break;
            case 0x53: c = 0x7F; break; // Delete key
            default: break;
        }
        if (c) {
            if (c == KEY_UP) {
                const char seq[] = {'\x1B','[','A'};
                keyboard_push_bytes(seq, 3);
            } else if (c == KEY_DOWN) {
                const char seq[] = {'\x1B','[','B'};
                keyboard_push_bytes(seq, 3);
            } else if (c == KEY_LEFT) {
                const char seq[] = {'\x1B','[','D'};
                keyboard_push_bytes(seq, 3);
            } else if (c == KEY_RIGHT) {
                const char seq[] = {'\x1B','[','C'};
                keyboard_push_bytes(seq, 3);
            } else if (c == KEY_PGUP) {
                const char seq[] = {'\x1B','[','5','~'};
                keyboard_push_bytes(seq, 4);
            } else if (c == KEY_PGDN) {
                const char seq[] = {'\x1B','[','6','~'};
                keyboard_push_bytes(seq, 4);
            } else if (c == 0x7F) {
                // Delete key - send VT100 escape sequence
                const char seq[] = {'\x1B','[','3','~'};
                keyboard_push_bytes(seq, 4);
            }
        }
        return;
    }

    if (scancode == 0x1D) { left_ctrl = !release; return; }
    if (scancode == 0x2A) { left_shift = !release; return; }
    if (scancode == 0x36) { right_shift = !release; return; }
    if (scancode == 0x38) { left_alt = !release; return; }
    if (scancode == 0x3A && !release) { caps_lock = !caps_lock; return; }

    if (release) return;

    if (scancode < sizeof(scancode_to_char)) {
        bool use_shift = left_shift || right_shift;
        char c;
        
        if (use_shift) {
            c = scancode_to_char_shift[scancode];
        } else {
            c = scancode_to_char[scancode];
        }
        
        if (left_ctrl || right_ctrl) {
            if (c >= 'a' && c <= 'z') {
                c = c - 'a' + 1;
            } else if (c >= 'A' && c <= 'Z') {
                c = c - 'A' + 1;
            }
        } else if (caps_lock) {
            if (c >= 'a' && c <= 'z') c -= 32;
            else if (c >= 'A' && c <= 'Z') c += 32;
        }
        
        if (c != 0) {
            switch ((unsigned char)c) {
                case KEY_UP: {
                    const char seq[] = {'\x1B','[','A'};
                    keyboard_push_bytes(seq, 3);
                    break;
                }
                case KEY_DOWN: {
                    const char seq[] = {'\x1B','[','B'};
                    keyboard_push_bytes(seq, 3);
                    break;
                }
                case KEY_LEFT: {
                    const char seq[] = {'\x1B','[','D'};
                    keyboard_push_bytes(seq, 3);
                    break;
                }
                case KEY_RIGHT: {
                    const char seq[] = {'\x1B','[','C'};
                    keyboard_push_bytes(seq, 3);
                    break;
                }
                case KEY_PGUP: {
                    const char seq[] = {'\x1B','[','5','~'};
                    keyboard_push_bytes(seq, 4);
                    break;
                }
                case KEY_PGDN: {
                    const char seq[] = {'\x1B','[','6','~'};
                    keyboard_push_bytes(seq, 4);
                    break;
                }
                default:
                    ring_buffer_push(c);
                    break;
            }
        }
    }
}

void keyboard_init(void) {
    while (inb(0x64) & 1) {
        inb(0x60);
    }
    wait_queue_init(&keyboard_wait_queue);
    register_interrupt_handler(33, keyboard_callback);
}
