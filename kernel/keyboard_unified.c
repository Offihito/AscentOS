// keyboard_unified.c — Unified Keyboard Driver
// kernel_mode == 0  →  TEXT terminal handler
// kernel_mode == 1  →  GUI handler
// Runtime'da kernel_mode değişkeni yönlendirir, derleme flag'i yok.

#include <stdint.h>
#include <stddef.h>

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

// nano API
#include "../apps/nano64.h"
#include "../apps/commands64.h"

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
// IDT
// ============================================================================
struct idt_entry {
    uint16_t offset_low, selector;
    uint8_t  ist, type_attr;
    uint16_t offset_mid;
    uint32_t offset_high, reserved;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct idt_entry idt[256];
static struct idt_ptr   idtr;

extern void isr_keyboard(void);
extern void isr_timer(void);
extern void isr_mouse(void);
extern void load_idt64(struct idt_ptr* ptr);

// ============================================================================
// Kernel mod (kernel64.c'de tanımlı)
// ============================================================================
extern volatile int kernel_mode;
extern volatile int gui_request_new_window;

// ============================================================================
// Userland ring buffer
// ============================================================================
#define KB_RING_SIZE 256
static volatile char kb_ring[KB_RING_SIZE];
static volatile int  kb_ring_head = 0;
static volatile int  kb_ring_tail = 0;
static volatile int  kb_userland_mode = 0;

void kb_set_userland_mode(int on) { kb_userland_mode = on; }
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
// Klavye durumu
// ============================================================================
static int shift_pressed = 0;
static int caps_lock     = 0;
static int ctrl_pressed  = 0;
static int extended_key  = 0;

static char input_buffer[256];
static int  buffer_pos = 0;

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
    // gfx
    if (cmd[0]=='g'&&cmd[1]=='f'&&cmd[2]=='x'&&cmd[3]=='\0') {
        extern volatile int request_gui_start;
        println64("GUI moduna geciliyor...", VGA_YELLOW);
        println64("  Mouse: sol tik surukle/tikla", VGA_CYAN);
        println64("  Klavye: N = yeni pencere", VGA_CYAN);
        request_gui_start = 1;
        return;
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
// IDT + PIC + Timer kurulumu
// ============================================================================
static void idt_set(int n, uint64_t h, uint16_t sel, uint8_t attr) {
    idt[n].offset_low  = h & 0xFFFF;
    idt[n].selector    = sel;
    idt[n].ist         = 0;
    idt[n].type_attr   = attr;
    idt[n].offset_mid  = (h >> 16) & 0xFFFF;
    idt[n].offset_high = (h >> 32) & 0xFFFFFFFF;
    idt[n].reserved    = 0;
}
static void pic_remap(void) {
    outb(0x20, 0x11); outb(0x21, 0x20); outb(0x21, 0x04); outb(0x21, 0x01);
    outb(0xA0, 0x11); outb(0xA1, 0x28); outb(0xA1, 0x02); outb(0xA1, 0x01);
    outb(0x21, 0xFF); outb(0xA1, 0xFF); // hepsini kapat, sonra açacağız
}
static void irq_enable(uint8_t irq) {
    uint16_t p = (irq < 8) ? 0x21 : 0xA1;
    if (irq >= 8) irq -= 8;
    outb(p, inb(p) & ~(1 << irq));
}

void init_interrupts64(void) {
    for (int i = 0; i < 256; i++) idt_set(i, 0, 0, 0);
    pic_remap();
    idt_set(32, (uint64_t)isr_timer,   0x08, 0x8E); // IRQ0 timer
    idt_set(33, (uint64_t)isr_keyboard, 0x08, 0x8E); // IRQ1 klavye
    idt_set(44, (uint64_t)isr_mouse,   0x08, 0x8E); // IRQ12 mouse
    idtr.limit = sizeof(idt) - 1;
    idtr.base  = (uint64_t)&idt;
    load_idt64(&idtr);
    irq_enable(0);   // timer
    irq_enable(1);   // klavye
    irq_enable(2);   // cascade
    irq_enable(12);  // mouse
    // PIT 1000 Hz
    uint32_t div = 1193182 / 1000;
    outb(0x43, 0x36); outb(0x40, div & 0xFF); outb(0x40, (div >> 8) & 0xFF);
    __asm__ volatile("sti");
    serial_print("[IRQ] IDT + PIC + Timer + KB + Mouse hazir\n");
}

// ============================================================================
// keyboard_handler64 — IRQ1 (isr_keyboard'dan çağrılır)
// ============================================================================
void keyboard_handler64(void) {
    uint8_t sc = inb(0x60);

    // ----------------------------------------------------------------
    // GUI MODU
    // ----------------------------------------------------------------
    if (kernel_mode == 1) {
        if (sc & 0x80) { outb(0x20, 0x20); return; } // release — ignore
        // N tusu scancodu 0x31, shift/caps durumundan bagimsiz direkt kontrol
        if (sc == 0x18) gui_request_new_window = 1; // O tusu
        outb(0x20, 0x20);
        return;
    }

    // ----------------------------------------------------------------
    // TEXT MODU
    // ----------------------------------------------------------------

    // E0 extended prefix (ok tuşları)
    if (sc == 0xE0) { extended_key = 1; outb(0x20, 0x20); return; }

    // Shift
    if (sc == 0x2A || sc == 0x36) { shift_pressed = 1; outb(0x20, 0x20); return; }
    if (sc == 0xAA || sc == 0xB6) { shift_pressed = 0; outb(0x20, 0x20); return; }
    // Caps
    if (sc == 0x3A) { caps_lock = !caps_lock; outb(0x20, 0x20); return; }
    // Ctrl
    if (sc == 0x1D) { ctrl_pressed = 1; outb(0x20, 0x20); return; }
    if (sc == 0x9D) { ctrl_pressed = 0; outb(0x20, 0x20); return; }

    // --- Nano modu ---
    if (is_nano_mode()) {
        // Ok tuşları (E0 prefix gerekli)
        if (extended_key) {
            extended_key = 0;
            if      (sc == 0x48) { nano_handle_arrow(0x48); nano_redraw(); }
            else if (sc == 0x50) { nano_handle_arrow(0x50); nano_redraw(); }
            else if (sc == 0x4B) { nano_handle_arrow(0x4B); nano_redraw(); }
            else if (sc == 0x4D) { nano_handle_arrow(0x4D); nano_redraw(); }
            outb(0x20, 0x20); return;
        }
        // Ok tuşu scancodeları E0 olmadan gelirse ignore
        if (sc == 0x48 || sc == 0x50 || sc == 0x4B || sc == 0x4D) {
            outb(0x20, 0x20); return;
        }
        // Release
        if (sc & 0x80) { outb(0x20, 0x20); return; }

        // Ctrl kombinasyonları — orijinal nano.h'daki NANO_SAVE / NANO_QUIT
        if (ctrl_pressed) {
            int result = NANO_CONTINUE;
            if      (sc == 0x1F) result = NANO_SAVE;  // Ctrl+S
            else if (sc == 0x10) result = NANO_QUIT;  // Ctrl+Q

            if (result == NANO_SAVE) {
                if (nano_save_file()) {
                    set_position64(23, 0);
                    print_str64("[ Dosya kaydedildi! ]                    ", 0x0A);
                    for (volatile int i = 0; i < 15000000; i++);
                } else {
                    set_position64(23, 0);
                    print_str64("[ HATA: Kayit basarisiz! ]               ", 0x0C);
                    for (volatile int i = 0; i < 15000000; i++);
                }
                nano_redraw();
                outb(0x20, 0x20); return;
            }
            if (result == NANO_QUIT) {
                EditorState* state = nano_get_state();
                if (state->modified) {
                    set_position64(23, 0);
                    print_str64("[ Degistirildi! Ctrl+S kaydet, tekrar Q cik ]   ", 0x0E);
                    state->modified = 0;
                    for (volatile int i = 0; i < 20000000; i++);
                    nano_redraw();
                } else {
                    set_nano_mode(0);
                    clear_screen64();
                    println64("nano editorden cikild.", 0x0A);
                    show_prompt64();
                }
                outb(0x20, 0x20); return;
            }
            // Ctrl+K: satir sil
            if (sc == 0x25) {
                EditorState* st = nano_get_state();
                if (st->line_count > 1) {
                    for (int i = st->cursor_y; i < st->line_count - 1; i++)
                        for (int j = 0; j < MAX_LINE_LENGTH; j++)
                            st->lines[i][j] = st->lines[i+1][j];
                    st->line_count--;
                    if (st->cursor_y >= st->line_count) st->cursor_y = st->line_count - 1;
                } else { st->lines[0][0] = '\0'; }
                st->cursor_x = 0; st->modified = 1;
                nano_redraw(); outb(0x20, 0x20); return;
            }
            outb(0x20, 0x20); return;
        }

        // Enter, Backspace, normal karakter
        if (sc == 0x01) { nano_handle_key(27);   nano_redraw(); outb(0x20, 0x20); return; } // ESC
        if (sc == 0x1C) { nano_handle_key('\n');  nano_redraw(); outb(0x20, 0x20); return; }
        if (sc == 0x0E) { nano_handle_key('\b');  nano_redraw(); outb(0x20, 0x20); return; }
        char nc = sc_to_char(sc);
        if (nc) { nano_handle_char(nc); nano_redraw(); }
        outb(0x20, 0x20); return;
    }

    // --- Normal terminal ---
    if (extended_key) {
        extended_key = 0;
        if (sc == 0x48) { scroll_up(3);   outb(0x20, 0x20); return; } // yukari ok
        if (sc == 0x50) { scroll_down(3); outb(0x20, 0x20); return; } // asagi ok
        outb(0x20, 0x20); return;
    }
    if (sc & 0x80) { outb(0x20, 0x20); return; } // release

    // Ctrl+L: temizle
    if (ctrl_pressed && sc == 0x26) { clear_screen64(); show_prompt64(); outb(0x20, 0x20); return; }
    // Ctrl+C: iptal
    if (ctrl_pressed && sc == 0x2E) {
        putchar64('\n', VGA_WHITE); buffer_pos = 0; input_buffer[0] = '\0';
        show_prompt64(); outb(0x20, 0x20); return;
    }

    // Enter
    if (sc == 0x1C) {
        input_buffer[buffer_pos] = '\0';
        char cmd[256];
        int ci = 0;
        while (ci < buffer_pos) { cmd[ci] = input_buffer[ci]; ci++; }
        cmd[ci] = '\0';
        buffer_pos = 0;
        if (kb_userland_mode) kb_ring_push('\n');
        else process_command64(cmd);
        outb(0x20, 0x20); return;
    }
    // Backspace
    if (sc == 0x0E) {
        if (kb_userland_mode) { kb_ring_push('\b'); }
        else if (buffer_pos > 0) { buffer_pos--; putchar64('\b', VGA_WHITE); }
        outb(0x20, 0x20); return;
    }
    // Karakter
    char c = sc_to_char(sc);
    if (c && buffer_pos < 255) {
        if (kb_userland_mode) { kb_ring_push(c); putchar64(c, VGA_WHITE); }
        else { input_buffer[buffer_pos++] = c; putchar64(c, VGA_WHITE); }
    }
    outb(0x20, 0x20);
}

// ============================================================================
// init_keyboard64
// ============================================================================
void init_keyboard64(void) {
    buffer_pos = 0; ctrl_pressed = 0; extended_key = 0;
    shift_pressed = 0; caps_lock = 0;
}