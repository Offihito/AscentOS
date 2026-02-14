// ============================================================================
// wm64.c - Pencere Yöneticisi - Minimize, Maximize, Close
// ============================================================================
#include "wm64.h"
#include <stddef.h>

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

void wm_init(WindowManager* wm, int screen_width, int screen_height) {
    wm->count = 0;
    wm->next_id = 1;
    wm->screen_width = screen_width;
    wm->screen_height = screen_height;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        wm->windows[i].used = false;
    }
}

// Pencere çerçevesi (başlık + içerik alanı) çiz - layer buffer'a
static void wm_draw_window_frame(Compositor* comp, int layer_index, const char* title) {
    Layer* layer = &comp->layers[layer_index];
    int w = layer->bounds.width;
    int h = layer->bounds.height;

    // İçerik alanı (açık gri)
    layer_fill_rect(layer, 0, TITLE_BAR_HEIGHT, w, h - TITLE_BAR_HEIGHT, RGB(240, 240, 240));

    // Başlık çubuğu (koyu mavi)
    layer_fill_rect(layer, 0, 0, w, TITLE_BAR_HEIGHT, RGB(50, 100, 200));

    // Başlık metni (sol üst)
    layer_draw_string(layer, 6, 4, title, RGB(255, 255, 255), RGB(50, 100, 200));

    // Sağdan: Close (kırmızı), Maximize (gri), Minimize (gri)
    int bx = w - WM_RIGHT_MARGIN - WM_BUTTON_WIDTH * 3 - WM_BUTTON_GAP * 2;
    int by = 2;
    int bw = WM_BUTTON_WIDTH;
    int bh = TITLE_BAR_HEIGHT - 4;

    // Minimize (sol buton)
    layer_fill_rect(layer, bx, by, bw, bh, RGB(70, 130, 220));
    bx += bw + WM_BUTTON_GAP;
    // Maximize
    layer_fill_rect(layer, bx, by, bw, bh, RGB(70, 130, 220));
    bx += bw + WM_BUTTON_GAP;
    // Close (kırmızı)
    layer_fill_rect(layer, bx, by, bw, bh, RGB(200, 80, 80));

    compositor_mark_layer_dirty(comp, layer_index);
}

static WMWindow* wm_find(WindowManager* wm, int window_id) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (wm->windows[i].used && wm->windows[i].window_id == window_id)
            return &wm->windows[i];
    }
    return NULL;
}

int wm_create_window(Compositor* comp, WindowManager* wm, Taskbar* taskbar,
                    int x, int y, int width, int height, const char* title) {
    if (wm->count >= MAX_WINDOWS) return -1;

    int layer_idx = compositor_create_layer(comp, LAYER_TYPE_WINDOW, x, y, width, height);
    if (layer_idx < 0) return -1;

    int window_id = wm->next_id++;
    comp->layers[layer_idx].window_id = window_id;

    compositor_set_layer_alpha(comp, layer_idx, 255);
    compositor_set_layer_shadow(comp, layer_idx, true, 4, 4, 100, 5);

    wm_draw_window_frame(comp, layer_idx, title);

    // WM kaydı
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
    win->minimized = false;
    win->maximized = false;
    win->saved_rect.x = x;
    win->saved_rect.y = y;
    win->saved_rect.width = width;
    win->saved_rect.height = height;
    wm->count++;

    taskbar_add_window(taskbar, title, window_id);
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
        return;
    }
}

void wm_restore_window(Compositor* comp, WindowManager* wm, int window_id) {
    WMWindow* win = wm_find(wm, window_id);
    if (!win || !win->minimized) return;
    win->minimized = false;
    compositor_set_layer_visible(comp, win->layer_index, true);
    compositor_bring_to_front(comp, win->layer_index);
    compositor_mark_layer_dirty(comp, win->layer_index);
    compositor_add_global_dirty_rect(comp, win->saved_rect.x, win->saved_rect.y,
                                     win->saved_rect.width, win->saved_rect.height);
}

int wm_get_layer_index(WindowManager* wm, int window_id) {
    WMWindow* win = wm_find(wm, window_id);
    return win ? win->layer_index : -1;
}

// z_order'a göre en üstteki pencere (z_sorted son elemanlar = üstte)
int wm_get_window_at(Compositor* comp, WindowManager* wm,
                     int screen_x, int screen_y, int* out_local_x, int* out_local_y) {
    for (int z = MAX_LAYERS - 1; z >= 0; z--) {
        int layer_idx = comp->z_sorted[z];
        if (layer_idx < 0) continue;
        Layer* layer = &comp->layers[layer_idx];
        if (!layer->active || !layer->visible || layer->type != LAYER_TYPE_WINDOW)
            continue;
        if (layer->window_id < 0) continue;

        int lx = screen_x - layer->bounds.x;
        int ly = screen_y - layer->bounds.y;
        if (lx >= 0 && lx < layer->bounds.width && ly >= 0 && ly < layer->bounds.height) {
            if (out_local_x) *out_local_x = lx;
            if (out_local_y) *out_local_y = ly;
            return layer->window_id;
        }
    }
    return -1;
}

WMHitResult wm_hit_test(int win_width, int win_height, int local_x, int local_y) {
    if (local_y < 0 || local_y >= TITLE_BAR_HEIGHT) return WMHIT_NONE;

    int bx = win_width - WM_RIGHT_MARGIN - WM_BUTTON_WIDTH * 3 - WM_BUTTON_GAP * 2;
    int by = 2;
    int bw = WM_BUTTON_WIDTH;
    int bh = TITLE_BAR_HEIGHT - 4;

    // Close (en sağ)
    int close_x = bx + (WM_BUTTON_WIDTH + WM_BUTTON_GAP) * 2;
    if (local_x >= close_x && local_x < close_x + bw && local_y >= by && local_y < by + bh)
        return WMHIT_CLOSE;
    // Maximize
    int max_x = bx + (WM_BUTTON_WIDTH + WM_BUTTON_GAP);
    if (local_x >= max_x && local_x < max_x + bw && local_y >= by && local_y < by + bh)
        return WMHIT_MAXIMIZE;
    // Minimize
    if (local_x >= bx && local_x < bx + bw && local_y >= by && local_y < by + bh)
        return WMHIT_MINIMIZE;

    return WMHIT_TITLE;
}

void wm_handle_click(Compositor* comp, WindowManager* wm, Taskbar* taskbar,
                    int window_id, int local_x, int local_y) {
    WMWindow* win = wm_find(wm, window_id);
    if (!win) return;

    Layer* layer = &comp->layers[win->layer_index];
    WMHitResult hit = wm_hit_test(layer->bounds.width, layer->bounds.height, local_x, local_y);

    switch (hit) {
        case WMHIT_CLOSE:
            wm_destroy_window(comp, wm, taskbar, window_id);
            return;
        case WMHIT_MINIMIZE:
            compositor_set_layer_visible(comp, win->layer_index, false);
            win->minimized = true;
            compositor_add_global_dirty_rect(comp, layer->bounds.x, layer->bounds.y,
                                            layer->bounds.width, layer->bounds.height);
            return;
        case WMHIT_MAXIMIZE: {
            if (win->maximized) {
                compositor_move_layer(comp, win->layer_index, win->saved_rect.x, win->saved_rect.y);
                compositor_resize_layer(comp, win->layer_index, win->saved_rect.width, win->saved_rect.height);
                wm_draw_window_frame(comp, win->layer_index, win->title);
                win->maximized = false;
            } else {
                win->saved_rect.x = layer->bounds.x;
                win->saved_rect.y = layer->bounds.y;
                win->saved_rect.width = layer->bounds.width;
                win->saved_rect.height = layer->bounds.height;
                compositor_move_layer(comp, win->layer_index, 0, 0);
                compositor_resize_layer(comp, win->layer_index, wm->screen_width, wm->screen_height - 40);
                wm_draw_window_frame(comp, win->layer_index, win->title);
                win->maximized = true;
            }
            compositor_bring_to_front(comp, win->layer_index);
            compositor_mark_layer_dirty(comp, win->layer_index);
            return;
        }
        case WMHIT_TITLE:
            compositor_bring_to_front(comp, win->layer_index);
            break;
        case WMHIT_NONE:
        default:
            break;
    }
}
