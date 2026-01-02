// mouse64.c - PS/2 Mouse Driver (Completely Rewritten)
#include <stdint.h>
#include <stdbool.h>
#include "mouse64.h"
#include "gui64.h"

// Port I/O
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// PS/2 portları
#define PS2_DATA    0x60
#define PS2_STATUS  0x64
#define PS2_COMMAND 0x64

// Mouse state
static MouseState mouse_state = {0};
static uint8_t mouse_cycle = 0;
static uint8_t mouse_packet[4];
static bool mouse_packet_ready = false;

// Serial debug
extern void serial_write(char c);
extern void serial_print(const char* str);

// Forward declaration
static void process_mouse_packet(void);

// Basit delay
static void io_wait(void) {
    for (volatile int i = 0; i < 1000; i++);
}

// PS/2 controller bekleme fonksiyonları
static bool mouse_wait_input(void) {
    uint32_t timeout = 100000;
    while (timeout--) {
        if ((inb(PS2_STATUS) & 2) == 0) {
            return true;
        }
    }
    return false;
}

static bool mouse_wait_output(void) {
    uint32_t timeout = 100000;
    while (timeout--) {
        if ((inb(PS2_STATUS) & 1) == 1) {
            return true;
        }
    }
    return false;
}

// Mouse'a komut gönder
static void mouse_write(uint8_t data) {
    mouse_wait_input();
    outb(PS2_COMMAND, 0xD4);
    mouse_wait_input();
    outb(PS2_DATA, data);
}

// Mouse'tan veri oku
static uint8_t mouse_read(void) {
    mouse_wait_output();
    return inb(PS2_DATA);
}

// Mouse'u başlat
void init_mouse64(void) {
    serial_print("Initializing PS/2 Mouse...\n");
    
    // Mouse pozisyonunu ekranın ortasına ayarla
    mouse_state.x = gui_get_width() / 2;
    mouse_state.y = gui_get_height() / 2;
    mouse_state.left_button = false;
    mouse_state.right_button = false;
    mouse_state.middle_button = false;
    
    mouse_cycle = 0;
    mouse_packet_ready = false;
    
    serial_print("Mouse initial position: ");
    serial_print("center of screen\n");
    
    // Auxiliary device'ı etkinleştir (mouse)
    mouse_wait_input();
    outb(PS2_COMMAND, 0xA8);
    io_wait();
    
    // Controller configuration byte'ını oku
    mouse_wait_input();
    outb(PS2_COMMAND, 0x20);
    mouse_wait_output();
    uint8_t status = inb(PS2_DATA);
    
    // IRQ12'yi etkinleştir (bit 1), IRQ1'i koru (bit 0)
    status |= 0x02;   // IRQ12 enable
    status &= ~0x20;  // Clock enable for mouse
    
    // Configuration byte'ı geri yaz
    mouse_wait_input();
    outb(PS2_COMMAND, 0x60);
    mouse_wait_input();
    outb(PS2_DATA, status);
    io_wait();
    
    // Mouse'u default ayarlara al
    mouse_write(0xF6);
    mouse_read();  // ACK bekle
    io_wait();
    
    // Data reporting'i etkinleştir
    mouse_write(0xF4);
    mouse_read();  // ACK bekle
    io_wait();
    
    serial_print("PS/2 Mouse initialized!\n");
}

// Mouse interrupt handler - basitleştirilmiş ve temiz
void mouse_handler64(void) {
    // Veriyi oku
    uint8_t data = inb(PS2_DATA);
    
    // 3-byte paket döngüsü
    if (mouse_cycle == 0) {
        // Byte 0: Flags
        // Bit 3 her zaman 1 olmalı (packet sync)
        if ((data & 0x08) == 0) {
            return;  // Geçersiz paket başlangıcı
        }
        mouse_packet[0] = data;
        mouse_cycle = 1;
    }
    else if (mouse_cycle == 1) {
        // Byte 1: X movement (raw)
        mouse_packet[1] = data;
        mouse_cycle = 2;
    }
    else if (mouse_cycle == 2) {
        // Byte 2: Y movement (raw)
        mouse_packet[2] = data;
        mouse_cycle = 0;  // Paketi tamamladık
        
        // Paketi işle
        process_mouse_packet();
    }
}

// Mouse paketini işle - ayrı fonksiyon, daha temiz
static void process_mouse_packet(void) {
    uint8_t flags = mouse_packet[0];
    uint8_t x_raw = mouse_packet[1];
    uint8_t y_raw = mouse_packet[2];
    
    // Tuş durumları
    mouse_state.left_button = (flags & 0x01) != 0;
    mouse_state.right_button = (flags & 0x02) != 0;
    mouse_state.middle_button = (flags & 0x04) != 0;
    
    // Overflow kontrolü - overflow varsa hareketi yoksay
    if (flags & 0x40) return;  // X overflow
    if (flags & 0x80) return;  // Y overflow
    
    // X hareketi hesapla
    int x_movement = (int)x_raw;
    if (flags & 0x10) {
        // X negatif (9-bit sign extension)
        x_movement = x_raw | 0xFFFFFF00;
    }
    
    // Y hareketi hesapla
    int y_movement = (int)y_raw;
    if (flags & 0x20) {
        // Y negatif (9-bit sign extension)
        y_movement = y_raw | 0xFFFFFF00;
    }
    
    // Y eksenini ters çevir - PS/2 mouse yukarı=pozitif gönderir
    // Ama ekranda yukarı=0, aşağı=yüksek değer
    y_movement = -y_movement;
    
    // Hassasiyeti ayarla (isteğe bağlı)
    x_movement = (x_movement * 2);  // 2x hız - daha smooth
    y_movement = (y_movement * 2);
    
    // Yeni pozisyon
    mouse_state.x += x_movement;
    mouse_state.y += y_movement;
    
    // Sınırları kontrol et
    int max_x = gui_get_width() - 1;
    int max_y = gui_get_height() - 1;
    
    if (mouse_state.x < 0) mouse_state.x = 0;
    if (mouse_state.x > max_x) mouse_state.x = max_x;
    if (mouse_state.y < 0) mouse_state.y = 0;
    if (mouse_state.y > max_y) mouse_state.y = max_y;
}

// Mouse durumunu al
void mouse_get_state(MouseState* state) {
    state->x = mouse_state.x;
    state->y = mouse_state.y;
    state->left_button = mouse_state.left_button;
    state->right_button = mouse_state.right_button;
    state->middle_button = mouse_state.middle_button;
}