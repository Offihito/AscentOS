// ============================================================================
// taskbar64.c - Enhanced Windows 7 Style Taskbar Implementation
// ============================================================================
#include "taskbar64.h"
#include "gui64.h"
#include <stddef.h>

// Windows 7 Aero Renkleri
#define TASKBAR_TOP_COLOR       RGB(35, 35, 40)
#define TASKBAR_BOTTOM_COLOR    RGB(15, 15, 20)
#define TASKBAR_GLASS_OVERLAY   RGB(45, 45, 55)

#define START_ORB_OUTER         RGB(23, 114, 176)
#define START_ORB_INNER         RGB(61, 150, 210)
#define START_ORB_GLOW          RGB(100, 180, 240)
#define START_ORB_HOVER         RGB(80, 165, 225)

#define BUTTON_NORMAL_TOP       RGB(45, 45, 55)
#define BUTTON_NORMAL_BOTTOM    RGB(30, 30, 40)
#define BUTTON_HOVER_TOP        RGB(65, 120, 185)
#define BUTTON_HOVER_BOTTOM     RGB(45, 95, 160)
#define BUTTON_ACTIVE_TOP       RGB(40, 90, 155)
#define BUTTON_ACTIVE_BOTTOM    RGB(25, 70, 135)
#define BUTTON_GLOW             RGB(120, 200, 255)

#define CLOCK_BG                RGB(25, 25, 35)
#define CLOCK_TEXT              RGB(220, 240, 255)

// Yardımcı fonksiyon: string kopyalama
static void str_copy(char* dest, const char* src, size_t max_len) {
    size_t i;
    for (i = 0; i < max_len - 1 && src[i]; i++) {
        dest[i] = src[i];
    }
    dest[i] = '\0';
}

// Gelişmiş gradient çizimi (yumuşak geçişler)
static void draw_gradient_rect(int x, int y, int w, int h, Color top_color, Color bottom_color) {
    uint8_t r1 = GET_RED(top_color), g1 = GET_GREEN(top_color), b1 = GET_BLUE(top_color);
    uint8_t r2 = GET_RED(bottom_color), g2 = GET_GREEN(bottom_color), b2 = GET_BLUE(bottom_color);
    
    for (int py = 0; py < h; py++) {
        float ratio = (float)py / (float)h;
        // Smooth easing (cubic)
        ratio = ratio * ratio * (3.0f - 2.0f * ratio);
        
        uint8_t r = (uint8_t)(r1 + (r2 - r1) * ratio);
        uint8_t g = (uint8_t)(g1 + (g2 - g1) * ratio);
        uint8_t b = (uint8_t)(b1 + (b2 - b1) * ratio);
        Color line_color = RGB(r, g, b);
        
        for (int px = 0; px < w; px++) {
            gui_put_pixel(x + px, y + py, line_color);
        }
    }
}

// Windows 7 cam efekti (glass overlay)
static void draw_glass_overlay(int x, int y, int w, int h) {
    for (int py = 0; py < h / 2; py++) {
        float alpha = 0.3f - (float)py / (float)h * 0.25f;
        Color overlay = TASKBAR_GLASS_OVERLAY;
        
        for (int px = 0; px < w; px++) {
            Color current = gui_get_pixel(x + px, y + py);
            Color blended = gui_blend_colors(overlay, current, (uint8_t)(alpha * 255));
            gui_put_pixel(x + px, y + py, blended);
        }
    }
}

// Parlama efekti (glow)
static void draw_glow(int x, int y, int w, int h, Color glow_color, uint8_t intensity) {
    for (int py = 0; py < h; py++) {
        for (int px = 0; px < w; px++) {
            Color current = gui_get_pixel(x + px, y + py);
            Color blended = gui_blend_colors(glow_color, current, intensity);
            gui_put_pixel(x + px, y + py, blended);
        }
    }
}

// Windows 7 Start Orb çizimi (Siyah küre + Yeşil halka)
static void draw_start_orb(int x, int y, int size, bool hovered, bool pressed) {
    int radius = size / 2;
    
    // Yeşil halka rengi
    Color ring_color = RGB(0, 255, 0);
    if (hovered) ring_color = RGB(100, 255, 100);
    if (pressed) ring_color = RGB(0, 180, 0);
    
    // Tüm pikselleri tara
    for (int py = 0; py < size; py++) {
        for (int px = 0; px < size; px++) {
            int dx = px - radius;
            int dy = py - radius;
            float distance = (float)(dx * dx + dy * dy);
            float dist = distance / 1.0f;
            
            // Halka: 10-14 piksel arası
            if (dist >= 100.0f && dist <= 225.0f) {
                gui_put_pixel(x + px, y + py, ring_color);
            }
            // Siyah küre: 0-9 piksel
            else if (dist <= 81.0f) {
                gui_put_pixel(x + px, y + py, RGB(0, 0, 0));
            }
        }
    }
}

// Windows 7 tarzı buton çizimi
static void draw_taskbar_button(int x, int y, int w, int h, const char* title, 
                                bool hovered, bool active, bool has_icon) {
    // Buton arkaplan gradient
    Color top, bottom;
    if (active) {
        top = BUTTON_ACTIVE_TOP;
        bottom = BUTTON_ACTIVE_BOTTOM;
    } else if (hovered) {
        top = BUTTON_HOVER_TOP;
        bottom = BUTTON_HOVER_BOTTOM;
    } else {
        top = BUTTON_NORMAL_TOP;
        bottom = BUTTON_NORMAL_BOTTOM;
    }
    
    draw_gradient_rect(x, y, w, h, top, bottom);
    
    // Parlama efekti (hover/active)
    if (hovered || active) {
        draw_glow(x + 1, y + 1, w - 2, h / 3, BUTTON_GLOW, hovered ? 40 : 25);
    }
    
    // Çerçeve
    Color border_light = RGB(100, 130, 170);
    Color border_dark = RGB(15, 25, 45);
    
    if (active) {
        border_light = RGB(60, 110, 175);
        border_dark = RGB(25, 55, 105);
    }
    
    // Dış çerçeve
    gui_draw_line(x, y, x + w - 1, y, border_light);
    gui_draw_line(x, y, x, y + h - 1, border_light);
    gui_draw_line(x + w - 1, y, x + w - 1, y + h - 1, border_dark);
    gui_draw_line(x, y + h - 1, x + w - 1, y + h - 1, border_dark);
    
    // Aktif göstergesi (alt çizgi)
    if (active) {
        Color indicator = RGB(120, 200, 255);
        gui_fill_rect(x + 4, y + h - 4, w - 8, 3, indicator);
        
        // Parlama efekti
        for (int i = 0; i < w - 8; i++) {
            float ratio = (float)i / (float)(w - 8);
            if (ratio > 0.5f) ratio = 1.0f - ratio;
            uint8_t alpha = (uint8_t)(ratio * 150);
            Color glow = gui_blend_colors(RGB(180, 220, 255), indicator, alpha);
            gui_put_pixel(x + 4 + i, y + h - 5, glow);
        }
    }
    
    // İkon (basitleştirilmiş pencere ikonu)
    if (has_icon) {
        int icon_x = x + 8;
        int icon_y = y + (h - 16) / 2;
        
        // Pencere şekli
        gui_fill_rect(icon_x, icon_y, 16, 14, RGB(180, 200, 230));
        gui_fill_rect(icon_x, icon_y, 16, 3, RGB(50, 120, 200));
        
        // Kapatma düğmesi
        gui_fill_rect(icon_x + 13, icon_y + 1, 2, 1, RGB(220, 80, 80));
    }
    
    // Metin
    int text_x = has_icon ? x + 30 : x + 8;
    int text_y = y + (h - 8) / 2;
    
    Color text_color = active ? RGB(255, 255, 255) : RGB(220, 230, 240);
    gui_draw_string(text_x, text_y, title, text_color, 0);
}

// Sayıyı 2 basamaklı string'e çevir
static void num_to_str2(uint8_t num, char* buf) {
    buf[0] = '0' + (num / 10);
    buf[1] = '0' + (num % 10);
    buf[2] = '\0';
}

// Sadece saat metnini çiz (yardımcı fonksiyon)
static void taskbar_draw_clock_text(const Taskbar* taskbar, int clock_x, int clock_y, int clock_h) {
    // Saat string'i
    char time_str[16];
    char h_str[3], m_str[3], s_str[3];
    
    num_to_str2(taskbar->current_hours, h_str);
    num_to_str2(taskbar->current_minutes, m_str);
    num_to_str2(taskbar->current_seconds, s_str);
    
    int idx = 0;
    time_str[idx++] = h_str[0];
    time_str[idx++] = h_str[1];
    time_str[idx++] = ':';
    time_str[idx++] = m_str[0];
    time_str[idx++] = m_str[1];
    time_str[idx++] = ':';
    time_str[idx++] = s_str[0];
    time_str[idx++] = s_str[1];
    time_str[idx] = '\0';
    
    // Saat
    int text_x = clock_x + 8;
    int text_y = clock_y + (clock_h - 8) / 2;
    gui_draw_string(text_x, text_y, time_str, CLOCK_TEXT, 0);
    
    // Tarih
    gui_draw_string(text_x + 70, text_y, "27/12/25", RGB(180, 200, 220), 0);
}

// Taskbar başlat
void taskbar_init(Taskbar* taskbar, int screen_width, int screen_height) {
    taskbar->y = screen_height - TASKBAR_HEIGHT;
    taskbar->width = screen_width;
    taskbar->start_button_pressed = false;
    taskbar->start_menu_open = false;
    taskbar->button_count = 0;
    taskbar->hovered_button = -1;
    taskbar->current_hours = 0;
    taskbar->current_minutes = 0;
    taskbar->current_seconds = 0;
    
    for (int i = 0; i < MAX_TASKBAR_BUTTONS; i++) {
        taskbar->buttons[i].active = false;
    }
}

// Saati güncelle
void taskbar_update_time(Taskbar* taskbar) {
    gui_get_rtc_time(&taskbar->current_hours, &taskbar->current_minutes, &taskbar->current_seconds);
}

// Taskbar'ı çiz
void taskbar_draw(const Taskbar* taskbar) {
    // Ana taskbar gradient arkaplan
    draw_gradient_rect(0, taskbar->y, taskbar->width, TASKBAR_HEIGHT, 
                      TASKBAR_TOP_COLOR, TASKBAR_BOTTOM_COLOR);
    
    // Cam efekti (Windows 7 Aero)
    draw_glass_overlay(0, taskbar->y, taskbar->width, TASKBAR_HEIGHT);
    
    // Üst parlak çizgi (Aero glass border)
    for (int x = 0; x < taskbar->width; x++) {
        Color glow = gui_blend_colors(RGB(120, 140, 160), 
                                     gui_get_pixel(x, taskbar->y), 180);
        gui_put_pixel(x, taskbar->y, glow);
    }
    
    // ===== START ORB =====
    int orb_size = 32;
    int orb_x = 6;
    int orb_y = taskbar->y + (TASKBAR_HEIGHT - orb_size) / 2;
    bool orb_hovered = (taskbar->hovered_button == -2);
    
    draw_start_orb(orb_x, orb_y, orb_size, orb_hovered, 
                   taskbar->start_button_pressed || taskbar->start_menu_open);
    
    // ===== TASKBAR BUTTONS =====
    int button_area_start = orb_x + orb_size + 12;
    int button_spacing = 6;
    int button_h = TASKBAR_HEIGHT - 8;
    int button_y = taskbar->y + 4;
    
    for (int i = 0; i < taskbar->button_count; i++) {
        if (!taskbar->buttons[i].active) continue;
        
        int btn_x = button_area_start + i * (TASKBAR_BUTTON_WIDTH + button_spacing);
        
        // Başlık kısaltma
        char short_title[24];
        int title_len = 0;
        while (taskbar->buttons[i].title[title_len] && title_len < 18) {
            short_title[title_len] = taskbar->buttons[i].title[title_len];
            title_len++;
        }
        if (taskbar->buttons[i].title[title_len]) {
            short_title[title_len++] = '.';
            short_title[title_len++] = '.';
        }
        short_title[title_len] = '\0';
        
        draw_taskbar_button(btn_x, button_y, TASKBAR_BUTTON_WIDTH, button_h,
                           short_title,
                           taskbar->hovered_button == i,
                           taskbar->buttons[i].is_focused,
                           true);
    }
    
    // ===== SYSTEM TRAY & CLOCK =====
    int clock_width = 130;
    int clock_x = taskbar->width - clock_width - 50;
    int clock_y = button_y;
    int clock_h = button_h;
    
    // Saat alanı arkaplan
    gui_fill_rect(clock_x, clock_y, clock_width, clock_h, CLOCK_BG);
    
    // İnce çerçeve
    gui_draw_line(clock_x, clock_y, clock_x, clock_y + clock_h - 1, RGB(50, 60, 75));
    
    // Saat gösterimini çiz
    taskbar_draw_clock_text(taskbar, clock_x, clock_y, clock_h);
}

// Saat gösterimini güncelle (optimize edilmiş)
void taskbar_update_clock_display(Taskbar* taskbar, bool full_redraw) {
    int clock_width = 130;
    int clock_x = taskbar->width - clock_width - 50;
    int button_h = TASKBAR_HEIGHT - 8;
    int clock_y = taskbar->y + 4;
    int clock_h = button_h;
    
    if (full_redraw) {
        // Saat ve dakika değişti, tüm saat alanını temizle ve yeniden çiz
        gui_fill_rect(clock_x, clock_y, clock_width, clock_h, CLOCK_BG);
        gui_draw_line(clock_x, clock_y, clock_x, clock_y + clock_h - 1, RGB(50, 60, 75));
        taskbar_draw_clock_text(taskbar, clock_x, clock_y, clock_h);
    } else {
        // Sadece saniye değişti, sadece saniye kısmını güncelle
        char s_str[3];
        num_to_str2(taskbar->current_seconds, s_str);
        
        int text_x = clock_x + 8 + 48; // Saniye başlangıç pozisyonu (HH:MM: sonrası)
        int text_y = clock_y + (clock_h - 8) / 2;
        
        // Sadece saniye alanını temizle (2 karakter genişliği = 16 piksel)
        gui_fill_rect(text_x, text_y, 16, 8, CLOCK_BG);
        
        // Saniye metni
        char ss[3];
        ss[0] = s_str[0];
        ss[1] = s_str[1];
        ss[2] = '\0';
        gui_draw_string(text_x, text_y, ss, CLOCK_TEXT, 0);
    }
}

// Pencere ekle
int taskbar_add_window(Taskbar* taskbar, const char* title, int window_id) {
    if (taskbar->button_count >= MAX_TASKBAR_BUTTONS) return -1;
    
    int idx = taskbar->button_count++;
    taskbar->buttons[idx].active = true;
    taskbar->buttons[idx].visible = true;
    taskbar->buttons[idx].window_id = window_id;
    taskbar->buttons[idx].is_focused = false;
    str_copy(taskbar->buttons[idx].title, title, 64);
    
    return idx;
}

// Pencere kaldır
void taskbar_remove_window(Taskbar* taskbar, int window_id) {
    for (int i = 0; i < taskbar->button_count; i++) {
        if (taskbar->buttons[i].active && taskbar->buttons[i].window_id == window_id) {
            taskbar->buttons[i].active = false;
            
            for (int j = i; j < taskbar->button_count - 1; j++) {
                taskbar->buttons[j] = taskbar->buttons[j + 1];
            }
            taskbar->button_count--;
            return;
        }
    }
}

// Pencere odağı ayarla
void taskbar_set_focus(Taskbar* taskbar, int window_id) {
    for (int i = 0; i < taskbar->button_count; i++) {
        if (taskbar->buttons[i].active) {
            taskbar->buttons[i].is_focused = (taskbar->buttons[i].window_id == window_id);
        }
    }
}

// Mouse hareket kontrolü
void taskbar_handle_mouse_move(Taskbar* taskbar, int mouse_x, int mouse_y) {
    if (mouse_y < taskbar->y) {
        taskbar->hovered_button = -1;
        return;
    }
    
    // Start orb
    int orb_size = 32;
    int orb_x = 6;
    int orb_y = taskbar->y + (TASKBAR_HEIGHT - orb_size) / 2;
    if (mouse_x >= orb_x && mouse_x < orb_x + orb_size &&
        mouse_y >= orb_y && mouse_y < orb_y + orb_size) {
        taskbar->hovered_button = -2;
        return;
    }
    
    // Taskbar butonları
    int button_area_start = orb_x + orb_size + 12;
    int button_spacing = 6;
    int button_y = taskbar->y + 4;
    
    for (int i = 0; i < taskbar->button_count; i++) {
        if (!taskbar->buttons[i].active) continue;
        
        int btn_x = button_area_start + i * (TASKBAR_BUTTON_WIDTH + button_spacing);
        
        if (mouse_x >= btn_x && mouse_x < btn_x + TASKBAR_BUTTON_WIDTH &&
            mouse_y >= button_y && mouse_y < button_y + TASKBAR_HEIGHT - 8) {
            taskbar->hovered_button = i;
            return;
        }
    }
    
    taskbar->hovered_button = -1;
}

// Mouse tıklama kontrolü
int taskbar_handle_mouse_click(Taskbar* taskbar, int mouse_x, int mouse_y) {
    if (mouse_y < taskbar->y) return -1;
    
    // Start orb
    int orb_size = 32;
    int orb_x = 6;
    int orb_y = taskbar->y + (TASKBAR_HEIGHT - orb_size) / 2;
    if (mouse_x >= orb_x && mouse_x < orb_x + orb_size &&
        mouse_y >= orb_y && mouse_y < orb_y + orb_size) {
        taskbar->start_button_pressed = true;
        taskbar->start_menu_open = !taskbar->start_menu_open;
        return -2;
    }
    
    // Taskbar butonları
    int button_area_start = orb_x + 32 + 12;
    int button_spacing = 6;
    int button_y = taskbar->y + 4;
    
    for (int i = 0; i < taskbar->button_count; i++) {
        if (!taskbar->buttons[i].active) continue;
        
        int btn_x = button_area_start + i * (TASKBAR_BUTTON_WIDTH + button_spacing);
        
        if (mouse_x >= btn_x && mouse_x < btn_x + TASKBAR_BUTTON_WIDTH &&
            mouse_y >= button_y && mouse_y < button_y + TASKBAR_HEIGHT - 8) {
            return taskbar->buttons[i].window_id;
        }
    }
    
    return -1;
}