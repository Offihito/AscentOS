// ============================================================================
// compositor64.h - Compositing Window Manager
// ============================================================================
#ifndef COMPOSITOR64_H
#define COMPOSITOR64_H

#include <stdint.h>
#include <stdbool.h>
#include "gui64.h"

// ============================================================================
// LAYER SYSTEM
// ============================================================================

#define MAX_LAYERS      16
#define MAX_DIRTY_RECTS 32

typedef struct {
    int x, y;
    int width, height;
} Rect;

typedef enum {
    LAYER_TYPE_DESKTOP,
    LAYER_TYPE_WINDOW,
    LAYER_TYPE_TASKBAR,
    LAYER_TYPE_CURSOR
} LayerType;

typedef struct {
    bool     active;
    bool     visible;
    bool     dirty;
    LayerType type;
    int      z_order;
    Rect     bounds;
    uint32_t* buffer;
    int      window_id;

    // Alpha & shadow
    uint8_t  alpha;
    bool     has_shadow;
    int      shadow_offset_x;
    int      shadow_offset_y;
    uint8_t  shadow_alpha;
    int      shadow_blur_radius;
} Layer;

// ============================================================================
// COMPOSITOR STATE
// ============================================================================

typedef struct {
    Layer layers[MAX_LAYERS];
    int   layer_count;
    int   z_sorted[MAX_LAYERS];

    Rect  global_dirty_rects[MAX_DIRTY_RECTS];
    int   global_dirty_count;

    int   screen_width;
    int   screen_height;
    Color desktop_color;
} Compositor;

// ============================================================================
// CORE
// ============================================================================

void compositor_init(Compositor* comp, int width, int height, Color desktop_color);

// ============================================================================
// LAYER MANAGEMENT
// ============================================================================

int  compositor_create_layer(Compositor* comp, LayerType type, int x, int y,
                             int width, int height);
void compositor_destroy_layer(Compositor* comp, int layer_index);
void compositor_set_layer_visible(Compositor* comp, int layer_index, bool visible);
void compositor_move_layer(Compositor* comp, int layer_index, int x, int y);
void compositor_resize_layer(Compositor* comp, int layer_index, int width, int height);

// ============================================================================
// Z-ORDER
// ============================================================================

void compositor_bring_to_front(Compositor* comp, int layer_index);
void compositor_rebuild_z_order(Compositor* comp);

// ============================================================================
// DIRTY TRACKING
// ============================================================================

void compositor_mark_layer_dirty(Compositor* comp, int layer_index);
void compositor_add_global_dirty_rect(Compositor* comp, int x, int y,
                                     int width, int height);
void compositor_merge_dirty_rects(Compositor* comp);
void compositor_clear_dirty_flags(Compositor* comp);

// ============================================================================
// RENDERING
// ============================================================================

void compositor_render(Compositor* comp);
void compositor_render_dirty(Compositor* comp);

// ============================================================================
// RECT HELPERS
// ============================================================================

bool rect_intersect(const Rect* r1, const Rect* r2);
Rect rect_intersection(const Rect* r1, const Rect* r2);
bool rect_is_valid(const Rect* r);
void rect_clamp_to_screen(Rect* r, int screen_width, int screen_height);

// ============================================================================
// DRAWING PRIMITIVES
// ============================================================================

void layer_fill_rect(Layer* layer, int x, int y, int width, int height, Color color);
void layer_draw_string(Layer* layer, int x, int y, const char* str,
                       Color fg, Color bg);
void layer_blit(Layer* layer, int x, int y, const uint32_t* pixels,
                int width, int height);

// ============================================================================
// ALPHA BLENDING & SHADOW
// ============================================================================

void  compositor_set_layer_alpha(Compositor* comp, int layer_index, uint8_t alpha);
void  compositor_set_layer_shadow(Compositor* comp, int layer_index,
                                  bool enabled, int offset_x, int offset_y,
                                  uint8_t shadow_alpha, int blur_radius);
Color alpha_blend(Color fg, Color bg, uint8_t alpha);

// gui64 katmanı güçlü implementasyon sağlayabilir; yoksa fallback kullanılır
void gui_blit_scanline(int screen_x, int screen_y, const uint32_t* src, int count);

#endif // COMPOSITOR64_H