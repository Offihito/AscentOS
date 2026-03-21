// keyboard_unified.c — Unified Keyboard Driver
// kernel_mode == 0  →  TEXT terminal handler
// Runtime'da kernel_mode değişkeni yönlendirir, derleme flag'i yok.

#include <stdint.h>
#include <stddef.h>
#include "../kernel/task.h"
#include "../kernel/idt64.h"
// ============================================================================
// Forward declarations — kullanılmadan önce bildirilir
// ============================================================================
extern void serial_print(const char* s);
extern void scroll_up(size_t lines);
extern void scroll_down(size_t lines);
extern void putchar64(char c, uint8_t color);
extern void print_str64(const char* str, uint8_t color);
extern void println64(const char* str, uint8_t color);
extern void set_position64(size_t row, size_t col);
extern void clear_screen64(void);

// Sinyal altyapısı — Ctrl+C (SIGINT) ve Ctrl+Z (SIGTSTP) için
#include "../kernel/signal64.h"
#include "../commands/commands64.h"

// ============================================================================
// I/O
// ============================================================================
static inline uint8_t inb(uint16_t port) {
    uint8_t ret; __asm__ volatile("inb %1,%0":"=a"(ret):"Nd"(port)); return ret;
}
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0,%1"::"a"(val),"Nd"(port));
}

// ============================================================================
// Kernel mod (kernel64.c'de tanımlı)
// ============================================================================
extern volatile int kernel_mode;
// syscall.c'de tanımlı — Doom raw mod aktifken 1, normal modda 0
extern int kb_raw_mode;

// ============================================================================
// Userland ring buffer
// ============================================================================
// Klavye durumu (kb_set_userland_mode'dan önce tanımlanmalı)
// ============================================================================
static int shift_pressed = 0;
static int caps_lock     = 0;
static int ctrl_pressed  = 0;
static int extended_key  = 0;

static char input_buffer[256];
static int  buffer_pos = 0;

// ============================================================================
#define KB_RING_SIZE 512
static volatile char kb_ring[KB_RING_SIZE];
static volatile int  kb_ring_head = 0;
static volatile int  kb_ring_tail = 0;
static volatile int  kb_userland_mode = 0;
// 1 → Enter '\r' gönderir (kilo/raw-mode uygulamalar)
// 0 → Enter '\n' gönderir (shell, lua, calculator, vb.)
static volatile int  kb_enter_sends_cr = 0;

void kb_set_enter_cr(int cr) { kb_enter_sends_cr = cr; }

void kb_set_userland_mode(int on) {
    kb_userland_mode = on;
    if (on) {
        // Userland task başlıyor: ring buffer ve input_buffer'ı temizle
        kb_ring_head = kb_ring_tail = 0;
        buffer_pos = 0;
        clear_screen64();
    } else {
        // Kernel shell'e dönüş: buffer'ı temizle, stale veri kalmasın
        kb_ring_head = kb_ring_tail = 0;
        buffer_pos = 0;
        kb_enter_sends_cr = 0;  // Kernel shell için her zaman '\n'
    }
}
int  kb_userland_active(void)     { return kb_userland_mode; }

void kb_ring_push(char c) {
    int next = (kb_ring_head + 1) % KB_RING_SIZE;
    if (next != kb_ring_tail) { kb_ring[kb_ring_head] = c; kb_ring_head = next; }
}
int kb_ring_pop(void) {
    if (kb_ring_head == kb_ring_tail) return -1;
    char c = kb_ring[kb_ring_tail];
    kb_ring_tail = (kb_ring_tail + 1) % KB_RING_SIZE;
    return (unsigned char)c;
}

// ============================================================================
// Scancode → ASCII
// ============================================================================
static const char sc_normal[128] = {
    0,   0,  '1','2','3','4','5','6','7','8','9','0','-','=', 0, '\t',
    'q','w','e','r','t','y','u','i','o','p','[',']','\n', 0, 'a','s',
    'd','f','g','h','j','k','l',';','\'','`', 0,'\\','z','x','c','v',
    'b','n','m',',','.','/', 0, '*', 0, ' ',  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0, '7','8','9','-','4','5','6','+','1',
    '2','3','0','.', 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
};
static const char sc_shift[128] = {
    0,   0,  '!','@','#','$','%','^','&','*','(',')','_','+', 0, '\t',
    'Q','W','E','R','T','Y','U','I','O','P','{','}','\n', 0, 'A','S',
    'D','F','G','H','J','K','L',':','"', '~', 0, '|', 'Z','X','C','V',
    'B','N','M','<','>','?', 0, '*', 0, ' ',  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0, '7','8','9','-','4','5','6','+','1',
    '2','3','0','.', 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
};

static char sc_to_char(uint8_t sc) {
    if (sc >= 128) return 0;
    char c = shift_pressed ? sc_shift[sc] : sc_normal[sc];
    if (caps_lock && c >= 'a' && c <= 'z') c = c - 'a' + 'A';
    if (caps_lock && c >= 'A' && c <= 'Z' && shift_pressed) c = c - 'A' + 'a';
    return c;
}

// ============================================================================
// TEXT MODE yardımcı fonksiyonlar
// ============================================================================
#define VGA_WHITE  0x0F
#define VGA_GREEN  0x0A
#define VGA_CYAN   0x0B
#define VGA_YELLOW 0x0E

void show_prompt64(void) {
    print_str64("AscentOS", VGA_CYAN);
    print_str64("$ ", VGA_GREEN);
}

// task_exit() tarafından çağrılır — ekranı temizleyip shell prompt'unu geri getirir.
// foreground task bittiğinde shell'in kullanıcıya geri döndüğünü gösterir.
void shell_restore_prompt(void) {
    // Ekranı temizlemek yerine sadece yeni satır + prompt: kilo çıktıktan sonra
    // terminal durumunu bozmamak için sade tutalım.
    println64("", VGA_WHITE);
    println64("Program Ended", VGA_YELLOW);
    show_prompt64();
}

void process_command64(const char* cmd) {
    if (cmd[0] == '\0') { println64("", VGA_WHITE); show_prompt64(); return; }
    putchar64('\n', VGA_WHITE);

    // clear
    if (cmd[0]=='c'&&cmd[1]=='l'&&cmd[2]=='e'&&cmd[3]=='a'&&cmd[4]=='r'&&cmd[5]=='\0') {
        clear_screen64(); show_prompt64(); return;
    }
    // reboot
    if (cmd[0]=='r'&&cmd[1]=='e'&&cmd[2]=='b'&&cmd[3]=='o'&&cmd[4]=='o'&&cmd[5]=='t'&&cmd[6]=='\0') {
        println64("Rebooting...", VGA_YELLOW);
        uint8_t g = 0x02; while (g & 0x02) g = inb(0x64);
        outb(0x64, 0xFE);
        __asm__ volatile("cli");
        struct idt_ptr nil = {0, 0}; __asm__ volatile("lidt %0"::"m"(nil));
        __asm__ volatile("int $0x00");
        while (1) __asm__ volatile("hlt");
    }
    CommandOutput output;
    if (execute_command64(cmd, &output)) {
        for (int i = 0; i < output.line_count; i++)
            println64(output.lines[i], output.colors[i]);
    }
    println64("", VGA_WHITE);
    show_prompt64();
}

// ============================================================================
// keyboard_handler64 — IRQ1 (isr_keyboard'dan çağrılır)
// ============================================================================
void keyboard_handler64(void) {
    uint8_t sc = inb(0x60);

    // ----------------------------------------------------------------
    // TEXT MODU
    // ----------------------------------------------------------------

    // E0 extended prefix (ok tuşları)
    if (sc == 0xE0) { extended_key = 1; outb(0x20, 0x20); return; }

    // Shift press/release
    if (sc == 0x2A || sc == 0x36) {
        shift_pressed = 1;
        if (kb_userland_mode && kb_raw_mode) kb_ring_push((char)0xb6); // KEY_RSHIFT press
        outb(0x20, 0x20); return;
    }
    if (sc == 0xAA || sc == 0xB6) {
        shift_pressed = 0;
        if (kb_userland_mode && kb_raw_mode) { kb_ring_push((char)0x80); kb_ring_push((char)0xb6); } // KEY_RSHIFT release
        outb(0x20, 0x20); return;
    }
    // Caps
    if (sc == 0x3A) { caps_lock = !caps_lock; outb(0x20, 0x20); return; }
    // Ctrl press/release
    if (sc == 0x1D) {
        ctrl_pressed = 1;
        if (kb_userland_mode && kb_raw_mode) kb_ring_push((char)0x9d); // KEY_RCTRL press
        outb(0x20, 0x20); return;
    }
    if (sc == 0x9D) {
        ctrl_pressed = 0;
        if (kb_userland_mode && kb_raw_mode) { kb_ring_push((char)0x80); kb_ring_push((char)0x9d); } // KEY_RCTRL release
        outb(0x20, 0x20); return;
    }

    // Extended key (ok tuşları)
    if (extended_key) {
        extended_key = 0;
        if (kb_userland_mode) {
            if (kb_raw_mode) {
                // Doom raw mode: custom single-byte key codes + release events
                switch (sc) {
                case 0x48: kb_ring_push((char)0xad); break; // ↑ press
                case 0x50: kb_ring_push((char)0xaf); break; // ↓ press
                case 0x4D: kb_ring_push((char)0xae); break; // → press
                case 0x4B: kb_ring_push((char)0xac); break; // ← press
                case 0x47: kb_ring_push((char)0xac); break; // Home
                case 0x4F: kb_ring_push((char)0xae); break; // End
                case 0x49: kb_ring_push((char)0xad); break; // PgUp
                case 0x51: kb_ring_push((char)0xaf); break; // PgDn
                case 0x53: kb_ring_push((char)0x7f); break; // Del
                // Release events (0x80 | press scancode)
                case 0xC8: kb_ring_push((char)0x80); kb_ring_push((char)0xad); break; // ↑ release
                case 0xD0: kb_ring_push((char)0x80); kb_ring_push((char)0xaf); break; // ↓ release
                case 0xCD: kb_ring_push((char)0x80); kb_ring_push((char)0xae); break; // → release
                case 0xCB: kb_ring_push((char)0x80); kb_ring_push((char)0xac); break; // ← release
                }
            } else {
                // Canonical mode (kilo vb.): VT100 escape sequences
                switch (sc) {
                case 0x48: kb_ring_push('\x1b'); kb_ring_push('['); kb_ring_push('A'); break; // ↑
                case 0x50: kb_ring_push('\x1b'); kb_ring_push('['); kb_ring_push('B'); break; // ↓
                case 0x4D: kb_ring_push('\x1b'); kb_ring_push('['); kb_ring_push('C'); break; // →
                case 0x4B: kb_ring_push('\x1b'); kb_ring_push('['); kb_ring_push('D'); break; // ←
                case 0x47: kb_ring_push('\x1b'); kb_ring_push('['); kb_ring_push('H'); break; // Home
                case 0x4F: kb_ring_push('\x1b'); kb_ring_push('['); kb_ring_push('F'); break; // End
                case 0x53: kb_ring_push('\x1b'); kb_ring_push('['); kb_ring_push('3'); kb_ring_push('~'); break; // Del
                case 0x49: kb_ring_push('\x1b'); kb_ring_push('['); kb_ring_push('5'); kb_ring_push('~'); break; // PgUp
                case 0x51: kb_ring_push('\x1b'); kb_ring_push('['); kb_ring_push('6'); kb_ring_push('~'); break; // PgDn
                }
            }
        } else {
            if (sc == 0x48) { scroll_up(3);   outb(0x20, 0x20); return; }
            if (sc == 0x50) { scroll_down(3); outb(0x20, 0x20); return; }
        }
        outb(0x20, 0x20); return;
    }

    // Key release — Doom modunda release event gönder
    if (sc & 0x80) {
        if (kb_userland_mode && kb_raw_mode) {
            uint8_t press_sc = sc & 0x7F;
            if (press_sc != 0x1D && press_sc != 0x2A && press_sc != 0x36) {
                char c = sc_to_char(press_sc);
                if (c) { kb_ring_push((char)0x80); kb_ring_push(c); }
            }
        }
        outb(0x20, 0x20); return;
    }

    if (ctrl_pressed && sc == 0x26 && !kb_userland_mode) { clear_screen64(); show_prompt64(); outb(0x20, 0x20); return; }

    // Ctrl Combinations
    if (ctrl_pressed) {
        if (kb_userland_mode && !kb_raw_mode) {
            if (sc == 0x2E) { kb_ring_push(3);  outb(0x20, 0x20); return; } // Ctrl+C
            if (sc == 0x2C) { kb_ring_push(26); outb(0x20, 0x20); return; } // Ctrl+Z
            if (sc == 0x20) { kb_ring_push(4);  outb(0x20, 0x20); return; } // Ctrl+D
            char ascii = sc_to_char(sc);
            if (ascii >= 'a' && ascii <= 'z') kb_ring_push(ascii & 0x1F);
            else if (ascii >= 'A' && ascii <= 'Z') kb_ring_push(ascii & 0x1F);
            outb(0x20, 0x20); return;
        }
        if (!kb_userland_mode) {
            if (sc == 0x2E) { extern task_t* task_get_current(void); task_t* fg = task_get_current(); if (fg) signal_send((int)fg->pid, SIGINT); outb(0x20, 0x20); return; }
            if (sc == 0x2C) { extern task_t* task_get_current(void); task_t* fg = task_get_current(); if (fg) signal_send((int)fg->pid, SIGTSTP); outb(0x20, 0x20); return; }
        }
        outb(0x20, 0x20); return;
    }

    // Enter
    if (sc == 0x1C) {
        if (kb_userland_mode) {
            kb_ring_push(kb_enter_sends_cr ? '\r' : '\n');
        } else {
            input_buffer[buffer_pos] = '\0';
            char cmd[256];
            int ci = 0;
            while (ci < buffer_pos) { cmd[ci] = input_buffer[ci]; ci++; }
            cmd[ci] = '\0';
            buffer_pos = 0;
            process_command64(cmd);
        }
        outb(0x20, 0x20); return;
    }
    // ESC
    if (sc == 0x01) {
        if (kb_userland_mode) kb_ring_push((char)27);
        outb(0x20, 0x20); return;
    }
    // Backspace
    if (sc == 0x0E) {
        if (kb_userland_mode) {
            kb_ring_push('\x7f');
        } else if (buffer_pos > 0) { buffer_pos--; putchar64('\b', VGA_WHITE); }
        outb(0x20, 0x20); return;
    }
    // Normal Character
    char c = sc_to_char(sc);
    if (c) {
        if (kb_userland_mode) {
            kb_ring_push(c);
        } else if (buffer_pos < 255) {
            input_buffer[buffer_pos++] = c;
            putchar64(c, VGA_WHITE);
        }
    }
    outb(0x20, 0x20);
}

void init_keyboard64(void) {
    buffer_pos = 0; ctrl_pressed = 0; extended_key = 0;
    shift_pressed = 0; caps_lock = 0;
}