// ============================================================================
// taskbar64.h - Windows 7 Style Taskbar
// ============================================================================
#ifndef TASKBAR64_H
#define TASKBAR64_H

#include <stdint.h>
#include <stdbool.h>
#include "gui64.h"

#define TASKBAR_HEIGHT 40
#define START_BUTTON_WIDTH 60
#define CLOCK_AREA_WIDTH 160
#define TASKBAR_BUTTON_WIDTH 180
#define MAX_TASKBAR_BUTTONS 8

typedef struct {
    bool active;
    bool visible;
    char title[64];
    int window_id;
    bool is_focused;
} TaskbarButton;

typedef struct {
    int y;                          // Panel Y konumu (genellikle screen_height - TASKBAR_HEIGHT)
    int width;                      // Ekran genişliği
    bool start_button_pressed;
    bool start_menu_open;
    TaskbarButton buttons[MAX_TASKBAR_BUTTONS];
    int button_count;
    int hovered_button;             // -2: start, -1: hiçbiri, 0+: taskbar button
    uint8_t current_hours;
    uint8_t current_minutes;
    uint8_t current_seconds;
} Taskbar;

// Taskbar başlatma ve güncelleme
void taskbar_init(Taskbar* taskbar, int screen_width, int screen_height);
void taskbar_update_time(Taskbar* taskbar);
void taskbar_draw(const Taskbar* taskbar);
void taskbar_update_clock_display(Taskbar* taskbar, bool full_redraw);

// Pencere yönetimi
int taskbar_add_window(Taskbar* taskbar, const char* title, int window_id);
void taskbar_remove_window(Taskbar* taskbar, int window_id);
void taskbar_set_focus(Taskbar* taskbar, int window_id);

// Mouse etkileşimi
void taskbar_handle_mouse_move(Taskbar* taskbar, int mouse_x, int mouse_y);
int taskbar_handle_mouse_click(Taskbar* taskbar, int mouse_x, int mouse_y);

#endif