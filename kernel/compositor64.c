// ============================================================================
// compositor64.c - Compositing Window Manager Implementation
// ============================================================================
#include "compositor64.h"
#include <stddef.h>

// External memory allocation (simple for now)
extern void* malloc(size_t size);
extern void free(void* ptr);

// Memory helpers
static void* comp_malloc(size_t size) {
    // Simple allocation at 6MB for now
    static uint8_t* heap = (uint8_t*)0x600000;
    static size_t offset = 0;
    
    void* ptr = heap + offset;
    offset += size;
    return ptr;
}

// ============================================================================
// INITIALIZATION
// ============================================================================

void compositor_init(Compositor* comp, int width, int height, Color desktop_color) {
    comp->screen_width = width;
    comp->screen_height = height;
    comp->desktop_color = desktop_color;
    comp->layer_count = 0;
    comp->global_dirty_count = 0;
    comp->use_composition_buffer = false;
    comp->composition_buffer = NULL;
    
    // Initialize all layers as inactive
    for (int i = 0; i < MAX_LAYERS; i++) {
        comp->layers[i].active = false;
        comp->layers[i].visible = false;
        comp->layers[i].dirty = false;
        comp->layers[i].buffer = NULL;
        comp->layers[i].window_id = -1;
        comp->layers[i].z_order = 0;
        comp->layers[i].dirty_rect_count = 0;
        comp->z_sorted[i] = -1;
    }
    
    // Create desktop layer (always layer 0)
    int desktop_idx = compositor_create_layer(comp, LAYER_TYPE_DESKTOP, 
                                             0, 0, width, height - 40);
    if (desktop_idx >= 0) {
        // Fill desktop with color
        Layer* desktop = &comp->layers[desktop_idx];
        for (int i = 0; i < width * (height - 40); i++) {
            desktop->buffer[i] = desktop_color;
        }
        desktop->visible = true;
        desktop->z_order = 0;
    }
    
    compositor_rebuild_z_order(comp);
}

void compositor_shutdown(Compositor* comp) {
    // Free all layer buffers
    for (int i = 0; i < MAX_LAYERS; i++) {
        if (comp->layers[i].active && comp->layers[i].buffer) {
            // Note: In real implementation, free() would be used
            comp->layers[i].buffer = NULL;
        }
    }
    
    if (comp->composition_buffer) {
        comp->composition_buffer = NULL;
    }
}

// ============================================================================
// LAYER MANAGEMENT
// ============================================================================

int compositor_create_layer(Compositor* comp, LayerType type, int x, int y, 
                            int width, int height) {
    // Find free layer slot
    int idx = -1;
    for (int i = 0; i < MAX_LAYERS; i++) {
        if (!comp->layers[i].active) {
            idx = i;
            break;
        }
    }
    
    if (idx < 0) return -1;  // No free slots
    
    Layer* layer = &comp->layers[idx];
    
    // Allocate buffer
    size_t buffer_size = width * height * sizeof(uint32_t);
    layer->buffer = (uint32_t*)comp_malloc(buffer_size);
    
    if (!layer->buffer) return -1;
    
    // Initialize layer
    layer->active = true;
    layer->visible = true;
    layer->dirty = true;
    layer->type = type;
    layer->bounds.x = x;
    layer->bounds.y = y;
    layer->bounds.width = width;
    layer->bounds.height = height;
    layer->window_id = -1;
    layer->dirty_rect_count = 0;
    
    // Phase 2: Alpha & Shadow defaults
    layer->alpha = 255;              // Fully opaque by default
    layer->has_shadow = false;       // No shadow by default
    layer->shadow_offset_x = 4;
    layer->shadow_offset_y = 4;
    layer->shadow_alpha = 128;       // 50% shadow opacity
    layer->shadow_blur_radius = 4;   // Moderate blur
    
    // Set default z-order based on type
    switch (type) {
        case LAYER_TYPE_DESKTOP:
            layer->z_order = 0;
            layer->has_shadow = false;  // Desktop never has shadow
            break;
        case LAYER_TYPE_WINDOW:
            layer->z_order = 100 + comp->layer_count;
            layer->has_shadow = true;   // Windows have shadows by default
            break;
        case LAYER_TYPE_TASKBAR:
            layer->z_order = 900;
            layer->has_shadow = false;  // Taskbar no shadow
            break;
        case LAYER_TYPE_CURSOR:
            layer->z_order = 1000;
            layer->has_shadow = false;  // Cursor no shadow
            break;
    }
    
    comp->layer_count++;
    
    // Clear buffer to transparent/black
    for (int i = 0; i < width * height; i++) {
        layer->buffer[i] = 0x00000000;
    }
    
    return idx;
}

void compositor_destroy_layer(Compositor* comp, int layer_index) {
    if (layer_index < 0 || layer_index >= MAX_LAYERS) return;
    if (!comp->layers[layer_index].active) return;
    
    Layer* layer = &comp->layers[layer_index];
    
    // Mark region as dirty
    compositor_add_global_dirty_rect(comp, layer->bounds.x, layer->bounds.y,
                                    layer->bounds.width, layer->bounds.height);
    
    // Free buffer (in real implementation)
    layer->buffer = NULL;
    layer->active = false;
    layer->visible = false;
    
    comp->layer_count--;
    
    compositor_rebuild_z_order(comp);
}

int compositor_get_layer_by_window(Compositor* comp, int window_id) {
    for (int i = 0; i < MAX_LAYERS; i++) {
        if (comp->layers[i].active && comp->layers[i].window_id == window_id) {
            return i;
        }
    }
    return -1;
}

void compositor_set_layer_visible(Compositor* comp, int layer_index, bool visible) {
    if (layer_index < 0 || layer_index >= MAX_LAYERS) return;
    if (!comp->layers[layer_index].active) return;
    
    Layer* layer = &comp->layers[layer_index];
    
    if (layer->visible != visible) {
        layer->visible = visible;
        // Mark layer region as dirty
        compositor_add_global_dirty_rect(comp, layer->bounds.x, layer->bounds.y,
                                        layer->bounds.width, layer->bounds.height);
    }
}

void compositor_move_layer(Compositor* comp, int layer_index, int x, int y) {
    if (layer_index < 0 || layer_index >= MAX_LAYERS) return;
    if (!comp->layers[layer_index].active) return;
    
    Layer* layer = &comp->layers[layer_index];
    
    // Mark old position as dirty
    compositor_add_global_dirty_rect(comp, layer->bounds.x, layer->bounds.y,
                                    layer->bounds.width, layer->bounds.height);
    
    // Update position
    layer->bounds.x = x;
    layer->bounds.y = y;
    
    // Mark new position as dirty
    compositor_add_global_dirty_rect(comp, x, y,
                                    layer->bounds.width, layer->bounds.height);
}

void compositor_resize_layer(Compositor* comp, int layer_index, int width, int height) {
    if (layer_index < 0 || layer_index >= MAX_LAYERS) return;
    if (!comp->layers[layer_index].active) return;
    
    Layer* layer = &comp->layers[layer_index];
    
    // Mark old size as dirty
    compositor_add_global_dirty_rect(comp, layer->bounds.x, layer->bounds.y,
                                    layer->bounds.width, layer->bounds.height);
    
    // Reallocate buffer
    size_t new_size = width * height * sizeof(uint32_t);
    uint32_t* new_buffer = (uint32_t*)comp_malloc(new_size);
    
    if (new_buffer) {
        // Clear new buffer
        for (int i = 0; i < width * height; i++) {
            new_buffer[i] = 0x00000000;
        }
        
        // Copy old content if possible
        int copy_width = (width < layer->bounds.width) ? width : layer->bounds.width;
        int copy_height = (height < layer->bounds.height) ? height : layer->bounds.height;
        
        for (int y = 0; y < copy_height; y++) {
            for (int x = 0; x < copy_width; x++) {
                new_buffer[y * width + x] = layer->buffer[y * layer->bounds.width + x];
            }
        }
        
        layer->buffer = new_buffer;
        layer->bounds.width = width;
        layer->bounds.height = height;
        layer->dirty = true;
    }
    
    // Mark new size as dirty
    compositor_add_global_dirty_rect(comp, layer->bounds.x, layer->bounds.y,
                                    width, height);
}

// ============================================================================
// Z-ORDER MANAGEMENT
// ============================================================================

void compositor_rebuild_z_order(Compositor* comp) {
    // Simple bubble sort by z_order
    for (int i = 0; i < MAX_LAYERS; i++) {
        comp->z_sorted[i] = -1;
    }
    
    int sorted_count = 0;
    
    // Collect active layers
    for (int i = 0; i < MAX_LAYERS; i++) {
        if (comp->layers[i].active) {
            comp->z_sorted[sorted_count++] = i;
        }
    }
    
    // Sort by z_order
    for (int i = 0; i < sorted_count - 1; i++) {
        for (int j = i + 1; j < sorted_count; j++) {
            int idx_i = comp->z_sorted[i];
            int idx_j = comp->z_sorted[j];
            
            if (comp->layers[idx_i].z_order > comp->layers[idx_j].z_order) {
                // Swap
                comp->z_sorted[i] = idx_j;
                comp->z_sorted[j] = idx_i;
            }
        }
    }
}

void compositor_bring_to_front(Compositor* comp, int layer_index) {
    if (layer_index < 0 || layer_index >= MAX_LAYERS) return;
    if (!comp->layers[layer_index].active) return;
    
    Layer* layer = &comp->layers[layer_index];
    
    // Find highest z_order (excluding cursor/taskbar)
    int max_z = 0;
    for (int i = 0; i < MAX_LAYERS; i++) {
        if (comp->layers[i].active && 
            comp->layers[i].type == LAYER_TYPE_WINDOW &&
            i != layer_index) {
            if (comp->layers[i].z_order > max_z) {
                max_z = comp->layers[i].z_order;
            }
        }
    }
    
    layer->z_order = max_z + 1;
    
    // Ensure taskbar and cursor stay on top
    for (int i = 0; i < MAX_LAYERS; i++) {
        if (comp->layers[i].active) {
            if (comp->layers[i].type == LAYER_TYPE_TASKBAR) {
                comp->layers[i].z_order = 900;
            } else if (comp->layers[i].type == LAYER_TYPE_CURSOR) {
                comp->layers[i].z_order = 1000;
            }
        }
    }
    
    compositor_rebuild_z_order(comp);
    compositor_mark_layer_dirty(comp, layer_index);
}

void compositor_send_to_back(Compositor* comp, int layer_index) {
    if (layer_index < 0 || layer_index >= MAX_LAYERS) return;
    if (!comp->layers[layer_index].active) return;
    if (comp->layers[layer_index].type != LAYER_TYPE_WINDOW) return;
    
    comp->layers[layer_index].z_order = 1;
    compositor_rebuild_z_order(comp);
    compositor_mark_layer_dirty(comp, layer_index);
}

void compositor_raise_layer(Compositor* comp, int layer_index) {
    if (layer_index < 0 || layer_index >= MAX_LAYERS) return;
    comp->layers[layer_index].z_order += 1;
    compositor_rebuild_z_order(comp);
}

void compositor_lower_layer(Compositor* comp, int layer_index) {
    if (layer_index < 0 || layer_index >= MAX_LAYERS) return;
    if (comp->layers[layer_index].z_order > 1) {
        comp->layers[layer_index].z_order -= 1;
    }
    compositor_rebuild_z_order(comp);
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

void compositor_mark_rect_dirty(Compositor* comp, int layer_index,
                               int x, int y, int width, int height) {
    if (layer_index < 0 || layer_index >= MAX_LAYERS) return;
    if (!comp->layers[layer_index].active) return;
    
    Layer* layer = &comp->layers[layer_index];
    
    // Add to layer's dirty rects
    if (layer->dirty_rect_count < MAX_DIRTY_RECTS) {
        Rect* rect = &layer->dirty_rects[layer->dirty_rect_count++];
        rect->x = x;
        rect->y = y;
        rect->width = width;
        rect->height = height;
    }
    
    layer->dirty = true;
    
    // Add to global dirty rects (in screen space)
    compositor_add_global_dirty_rect(comp, layer->bounds.x + x, layer->bounds.y + y,
                                    width, height);
}

void compositor_add_global_dirty_rect(Compositor* comp, int x, int y,
                                     int width, int height) {
    if (comp->global_dirty_count >= MAX_DIRTY_RECTS) {
        // If too many dirty rects, just mark entire screen dirty
        comp->global_dirty_rects[0].x = 0;
        comp->global_dirty_rects[0].y = 0;
        comp->global_dirty_rects[0].width = comp->screen_width;
        comp->global_dirty_rects[0].height = comp->screen_height;
        comp->global_dirty_count = 1;
        return;
    }
    
    Rect* rect = &comp->global_dirty_rects[comp->global_dirty_count++];
    rect->x = x;
    rect->y = y;
    rect->width = width;
    rect->height = height;
    
    // Clamp to screen
    rect_clamp_to_screen(rect, comp->screen_width, comp->screen_height);
}

void compositor_merge_dirty_rects(Compositor* comp) {
    // Simple merge: combine overlapping rectangles
    // This is a basic implementation - can be optimized
    
    bool merged = true;
    while (merged && comp->global_dirty_count > 1) {
        merged = false;
        
        for (int i = 0; i < comp->global_dirty_count - 1; i++) {
            for (int j = i + 1; j < comp->global_dirty_count; j++) {
                if (rect_intersect(&comp->global_dirty_rects[i],
                                  &comp->global_dirty_rects[j])) {
                    // Merge j into i
                    Rect* r1 = &comp->global_dirty_rects[i];
                    Rect* r2 = &comp->global_dirty_rects[j];
                    
                    int x1 = (r1->x < r2->x) ? r1->x : r2->x;
                    int y1 = (r1->y < r2->y) ? r1->y : r2->y;
                    int x2 = ((r1->x + r1->width) > (r2->x + r2->width)) ?
                             (r1->x + r1->width) : (r2->x + r2->width);
                    int y2 = ((r1->y + r1->height) > (r2->y + r2->height)) ?
                             (r1->y + r1->height) : (r2->y + r2->height);
                    
                    r1->x = x1;
                    r1->y = y1;
                    r1->width = x2 - x1;
                    r1->height = y2 - y1;
                    
                    // Remove j
                    for (int k = j; k < comp->global_dirty_count - 1; k++) {
                        comp->global_dirty_rects[k] = comp->global_dirty_rects[k + 1];
                    }
                    comp->global_dirty_count--;
                    merged = true;
                    break;
                }
            }
            if (merged) break;
        }
    }
}

void compositor_clear_dirty_flags(Compositor* comp) {
    comp->global_dirty_count = 0;
    
    for (int i = 0; i < MAX_LAYERS; i++) {
        if (comp->layers[i].active) {
            comp->layers[i].dirty = false;
            comp->layers[i].dirty_rect_count = 0;
        }
    }
}

// ============================================================================
// RENDERING
// ============================================================================

void compositor_render(Compositor* comp) {
    // Full screen composite - render all visible layers in z-order
    
    for (int z_idx = 0; z_idx < MAX_LAYERS; z_idx++) {
        int layer_idx = comp->z_sorted[z_idx];
        if (layer_idx < 0) break;
        
        Layer* layer = &comp->layers[layer_idx];
        if (!layer->visible) continue;
        
        // Render with effects (shadow + alpha)
        compositor_render_layer_with_effects(comp, layer_idx, 0, 0);
    }
    
    compositor_clear_dirty_flags(comp);
}

void compositor_render_dirty(Compositor* comp) {
    // Optimized render - only update dirty rectangles
    
    if (comp->global_dirty_count == 0) return;
    
    // Merge overlapping rects
    compositor_merge_dirty_rects(comp);
    
    // For each dirty rectangle
    for (int dirty_idx = 0; dirty_idx < comp->global_dirty_count; dirty_idx++) {
        Rect* dirty = &comp->global_dirty_rects[dirty_idx];
        
        // Composite all layers in z-order within this dirty rect
        for (int z_idx = 0; z_idx < MAX_LAYERS; z_idx++) {
            int layer_idx = comp->z_sorted[z_idx];
            if (layer_idx < 0) break;
            
            Layer* layer = &comp->layers[layer_idx];
            if (!layer->visible) continue;
            
            // Check if layer intersects dirty rect
            if (!rect_intersect(&layer->bounds, dirty)) continue;
            
            // Get intersection
            Rect clip = rect_intersection(&layer->bounds, dirty);
            
            // Blit clipped region
            for (int y = clip.y; y < clip.y + clip.height; y++) {
                for (int x = clip.x; x < clip.x + clip.width; x++) {
                    // Convert screen coords to layer coords
                    int layer_x = x - layer->bounds.x;
                    int layer_y = y - layer->bounds.y;
                    
                    if (layer_x >= 0 && layer_x < layer->bounds.width &&
                        layer_y >= 0 && layer_y < layer->bounds.height) {
                        
                        uint32_t pixel = layer->buffer[layer_y * layer->bounds.width + layer_x];
                        gui_put_pixel(x, y, pixel);
                    }
                }
            }
        }
    }
    
    compositor_clear_dirty_flags(comp);
}

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

bool rect_intersect(const Rect* r1, const Rect* r2) {
    return !(r1->x + r1->width <= r2->x ||
             r2->x + r2->width <= r1->x ||
             r1->y + r1->height <= r2->y ||
             r2->y + r2->height <= r1->y);
}

Rect rect_intersection(const Rect* r1, const Rect* r2) {
    Rect result;
    result.x = (r1->x > r2->x) ? r1->x : r2->x;
    result.y = (r1->y > r2->y) ? r1->y : r2->y;
    
    int x2 = ((r1->x + r1->width) < (r2->x + r2->width)) ?
             (r1->x + r1->width) : (r2->x + r2->width);
    int y2 = ((r1->y + r1->height) < (r2->y + r2->height)) ?
             (r1->y + r1->height) : (r2->y + r2->height);
    
    result.width = x2 - result.x;
    result.height = y2 - result.y;
    
    if (result.width < 0) result.width = 0;
    if (result.height < 0) result.height = 0;
    
    return result;
}

bool rect_is_valid(const Rect* r) {
    return r->width > 0 && r->height > 0;
}

void rect_clamp_to_screen(Rect* r, int screen_width, int screen_height) {
    if (r->x < 0) {
        r->width += r->x;
        r->x = 0;
    }
    if (r->y < 0) {
        r->height += r->y;
        r->y = 0;
    }
    if (r->x + r->width > screen_width) {
        r->width = screen_width - r->x;
    }
    if (r->y + r->height > screen_height) {
        r->height = screen_height - r->y;
    }
    if (r->width < 0) r->width = 0;
    if (r->height < 0) r->height = 0;
}

// ============================================================================
// LAYER DRAWING PRIMITIVES
// ============================================================================

void layer_fill_rect(Layer* layer, int x, int y, int width, int height, Color color) {
    for (int py = y; py < y + height && py < layer->bounds.height; py++) {
        for (int px = x; px < x + width && px < layer->bounds.width; px++) {
            if (px >= 0 && py >= 0) {
                layer->buffer[py * layer->bounds.width + px] = color;
            }
        }
    }
}

void layer_draw_string(Layer* layer, int x, int y, const char* str, Color fg, Color bg) {
    // Simple implementation - would use font rendering
    // For now, just mark as dirty
    (void)layer;
    (void)x;
    (void)y;
    (void)str;
    (void)fg;
    (void)bg;
}

void layer_blit(Layer* layer, int x, int y, const uint32_t* pixels,
                int width, int height) {
    for (int py = 0; py < height; py++) {
        for (int px = 0; px < width; px++) {
            int dst_x = x + px;
            int dst_y = y + py;
            
            if (dst_x >= 0 && dst_x < layer->bounds.width &&
                dst_y >= 0 && dst_y < layer->bounds.height) {
                layer->buffer[dst_y * layer->bounds.width + dst_x] =
                    pixels[py * width + px];
            }
        }
    }
}

// ============================================================================
// PHASE 2: ALPHA BLENDING & EFFECTS IMPLEMENTATION
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
    layer->has_shadow = enabled;
    layer->shadow_offset_x = offset_x;
    layer->shadow_offset_y = offset_y;
    layer->shadow_alpha = shadow_alpha;
    layer->shadow_blur_radius = blur_radius;
    
    // Mark layer and shadow area as dirty
    compositor_mark_layer_dirty(comp, layer_index);
    if (enabled) {
        // Also mark shadow area
        compositor_add_global_dirty_rect(comp,
            layer->bounds.x + offset_x,
            layer->bounds.y + offset_y,
            layer->bounds.width + blur_radius * 2,
            layer->bounds.height + blur_radius * 2);
    }
}

// Alpha blend two colors
Color alpha_blend(Color fg, Color bg, uint8_t alpha) {
    if (alpha == 255) return fg;
    if (alpha == 0) return bg;
    
    uint8_t fg_r = GET_RED(fg);
    uint8_t fg_g = GET_GREEN(fg);
    uint8_t fg_b = GET_BLUE(fg);
    
    uint8_t bg_r = GET_RED(bg);
    uint8_t bg_g = GET_GREEN(bg);
    uint8_t bg_b = GET_BLUE(bg);
    
    // Standard alpha blending formula: result = fg * alpha + bg * (1 - alpha)
    uint8_t out_r = (fg_r * alpha + bg_r * (255 - alpha)) / 255;
    uint8_t out_g = (fg_g * alpha + bg_g * (255 - alpha)) / 255;
    uint8_t out_b = (fg_b * alpha + bg_b * (255 - alpha)) / 255;
    
    return RGB(out_r, out_g, out_b);
}

// Simple box blur (fast but not highest quality)
void box_blur(uint32_t* buffer, int width, int height, int radius) {
    if (radius <= 0) return;
    
    // Horizontal pass
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int r_sum = 0, g_sum = 0, b_sum = 0;
            int count = 0;
            
            for (int dx = -radius; dx <= radius; dx++) {
                int sample_x = x + dx;
                if (sample_x >= 0 && sample_x < width) {
                    Color pixel = buffer[y * width + sample_x];
                    r_sum += GET_RED(pixel);
                    g_sum += GET_GREEN(pixel);
                    b_sum += GET_BLUE(pixel);
                    count++;
                }
            }
            
            buffer[y * width + x] = RGB(r_sum / count, g_sum / count, b_sum / count);
        }
    }
    
    // Vertical pass
    for (int x = 0; x < width; x++) {
        for (int y = 0; y < height; y++) {
            int r_sum = 0, g_sum = 0, b_sum = 0;
            int count = 0;
            
            for (int dy = -radius; dy <= radius; dy++) {
                int sample_y = y + dy;
                if (sample_y >= 0 && sample_y < height) {
                    Color pixel = buffer[sample_y * width + x];
                    r_sum += GET_RED(pixel);
                    g_sum += GET_GREEN(pixel);
                    b_sum += GET_BLUE(pixel);
                    count++;
                }
            }
            
            buffer[y * width + x] = RGB(r_sum / count, g_sum / count, b_sum / count);
        }
    }
}

// Generate shadow buffer for a layer
uint32_t* generate_shadow_buffer(const Layer* layer) {
    int shadow_width = layer->bounds.width + layer->shadow_blur_radius * 2;
    int shadow_height = layer->bounds.height + layer->shadow_blur_radius * 2;
    
    // Allocate shadow buffer (temp allocation at 7MB)
    static uint8_t* shadow_heap = (uint8_t*)0x700000;
    static size_t shadow_offset = 0;
    
    uint32_t* shadow_buffer = (uint32_t*)(shadow_heap + shadow_offset);
    size_t shadow_size = shadow_width * shadow_height * sizeof(uint32_t);
    shadow_offset += shadow_size;
    
    // Clear shadow buffer
    for (int i = 0; i < shadow_width * shadow_height; i++) {
        shadow_buffer[i] = 0x00000000;
    }
    
    // Create shadow shape from layer's non-transparent pixels
    int blur_offset = layer->shadow_blur_radius;
    for (int y = 0; y < layer->bounds.height; y++) {
        for (int x = 0; x < layer->bounds.width; x++) {
            Color pixel = layer->buffer[y * layer->bounds.width + x];
            
            // If pixel is not fully transparent, add to shadow
            if (pixel != 0x00000000) {
                int shadow_x = x + blur_offset;
                int shadow_y = y + blur_offset;
                shadow_buffer[shadow_y * shadow_width + shadow_x] = 
                    RGB(0, 0, 0);  // Black shadow
            }
        }
    }
    
    // Apply blur
    box_blur(shadow_buffer, shadow_width, shadow_height, layer->shadow_blur_radius);
    
    return shadow_buffer;
}

// Render layer with all effects
void compositor_render_layer_with_effects(Compositor* comp, int layer_index,
                                         int screen_x, int screen_y) {
    (void)screen_x;
    (void)screen_y;
    
    if (layer_index < 0 || layer_index >= MAX_LAYERS) return;
    
    Layer* layer = &comp->layers[layer_index];
    if (!layer->visible || !layer->active) return;
    
    // 1. Draw shadow first (if enabled)
    if (layer->has_shadow) {
        int shadow_x = layer->bounds.x + layer->shadow_offset_x;
        int shadow_y = layer->bounds.y + layer->shadow_offset_y;
        
        // Simple shadow: just darkened rectangle for now
        // Full implementation would use generate_shadow_buffer()
        for (int y = 0; y < layer->bounds.height; y++) {
            for (int x = 0; x < layer->bounds.width; x++) {
                int px = shadow_x + x;
                int py = shadow_y + y;
                
                if (px >= 0 && px < comp->screen_width &&
                    py >= 0 && py < comp->screen_height) {
                    
                    Color bg = gui_get_pixel(px, py);
                    Color shadow = RGB(0, 0, 0);
                    Color blended = alpha_blend(shadow, bg, layer->shadow_alpha);
                    gui_put_pixel(px, py, blended);
                }
            }
        }
    }
    
    // 2. Draw layer with alpha blending
    for (int y = 0; y < layer->bounds.height; y++) {
        for (int x = 0; x < layer->bounds.width; x++) {
            int screen_px = layer->bounds.x + x;
            int screen_py = layer->bounds.y + y;
            
            if (screen_px >= 0 && screen_px < comp->screen_width &&
                screen_py >= 0 && screen_py < comp->screen_height) {
                
                Color layer_pixel = layer->buffer[y * layer->bounds.width + x];
                
                // Apply layer alpha
                if (layer->alpha < 255) {
                    Color bg = gui_get_pixel(screen_px, screen_py);
                    layer_pixel = alpha_blend(layer_pixel, bg, layer->alpha);
                }
                
                gui_put_pixel(screen_px, screen_py, layer_pixel);
            }
        }
    }
}