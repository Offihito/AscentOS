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

#define MAX_LAYERS 16
#define MAX_DIRTY_RECTS 32

// Rectangle structure for dirty regions
typedef struct {
    int x, y;
    int width, height;
} Rect;

// Layer types
typedef enum {
    LAYER_TYPE_DESKTOP,      // Background layer (lowest)
    LAYER_TYPE_WINDOW,       // Normal window
    LAYER_TYPE_TASKBAR,      // Taskbar (always on top)
    LAYER_TYPE_CURSOR        // Cursor (highest)
} LayerType;

// Single compositing layer
typedef struct {
    bool active;                    // Is this layer slot in use?
    bool visible;                   // Is this layer visible?
    bool dirty;                     // Does this layer need redraw?
    LayerType type;                 // Layer type
    int z_order;                    // Z-order (lower = background, higher = foreground)
    
    Rect bounds;                    // Layer position and size
    uint32_t* buffer;               // Layer pixel buffer (RGBA or RGB)
    
    // For windows
    int window_id;                  // Associated window ID (-1 if not a window)
    
    // Dirty rectangles for this layer
    Rect dirty_rects[MAX_DIRTY_RECTS];
    int dirty_rect_count;
    
    // *** PHASE 2: ALPHA BLENDING & EFFECTS ***
    uint8_t alpha;                  // Layer opacity (0=transparent, 255=opaque)
    bool has_shadow;                // Does this layer cast a shadow?
    int shadow_offset_x;            // Shadow offset X
    int shadow_offset_y;            // Shadow offset Y
    uint8_t shadow_alpha;           // Shadow opacity
    int shadow_blur_radius;         // Shadow blur amount (0=sharp, higher=softer)
} Layer;

// ============================================================================
// COMPOSITOR STATE
// ============================================================================

typedef struct {
    Layer layers[MAX_LAYERS];
    int layer_count;
    
    // Z-order sorted indices (layers[z_sorted[0]] is bottom-most)
    int z_sorted[MAX_LAYERS];
    
    // Global dirty rectangles (union of all layer dirty rects)
    Rect global_dirty_rects[MAX_DIRTY_RECTS];
    int global_dirty_count;
    
    // Screen dimensions
    int screen_width;
    int screen_height;
    
    // Desktop layer (always index 0)
    Color desktop_color;
    
    // Composition buffer (optional, for effects)
    uint32_t* composition_buffer;
    bool use_composition_buffer;
} Compositor;

// ============================================================================
// CORE FUNCTIONS
// ============================================================================

// Initialize compositor
void compositor_init(Compositor* comp, int width, int height, Color desktop_color);

// Shutdown compositor
void compositor_shutdown(Compositor* comp);

// ============================================================================
// LAYER MANAGEMENT
// ============================================================================

// Create a new layer and return its index (-1 on failure)
int compositor_create_layer(Compositor* comp, LayerType type, int x, int y, 
                            int width, int height);

// Destroy a layer
void compositor_destroy_layer(Compositor* comp, int layer_index);

// Get layer by window ID
int compositor_get_layer_by_window(Compositor* comp, int window_id);

// Set layer visibility
void compositor_set_layer_visible(Compositor* comp, int layer_index, bool visible);

// Move layer
void compositor_move_layer(Compositor* comp, int layer_index, int x, int y);

// Resize layer (reallocates buffer)
void compositor_resize_layer(Compositor* comp, int layer_index, int width, int height);

// ============================================================================
// Z-ORDER MANAGEMENT
// ============================================================================

// Bring layer to front
void compositor_bring_to_front(Compositor* comp, int layer_index);

// Send layer to back
void compositor_send_to_back(Compositor* comp, int layer_index);

// Raise layer (swap with layer above)
void compositor_raise_layer(Compositor* comp, int layer_index);

// Lower layer (swap with layer below)
void compositor_lower_layer(Compositor* comp, int layer_index);

// Rebuild z-order sorted array
void compositor_rebuild_z_order(Compositor* comp);

// ============================================================================
// DIRTY RECTANGLE TRACKING
// ============================================================================

// Mark entire layer as dirty
void compositor_mark_layer_dirty(Compositor* comp, int layer_index);

// Mark specific rectangle as dirty on a layer
void compositor_mark_rect_dirty(Compositor* comp, int layer_index, 
                                int x, int y, int width, int height);

// Add dirty rect to global list
void compositor_add_global_dirty_rect(Compositor* comp, int x, int y, 
                                     int width, int height);

// Merge overlapping dirty rectangles (optimization)
void compositor_merge_dirty_rects(Compositor* comp);

// Clear all dirty flags
void compositor_clear_dirty_flags(Compositor* comp);

// ============================================================================
// RENDERING
// ============================================================================

// Composite all layers and render to screen
void compositor_render(Compositor* comp);

// Composite only dirty regions (optimized)
void compositor_render_dirty(Compositor* comp);

// Draw a single layer to its buffer
void compositor_draw_layer(Compositor* comp, int layer_index);

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

// Check if two rectangles intersect
bool rect_intersect(const Rect* r1, const Rect* r2);

// Get intersection of two rectangles
Rect rect_intersection(const Rect* r1, const Rect* r2);

// Check if rectangle is valid (positive width/height)
bool rect_is_valid(const Rect* r);

// Clamp rectangle to screen bounds
void rect_clamp_to_screen(Rect* r, int screen_width, int screen_height);

// ============================================================================
// DRAWING PRIMITIVES (to layer buffer)
// ============================================================================

// Fill rectangle in layer buffer
void layer_fill_rect(Layer* layer, int x, int y, int width, int height, Color color);

// Draw string in layer buffer
void layer_draw_string(Layer* layer, int x, int y, const char* str, Color fg, Color bg);

// Copy pixels to layer buffer
void layer_blit(Layer* layer, int x, int y, const uint32_t* pixels, 
                int width, int height);

// ============================================================================
// PHASE 2: ALPHA BLENDING & EFFECTS
// ============================================================================

// Set layer opacity (0-255)
void compositor_set_layer_alpha(Compositor* comp, int layer_index, uint8_t alpha);

// Enable/disable shadow for a layer
void compositor_set_layer_shadow(Compositor* comp, int layer_index, 
                                bool enabled, int offset_x, int offset_y,
                                uint8_t shadow_alpha, int blur_radius);

// Alpha blend two colors (foreground over background)
Color alpha_blend(Color fg, Color bg, uint8_t alpha);

// Generate shadow buffer for a layer (cached)
uint32_t* generate_shadow_buffer(const Layer* layer);

// Apply box blur to buffer (for shadow softening)
void box_blur(uint32_t* buffer, int width, int height, int radius);

// Render layer with alpha blending and shadow
void compositor_render_layer_with_effects(Compositor* comp, int layer_index,
                                         int screen_x, int screen_y);

#endif // COMPOSITOR64_H