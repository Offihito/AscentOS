// ============================================================================
// compositor64.c - Compositing Window Manager Implementation
// ============================================================================
#include "compositor64.h"
#include <stddef.h>

// ============================================================================
// Bellek yönetimi
//
// Bump allocator — layer buffer'ları bir kez alınır, serbest bırakılmaz.
// Pencere layer'ları oluşturulurken tam ekran kapasitesi ayrılır; böylece
// resize sırasında hiç yeni tahsis gerekmez.
// ============================================================================
#define COMP_HEAP_SIZE (128 * 1024 * 1024UL)
static uint8_t* comp_heap     = (uint8_t*)0x600000;
static size_t   comp_heap_off = 0;

static void* comp_malloc(size_t size) {
    if (comp_heap_off + size > COMP_HEAP_SIZE) return (void*)0;
    void* ptr = comp_heap + comp_heap_off;
    comp_heap_off += (size + 7) & ~7UL;
    return ptr;
}

// Her layer slotunun buffer kapasitesi (piksel cinsinden)
static int layer_buffer_capacity[MAX_LAYERS];

// ============================================================================
// INITIALIZATION
// ============================================================================

void compositor_init(Compositor* comp, int width, int height, Color desktop_color) {
    comp->screen_width  = width;
    comp->screen_height = height;
    comp->desktop_color = desktop_color;
    comp->layer_count   = 0;
    comp->global_dirty_count = 0;

    for (int i = 0; i < MAX_LAYERS; i++) {
        comp->layers[i].active         = false;
        comp->layers[i].visible        = false;
        comp->layers[i].dirty          = false;
        comp->layers[i].buffer         = NULL;
        comp->layers[i].window_id      = -1;
        comp->layers[i].z_order        = 0;
        comp->z_sorted[i]              = -1;
        layer_buffer_capacity[i]       = 0;
    }

    // Masaüstü layer'ı (her zaman index 0)
    int desktop_idx = compositor_create_layer(comp, LAYER_TYPE_DESKTOP,
                                              0, 0, width, height - 40);
    if (desktop_idx >= 0) {
        Layer* desktop = &comp->layers[desktop_idx];
        for (int i = 0; i < width * (height - 40); i++)
            desktop->buffer[i] = desktop_color;
        desktop->visible = true;
        desktop->z_order = 0;
    }

    compositor_rebuild_z_order(comp);
}

// ============================================================================
// LAYER MANAGEMENT
// ============================================================================

int compositor_create_layer(Compositor* comp, LayerType type, int x, int y,
                            int width, int height) {
    int idx = -1;
    for (int i = 0; i < MAX_LAYERS; i++) {
        if (!comp->layers[i].active) { idx = i; break; }
    }
    if (idx < 0) return -1;

    Layer* layer = &comp->layers[idx];

    // Pencere layer'ları için tam ekran kapasitesi ayır — resize'da
    // yeni tahsis gerekmez, çökme olmaz.
    int alloc_w = width;
    int alloc_h = height;
    if (type == LAYER_TYPE_WINDOW) {
        if (comp->screen_width  > alloc_w) alloc_w = comp->screen_width;
        if (comp->screen_height > alloc_h) alloc_h = comp->screen_height;
    }
    layer->buffer = (uint32_t*)comp_malloc((size_t)alloc_w * alloc_h * sizeof(uint32_t));
    if (!layer->buffer) return -1;
    layer_buffer_capacity[idx] = alloc_w * alloc_h;

    layer->active         = true;
    layer->visible        = true;
    layer->dirty          = true;
    layer->type           = type;
    layer->bounds.x       = x;
    layer->bounds.y       = y;
    layer->bounds.width   = width;
    layer->bounds.height  = height;
    layer->window_id      = -1;
    layer->alpha          = 255;
    layer->has_shadow     = false;
    layer->shadow_offset_x = 4;
    layer->shadow_offset_y = 4;
    layer->shadow_alpha    = 128;
    layer->shadow_blur_radius = 0;

    switch (type) {
        case LAYER_TYPE_DESKTOP: layer->z_order = 0;                        break;
        case LAYER_TYPE_WINDOW:  layer->z_order = 100 + comp->layer_count;
                                 layer->has_shadow = true;                   break;
        case LAYER_TYPE_TASKBAR: layer->z_order = 900;                       break;
        case LAYER_TYPE_CURSOR:  layer->z_order = 1000;                      break;
    }

    comp->layer_count++;

    for (int i = 0; i < width * height; i++)
        layer->buffer[i] = 0x00000000;

    compositor_rebuild_z_order(comp);
    return idx;
}

void compositor_destroy_layer(Compositor* comp, int layer_index) {
    if (layer_index < 0 || layer_index >= MAX_LAYERS) return;
    if (!comp->layers[layer_index].active) return;

    Layer* layer = &comp->layers[layer_index];
    compositor_add_global_dirty_rect(comp, layer->bounds.x, layer->bounds.y,
                                    layer->bounds.width, layer->bounds.height);
    layer->buffer  = NULL;
    layer->active  = false;
    layer->visible = false;
    comp->layer_count--;
    compositor_rebuild_z_order(comp);
}

void compositor_set_layer_visible(Compositor* comp, int layer_index, bool visible) {
    if (layer_index < 0 || layer_index >= MAX_LAYERS) return;
    if (!comp->layers[layer_index].active) return;

    Layer* layer = &comp->layers[layer_index];
    if (layer->visible != visible) {
        layer->visible = visible;
        compositor_add_global_dirty_rect(comp, layer->bounds.x, layer->bounds.y,
                                        layer->bounds.width, layer->bounds.height);
    }
}

void compositor_move_layer(Compositor* comp, int layer_index, int x, int y) {
    if (layer_index < 0 || layer_index >= MAX_LAYERS) return;
    if (!comp->layers[layer_index].active) return;

    Layer* layer = &comp->layers[layer_index];
    int w = layer->bounds.width;
    int h = layer->bounds.height;

    // Eski konumu dirty işaretle (gölge dahil)
    int dx1 = layer->bounds.x, dy1 = layer->bounds.y;
    int dx2 = dx1 + w,         dy2 = dy1 + h;
    if (layer->has_shadow) {
        int sx = dx1 + layer->shadow_offset_x;
        int sy = dy1 + layer->shadow_offset_y;
        if (sx + w > dx2) dx2 = sx + w;
        if (sy + h > dy2) dy2 = sy + h;
    }
    compositor_add_global_dirty_rect(comp, dx1, dy1, dx2 - dx1, dy2 - dy1);

    layer->bounds.x = x;
    layer->bounds.y = y;

    // Yeni konumu dirty işaretle
    dx1 = x; dy1 = y; dx2 = x + w; dy2 = y + h;
    if (layer->has_shadow) {
        int sx = x + layer->shadow_offset_x;
        int sy = y + layer->shadow_offset_y;
        if (sx + w > dx2) dx2 = sx + w;
        if (sy + h > dy2) dy2 = sy + h;
    }
    compositor_add_global_dirty_rect(comp, dx1, dy1, dx2 - dx1, dy2 - dy1);
}

void compositor_resize_layer(Compositor* comp, int layer_index, int width, int height) {
    if (layer_index < 0 || layer_index >= MAX_LAYERS) return;
    if (!comp->layers[layer_index].active) return;

    if (width  < 1) width  = 1;
    if (height < 1) height = 1;

    Layer* layer = &comp->layers[layer_index];
    compositor_add_global_dirty_rect(comp, layer->bounds.x, layer->bounds.y,
                                    layer->bounds.width, layer->bounds.height);

    int new_pixels = width * height;

    if (new_pixels <= layer_buffer_capacity[layer_index]) {
        // Kapasitede yer var — sıfır tahsis, içeriği yerinde yeniden düzenle
        int old_w  = layer->bounds.width;
        int old_h  = layer->bounds.height;
        int copy_w = width  < old_w ? width  : old_w;
        int copy_h = height < old_h ? height : old_h;

        if (width != old_w) {
            if (width < old_w) {
                // Daraldı: yukarıdan aşağı (örtüşme yok)
                for (int y = 0; y < copy_h; y++) {
                    uint32_t* src = layer->buffer + y * old_w;
                    uint32_t* dst = layer->buffer + y * width;
                    for (int x = 0; x < copy_w; x++) dst[x] = src[x];
                }
            } else {
                // Genişledi: aşağıdan yukarı (örtüşme var)
                for (int y = copy_h - 1; y >= 0; y--) {
                    uint32_t* src = layer->buffer + y * old_w;
                    uint32_t* dst = layer->buffer + y * width;
                    for (int x = copy_w - 1; x >= 0; x--) dst[x] = src[x];
                    for (int x = copy_w; x < width; x++) dst[x] = 0x00000000;
                }
            }
        }
        for (int y = copy_h; y < height; y++) {
            uint32_t* row = layer->buffer + y * width;
            for (int x = 0; x < width; x++) row[x] = 0x00000000;
        }
    } else {
        // Kapasite aşıldı — yeni buffer tahsis et (nadir yol)
        uint32_t* new_buf = (uint32_t*)comp_malloc((size_t)new_pixels * sizeof(uint32_t));
        if (!new_buf) return;

        for (int i = 0; i < new_pixels; i++) new_buf[i] = 0x00000000;

        int old_w  = layer->bounds.width;
        int old_h  = layer->bounds.height;
        int copy_w = width  < old_w ? width  : old_w;
        int copy_h = height < old_h ? height : old_h;
        for (int y = 0; y < copy_h; y++)
            for (int x = 0; x < copy_w; x++)
                new_buf[y * width + x] = layer->buffer[y * old_w + x];

        layer->buffer = new_buf;
        layer_buffer_capacity[layer_index] = new_pixels;
    }

    layer->bounds.width  = width;
    layer->bounds.height = height;
    layer->dirty = true;
    compositor_add_global_dirty_rect(comp, layer->bounds.x, layer->bounds.y, width, height);
}

// ============================================================================
// Z-ORDER MANAGEMENT
// ============================================================================

void compositor_rebuild_z_order(Compositor* comp) {
    for (int i = 0; i < MAX_LAYERS; i++) comp->z_sorted[i] = -1;

    int count = 0;
    for (int i = 0; i < MAX_LAYERS; i++)
        if (comp->layers[i].active) comp->z_sorted[count++] = i;

    // Bubble sort küçük liste için yeterli
    for (int i = 0; i < count - 1; i++)
        for (int j = i + 1; j < count; j++)
            if (comp->layers[comp->z_sorted[i]].z_order >
                comp->layers[comp->z_sorted[j]].z_order) {
                int tmp = comp->z_sorted[i];
                comp->z_sorted[i] = comp->z_sorted[j];
                comp->z_sorted[j] = tmp;
            }
}

void compositor_bring_to_front(Compositor* comp, int layer_index) {
    if (layer_index < 0 || layer_index >= MAX_LAYERS) return;
    if (!comp->layers[layer_index].active) return;

    int max_z = 0;
    for (int i = 0; i < MAX_LAYERS; i++)
        if (comp->layers[i].active &&
            comp->layers[i].type == LAYER_TYPE_WINDOW &&
            i != layer_index &&
            comp->layers[i].z_order > max_z)
            max_z = comp->layers[i].z_order;

    comp->layers[layer_index].z_order = max_z + 1;

    // Taskbar ve cursor her zaman üstte kalır
    for (int i = 0; i < MAX_LAYERS; i++) {
        if (!comp->layers[i].active) continue;
        if (comp->layers[i].type == LAYER_TYPE_TASKBAR) comp->layers[i].z_order = 900;
        if (comp->layers[i].type == LAYER_TYPE_CURSOR)  comp->layers[i].z_order = 1000;
    }

    compositor_rebuild_z_order(comp);
    compositor_mark_layer_dirty(comp, layer_index);
}

// ============================================================================
// DIRTY RECTANGLE TRACKING
// ============================================================================

void compositor_mark_layer_dirty(Compositor* comp, int layer_index) {
    if (layer_index < 0 || layer_index >= MAX_LAYERS) return;
    if (!comp->layers[layer_index].active) return;

    Layer* layer = &comp->layers[layer_index];
    layer->dirty = true;
    compositor_add_global_dirty_rect(comp, layer->bounds.x, layer->bounds.y,
                                    layer->bounds.width, layer->bounds.height);
}

void compositor_add_global_dirty_rect(Compositor* comp, int x, int y,
                                     int width, int height) {
    if (comp->global_dirty_count >= MAX_DIRTY_RECTS) {
        // Çok fazla rect varsa tüm ekranı kirlet
        comp->global_dirty_rects[0] = (Rect){0, 0, comp->screen_width, comp->screen_height};
        comp->global_dirty_count = 1;
        return;
    }
    Rect* r = &comp->global_dirty_rects[comp->global_dirty_count++];
    r->x = x; r->y = y; r->width = width; r->height = height;
    rect_clamp_to_screen(r, comp->screen_width, comp->screen_height);
}

void compositor_merge_dirty_rects(Compositor* comp) {
    bool merged = true;
    while (merged && comp->global_dirty_count > 1) {
        merged = false;
        for (int i = 0; i < comp->global_dirty_count - 1 && !merged; i++) {
            for (int j = i + 1; j < comp->global_dirty_count; j++) {
                if (!rect_intersect(&comp->global_dirty_rects[i],
                                    &comp->global_dirty_rects[j])) continue;

                Rect* r1 = &comp->global_dirty_rects[i];
                Rect* r2 = &comp->global_dirty_rects[j];
                int x1 = r1->x < r2->x ? r1->x : r2->x;
                int y1 = r1->y < r2->y ? r1->y : r2->y;
                int x2 = (r1->x + r1->width)  > (r2->x + r2->width)  ? (r1->x + r1->width)  : (r2->x + r2->width);
                int y2 = (r1->y + r1->height) > (r2->y + r2->height) ? (r1->y + r1->height) : (r2->y + r2->height);
                r1->x = x1; r1->y = y1; r1->width = x2 - x1; r1->height = y2 - y1;

                // j'yi listeden çıkar
                for (int k = j; k < comp->global_dirty_count - 1; k++)
                    comp->global_dirty_rects[k] = comp->global_dirty_rects[k + 1];
                comp->global_dirty_count--;
                merged = true;
                break;
            }
        }
    }
}

void compositor_clear_dirty_flags(Compositor* comp) {
    comp->global_dirty_count = 0;
    for (int i = 0; i < MAX_LAYERS; i++)
        if (comp->layers[i].active)
            comp->layers[i].dirty = false;
}

// ============================================================================
// RENDERING
// ============================================================================

// gui_blit_scanline: gui64 katmanı güçlü implementasyonu sağlarsa bu
// weak fallback kullanılmaz; sağlamazsa piksel-piksel fallback devreye girer.
__attribute__((weak))
void gui_blit_scanline(int screen_x, int screen_y,
                       const uint32_t* src, int count) {
    for (int i = 0; i < count; i++)
        gui_put_pixel(screen_x + i, screen_y, src[i]);
}

static void fast_blit_region(Compositor* comp, Layer* layer,
                             int clip_x, int clip_y, int clip_w, int clip_h) {
    int lx0 = clip_x - layer->bounds.x;
    int ly0 = clip_y - layer->bounds.y;

    for (int row = 0; row < clip_h; row++) {
        int ly = ly0 + row;
        int sy = clip_y + row;
        if (sy < 0 || sy >= comp->screen_height) continue;
        if (ly < 0 || ly >= layer->bounds.height) continue;

        int w = clip_w;
        if (clip_x + w > comp->screen_width)    w = comp->screen_width  - clip_x;
        if (lx0 + w   > layer->bounds.width)    w = layer->bounds.width - lx0;
        if (w <= 0) continue;

        gui_blit_scanline(clip_x, sy, layer->buffer + ly * layer->bounds.width + lx0, w);
    }
}

// Full render — tüm ekranı yeniden çizer (shadow dahil)
void compositor_render(Compositor* comp) {
    for (int z = 0; z < MAX_LAYERS; z++) {
        int idx = comp->z_sorted[z];
        if (idx < 0) break;

        Layer* layer = &comp->layers[idx];
        if (!layer->visible || !layer->active) continue;

        // Gölge
        if (layer->has_shadow) {
            int sx = layer->bounds.x + layer->shadow_offset_x;
            int sy = layer->bounds.y + layer->shadow_offset_y;
            for (int y = 0; y < layer->bounds.height; y++) {
                for (int x = 0; x < layer->bounds.width; x++) {
                    int px = sx + x, py = sy + y;
                    if (px < 0 || px >= comp->screen_width)  continue;
                    if (py < 0 || py >= comp->screen_height) continue;
                    Color bg  = gui_get_pixel(px, py);
                    Color out = alpha_blend(RGB(0,0,0), bg, layer->shadow_alpha);
                    gui_put_pixel(px, py, out);
                }
            }
        }

        // Layer içeriği
        if (layer->alpha < 255) {
            for (int y = 0; y < layer->bounds.height; y++) {
                for (int x = 0; x < layer->bounds.width; x++) {
                    int px = layer->bounds.x + x, py = layer->bounds.y + y;
                    if (px < 0 || px >= comp->screen_width)  continue;
                    if (py < 0 || py >= comp->screen_height) continue;
                    Color src = layer->buffer[y * layer->bounds.width + x];
                    Color dst = gui_get_pixel(px, py);
                    gui_put_pixel(px, py, alpha_blend(src, dst, layer->alpha));
                }
            }
        } else {
            fast_blit_region(comp, layer,
                             layer->bounds.x, layer->bounds.y,
                             layer->bounds.width, layer->bounds.height);
        }
    }
    compositor_clear_dirty_flags(comp);
}

// Dirty render — sadece işaretli bölgeleri günceller (resize/drag için)
void compositor_render_dirty(Compositor* comp) {
    if (comp->global_dirty_count == 0) return;

    compositor_merge_dirty_rects(comp);

    for (int di = 0; di < comp->global_dirty_count; di++) {
        Rect* dirty = &comp->global_dirty_rects[di];
        if (dirty->width <= 0 || dirty->height <= 0) continue;

        for (int z = 0; z < MAX_LAYERS; z++) {
            int idx = comp->z_sorted[z];
            if (idx < 0) break;

            Layer* layer = &comp->layers[idx];
            if (!layer->visible || !layer->active) continue;
            if (!rect_intersect(&layer->bounds, dirty)) continue;

            Rect clip = rect_intersection(&layer->bounds, dirty);
            if (clip.width <= 0 || clip.height <= 0) continue;

            if (layer->alpha < 255) {
                int lx0 = clip.x - layer->bounds.x;
                int ly0 = clip.y - layer->bounds.y;
                for (int row = 0; row < clip.height; row++) {
                    int ly = ly0 + row, sy = clip.y + row;
                    if (sy < 0 || sy >= comp->screen_height) continue;
                    for (int col = 0; col < clip.width; col++) {
                        int lx = lx0 + col, sx = clip.x + col;
                        if (sx < 0 || sx >= comp->screen_width) continue;
                        Color s = layer->buffer[ly * layer->bounds.width + lx];
                        Color d = gui_get_pixel(sx, sy);
                        gui_put_pixel(sx, sy, alpha_blend(s, d, layer->alpha));
                    }
                }
            } else {
                fast_blit_region(comp, layer, clip.x, clip.y, clip.width, clip.height);
            }
        }
    }
    compositor_clear_dirty_flags(comp);
}

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

bool rect_intersect(const Rect* r1, const Rect* r2) {
    return !(r1->x + r1->width  <= r2->x ||
             r2->x + r2->width  <= r1->x ||
             r1->y + r1->height <= r2->y ||
             r2->y + r2->height <= r1->y);
}

Rect rect_intersection(const Rect* r1, const Rect* r2) {
    int x1 = r1->x > r2->x ? r1->x : r2->x;
    int y1 = r1->y > r2->y ? r1->y : r2->y;
    int x2 = (r1->x + r1->width)  < (r2->x + r2->width)  ? (r1->x + r1->width)  : (r2->x + r2->width);
    int y2 = (r1->y + r1->height) < (r2->y + r2->height) ? (r1->y + r1->height) : (r2->y + r2->height);
    Rect r = {x1, y1, x2 - x1, y2 - y1};
    if (r.width  < 0) r.width  = 0;
    if (r.height < 0) r.height = 0;
    return r;
}

bool rect_is_valid(const Rect* r) {
    return r->width > 0 && r->height > 0;
}

void rect_clamp_to_screen(Rect* r, int sw, int sh) {
    if (r->x < 0) { r->width  += r->x; r->x = 0; }
    if (r->y < 0) { r->height += r->y; r->y = 0; }
    if (r->x + r->width  > sw) r->width  = sw - r->x;
    if (r->y + r->height > sh) r->height = sh - r->y;
    if (r->width  < 0) r->width  = 0;
    if (r->height < 0) r->height = 0;
}

// ============================================================================
// DRAWING PRIMITIVES
// ============================================================================

void layer_fill_rect(Layer* layer, int x, int y, int width, int height, Color color) {
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + width;  if (x1 > layer->bounds.width)  x1 = layer->bounds.width;
    int y1 = y + height; if (y1 > layer->bounds.height) y1 = layer->bounds.height;
    if (x0 >= x1 || y0 >= y1) return;

    int len = x1 - x0;
    for (int py = y0; py < y1; py++) {
        uint32_t* row = layer->buffer + py * layer->bounds.width + x0;
        for (int i = 0; i < len; i++) row[i] = color;
    }
}

void layer_draw_string(Layer* layer, int x, int y, const char* str,
                       Color fg, Color bg) {
    int w = layer->bounds.width;
    int h = layer->bounds.height;
    int cx = x;
    while (*str) {
        char c = *str++;
        for (int row = 0; row < 8; row++) {
            int py = y + row;
            if (py < 0 || py >= h) continue;
            uint8_t glyph = gui_font_row(c, row);
            for (int col = 0; col < 8; col++) {
                int px = cx + col;
                if (px < 0 || px >= w) continue;
                layer->buffer[py * w + px] = (glyph & (1 << (7 - col))) ? fg : bg;
            }
        }
        cx += 8;
    }
}

void layer_blit(Layer* layer, int x, int y, const uint32_t* pixels,
                int width, int height) {
    for (int py = 0; py < height; py++) {
        int dy = y + py;
        if (dy < 0 || dy >= layer->bounds.height) continue;
        for (int px = 0; px < width; px++) {
            int dx = x + px;
            if (dx < 0 || dx >= layer->bounds.width) continue;
            layer->buffer[dy * layer->bounds.width + dx] = pixels[py * width + px];
        }
    }
}

// ============================================================================
// ALPHA BLENDING & SHADOW
// ============================================================================

void compositor_set_layer_alpha(Compositor* comp, int layer_index, uint8_t alpha) {
    if (layer_index < 0 || layer_index >= MAX_LAYERS) return;
    if (!comp->layers[layer_index].active) return;
    comp->layers[layer_index].alpha = alpha;
    compositor_mark_layer_dirty(comp, layer_index);
}

void compositor_set_layer_shadow(Compositor* comp, int layer_index,
                                bool enabled, int offset_x, int offset_y,
                                uint8_t shadow_alpha, int blur_radius) {
    if (layer_index < 0 || layer_index >= MAX_LAYERS) return;
    if (!comp->layers[layer_index].active) return;

    Layer* layer = &comp->layers[layer_index];
    layer->has_shadow        = enabled;
    layer->shadow_offset_x   = offset_x;
    layer->shadow_offset_y   = offset_y;
    layer->shadow_alpha      = shadow_alpha;
    layer->shadow_blur_radius = blur_radius;
    compositor_mark_layer_dirty(comp, layer_index);
}

Color alpha_blend(Color fg, Color bg, uint8_t alpha) {
    if (alpha == 255) return fg;
    if (alpha == 0)   return bg;
    uint8_t r = (GET_RED(fg)   * alpha + GET_RED(bg)   * (255 - alpha)) / 255;
    uint8_t g = (GET_GREEN(fg) * alpha + GET_GREEN(bg) * (255 - alpha)) / 255;
    uint8_t b = (GET_BLUE(fg)  * alpha + GET_BLUE(bg)  * (255 - alpha)) / 255;
    return RGB(r, g, b);
}