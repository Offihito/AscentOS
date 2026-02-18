// ============================================================================
// wm64.h - SerenityOS Style Window Manager
// ============================================================================
#ifndef WM64_H
#define WM64_H

#include <stdbool.h>
#include "compositor64.h"
#include "taskbar64.h"

// SerenityOS style constants
#define TITLE_BAR_HEIGHT 28
#define WM_BUTTON_WIDTH  20
#define WM_BUTTON_HEIGHT 18
#define WM_BUTTON_GAP    2
#define WM_RIGHT_MARGIN  4
#define WM_LEFT_MARGIN   4
#define MAX_WINDOWS      8
#define BORDER_WIDTH     2

// SerenityOS color palette
#define COLOR_WINDOW_BASE       RGB(192, 192, 192)  // Light gray
#define COLOR_WINDOW_FRAME      RGB(212, 208, 200)  // Window frame
#define COLOR_TITLE_ACTIVE      RGB(0, 0, 168)      // Active title bar (blue)
#define COLOR_TITLE_INACTIVE    RGB(128, 128, 128)  // Inactive title bar (gray)
#define COLOR_TITLE_TEXT        RGB(255, 255, 255)  // White text
#define COLOR_BUTTON_FACE       RGB(192, 192, 192)  // Button face
#define COLOR_BUTTON_SHADOW     RGB(128, 128, 128)  // Dark shadow
#define COLOR_BUTTON_HILIGHT    RGB(255, 255, 255)  // Highlight
#define COLOR_BUTTON_LIGHT      RGB(223, 223, 223)  // Light edge
#define COLOR_BORDER_DARK       RGB(64, 64, 64)     // Dark border
#define COLOR_BORDER_LIGHT      RGB(255, 255, 255)  // Light border
#define COLOR_CONTENT_BG        RGB(255, 255, 255)  // White content area

// Window states
typedef enum {
    WINDOW_STATE_NORMAL,
    WINDOW_STATE_MINIMIZED,
    WINDOW_STATE_MAXIMIZED
} WindowState;

// Hit test results
typedef enum {
    WMHIT_NONE,
    WMHIT_TITLE,
    WMHIT_MINIMIZE,
    WMHIT_MAXIMIZE,
    WMHIT_CLOSE,
    // Resize edges
    WMHIT_RESIZE_N,
    WMHIT_RESIZE_S,
    WMHIT_RESIZE_E,
    WMHIT_RESIZE_W,
    WMHIT_RESIZE_NE,
    WMHIT_RESIZE_NW,
    WMHIT_RESIZE_SE,
    WMHIT_RESIZE_SW
} WMHitResult;

// Resize grab state
typedef struct {
    bool active;
    WMHitResult edge;       // Which edge/corner being dragged
    int start_mouse_x;      // Screen coords at grab start
    int start_mouse_y;
    int start_win_x;        // Window position at grab start
    int start_win_y;
    int start_win_w;        // Window size at grab start
    int start_win_h;
    int window_id;
} WMResizeState;

// Minimum window dimensions
#define WM_MIN_WIDTH  120
#define WM_MIN_HEIGHT (TITLE_BAR_HEIGHT + BORDER_WIDTH * 2 + 20)

// Button hover state
typedef struct {
    bool minimize_hover;
    bool maximize_hover;
    bool close_hover;
} ButtonHoverState;

// Single window state
typedef struct {
    bool used;
    int  layer_index;
    int  window_id;
    char title[64];
    WindowState state;
    Rect saved_rect;           // Pre-maximize size/position
    ButtonHoverState hover;    // Button hover states
    bool has_focus;            // Is window focused?
} WMWindow;

// Window manager state
typedef struct {
    WMWindow windows[MAX_WINDOWS];
    int      count;
    int      next_id;
    int      screen_width;
    int      screen_height;
    int      focused_window_id;
    WMResizeState resize;   // Active resize grab state
} WindowManager;

// Core functions
void wm_init(WindowManager* wm, int screen_width, int screen_height);

// Window creation and management
int wm_create_window(Compositor* comp, WindowManager* wm, Taskbar* taskbar,
                     int x, int y, int width, int height, const char* title);

void wm_destroy_window(Compositor* comp, WindowManager* wm, Taskbar* taskbar,
                       int window_id);

// Window state changes
void wm_minimize_window(Compositor* comp, WindowManager* wm, int window_id);
void wm_maximize_window(Compositor* comp, WindowManager* wm, int window_id);
void wm_restore_window(Compositor* comp, WindowManager* wm, int window_id);
void wm_toggle_maximize(Compositor* comp, WindowManager* wm, int window_id);

// Focus management
void wm_focus_window(Compositor* comp, WindowManager* wm, int window_id);
int wm_get_focused_window(WindowManager* wm);

// Hover management
void wm_update_hover(WindowManager* wm, Compositor* comp, int window_id, 
                     int local_x, int local_y);
void wm_clear_hover(WindowManager* wm, int window_id);

// Helper functions
int wm_get_layer_index(WindowManager* wm, int window_id);
int wm_get_window_at(Compositor* comp, WindowManager* wm,
                     int screen_x, int screen_y, int* out_local_x, int* out_local_y);

// Hit testing
WMHitResult wm_hit_test(int win_width, int win_height, int local_x, int local_y);

// Click handling
void wm_handle_click(Compositor* comp, WindowManager* wm, Taskbar* taskbar,
                    int window_id, int local_x, int local_y);

// Resize functions
// Begin a resize drag (call on mouse-down when hit test returns a WMHIT_RESIZE_* edge)
void wm_begin_resize(WindowManager* wm, Compositor* comp, int window_id,
                     WMHitResult edge, int screen_x, int screen_y);

// Update resize during mouse-move (call every mouse move while resize is active)
void wm_update_resize(WindowManager* wm, Compositor* comp, int screen_x, int screen_y);

// Finish resize (call on mouse-up)
void wm_end_resize(WindowManager* wm);

// Returns true if a resize drag is currently in progress
bool wm_is_resizing(const WindowManager* wm);

// Drawing functions (internal)
void wm_draw_window_frame(Compositor* comp, int layer_index, WMWindow* win);

#endif // WM64_H