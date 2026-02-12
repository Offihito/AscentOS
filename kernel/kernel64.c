// kernel64_higher_half.c - AscentOS 64-bit Higher Half Kernel
// Kernel 0xFFFFFFFF80000000 adresinde çalışır

#include <stdint.h>
#include <stddef.h>
#include "memory_unified.h"
#include "vmm64.h"

// Higher half kernel base adresi
#define KERNEL_VMA      0xFFFFFFFF80000000ULL
#define KERNEL_PHYS     0x100000ULL

// Fiziksel adresi sanal adrese çevir
#define PHYS_TO_VIRT(addr) ((void*)((uint64_t)(addr) + KERNEL_VMA - KERNEL_PHYS))
// Sanal adresi fiziksel adrese çevir
#define VIRT_TO_PHYS(addr) ((uint64_t)(addr) - KERNEL_VMA + KERNEL_PHYS)

// I/O & Serial
#define COM1 0x3F8
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
void serial_write(char c) {
    while ((inb(COM1 + 5) & 0x20) == 0);
    outb(COM1, c);
}
void serial_print(const char* str) {
    while (*str) {
        if (*str == '\n') serial_write('\r');
        serial_write(*str++);
    }
}

// String Utils
size_t strlen64(const char* str) {
    size_t len = 0;
    while (str[len]) len++;
    return len;
}
int strcmp64(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) { s1++; s2++; }
    return *(const uint8_t*)s1 - *(const uint8_t*)s2;
}
void* memset64(void* dest, int val, size_t n) {
    uint8_t* d = (uint8_t*)dest;
    while (n--) *d++ = (uint8_t)val;
    return dest;
}
void* memcpy64(void* dest, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;
    while (n--) *d++ = *s++;
    return dest;
}

// Number to string helper
void uint64_to_hex(uint64_t num, char* buf) {
    const char* hex = "0123456789ABCDEF";
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 16; i++) {
        buf[2 + i] = hex[(num >> (60 - i * 4)) & 0xF];
    }
    buf[18] = '\0';
}

// CPU Info
void get_cpu_info(char* vendor) {
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0));
    *(uint32_t*)(vendor + 0) = ebx;
    *(uint32_t*)(vendor + 4) = edx;
    *(uint32_t*)(vendor + 8) = ecx;
    vendor[12] = '\0';
}

// ============ TEXT MODE ============
#ifdef TEXT_MODE
void init_vga64(void);
void print_str64(const char* str, uint8_t color);
void println64(const char* str, uint8_t color);
void init_interrupts64(void);
void init_keyboard64(void);
void init_memory64(void);
void init_commands64(void);
void show_prompt64(void);
void task_init(void);
void scheduler_init(void);

#define VGA_GREEN 0x02
#define VGA_CYAN 0x03
#define VGA_YELLOW 0x0E
#define VGA_LIGHT_GREEN 0x0A
#define VGA_RED 0x04
#define VGA_MAGENTA 0x05

void kernel_main(uint64_t multiboot_info) {
    // Multiboot info fiziksel adreste, kullanmadan önce dönüştürülmeli
    (void)multiboot_info;
    
    serial_print("\n");
    serial_print("=== AscentOS 64-bit Higher Half Kernel ===\n");
    
    // Kernel adres bilgilerini göster
    char addr_buf[19];
    
    serial_print("Kernel virtual base: ");
    uint64_to_hex(KERNEL_VMA, addr_buf);
    serial_print(addr_buf);
    serial_print("\n");
    
    serial_print("Kernel physical base: ");
    uint64_to_hex(KERNEL_PHYS, addr_buf);
    serial_print(addr_buf);
    serial_print("\n");
    
    // kernel_main fonksiyonunun adresi (higher half'te olmalı)
    serial_print("kernel_main address: ");
    uint64_to_hex((uint64_t)kernel_main, addr_buf);
    serial_print(addr_buf);
    serial_print("\n\n");
    
    init_vga64();
    println64("===============================================================", VGA_CYAN);
    println64("===         ASCENTOS 64-BIT HIGHER HALF v1.1              ===", VGA_LIGHT_GREEN);
    println64("===         Kernel @ 0xFFFFFFFF80000000                   ===", VGA_YELLOW);
    println64("===            Now in 64-bit Long Mode!                   ===", VGA_MAGENTA);
    println64("===============================================================", VGA_CYAN);
    
    char cpu_vendor[13];
    get_cpu_info(cpu_vendor);
    print_str64("  OK CPU: ", VGA_GREEN);
    println64(cpu_vendor, VGA_YELLOW);
    
    // Adres bilgilerini ekranda da göster
    print_str64("  OK Higher Half Kernel @ ", VGA_GREEN);
    println64("0xFFFFFFFF80000000", VGA_CYAN);
    
    init_memory64();
    
    // Initialize PMM with a simple memory map
    // In a real system, this would come from multiboot info
    struct memory_map_entry memory_map[] = {
        {0x0, 0x9FC00, 1, 0},              // Usable: 0-639KB
        {0x9FC00, 0x400, 2, 0},            // Reserved: 639-640KB
        {0xF0000, 0x10000, 2, 0},          // Reserved: BIOS ROM
        {0x100000, 0x1FF00000, 1, 0},      // Usable: 1MB-512MB
    };
    pmm_init(memory_map, 4);
    println64("  OK Physical Memory Manager initialized", VGA_GREEN);
    
    // Initialize VMM
    vmm_init();
    println64("  OK Virtual Memory Manager initialized", VGA_GREEN);
    
    // Initialize multitasking
    task_init();
    println64("  OK Task system initialized", VGA_GREEN);
    scheduler_init();
    println64("  OK Scheduler initialized", VGA_GREEN);
    
    init_interrupts64();
    init_keyboard64();
    
    println64("", VGA_GREEN);
    
    init_commands64();
    
    println64("AscentOS ready! Type 'help' for commands", VGA_LIGHT_GREEN);
    println64("", VGA_GREEN);
    
    show_prompt64();
    
    while (1) __asm__ volatile ("hlt");
}
#endif

// ============ GUI MODE ============
#ifdef GUI_MODE

#include "gui64.h"
#include "mouse64.h"
#include "taskbar64.h"

void init_interrupts64(void);
void init_commands64(void);
void task_init(void);
void scheduler_init(void);

bool needs_full_redraw = false;

static void redraw_all(Color bg_color, Taskbar* tbar, int screen_h) {
    // Basit arkaplan çiz (desktop_color kullan)
    int screen_w = gui_get_width();
    for (int y = 0; y < screen_h - 40; y++) {
        for (int x = 0; x < screen_w; x++) {
            gui_put_pixel(x, y, bg_color);
        }
    }
    taskbar_draw(tbar);
}

void kernel_main(uint64_t multiboot_info) {
    (void)multiboot_info;
    
    serial_print("\n=== AscentOS 64-bit Higher Half Kernel (GUI) ===\n");
    
    // Kernel adres bilgilerini göster
    char addr_buf[19];
    
    serial_print("Kernel virtual base: ");
    uint64_to_hex(KERNEL_VMA, addr_buf);
    serial_print(addr_buf);
    serial_print("\n");
    
    serial_print("Kernel physical base: ");
    uint64_to_hex(KERNEL_PHYS, addr_buf);
    serial_print(addr_buf);
    serial_print("\n");
    
    serial_print("kernel_main address: ");
    uint64_to_hex((uint64_t)kernel_main, addr_buf);
    serial_print(addr_buf);
    serial_print("\n\n");
    
    // Initialize graphics mode first
    gui_init();
    serial_print("Graphics mode initialized successfully\n");
    
    int screen_width = gui_get_width();
    int screen_height = gui_get_height();
    
    serial_print("Screen resolution: ");
    char width_str[10];
    int w = screen_width;
    int i = 0;
    do {
        width_str[i++] = '0' + (w % 10);
        w /= 10;
    } while (w > 0);
    width_str[i] = '\0';
    // Reverse
    for (int j = 0; j < i/2; j++) {
        char tmp = width_str[j];
        width_str[j] = width_str[i-1-j];
        width_str[i-1-j] = tmp;
    }
    serial_print(width_str);
    serial_print("x");
    
    // Initialize interrupts and keyboard
    init_interrupts64();
    serial_print("\nInterrupts initialized\n");
    
    char cpu_vendor[13];
    get_cpu_info(cpu_vendor);
    serial_print("CPU: ");
    serial_print(cpu_vendor);
    serial_print("\n");
    
    // Initialize PMM
    struct memory_map_entry memory_map[] = {
        {0x0, 0x9FC00, 1, 0},              // Usable: 0-639KB
        {0x9FC00, 0x400, 2, 0},            // Reserved: 639-640KB
        {0xF0000, 0x10000, 2, 0},          // Reserved: BIOS ROM
        {0x100000, 0x1F400000, 1, 0},      // Usable: 1MB-500MB
    };
    pmm_init(memory_map, 4);
    serial_print("PMM initialized\n");
    
    // Initialize VMM
    vmm_init();
    serial_print("VMM initialized\n");
    
    // Initialize multitasking
    task_init();
    serial_print("Task system initialized\n");
    scheduler_init();
    serial_print("Scheduler initialized\n");
    
    // Initialize mouse
    init_mouse64();
    serial_print("Mouse initialized\n");
    
    // Initialize taskbar
    Taskbar taskbar;
    taskbar_init(&taskbar, screen_width, screen_height);
    serial_print("Taskbar initialized\n");
    
    // Desktop color (fallback)
    Color desktop_color = 0x2C2C2C;  // RGB olarak gri renk
    
    // Initial draw - basit arkaplan
    for (int y = 0; y < screen_height - 40; y++) {
        for (int x = 0; x < screen_width; x++) {
            gui_put_pixel(x, y, desktop_color);
        }
    }
    taskbar_draw(&taskbar);
    serial_print("Initial GUI draw complete\n");
    serial_print("Higher Half Kernel GUI initialized successfully!\n");
    
    // Mouse state tracking
    MouseState mouse;
    int last_mouse_x = -1;
    int last_mouse_y = -1;
    bool last_left_button = false;
    
    // Cursor buffer for restoration
    Color cursor_buffer[20 * 18];
    int prev_x = -100;
    int prev_y = -100;
    bool cursor_needs_restore = false;
    
    // Clock tracking
    uint8_t last_seconds = 0xFF;
    
    serial_print("Entering main GUI loop\n");
    
    while (1) {
        // Get current mouse state
        mouse_get_state(&mouse);
        
        // Update clock
        uint8_t hours, minutes, seconds;
        gui_get_rtc_time(&hours, &minutes, &seconds);
        
        if (seconds != last_seconds) {
            uint8_t old_hours = taskbar.current_hours;
            uint8_t old_minutes = taskbar.current_minutes;
            
            taskbar.current_hours = hours;
            taskbar.current_minutes = minutes;
            taskbar.current_seconds = seconds;
            last_seconds = seconds;
            
            bool time_changed = (old_hours != hours || old_minutes != minutes);
            taskbar_update_clock_display(&taskbar, time_changed);
        }
        
        bool mouse_moved = (mouse.x != last_mouse_x || mouse.y != last_mouse_y);
        
        if (mouse_moved) {
            if (mouse.y >= screen_height - 40) {
                int old_hovered = taskbar.hovered_button;
                taskbar_handle_mouse_move(&taskbar, mouse.x, mouse.y);
                
                if (old_hovered != taskbar.hovered_button) {
                    taskbar_draw(&taskbar);
                }
            }
            
            last_mouse_x = mouse.x;
            last_mouse_y = mouse.y;
        }
        
        if (mouse.left_button && !last_left_button) {
            int clicked_id = taskbar_handle_mouse_click(&taskbar, mouse.x, mouse.y);
            (void)clicked_id;
        }
        
        if (!mouse.left_button && last_left_button) {
            // Mouse released
        }
        
        last_left_button = mouse.left_button;
        
        if (needs_full_redraw) {
            if (cursor_needs_restore && prev_x >= 0) {
                for (int row = 0; row < 20; row++) {
                    for (int col = 0; col < 18; col++) {
                        int px = prev_x + col;
                        int py = prev_y + row;
                        if (gui_is_valid_coord(px, py)) {
                            gui_put_pixel(px, py, cursor_buffer[row * 18 + col]);
                        }
                    }
                }
            }
            
            redraw_all(desktop_color, &taskbar, screen_height);
            needs_full_redraw = false;
            prev_x = -100;
            prev_y = -100;
            cursor_needs_restore = false;
        }
        
        if (mouse.x != prev_x || mouse.y != prev_y) {
            if (cursor_needs_restore && prev_x >= 0) {
                for (int row = 0; row < 20; row++) {
                    for (int col = 0; col < 18; col++) {
                        int px = prev_x + col;
                        int py = prev_y + row;
                        if (gui_is_valid_coord(px, py)) {
                            gui_put_pixel(px, py, cursor_buffer[row * 18 + col]);
                        }
                    }
                }
            }
            
            // Always draw cursor
            for (int row = 0; row < 20; row++) {
                for (int col = 0; col < 18; col++) {
                    int px = mouse.x + col;
                    int py = mouse.y + row;
                    if (gui_is_valid_coord(px, py)) {
                        cursor_buffer[row * 18 + col] = gui_get_pixel(px, py);
                    }
                }
            }
            
            gui_draw_cursor(mouse.x, mouse.y);
            prev_x = mouse.x;
            prev_y = mouse.y;
            cursor_needs_restore = true;
        }
        
        __asm__ volatile ("pause;");
    }
}

#endif

#if !defined(TEXT_MODE) && !defined(GUI_MODE)
#error "Either TEXT_MODE or GUI_MODE must be defined!"
#endif
#if defined(TEXT_MODE) && defined(GUI_MODE)
#error "Both modes cannot be defined!"
#endif