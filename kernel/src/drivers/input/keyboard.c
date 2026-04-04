#include "drivers/input/keyboard.h"
#include "cpu/isr.h"
#include "io/io.h"
#include "console/console.h"
#include <stdint.h>
#include <stdbool.h>

const char scancode_to_char[] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b', 
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 
    0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, 
    '*', 0, ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, KEY_UP, KEY_PGUP, 
    '-', KEY_LEFT, 0, KEY_RIGHT, '+', 0, KEY_DOWN, KEY_PGDN, 0, 0, 0, 0, 0, 0, 0, 0
};

const char scancode_to_char_shift[] = {
    0,  27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b', 
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 
    0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, 
    '*', 0, ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, KEY_UP, KEY_PGUP, 
    '-', KEY_LEFT, 0, KEY_RIGHT, '+', 0, KEY_DOWN, KEY_PGDN, 0, 0, 0, 0, 0, 0, 0, 0
};

#define KBD_BUFFER_SIZE 256
static char kbd_buffer[KBD_BUFFER_SIZE];
static volatile uint32_t kbd_head = 0;
static volatile uint32_t kbd_tail = 0;

static bool left_shift = false;
static bool right_shift = false;
static bool caps_lock = false;

static void ring_buffer_push(char c) {
    uint32_t next = (kbd_head + 1) % KBD_BUFFER_SIZE;
    if (next != kbd_tail) {
        kbd_buffer[kbd_head] = c;
        kbd_head = next;
    }
}

char keyboard_get_char(void) {
    while (kbd_head == kbd_tail) {
        __asm__ volatile ("hlt"); // Yield CPU until interrupt fires
    }
    char c = kbd_buffer[kbd_tail];
    kbd_tail = (kbd_tail + 1) % KBD_BUFFER_SIZE;
    return c;
}

static void keyboard_callback(struct registers *regs) {
    (void)regs;
    uint8_t scancode = inb(0x60);
    if (scancode == 0) return;

    bool release = (scancode & 0x80) != 0;
    scancode &= 0x7F;

    if (scancode == 0x2A) { left_shift = !release; return; }
    if (scancode == 0x36) { right_shift = !release; return; }
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
        
        // Handle Caps Lock (flips cases)
        if (caps_lock) {
            if (c >= 'a' && c <= 'z') c -= 32;
            else if (c >= 'A' && c <= 'Z') c += 32;
        }
        
        if (c != 0) {
            ring_buffer_push(c);
        }
    }
}

void keyboard_init(void) {
    while (inb(0x64) & 1) {
        inb(0x60);
    }
    register_interrupt_handler(33, keyboard_callback);
    console_puts("[OK] PS/2 Keyboard Driver Initialized on IRQ 1.\n");
}
