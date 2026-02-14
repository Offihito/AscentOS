// ============================================================================
// wm64.h - Pencere Yöneticisi (Window Manager) - Minimize, Maximize, Close
// ============================================================================
#ifndef WM64_H
#define WM64_H

#include <stdbool.h>
#include "compositor64.h"
#include "taskbar64.h"

#define TITLE_BAR_HEIGHT 24
#define WM_BUTTON_WIDTH  24
#define WM_BUTTON_GAP   2
#define WM_RIGHT_MARGIN 4
#define MAX_WINDOWS     8

// Pencere başlık çubuğu tıklama sonucu
typedef enum {
    WMHIT_NONE,
    WMHIT_TITLE,      // Sürükleme için (sonra eklenecek)
    WMHIT_MINIMIZE,
    WMHIT_MAXIMIZE,
    WMHIT_CLOSE
} WMHitResult;

// Tek pencere durumu
typedef struct {
    bool used;
    int  layer_index;
    int  window_id;
    char title[64];
    bool minimized;
    bool maximized;
    Rect saved_rect;   // Maximize öncesi boyut/konum
} WMWindow;

// Pencere yöneticisi durumu
typedef struct {
    WMWindow windows[MAX_WINDOWS];
    int      count;
    int      next_id;
    int      screen_width;
    int      screen_height;
} WindowManager;

void wm_init(WindowManager* wm, int screen_width, int screen_height);

// Pencere oluşturur; taskbar'a ekler. Başarıda window_id, hata -1
int wm_create_window(Compositor* comp, WindowManager* wm, Taskbar* taskbar,
                     int x, int y, int width, int height, const char* title);

// Pencereyi kapatır; layer ve taskbar'dan kaldırır
void wm_destroy_window(Compositor* comp, WindowManager* wm, Taskbar* taskbar,
                       int window_id);

// Minimize edilmiş pencereyi taskbar'dan tıklanınca tekrar gösterir
void wm_restore_window(Compositor* comp, WindowManager* wm, int window_id);

// window_id ile layer index (compositor için); yoksa -1
int wm_get_layer_index(WindowManager* wm, int window_id);

// Ekran koordinatında hangi pencere var? (z-order'a göre en üstteki)
// Pencere yoksa -1, varsa window_id (local_x, local_y doldurulur)
int wm_get_window_at(Compositor* comp, WindowManager* wm,
                     int screen_x, int screen_y, int* out_local_x, int* out_local_y);

// Pencere içi (layer koordinatı) tıklama: minimize/maximize/close işle
void wm_handle_click(Compositor* comp, WindowManager* wm, Taskbar* taskbar,
                    int window_id, int local_x, int local_y);

// Başlık çubuğunda (local_x, local_y) hangi bölge? (layer koordinatları)
WMHitResult wm_hit_test(int win_width, int win_height, int local_x, int local_y);

#endif // WM64_H
