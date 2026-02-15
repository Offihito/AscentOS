// ============================================================================
// wm64.c - SerenityOS Style Window Manager Implementation
// ============================================================================
#include "wm64.h"
#include <stddef.h>

// ============================================================================
// Helper Functions
// ============================================================================

static size_t str_len(const char* s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static void str_copy(char* dest, const char* src, size_t max_len) {
    size_t i = 0;
    while (i < max_len - 1 && src[i]) {
        dest[i] = src[i];
        i++;
    }
    dest[i] = '\0';
}

static WMWindow* wm_find(WindowManager* wm, int window_id) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (wm->windows[i].used && wm->windows[i].window_id == window_id)
            return &wm->windows[i];
    }
    return NULL;
}

// ============================================================================
// SerenityOS Style Drawing Functions
// ============================================================================

// Draw 3D raised bevel (classic Windows 95/SerenityOS style)
static void draw_raised_box(Layer* layer, int x, int y, int w, int h) {
    // Top and left highlights
    layer_fill_rect(layer, x, y, w, 1, COLOR_BUTTON_HILIGHT);           // Top
    layer_fill_rect(layer, x, y, 1, h, COLOR_BUTTON_HILIGHT);           // Left
    layer_fill_rect(layer, x + 1, y + 1, w - 2, 1, COLOR_BUTTON_LIGHT); // Top inner
    layer_fill_rect(layer, x + 1, y + 1, 1, h - 2, COLOR_BUTTON_LIGHT); // Left inner
    
    // Bottom and right shadows
    layer_fill_rect(layer, x, y + h - 1, w, 1, COLOR_BORDER_DARK);        // Bottom
    layer_fill_rect(layer, x + w - 1, y, 1, h, COLOR_BORDER_DARK);        // Right
    layer_fill_rect(layer, x + 1, y + h - 2, w - 2, 1, COLOR_BUTTON_SHADOW); // Bottom inner
    layer_fill_rect(layer, x + w - 2, y + 1, 1, h - 2, COLOR_BUTTON_SHADOW); // Right inner
}

// Draw 3D pressed bevel
static void draw_pressed_box(Layer* layer, int x, int y, int w, int h) {
    // Reverse the colors for pressed effect
    layer_fill_rect(layer, x, y, w, 1, COLOR_BORDER_DARK);           // Top
    layer_fill_rect(layer, x, y, 1, h, COLOR_BORDER_DARK);           // Left
    layer_fill_rect(layer, x + 1, y + 1, w - 2, 1, COLOR_BUTTON_SHADOW); // Top inner
    layer_fill_rect(layer, x + 1, y + 1, 1, h - 2, COLOR_BUTTON_SHADOW); // Left inner
    
    layer_fill_rect(layer, x, y + h - 1, w, 1, COLOR_BUTTON_HILIGHT);    // Bottom
    layer_fill_rect(layer, x + w - 1, y, 1, h, COLOR_BUTTON_HILIGHT);    // Right
}

// Draw classic window button with icon
static void draw_window_button(Layer* layer, int x, int y, int w, int h, 
                               const char* type, bool pressed) {
    // Button face
    layer_fill_rect(layer, x + 2, y + 2, w - 4, h - 4, COLOR_BUTTON_FACE);
    
    // 3D effect
    if (pressed) {
        draw_pressed_box(layer, x, y, w, h);
    } else {
        draw_raised_box(layer, x, y, w, h);
    }
    
    // Draw icon in the center
    int cx = x + w / 2;
    int cy = y + h / 2;
    int offset = pressed ? 1 : 0; // Shift when pressed
    
    if (type[0] == 'X') {  // Close button
        // Draw X (2 pixels thick)
        for (int i = -3; i <= 3; i++) {
            // Main diagonals
            if (cx + i + offset >= 0 && cx + i + offset < layer->bounds.width &&
                cy + i + offset >= 0 && cy + i + offset < layer->bounds.height) {
                layer->buffer[(cy + i + offset) * layer->bounds.width + (cx + i + offset)] = COLOR_BORDER_DARK;
                layer->buffer[(cy + i + offset) * layer->bounds.width + (cx + i + offset + 1)] = COLOR_BORDER_DARK;
            }
            if (cx + i + offset >= 0 && cx + i + offset < layer->bounds.width &&
                cy - i + offset >= 0 && cy - i + offset < layer->bounds.height) {
                layer->buffer[(cy - i + offset) * layer->bounds.width + (cx + i + offset)] = COLOR_BORDER_DARK;
                layer->buffer[(cy - i + offset) * layer->bounds.width + (cx + i + offset + 1)] = COLOR_BORDER_DARK;
            }
        }
    } else if (type[0] == '_') {  // Minimize button
        // Draw horizontal line
        for (int i = -4; i <= 4; i++) {
            int px = cx + i + offset;
            int py = cy + 2 + offset;
            if (px >= 0 && px < layer->bounds.width && py >= 0 && py < layer->bounds.height) {
                layer->buffer[py * layer->bounds.width + px] = COLOR_BORDER_DARK;
                layer->buffer[(py + 1) * layer->bounds.width + px] = COLOR_BORDER_DARK;
            }
        }
    } else if (type[0] == 'M') {  // Maximize button
        // Draw rectangle
        for (int i = -3; i <= 3; i++) {
            // Top and bottom
            int px = cx + i + offset;
            if (px >= 0 && px < layer->bounds.width) {
                if (cy - 3 + offset >= 0 && cy - 3 + offset < layer->bounds.height) {
                    layer->buffer[(cy - 3 + offset) * layer->bounds.width + px] = COLOR_BORDER_DARK;
                    layer->buffer[(cy - 2 + offset) * layer->bounds.width + px] = COLOR_BORDER_DARK;
                }
                if (cy + 3 + offset >= 0 && cy + 3 + offset < layer->bounds.height) {
                    layer->buffer[(cy + 3 + offset) * layer->bounds.width + px] = COLOR_BORDER_DARK;
                }
            }
            // Left and right
            int py = cy + i + offset;
            if (py >= 0 && py < layer->bounds.height) {
                if (cx - 3 + offset >= 0 && cx - 3 + offset < layer->bounds.width) {
                    layer->buffer[py * layer->bounds.width + (cx - 3 + offset)] = COLOR_BORDER_DARK;
                }
                if (cx + 3 + offset >= 0 && cx + 3 + offset < layer->bounds.width) {
                    layer->buffer[py * layer->bounds.width + (cx + 3 + offset)] = COLOR_BORDER_DARK;
                }
            }
        }
    }
}

// Draw title bar with SerenityOS style
static void draw_title_bar(Layer* layer, const char* title, bool has_focus,
                          ButtonHoverState* hover) {
    (void)has_focus; // Tüm pencereler mavi
    int w = layer->bounds.width;
    
    // Title bar background - her zaman koyu mavi
    uint32_t title_color = COLOR_TITLE_ACTIVE;
    layer_fill_rect(layer, BORDER_WIDTH, BORDER_WIDTH, 
                   w - BORDER_WIDTH * 2, TITLE_BAR_HEIGHT, title_color);
    
    // Title text (left aligned with small margin)
    layer_draw_string(layer, BORDER_WIDTH + 6, BORDER_WIDTH + 6, 
                     title, COLOR_TITLE_TEXT, title_color);
    
    // Buttons (right side)
    int btn_x = w - WM_RIGHT_MARGIN - WM_BUTTON_WIDTH;
    int btn_y = BORDER_WIDTH + (TITLE_BAR_HEIGHT - WM_BUTTON_HEIGHT) / 2;
    
    // Close button (rightmost)
    draw_window_button(layer, btn_x, btn_y, WM_BUTTON_WIDTH, WM_BUTTON_HEIGHT,
                      "X", hover->close_hover);
    btn_x -= WM_BUTTON_WIDTH + WM_BUTTON_GAP;
    
    // Maximize button
    draw_window_button(layer, btn_x, btn_y, WM_BUTTON_WIDTH, WM_BUTTON_HEIGHT,
                      "M", hover->maximize_hover);
    btn_x -= WM_BUTTON_WIDTH + WM_BUTTON_GAP;
    
    // Minimize button
    draw_window_button(layer, btn_x, btn_y, WM_BUTTON_WIDTH, WM_BUTTON_HEIGHT,
                      "_", hover->minimize_hover);
}

// Draw complete window frame
void wm_draw_window_frame(Compositor* comp, int layer_index, WMWindow* win) {
    Layer* layer = &comp->layers[layer_index];
    int w = layer->bounds.width;
    int h = layer->bounds.height;
    
    // Outer border (dark)
    layer_fill_rect(layer, 0, 0, w, BORDER_WIDTH, COLOR_BORDER_DARK);           // Top
    layer_fill_rect(layer, 0, 0, BORDER_WIDTH, h, COLOR_BORDER_DARK);           // Left
    layer_fill_rect(layer, 0, h - BORDER_WIDTH, w, BORDER_WIDTH, COLOR_BORDER_DARK); // Bottom
    layer_fill_rect(layer, w - BORDER_WIDTH, 0, BORDER_WIDTH, h, COLOR_BORDER_DARK); // Right
    
    // Window frame (light gray)
    layer_fill_rect(layer, BORDER_WIDTH, BORDER_WIDTH + TITLE_BAR_HEIGHT,
                   w - BORDER_WIDTH * 2, h - BORDER_WIDTH - TITLE_BAR_HEIGHT - BORDER_WIDTH,
                   COLOR_WINDOW_FRAME);
    
    // Content area (white, slightly inset)
    int content_x = BORDER_WIDTH + 2;
    int content_y = BORDER_WIDTH + TITLE_BAR_HEIGHT + 2;
    int content_w = w - BORDER_WIDTH * 2 - 4;
    int content_h = h - BORDER_WIDTH - TITLE_BAR_HEIGHT - BORDER_WIDTH - 4;
    
    layer_fill_rect(layer, content_x, content_y, content_w, content_h, COLOR_CONTENT_BG);
    
    // Content area inset border
    layer_fill_rect(layer, content_x - 1, content_y - 1, content_w + 2, 1, COLOR_BUTTON_SHADOW);
    layer_fill_rect(layer, content_x - 1, content_y - 1, 1, content_h + 2, COLOR_BUTTON_SHADOW);
    layer_fill_rect(layer, content_x - 1, content_y + content_h, content_w + 2, 1, COLOR_BUTTON_HILIGHT);
    layer_fill_rect(layer, content_x + content_w, content_y - 1, 1, content_h + 2, COLOR_BUTTON_HILIGHT);
    
    // Draw title bar
    draw_title_bar(layer, win->title, win->has_focus, &win->hover);
    
    compositor_mark_layer_dirty(comp, layer_index);
}

// ============================================================================
// Window Management
// ============================================================================

void wm_init(WindowManager* wm, int screen_width, int screen_height) {
    wm->count = 0;
    wm->next_id = 1;
    wm->screen_width = screen_width;
    wm->screen_height = screen_height;
    wm->focused_window_id = -1;
    
    for (int i = 0; i < MAX_WINDOWS; i++) {
        wm->windows[i].used = false;
        wm->windows[i].hover.minimize_hover = false;
        wm->windows[i].hover.maximize_hover = false;
        wm->windows[i].hover.close_hover = false;
    }
}

int wm_create_window(Compositor* comp, WindowManager* wm, Taskbar* taskbar,
                     int x, int y, int width, int height, const char* title) {
    if (wm->count >= MAX_WINDOWS) return -1;
    
    int layer_idx = compositor_create_layer(comp, LAYER_TYPE_WINDOW, x, y, width, height);
    if (layer_idx < 0) return -1;
    
    int window_id = wm->next_id++;
    comp->layers[layer_idx].window_id = window_id;
    
    // Classic shadow (smaller, more subtle)
    compositor_set_layer_alpha(comp, layer_idx, 255);
    compositor_set_layer_shadow(comp, layer_idx, true, 2, 2, 60, 3);
    
    // WM record
    int slot = -1;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!wm->windows[i].used) { slot = i; break; }
    }
    if (slot < 0) {
        compositor_destroy_layer(comp, layer_idx);
        return -1;
    }
    
    WMWindow* win = &wm->windows[slot];
    win->used = true;
    win->layer_index = layer_idx;
    win->window_id = window_id;
    str_copy(win->title, title, sizeof(win->title));
    win->state = WINDOW_STATE_NORMAL;
    win->saved_rect.x = x;
    win->saved_rect.y = y;
    win->saved_rect.width = width;
    win->saved_rect.height = height;
    win->has_focus = true;
    win->hover.minimize_hover = false;
    win->hover.maximize_hover = false;
    win->hover.close_hover = false;
    wm->count++;
    
    // Draw frame
    wm_draw_window_frame(comp, layer_idx, win);
    
    // Add to taskbar
    taskbar_add_window(taskbar, title, window_id);
    
    // Focus window
    wm_focus_window(comp, wm, window_id);
    
    return window_id;
}

void wm_destroy_window(Compositor* comp, WindowManager* wm, Taskbar* taskbar,
                       int window_id) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!wm->windows[i].used || wm->windows[i].window_id != window_id)
            continue;
        
        compositor_destroy_layer(comp, wm->windows[i].layer_index);
        taskbar_remove_window(taskbar, window_id);
        wm->windows[i].used = false;
        wm->count--;
        
        // Move focus to another window
        if (wm->focused_window_id == window_id) {
            wm->focused_window_id = -1;
            for (int j = 0; j < MAX_WINDOWS; j++) {
                if (wm->windows[j].used && wm->windows[j].state != WINDOW_STATE_MINIMIZED) {
                    wm_focus_window(comp, wm, wm->windows[j].window_id);
                    break;
                }
            }
        }
        return;
    }
}

// ============================================================================
// Window State Changes
// ============================================================================

void wm_minimize_window(Compositor* comp, WindowManager* wm, int window_id) {
    WMWindow* win = wm_find(wm, window_id);
    if (!win || win->state == WINDOW_STATE_MINIMIZED) return;
    
    Layer* layer = &comp->layers[win->layer_index];
    win->state = WINDOW_STATE_MINIMIZED;
    compositor_set_layer_visible(comp, win->layer_index, false);
    compositor_add_global_dirty_rect(comp, layer->bounds.x, layer->bounds.y,
                                    layer->bounds.width, layer->bounds.height);
}

void wm_maximize_window(Compositor* comp, WindowManager* wm, int window_id) {
    WMWindow* win = wm_find(wm, window_id);
    if (!win) return;
    
    Layer* layer = &comp->layers[win->layer_index];
    
    if (win->state != WINDOW_STATE_MAXIMIZED) {
        // Save current state
        win->saved_rect.x = layer->bounds.x;
        win->saved_rect.y = layer->bounds.y;
        win->saved_rect.width = layer->bounds.width;
        win->saved_rect.height = layer->bounds.height;
        
        // Maximize (taskbar height 40)
        compositor_move_layer(comp, win->layer_index, 0, 0);
        compositor_resize_layer(comp, win->layer_index, 
                               wm->screen_width, wm->screen_height - 40);
        win->state = WINDOW_STATE_MAXIMIZED;
    } else {
        // Restore
        compositor_move_layer(comp, win->layer_index, 
                            win->saved_rect.x, win->saved_rect.y);
        compositor_resize_layer(comp, win->layer_index, 
                              win->saved_rect.width, win->saved_rect.height);
        win->state = WINDOW_STATE_NORMAL;
    }
    
    wm_draw_window_frame(comp, win->layer_index, win);
    compositor_bring_to_front(comp, win->layer_index);
}

void wm_restore_window(Compositor* comp, WindowManager* wm, int window_id) {
    WMWindow* win = wm_find(wm, window_id);
    if (!win || win->state != WINDOW_STATE_MINIMIZED) return;
    
    win->state = WINDOW_STATE_NORMAL;
    compositor_set_layer_visible(comp, win->layer_index, true);
    compositor_bring_to_front(comp, win->layer_index);
    wm_draw_window_frame(comp, win->layer_index, win);
    wm_focus_window(comp, wm, window_id);
    
    Layer* layer = &comp->layers[win->layer_index];
    compositor_add_global_dirty_rect(comp, layer->bounds.x, layer->bounds.y,
                                    layer->bounds.width, layer->bounds.height);
}

void wm_toggle_maximize(Compositor* comp, WindowManager* wm, int window_id) {
    wm_maximize_window(comp, wm, window_id);
}

// ============================================================================
// Focus Management
// ============================================================================

void wm_focus_window(Compositor* comp, WindowManager* wm, int window_id) {
    // Update previous focused window
    if (wm->focused_window_id >= 0) {
        WMWindow* old_win = wm_find(wm, wm->focused_window_id);
        if (old_win) {
            old_win->has_focus = false;
            wm_draw_window_frame(comp, old_win->layer_index, old_win);
        }
    }
    
    // Focus new window
    WMWindow* win = wm_find(wm, window_id);
    if (win) {
        win->has_focus = true;
        wm->focused_window_id = window_id;
        compositor_bring_to_front(comp, win->layer_index);
        wm_draw_window_frame(comp, win->layer_index, win);
    }
}

int wm_get_focused_window(WindowManager* wm) {
    return wm->focused_window_id;
}

// ============================================================================
// Hover Management
// ============================================================================

void wm_update_hover(WindowManager* wm, Compositor* comp, int window_id, 
                     int local_x, int local_y) {
    WMWindow* win = wm_find(wm, window_id);
    if (!win) return;
    
    Layer* layer = &comp->layers[win->layer_index];
    int w = layer->bounds.width;
    
    bool old_min = win->hover.minimize_hover;
    bool old_max = win->hover.maximize_hover;
    bool old_close = win->hover.close_hover;
    
    win->hover.minimize_hover = false;
    win->hover.maximize_hover = false;
    win->hover.close_hover = false;
    
    // Check if in title bar
    if (local_y >= BORDER_WIDTH && local_y < BORDER_WIDTH + TITLE_BAR_HEIGHT) {
        int btn_y = BORDER_WIDTH + (TITLE_BAR_HEIGHT - WM_BUTTON_HEIGHT) / 2;
        int btn_x = w - WM_RIGHT_MARGIN - WM_BUTTON_WIDTH;
        
        // Close button (rightmost)
        if (local_x >= btn_x && local_x < btn_x + WM_BUTTON_WIDTH &&
            local_y >= btn_y && local_y < btn_y + WM_BUTTON_HEIGHT) {
            win->hover.close_hover = true;
        }
        btn_x -= WM_BUTTON_WIDTH + WM_BUTTON_GAP;
        
        // Maximize button
        if (local_x >= btn_x && local_x < btn_x + WM_BUTTON_WIDTH &&
            local_y >= btn_y && local_y < btn_y + WM_BUTTON_HEIGHT) {
            win->hover.maximize_hover = true;
        }
        btn_x -= WM_BUTTON_WIDTH + WM_BUTTON_GAP;
        
        // Minimize button
        if (local_x >= btn_x && local_x < btn_x + WM_BUTTON_WIDTH &&
            local_y >= btn_y && local_y < btn_y + WM_BUTTON_HEIGHT) {
            win->hover.minimize_hover = true;
        }
    }
    
    // Redraw if hover changed
    if (old_min != win->hover.minimize_hover || 
        old_max != win->hover.maximize_hover || 
        old_close != win->hover.close_hover) {
        wm_draw_window_frame(comp, win->layer_index, win);
    }
}

void wm_clear_hover(WindowManager* wm, int window_id) {
    WMWindow* win = wm_find(wm, window_id);
    if (!win) return;
    
    win->hover.minimize_hover = false;
    win->hover.maximize_hover = false;
    win->hover.close_hover = false;
}

// ============================================================================
// Helper Functions
// ============================================================================

int wm_get_layer_index(WindowManager* wm, int window_id) {
    WMWindow* win = wm_find(wm, window_id);
    return win ? win->layer_index : -1;
}

int wm_get_window_at(Compositor* comp, WindowManager* wm,
                     int screen_x, int screen_y, int* out_local_x, int* out_local_y) {
    (void)wm; // Unused but needed for API compatibility
    
    for (int z = MAX_LAYERS - 1; z >= 0; z--) {
        int layer_idx = comp->z_sorted[z];
        if (layer_idx < 0) continue;
        Layer* layer = &comp->layers[layer_idx];
        if (!layer->active || !layer->visible || layer->type != LAYER_TYPE_WINDOW)
            continue;
        if (layer->window_id < 0) continue;
        
        int lx = screen_x - layer->bounds.x;
        int ly = screen_y - layer->bounds.y;
        if (lx >= 0 && lx < layer->bounds.width && 
            ly >= 0 && ly < layer->bounds.height) {
            if (out_local_x) *out_local_x = lx;
            if (out_local_y) *out_local_y = ly;
            return layer->window_id;
        }
    }
    return -1;
}

WMHitResult wm_hit_test(int win_width, int win_height, int local_x, int local_y) {
    (void)win_height; // Not used currently
    
    // Outside title bar
    if (local_y < BORDER_WIDTH || local_y >= BORDER_WIDTH + TITLE_BAR_HEIGHT)
        return WMHIT_NONE;
    
    // Check buttons (right side)
    int btn_y = BORDER_WIDTH + (TITLE_BAR_HEIGHT - WM_BUTTON_HEIGHT) / 2;
    int btn_x = win_width - WM_RIGHT_MARGIN - WM_BUTTON_WIDTH;
    
    // Close button (rightmost)
    if (local_x >= btn_x && local_x < btn_x + WM_BUTTON_WIDTH &&
        local_y >= btn_y && local_y < btn_y + WM_BUTTON_HEIGHT) {
        return WMHIT_CLOSE;
    }
    btn_x -= WM_BUTTON_WIDTH + WM_BUTTON_GAP;
    
    // Maximize button
    if (local_x >= btn_x && local_x < btn_x + WM_BUTTON_WIDTH &&
        local_y >= btn_y && local_y < btn_y + WM_BUTTON_HEIGHT) {
        return WMHIT_MAXIMIZE;
    }
    btn_x -= WM_BUTTON_WIDTH + WM_BUTTON_GAP;
    
    // Minimize button
    if (local_x >= btn_x && local_x < btn_x + WM_BUTTON_WIDTH &&
        local_y >= btn_y && local_y < btn_y + WM_BUTTON_HEIGHT) {
        return WMHIT_MINIMIZE;
    }
    
    // Title bar (draggable area)
    return WMHIT_TITLE;
}

void wm_handle_click(Compositor* comp, WindowManager* wm, Taskbar* taskbar,
                    int window_id, int local_x, int local_y) {
    WMWindow* win = wm_find(wm, window_id);
    if (!win) return;
    
    Layer* layer = &comp->layers[win->layer_index];
    WMHitResult hit = wm_hit_test(layer->bounds.width, layer->bounds.height, 
                                   local_x, local_y);
    
    switch (hit) {
        case WMHIT_CLOSE:
            wm_destroy_window(comp, wm, taskbar, window_id);
            return;
            
        case WMHIT_MINIMIZE:
            wm_minimize_window(comp, wm, window_id);
            return;
            
        case WMHIT_MAXIMIZE:
            wm_toggle_maximize(comp, wm, window_id);
            return;
            
        case WMHIT_TITLE:
            wm_focus_window(comp, wm, window_id);
            break;
            
        case WMHIT_NONE:
        default:
            break;
    }
}