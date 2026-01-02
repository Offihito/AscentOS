// 64-bit VGA Extended Text Mode Driver (132x80)
// Enhanced with scrolling support and better compatibility

#include <stdint.h>
#include <stddef.h>

// Extended text mode - Maximum compatibility size
#define VGA_WIDTH 132
#define VGA_HEIGHT 80
#define VGA_MEMORY 0xB8000

static uint16_t* vga_buffer = (uint16_t*)VGA_MEMORY;
static size_t vga_row = 0;
static size_t vga_col = 0;
static uint8_t vga_color = 0x0F;  // White on black

// Scroll buffer for up/down scrolling
#define SCROLL_BUFFER_SIZE 1000
static uint16_t scroll_buffer[SCROLL_BUFFER_SIZE * VGA_WIDTH];
static size_t scroll_buffer_lines = 0;
static size_t scroll_offset = 0;

// I/O fonksiyonları
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
void refresh_screen(void);
// VGA modunu ayarla (132x80 text mode)
void set_extended_text_mode(void) {
    // Mode 0x54 (132x80) için doğrudan VGA register'larını programlayacağız
    
    // Önce sequencer'ı unlock et
    outb(0x3C4, 0x00);
    outb(0x3C5, 0x01);
    
    // Miscellaneous Output Register - 132 column modu için
    outb(0x3C2, 0x67);
    
    // Sequencer registers
    outb(0x3C4, 0x01);
    outb(0x3C5, 0x01);  // Clocking mode
    
    outb(0x3C4, 0x02);
    outb(0x3C5, 0x0F);  // Map mask
    
    outb(0x3C4, 0x03);
    outb(0x3C5, 0x00);  // Character map select
    
    outb(0x3C4, 0x04);
    outb(0x3C5, 0x06);  // Memory mode
    
    // CRT Controller - unlock
    outb(0x3D4, 0x11);
    uint8_t val = inb(0x3D5) & 0x7F;
    outb(0x3D5, val);
    
    // CRT Controller registers - 132x80 için optimize edilmiş timing
    const uint8_t crtc_regs[][2] = {
        {0x00, 0x9C}, {0x01, 0x83}, {0x02, 0x86}, {0x03, 0x9F},
        {0x04, 0x89}, {0x05, 0x1F}, {0x06, 0x1F}, {0x07, 0x5F},
        {0x08, 0x00}, {0x09, 0x4F}, {0x0A, 0x0D}, {0x0B, 0x0E},
        {0x0C, 0x00}, {0x0D, 0x00}, {0x0E, 0x00}, {0x0F, 0x00},
        {0x10, 0x18}, {0x11, 0x8E}, {0x12, 0x0F}, {0x13, 0x42},
        {0x14, 0x1F}, {0x15, 0x17}, {0x16, 0x1A}, {0x17, 0xA3}
    };
    
    for (int i = 0; i < 24; i++) {
        outb(0x3D4, crtc_regs[i][0]);
        outb(0x3D5, crtc_regs[i][1]);
    }
    
    // Graphics Controller
    const uint8_t gc_regs[][2] = {
        {0x00, 0x00}, {0x01, 0x00}, {0x02, 0x00}, {0x03, 0x00},
        {0x04, 0x00}, {0x05, 0x10}, {0x06, 0x0E}, {0x07, 0x00},
        {0x08, 0xFF}
    };
    
    for (int i = 0; i < 9; i++) {
        outb(0x3CE, gc_regs[i][0]);
        outb(0x3CF, gc_regs[i][1]);
    }
    
    // Attribute Controller
    inb(0x3DA);  // Reset flip-flop
    
    const uint8_t ac_regs[][2] = {
        {0x00, 0x00}, {0x01, 0x01}, {0x02, 0x02}, {0x03, 0x03},
        {0x04, 0x04}, {0x05, 0x05}, {0x06, 0x14}, {0x07, 0x07},
        {0x08, 0x38}, {0x09, 0x39}, {0x0A, 0x3A}, {0x0B, 0x3B},
        {0x0C, 0x3C}, {0x0D, 0x3D}, {0x0E, 0x3E}, {0x0F, 0x3F},
        {0x10, 0x0C}, {0x11, 0x00}, {0x12, 0x0F}, {0x13, 0x08},
        {0x14, 0x00}
    };
    
    for (int i = 0; i < 21; i++) {
        outb(0x3C0, ac_regs[i][0]);
        outb(0x3C0, ac_regs[i][1]);
    }
    
    outb(0x3C0, 0x20);  // Enable display
    
    // Sequencer'ı tekrar lock et
    outb(0x3C4, 0x00);
    outb(0x3C5, 0x03);
}

// Ekranı temizle
void clear_screen64(void) {
    for (size_t i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        vga_buffer[i] = (vga_color << 8) | ' ';
    }
    vga_row = 0;
    vga_col = 0;
    scroll_offset = 0;
}

// Cursor'u güncelle
void update_cursor64(void) {
    uint16_t pos = vga_row * VGA_WIDTH + vga_col;
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}

// Save current line to scroll buffer
static void save_line_to_buffer(size_t line) {
    if (scroll_buffer_lines < SCROLL_BUFFER_SIZE) {
        size_t buffer_line = scroll_buffer_lines;
        for (size_t col = 0; col < VGA_WIDTH; col++) {
            scroll_buffer[buffer_line * VGA_WIDTH + col] = 
                vga_buffer[line * VGA_WIDTH + col];
        }
        scroll_buffer_lines++;
    } else {
        // Shift buffer up and add new line at bottom
        for (size_t i = 0; i < (SCROLL_BUFFER_SIZE - 1) * VGA_WIDTH; i++) {
            scroll_buffer[i] = scroll_buffer[i + VGA_WIDTH];
        }
        size_t buffer_line = SCROLL_BUFFER_SIZE - 1;
        for (size_t col = 0; col < VGA_WIDTH; col++) {
            scroll_buffer[buffer_line * VGA_WIDTH + col] = 
                vga_buffer[line * VGA_WIDTH + col];
        }
    }
}

// Scroll yap
void scroll64(void) {
    // Save top line to buffer before scrolling
    save_line_to_buffer(0);
    
    // Tüm satırları yukarı kaydır
    for (size_t row = 0; row < VGA_HEIGHT - 1; row++) {
        for (size_t col = 0; col < VGA_WIDTH; col++) {
            vga_buffer[row * VGA_WIDTH + col] = 
                vga_buffer[(row + 1) * VGA_WIDTH + col];
        }
    }
    
    // Son satırı temizle
    for (size_t col = 0; col < VGA_WIDTH; col++) {
        vga_buffer[(VGA_HEIGHT - 1) * VGA_WIDTH + col] = 
            (vga_color << 8) | ' ';
    }
    
    vga_row = VGA_HEIGHT - 1;
    vga_col = 0;
}

// Scroll up (view older content)
void scroll_up(size_t lines) {
    if (scroll_offset + lines > scroll_buffer_lines) {
        lines = scroll_buffer_lines - scroll_offset;
    }
    
    if (lines == 0) return;
    
    scroll_offset += lines;
    refresh_screen();
}

// Scroll down (view newer content)
void scroll_down(size_t lines) {
    if (lines > scroll_offset) {
        lines = scroll_offset;
    }
    
    if (lines == 0) return;
    
    scroll_offset -= lines;
    refresh_screen();
}

// Refresh screen from buffer
void refresh_screen(void) {
    if (scroll_offset == 0) {
        // Normal view - nothing to do
        return;
    }
    
    // Display from scroll buffer
    size_t buffer_start = scroll_buffer_lines - scroll_offset;
    size_t lines_from_buffer = (scroll_offset < VGA_HEIGHT) ? scroll_offset : VGA_HEIGHT;
    
    for (size_t row = 0; row < lines_from_buffer; row++) {
        size_t buffer_row = buffer_start + row;
        if (buffer_row < scroll_buffer_lines) {
            for (size_t col = 0; col < VGA_WIDTH; col++) {
                vga_buffer[row * VGA_WIDTH + col] = 
                    scroll_buffer[buffer_row * VGA_WIDTH + col];
            }
        }
    }
}

// Yeni satır
void newline64(void) {
    vga_col = 0;
    vga_row++;
    
    if (vga_row >= VGA_HEIGHT) {
        scroll64();
    }
    
    update_cursor64();
}

// Karakter yaz
void putchar64(char c, uint8_t color) {
    if (c == '\n') {
        newline64();
        return;
    }
    
    if (c == '\t') {
        vga_col = (vga_col + 4) & ~(4 - 1);
        if (vga_col >= VGA_WIDTH) {
            newline64();
        }
        return;
    }
    
    if (c == '\b') {
        if (vga_col > 0) {
            vga_col--;
            vga_buffer[vga_row * VGA_WIDTH + vga_col] = 
                (color << 8) | ' ';
        }
        update_cursor64();
        return;
    }
    
    vga_buffer[vga_row * VGA_WIDTH + vga_col] = (color << 8) | c;
    vga_col++;
    
    if (vga_col >= VGA_WIDTH) {
        newline64();
    }
    
    update_cursor64();
}

// String yaz
void print_str64(const char* str, uint8_t color) {
    while (*str) {
        putchar64(*str, color);
        str++;
    }
}

// String yaz + yeni satır
void println64(const char* str, uint8_t color) {
    print_str64(str, color);
    newline64();
}

// VGA'yı başlat
void init_vga64(void) {
    // Extended text mode'a geç
    set_extended_text_mode();
    
    // Cursor'u göster
    outb(0x3D4, 0x0A);
    outb(0x3D5, (inb(0x3D5) & 0xC0) | 0);
    
    outb(0x3D4, 0x0B);
    outb(0x3D5, (inb(0x3D5) & 0xE0) | 15);
    
    // Initialize scroll buffer
    scroll_buffer_lines = 0;
    scroll_offset = 0;
    
    clear_screen64();
}

// Rengi ayarla
void set_color64(uint8_t fg, uint8_t bg) {
    vga_color = (bg << 4) | (fg & 0x0F);
}

// Pozisyonu ayarla
void set_position64(size_t row, size_t col) {
    if (row < VGA_HEIGHT && col < VGA_WIDTH) {
        vga_row = row;
        vga_col = col;
        update_cursor64();
    }
}

// Mevcut pozisyonu al
void get_position64(size_t* row, size_t* col) {
    if (row) *row = vga_row;
    if (col) *col = vga_col;
}

// Ekran boyutlarını al
void get_screen_size64(size_t* width, size_t* height) {
    if (width) *width = VGA_WIDTH;
    if (height) *height = VGA_HEIGHT;
}

// Get scroll buffer info
void get_scroll_info64(size_t* buffer_lines, size_t* offset) {
    if (buffer_lines) *buffer_lines = scroll_buffer_lines;
    if (offset) *offset = scroll_offset;
}