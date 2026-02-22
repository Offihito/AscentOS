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
    serial_print("[MOUSE] Initializing PS/2 Mouse...\n");

    // Başlangıç pozisyonu
    mouse_state.x = gui_get_width() / 2;
    mouse_state.y = gui_get_height() / 2;
    mouse_state.left_button  = false;
    mouse_state.right_button = false;
    mouse_state.middle_button = false;
    mouse_cycle = 0;
    mouse_packet_ready = false;

    // 1) PS/2 controller'ı sıfırla — önce her iki porta gelen veriyi temizle
    for (int i = 0; i < 16; i++) {
        if (inb(PS2_STATUS) & 0x01)
            inb(PS2_DATA);  // gelen veriyi at
    }
    io_wait();

    // 2) Auxiliary device (mouse portu) etkinleştir
    mouse_wait_input();
    outb(PS2_COMMAND, 0xA8);
    io_wait();

    // 3) Controller Configuration Byte oku
    mouse_wait_input();
    outb(PS2_COMMAND, 0x20);
    mouse_wait_output();
    uint8_t config = inb(PS2_DATA);
    serial_print("[MOUSE] PS/2 config before: ");
    // basit hex print (serial_print_hex yoksa manuel)
    char hbuf[4];
    hbuf[0] = "0123456789ABCDEF"[(config >> 4) & 0xF];
    hbuf[1] = "0123456789ABCDEF"[config & 0xF];
    hbuf[2] = '\n'; hbuf[3] = '\0';
    serial_print(hbuf);

    // bit1 = IRQ12 enable, bit4 = keyboard clock, bit5 = mouse clock disable → temizle
    config |= 0x02;    // IRQ12 interrupt enable
    config &= ~0x20;   // mouse clock disable bit'ini temizle (= clock AÇIK)

    // 4) Güncel config'i geri yaz
    mouse_wait_input();
    outb(PS2_COMMAND, 0x60);
    mouse_wait_input();
    outb(PS2_DATA, config);
    io_wait();

    // 5) Mouse reset
    mouse_write(0xFF);
    uint8_t ack = mouse_read();
    serial_print("[MOUSE] Reset ACK: ");
    hbuf[0] = "0123456789ABCDEF"[(ack >> 4) & 0xF]; hbuf[1] = "0123456789ABCDEF"[ack & 0xF];
    hbuf[2] = '\n'; hbuf[3] = '\0';
    serial_print(hbuf);
    // Reset sonrası 0xAA (self-test OK) + device ID (0x00) gelir
    if (ack == 0xFA) {
        mouse_read(); // 0xAA
        mouse_read(); // 0x00 device ID
    }
    io_wait();

    // 6) Default ayarlar
    mouse_write(0xF6);
    mouse_read();  // ACK
    io_wait();

    // 7) Sample rate 80 — daha responsive
    mouse_write(0xF3);
    mouse_read();  // ACK
    mouse_write(80);
    mouse_read();  // ACK
    io_wait();

    // 8) Data reporting etkinleştir
    mouse_write(0xF4);
    mouse_read();  // ACK
    io_wait();

    serial_print("[MOUSE] PS/2 Mouse initialized OK!\n");
}

// İlk birkaç interrupt'ı logla
static int mouse_debug_count = 0;

// Mouse interrupt handler
void mouse_handler64(void) {
    // Status byte'ı kontrol et — veri gerçekten mouse'dan mı?
    // Bit 5 (0x20) = Mouse data (auxiliary device)
    // Bu kontrolü atlıyoruz çünkü IRQ12 zaten mouse'dan gelir,
    // ama status'u okuyarak garanti altına alıyoruz.
    uint8_t data = inb(PS2_DATA);

    if (mouse_debug_count < 9) {
        serial_print("[MOUSE IRQ] cycle=");
        char dbuf[2];
        dbuf[0] = '0' + mouse_cycle;
        dbuf[1] = '\0';
        serial_print(dbuf);
        serial_print(" data=");
        char hbuf[4];
        hbuf[0] = "0123456789ABCDEF"[(data >> 4) & 0xF];
        hbuf[1] = "0123456789ABCDEF"[data & 0xF];
        hbuf[2] = '\n'; hbuf[3] = '\0';
        serial_print(hbuf);
        mouse_debug_count++;
    }

    // 3-byte paket döngüsü
    if (mouse_cycle == 0) {
        // Byte 0: Flags — bit 3 her zaman 1 olmalı
        if ((data & 0x08) == 0) {
            // Desync: bu byte flags değil, yoksay ve sıfırla
            return;
        }
        mouse_packet[0] = data;
        mouse_cycle = 1;
    }
    else if (mouse_cycle == 1) {
        mouse_packet[1] = data;
        mouse_cycle = 2;
    }
    else if (mouse_cycle == 2) {
        mouse_packet[2] = data;
        mouse_cycle = 0;
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