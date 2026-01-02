// kernel64.c - AscentOS 64-bit Kernel with User-based Prompt
#include <stdint.h>
#include <stddef.h>

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

#include "accounts64.h"

void init_vga64(void);
void print_str64(const char* str, uint8_t color);
void println64(const char* str, uint8_t color);
void init_interrupts64(void);
void init_keyboard64(void);
void init_memory64(void);
void init_commands64(void);
void show_prompt64(void);

#define VGA_GREEN 0x02
#define VGA_CYAN 0x03
#define VGA_YELLOW 0x0E
#define VGA_LIGHT_GREEN 0x0A

void kernel_main(uint64_t multiboot_info) {
    (void)multiboot_info;
    serial_print("\n=== AscentOS 64-bit (TEXT) - User Prompt ===\n");
    
    init_vga64();
    println64("╔═══════════════════════════════════════════════════════════════╗", VGA_CYAN);
    println64("║                    ASCENTOS 64-BIT v1.0                       ║", VGA_LIGHT_GREEN);
    println64("║                 Now in 64-bit Long Mode!                      ║", VGA_YELLOW);
    println64("╚═══════════════════════════════════════════════════════════════╝", VGA_CYAN);
    
    char cpu_vendor[13];
    get_cpu_info(cpu_vendor);
    print_str64("  [✓] CPU: ", VGA_GREEN);
    println64(cpu_vendor, VGA_YELLOW);
    
    init_memory64();
    init_interrupts64();
    init_keyboard64();
    
    // Initialize account system
    accounts_init();
    println64("  [✓] Account system initialized", VGA_GREEN);
    println64("", VGA_GREEN);
    
    init_commands64();
    
    println64("AscentOS ready! Type 'help' for commands", VGA_LIGHT_GREEN);
    println64("Default users: root/root, guest/guest", 0x08);
    println64("Use 'login' command to authenticate", 0x08);
    println64("", VGA_GREEN);
    
    // Show initial prompt (guest by default)
    show_prompt64();
    
    while (1) __asm__ volatile ("hlt");
}
#endif

// ============ GUI MODE ============
#ifdef GUI_MODE

#include "gui64.h"
#include "mouse64.h"
#include "taskbar64.h"
#include "terminal64.h"
#include "accounts64.h"
#include "wallpaper64.h"

void init_interrupts64(void);
void keyboard_set_terminal(Terminal* term);

// Desktop icon structure
typedef struct {
    int x, y;
    int width, height;
    char label[32];
} DesktopIcon;

bool needs_full_redraw = false;

// Helper to draw desktop icon
static void draw_desktop_icon(DesktopIcon* icon) {
    wallpaper_draw();
    
    for (int sy = 0; sy < 16; sy++) {
        for (int sx = 0; sx < 16; sx++) {
            Color icon_color = COLOR_BLACK;
            
            if (sx == 0 || sx == 15 || sy == 0 || sy == 15) {
                icon_color = RGB(50, 50, 60);
            }
            else if (sy >= 1 && sy <= 3) {
                icon_color = RGB(30, 30, 35);
            }
            else if (sx >= 1 && sx <= 14 && sy >= 4 && sy <= 14) {
                icon_color = RGB(12, 12, 18);
                
                if ((sy == 6 || sy == 8) && (sx == 3 || sx == 4)) {
                    icon_color = RGB(0, 255, 0);
                }
                if (sy == 7 && (sx >= 3 && sx <= 5)) {
                    icon_color = RGB(0, 255, 0);
                }
                if ((sx == 7 || sx == 8) && (sy >= 7 && sy <= 9)) {
                    icon_color = RGB(0, 255, 0);
                }
                if (sy == 7 && (sx == 10 || sx == 12)) {
                    icon_color = RGB(0, 255, 0);
                }
            }
            
            for (int dy = 0; dy < 3; dy++) {
                for (int dx = 0; dx < 3; dx++) {
                    gui_put_pixel(icon->x + sx * 3 + dx, icon->y + sy * 3 + dy, icon_color);
                }
            }
        }
    }
    
    gui_draw_string(icon->x + 2, icon->y + 52, icon->label, COLOR_WHITE, COLOR_BLACK);
}

// Redraw everything
static void redraw_all(const Color desktop_color, DesktopIcon* icon, 
                      Terminal* term, Taskbar* taskbar, int screen_height) {
    // Draw wallpaper instead of solid color
        wallpaper_draw();
        
        draw_desktop_icon(icon);
        if (term->window.visible) {
            terminal_draw(term);
        }
        taskbar_draw(taskbar);
        }

void kernel_main(uint64_t multiboot_info) {
    (void)multiboot_info;
    serial_print("\n=== AscentOS GUI - User Prompt ===\n");
    
    extern void init_memory_gui(void);
    init_memory_gui();
    
    gui_init();
    wallpaper_init();
    // Initialize account system
    accounts_init();
    serial_print("Account system initialized\n");
    
    init_commands64();
    init_interrupts64();
    init_mouse64();
    
    const int screen_width = gui_get_width();
    const int screen_height = gui_get_height();
    
    Taskbar taskbar;
    taskbar_init(&taskbar, screen_width, screen_height);
    
    const Color desktop_color = RGB(0, 120, 215);
    wallpaper_draw();
    
    Terminal terminal;
    terminal_init(&terminal, 100, 100, 650, 400);
    terminal.window.visible = false;
    
    keyboard_set_terminal(&terminal);
    
    DesktopIcon term_icon;
    term_icon.x = 20;
    term_icon.y = 20;
    term_icon.width = 64;
    term_icon.height = 80;
    
    const char* label = "Terminal";
    int i;
    for (i = 0; label[i] && i < 31; i++) {
        term_icon.label[i] = label[i];
    }
    term_icon.label[i] = '\0';
    
    draw_desktop_icon(&term_icon);
    
    terminal_println(&terminal, "AscentOS Terminal v1.0");
    terminal_println(&terminal, "Default users: root/root, guest/guest");
    terminal_println(&terminal, "Use 'login' to authenticate");
    terminal_println(&terminal, "");
    terminal_show_prompt(&terminal);
    
    // Mouse cursor
    static Color cursor_buffer[18 * 20];
    int prev_x = -100, prev_y = -100;
    bool cursor_needs_restore = false;
    
    uint32_t frame_counter = 0;
    uint8_t last_seconds = 0xFF;
    
    bool needs_full_redraw = false;
    
    taskbar_draw(&taskbar);
    
    serial_print("User-based prompt active\n");
    
    while (1) {
        MouseState mouse;
        mouse_get_state(&mouse);
        
        // Clock update
        frame_counter++;
        if (frame_counter >= 60) {
            frame_counter = 0;
            
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
        }
        
        // Mouse movement
        static int last_mouse_x = 0, last_mouse_y = 0;
        bool mouse_moved = (mouse.x != last_mouse_x || mouse.y != last_mouse_y);
        
        if (mouse_moved) {
            if (terminal.is_dragging || terminal.is_resizing) {
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
                    cursor_needs_restore = false;
                }
                
                int old_x = terminal.window.x;
                int old_y = terminal.window.y;
                int old_width = terminal.window.width;
                int old_height = terminal.window.height;
                
                terminal_handle_mouse_move(&terminal, mouse.x, mouse.y, 
                                          screen_width, screen_height - 40);
                
                if (old_x != terminal.window.x || old_y != terminal.window.y ||
                    old_width != terminal.window.width || old_height != terminal.window.height) {
                    
                    gui_fill_rect(old_x, old_y, 
                                old_width + 6, 
                                old_height + 6, 
                                desktop_color);
                    
                    if ((old_x < term_icon.x + term_icon.width + 10) &&
                        (old_x + old_width > term_icon.x) &&
                        (old_y < term_icon.y + term_icon.height + 10) &&
                        (old_y + old_height > term_icon.y)) {
                        draw_desktop_icon(&term_icon);
                    }
                    
                    if (old_y + old_height + 6 > screen_height - 45) {
                        taskbar_draw(&taskbar);
                    }
                    
                    terminal_draw(&terminal);
                    
                    prev_x = -100;
                    prev_y = -100;
                }
            }
            else if (mouse.y >= screen_height - 40) {
                int old_hovered = taskbar.hovered_button;
                taskbar_handle_mouse_move(&taskbar, mouse.x, mouse.y);
                
                if (old_hovered != taskbar.hovered_button) {
                    taskbar_draw(&taskbar);
                }
            }
            
            last_mouse_x = mouse.x;
            last_mouse_y = mouse.y;
        }
        
        // Mouse buttons
        static bool last_left_button = false;
        
        if (mouse.left_button && !last_left_button) {
            if (terminal.window.visible && 
                terminal_handle_mouse_down(&terminal, mouse.x, mouse.y)) {
                if (terminal.is_dragging) {
                    serial_print("Drag started\n");
                } else if (terminal.is_resizing) {
                    serial_print("Resize started\n");
                }
            }
            else if (mouse.x >= term_icon.x && mouse.x < term_icon.x + term_icon.width &&
                     mouse.y >= term_icon.y && mouse.y < term_icon.y + term_icon.height) {
                terminal.window.visible = !terminal.window.visible;
                needs_full_redraw = true;
            }
            else {
                int clicked_id = taskbar_handle_mouse_click(&taskbar, mouse.x, mouse.y);
                if (clicked_id >= 0) {
                    taskbar_set_focus(&taskbar, clicked_id);
                    needs_full_redraw = true;
                } else if (clicked_id == -2) {
                    taskbar_draw(&taskbar);
                }
            }
        }
        
        if (!mouse.left_button && last_left_button) {
            if (terminal.is_dragging) {
                terminal_handle_mouse_up(&terminal);
                serial_print("Drag ended\n");
                prev_x = -100;
                cursor_needs_restore = false;
            }
            else if (terminal.is_resizing) {
                terminal_handle_mouse_up(&terminal);
                serial_print("Resize ended\n");
                prev_x = -100;
                cursor_needs_restore = false;
            }
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
        
        // Use redraw_all which now uses wallpaper
        redraw_all(desktop_color, &term_icon, &terminal, &taskbar, screen_height);
        needs_full_redraw = false;
        prev_x = -100;
        prev_y = -100;
        cursor_needs_restore = false;
    }
        
        // Mouse cursor
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
            
            if (!terminal.is_dragging && !terminal.is_resizing) {
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