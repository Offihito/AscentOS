// kernel64.c - AscentOS 64-bit Higher Half Kernel
// Kernel 0xFFFFFFFF80000000 adresinde çalışır

#include <stdint.h>
#include <stddef.h>
#include "memory_unified.h"
#include "vmm64.h"
#include "disk64.h"

// Higher half adresleme
#define KERNEL_VMA   0xFFFFFFFF80000000ULL
#define KERNEL_PHYS  0x100000ULL
#define PHYS_TO_VIRT(addr) ((void*)((uint64_t)(addr) + KERNEL_VMA - KERNEL_PHYS))
#define VIRT_TO_PHYS(addr) ((uint64_t)(addr) - KERNEL_VMA + KERNEL_PHYS)

// ============================================================================
// I/O & Serial
// ============================================================================
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
void serial_putchar(char c) {
    serial_write(c);
}

// ============================================================================
// String utilities
// ============================================================================
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

// ============================================================================
// Yardımcı fonksiyonlar
// ============================================================================
void uint64_to_hex(uint64_t num, char* buf) {
    const char* hex = "0123456789ABCDEF";
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 16; i++)
        buf[2 + i] = hex[(num >> (60 - i * 4)) & 0xF];
    buf[18] = '\0';
}

void get_cpu_info(char* vendor) {
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0));
    *(uint32_t*)(vendor + 0) = ebx;
    *(uint32_t*)(vendor + 4) = edx;
    *(uint32_t*)(vendor + 8) = ecx;
    vendor[12] = '\0';
}

// ============================================================================
// Multiboot2 Memory Map Parse
// Her iki mod da kullandığı için #ifdef dışında tanımlandı
// ============================================================================

extern uint64_t multiboot_mmap_addr;
extern uint32_t multiboot_mmap_entry_size;
extern uint32_t multiboot_mmap_total_size;

typedef struct {
    uint64_t base_addr;
    uint64_t length;
    uint32_t type;       // 1=usable, 2=reserved, 3=ACPI, 4=NVS, 5=bad
    uint32_t reserved;
} __attribute__((packed)) mb2_mmap_entry_t;

#define MAX_MMAP_ENTRIES 64
static struct memory_map_entry parsed_mmap[MAX_MMAP_ENTRIES];

static uint32_t parse_multiboot2_mmap(void) {
    uint32_t entry_size = multiboot_mmap_entry_size;
    uint32_t total_size = multiboot_mmap_total_size;
    uint64_t mmap_addr  = multiboot_mmap_addr;

    if (entry_size == 0 || total_size == 0 || mmap_addr == 0) {
        serial_print("[MMAP] WARNING: No multiboot2 memory map, using fallback!\n");
        return 0;
    }

    uint32_t count = 0, offset = 0;
    while (offset + entry_size <= total_size && count < MAX_MMAP_ENTRIES) {
        mb2_mmap_entry_t* e = (mb2_mmap_entry_t*)(uint64_t)(mmap_addr + offset);
        parsed_mmap[count].base          = (unsigned long)e->base_addr;
        parsed_mmap[count].length        = (unsigned long)e->length;
        parsed_mmap[count].type          = (unsigned int)e->type;
        parsed_mmap[count].acpi_extended = 0;

        char buf[19];
        serial_print("[MMAP] ");
        uint64_to_hex(e->base_addr, buf); serial_print(buf);
        serial_print(" len=");
        uint64_to_hex(e->length, buf);    serial_print(buf);
        serial_print(e->type == 1 ? " USABLE\n"   :
                     e->type == 2 ? " RESERVED\n" :
                     e->type == 3 ? " ACPI\n"     :
                     e->type == 4 ? " NVS\n"      : " OTHER\n");
        count++;
        offset += entry_size;
    }
    return count;
}

static void pmm_init_from_multiboot(void) {
    uint32_t count = parse_multiboot2_mmap();
    if (count > 0) {
        pmm_init(parsed_mmap, count);
    } else {
        struct memory_map_entry fallback[] = {
            {0x000000,   0x09FC00,    1, 0},
            {0x09FC00,   0x000400,    2, 0},
            {0x0F0000,   0x010000,    2, 0},
            {0x100000,   0x1FF00000,  1, 0},
        };
        pmm_init(fallback, 4);
        serial_print("[MMAP] WARNING: PMM using fallback memory map!\n");
    }
}
// ============================================================================
// TEXT MODE
// ============================================================================
#ifdef TEXT_MODE

void init_vga64(void);
void print_str64(const char* str, uint8_t color);
void println64(const char* str, uint8_t color);
void init_interrupts64(void);
void init_keyboard64(void);
void init_memory64(void);
void init_commands64(void);
void show_prompt64(void);
void gdt_install_user_segments(void);
void tss_init(void);
void task_init(void);
void scheduler_init(void);
void syscall_init(void);
extern void process_keyboard_buffer(void);

#define VGA_GREEN       0x02
#define VGA_CYAN        0x03
#define VGA_YELLOW      0x0E
#define VGA_LIGHT_GREEN 0x0A
#define VGA_RED         0x04
#define VGA_MAGENTA     0x05

void kernel_main(uint64_t multiboot_info) {
    (void)multiboot_info; // Assembly tarafı mmap bilgisini global değişkenlere kaydetti

    serial_print("\n=== AscentOS 64-bit Higher Half Kernel ===\n");

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

    print_str64("  OK Higher Half Kernel @ ", VGA_GREEN);
    println64("0xFFFFFFFF80000000", VGA_CYAN);

    init_memory64();
    pmm_init_from_multiboot();
    println64("  OK Physical Memory Manager initialized", VGA_GREEN);

    vmm_init();
    println64("  OK Virtual Memory Manager initialized", VGA_GREEN);

    gdt_install_user_segments();
    println64("  OK GDT: Ring-3 + TSS descriptors installed", VGA_GREEN);

    tss_init();
    println64("  OK TSS initialized, TR loaded", VGA_GREEN);

    task_init();
    println64("  OK Task system initialized", VGA_GREEN);

    scheduler_init();
    println64("  OK Scheduler initialized", VGA_GREEN);

    syscall_init();
    println64("  OK SYSCALL/SYSRET initialized", VGA_GREEN);

    init_interrupts64();
    init_keyboard64();
    init_commands64();

    println64("", VGA_GREEN);
    println64("AscentOS ready! Type 'help' for commands", VGA_LIGHT_GREEN);
    println64("", VGA_GREEN);

    show_prompt64();

    while (1) {
        __asm__ volatile ("sti; hlt");
    }
}

#endif // TEXT_MODE

// ============================================================================
// GUI MODE
// ============================================================================
#ifdef GUI_MODE

#include "gui64.h"
#include "mouse64.h"
#include "taskbar64.h"
#include "compositor64.h"
#include "wm64.h"

void init_interrupts64(void);
void init_commands64(void);
void task_init(void);
void scheduler_init(void);
void syscall_init(void);

bool needs_full_redraw    = false;
volatile int gui_request_new_window = 0;

static Compositor    g_compositor;
static WindowManager g_wm;
static int desktop_layer_idx = -1;
static int taskbar_layer_idx = -1;

static void redraw_all(Color bg_color, Taskbar* tbar, int screen_h) {
    (void)bg_color; (void)screen_h;
    if (desktop_layer_idx >= 0)
        compositor_mark_layer_dirty(&g_compositor, desktop_layer_idx);
    if (taskbar_layer_idx >= 0)
        compositor_mark_layer_dirty(&g_compositor, taskbar_layer_idx);
    compositor_render(&g_compositor);
    taskbar_draw(tbar);
}

void kernel_main(uint64_t multiboot_info) {
    (void)multiboot_info;

    serial_print("\n=== AscentOS 64-bit Higher Half Kernel (GUI) ===\n");

    gui_init();
    serial_print("Graphics mode initialized\n");

    int screen_width  = gui_get_width();
    int screen_height = gui_get_height();

    char cpu_vendor[13];
    get_cpu_info(cpu_vendor);
    serial_print("CPU: "); serial_print(cpu_vendor); serial_print("\n");

    init_interrupts64();
    serial_print("Interrupts initialized\n");

    pmm_init_from_multiboot();
    serial_print("PMM initialized\n");

    vmm_init();
    serial_print("VMM initialized\n");

    task_init();
    serial_print("Task system initialized\n");

    scheduler_init();
    serial_print("Scheduler initialized\n");

    syscall_init();
    serial_print("SYSCALL initialized\n");

    init_mouse64();
    serial_print("Mouse initialized\n");

    Color desktop_color = RGB(44, 44, 44);
    compositor_init(&g_compositor, screen_width, screen_height, desktop_color);
    serial_print("Compositor initialized\n");

    desktop_layer_idx = 0;

    taskbar_layer_idx = compositor_create_layer(&g_compositor, LAYER_TYPE_TASKBAR,
                                                0, screen_height - 40,
                                                screen_width, 40);
    if (taskbar_layer_idx >= 0) {
        Layer* tb_layer = &g_compositor.layers[taskbar_layer_idx];
        for (int i = 0; i < screen_width * 40; i++)
            tb_layer->buffer[i] = RGB(30, 30, 30);
        compositor_mark_layer_dirty(&g_compositor, taskbar_layer_idx);
        serial_print("Taskbar layer created\n");
    }

    compositor_render(&g_compositor);

    Taskbar taskbar;
    taskbar_init(&taskbar, screen_width, screen_height);

    uint8_t hours, minutes, seconds;
    gui_get_rtc_time(&hours, &minutes, &seconds);
    taskbar.current_hours   = hours;
    taskbar.current_minutes = minutes;
    taskbar.current_seconds = seconds;
    taskbar_draw(&taskbar);
    serial_print("Initial GUI draw complete\n");

    wm_init(&g_wm, screen_width, screen_height);
    int test_win_id = wm_create_window(&g_compositor, &g_wm, &taskbar,
                                       120, 80, 400, 300, "Pencere 1");
    (void)test_win_id;
    compositor_render(&g_compositor);
    taskbar_draw(&taskbar);

    // Mouse durumu
    MouseState mouse;
    int  last_mouse_x     = -1, last_mouse_y = -1;
    bool last_left_button = false;
    int  dragging_win_id  = -1;
    int  drag_anchor_mouse_x = 0, drag_anchor_mouse_y = 0;
    int  drag_anchor_win_x   = 0, drag_anchor_win_y   = 0;

    Color cursor_buffer[20 * 18];
    int  prev_x = -100, prev_y = -100;
    bool cursor_needs_restore = false;

    // İlk cursor pozisyonu
    mouse_get_state(&mouse);
    for (int row = 0; row < 20; row++)
        for (int col = 0; col < 18; col++)
            if (gui_is_valid_coord(mouse.x + col, mouse.y + row))
                cursor_buffer[row * 18 + col] = gui_get_pixel(mouse.x + col, mouse.y + row);
    gui_draw_cursor(mouse.x, mouse.y);
    prev_x = mouse.x; prev_y = mouse.y;
    cursor_needs_restore = true;

    uint8_t last_seconds = 0xFF;
    serial_print("Entering main GUI loop\n");

    while (1) {
        mouse_get_state(&mouse);

        // Saat
        gui_get_rtc_time(&hours, &minutes, &seconds);
        if (seconds != last_seconds) {
            uint8_t old_h = taskbar.current_hours;
            uint8_t old_m = taskbar.current_minutes;
            taskbar.current_hours   = hours;
            taskbar.current_minutes = minutes;
            taskbar.current_seconds = seconds;
            last_seconds = seconds;
            taskbar_update_clock_display(&taskbar, old_h != hours || old_m != minutes);
        }

        // Mouse hareketi
        bool mouse_moved = (mouse.x != last_mouse_x || mouse.y != last_mouse_y);
        if (mouse_moved) {
            if (dragging_win_id >= 0) {
                int layer_idx = wm_get_layer_index(&g_wm, dragging_win_id);
                if (layer_idx >= 0) {
                    int new_x = drag_anchor_win_x + (mouse.x - drag_anchor_mouse_x);
                    int new_y = drag_anchor_win_y + (mouse.y - drag_anchor_mouse_y);
                    compositor_move_layer(&g_compositor, layer_idx, new_x, new_y);
                    compositor_render_dirty(&g_compositor);
                    prev_x = -100; prev_y = -100;
                }
            } else if (mouse.y >= screen_height - 40) {
                int old_hovered = taskbar.hovered_button;
                taskbar_handle_mouse_move(&taskbar, mouse.x, mouse.y);
                if (old_hovered != taskbar.hovered_button)
                    taskbar_draw(&taskbar);
            }
            last_mouse_x = mouse.x;
            last_mouse_y = mouse.y;
        }

        // Sol tık
        if (mouse.left_button && !last_left_button) {
            if (mouse.y >= screen_height - 40) {
                int clicked_id = taskbar_handle_mouse_click(&taskbar, mouse.x, mouse.y);
                if (clicked_id >= 0) {
                    wm_restore_window(&g_compositor, &g_wm, clicked_id);
                    needs_full_redraw = true;
                }
            } else {
                int local_x, local_y;
                int win_id = wm_get_window_at(&g_compositor, &g_wm,
                                              mouse.x, mouse.y, &local_x, &local_y);
                if (win_id >= 0) {
                    int layer_idx  = wm_get_layer_index(&g_wm, win_id);
                    Layer* layer   = (layer_idx >= 0) ? &g_compositor.layers[layer_idx] : NULL;
                    WMHitResult hit = layer
                        ? wm_hit_test(layer->bounds.width, layer->bounds.height, local_x, local_y)
                        : WMHIT_NONE;
                    if (hit == WMHIT_TITLE) {
                        dragging_win_id     = win_id;
                        drag_anchor_mouse_x = mouse.x;
                        drag_anchor_mouse_y = mouse.y;
                        drag_anchor_win_x   = layer->bounds.x;
                        drag_anchor_win_y   = layer->bounds.y;
                    } else {
                        wm_handle_click(&g_compositor, &g_wm, &taskbar, win_id, local_x, local_y);
                        needs_full_redraw = true;
                    }
                }
            }
        }

        if (!mouse.left_button && last_left_button)
            dragging_win_id = -1;

        // Yeni pencere isteği (klavyeden)
        if (gui_request_new_window) {
            gui_request_new_window = 0;
            int n = g_wm.count + 1;
            char title[32];
            int idx = 0;
            title[idx++] = 'P'; title[idx++] = 'e'; title[idx++] = 'n';
            title[idx++] = 'c'; title[idx++] = 'e'; title[idx++] = 'r';
            title[idx++] = 'e'; title[idx++] = ' ';
            if (n >= 10) title[idx++] = '0' + (n / 10);
            title[idx++] = '0' + (n % 10);
            title[idx] = '\0';
            int x = 120 + (g_wm.count * 30) % 180;
            int y = 80  + (g_wm.count * 28) % 120;
            if (wm_create_window(&g_compositor, &g_wm, &taskbar, x, y, 400, 300, title) >= 0)
                needs_full_redraw = true;
        }

        last_left_button = mouse.left_button;

        // Tam yeniden çizim
        if (needs_full_redraw) {
            if (cursor_needs_restore && prev_x >= 0)
                for (int row = 0; row < 20; row++)
                    for (int col = 0; col < 18; col++)
                        if (gui_is_valid_coord(prev_x + col, prev_y + row))
                            gui_put_pixel(prev_x + col, prev_y + row,
                                          cursor_buffer[row * 18 + col]);
            redraw_all(RGB(44, 44, 44), &taskbar, screen_height);
            needs_full_redraw    = false;
            prev_x = -100; prev_y = -100;
            cursor_needs_restore = false;
        }

        // Cursor güncelle
        if (mouse.x != prev_x || mouse.y != prev_y) {
            if (cursor_needs_restore && prev_x >= 0)
                for (int row = 0; row < 20; row++)
                    for (int col = 0; col < 18; col++)
                        if (gui_is_valid_coord(prev_x + col, prev_y + row))
                            gui_put_pixel(prev_x + col, prev_y + row,
                                          cursor_buffer[row * 18 + col]);
            for (int row = 0; row < 20; row++)
                for (int col = 0; col < 18; col++)
                    if (gui_is_valid_coord(mouse.x + col, mouse.y + row))
                        cursor_buffer[row * 18 + col] =
                            gui_get_pixel(mouse.x + col, mouse.y + row);
            gui_draw_cursor(mouse.x, mouse.y);
            prev_x = mouse.x; prev_y = mouse.y;
            cursor_needs_restore = true;
        }

        __asm__ volatile ("pause");
    }
}

#endif // GUI_MODE

#if !defined(TEXT_MODE) && !defined(GUI_MODE)
#error "Either TEXT_MODE or GUI_MODE must be defined!"
#endif
#if defined(TEXT_MODE) && defined(GUI_MODE)
#error "Both modes cannot be defined simultaneously!"
#endif