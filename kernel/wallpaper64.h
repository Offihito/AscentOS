#ifndef WALLPAPER64_H
#define WALLPAPER64_H

#include <stdint.h>
#include <stdbool.h>

// Wallpaper modes
typedef enum {
    WALLPAPER_MODE_STRETCH,   // Stretch to fit screen
    WALLPAPER_MODE_CENTER,    // Center on screen
    WALLPAPER_MODE_TILE,      // Tile across screen
    WALLPAPER_MODE_FIT        // Fit to screen maintaining aspect ratio
} WallpaperMode;

// Wallpaper structure
typedef struct {
    uint32_t* pixels;         // Pixel data (RGBA format)
    uint32_t width;
    uint32_t height;
    WallpaperMode mode;
    bool loaded;
    char filename[64];
} Wallpaper;

// Initialize wallpaper system
void wallpaper_init(void);

// Load wallpaper from BMP file
bool wallpaper_load_bmp(const char* filename);

// Load wallpaper from raw pixel data
bool wallpaper_load_raw(uint32_t* pixels, uint32_t width, uint32_t height);

// Set wallpaper mode
void wallpaper_set_mode(WallpaperMode mode);

// Get current wallpaper mode
WallpaperMode wallpaper_get_mode(void);

// Draw wallpaper to screen
void wallpaper_draw(void);

// Unload current wallpaper
void wallpaper_unload(void);

// Check if wallpaper is loaded
bool wallpaper_is_loaded(void);

// Get wallpaper info
void wallpaper_get_info(char* buffer, int max_len);

// Built-in gradient wallpapers
void wallpaper_set_gradient_blue(void);
void wallpaper_set_gradient_purple(void);
void wallpaper_set_gradient_green(void);
void wallpaper_set_solid_color(uint32_t color);

// Wallpaper change notification
bool wallpaper_has_changed(void);
void wallpaper_clear_changed_flag(void);

#endif // WALLPAPER64_H