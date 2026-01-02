// wallpaper64.c - Custom Wallpaper System
#include "wallpaper64.h"
#include "gui64.h"
#include "files64.h"
#include <stddef.h>

// String utilities
extern int str_len(const char* str);
extern void str_cpy(char* dest, const char* src);
extern void str_concat(char* dest, const char* src);
extern void* memcpy64(void* dest, const void* src, size_t n);

// Global wallpaper state
static Wallpaper current_wallpaper = {0};

// Memory allocator (simple bump allocator for wallpaper data)
static uint8_t wallpaper_memory[800 * 600 * 4]; // Max 800x600 image
static size_t wallpaper_memory_used = 0;

static void* wallpaper_alloc(size_t size) {
    if (wallpaper_memory_used + size > sizeof(wallpaper_memory)) {
        return NULL;
    }
    void* ptr = &wallpaper_memory[wallpaper_memory_used];
    wallpaper_memory_used += size;
    return ptr;
}

static void wallpaper_free_all(void) {
    wallpaper_memory_used = 0;
}

// BMP file structures
#pragma pack(push, 1)
typedef struct {
    uint16_t type;           // 'BM'
    uint32_t size;
    uint16_t reserved1;
    uint16_t reserved2;
    uint32_t offset;
} BMPHeader;

typedef struct {
    uint32_t size;
    int32_t width;
    int32_t height;
    uint16_t planes;
    uint16_t bits_per_pixel;
    uint32_t compression;
    uint32_t image_size;
    int32_t x_pixels_per_meter;
    int32_t y_pixels_per_meter;
    uint32_t colors_used;
    uint32_t colors_important;
} BMPInfoHeader;
#pragma pack(pop)

void wallpaper_init(void) {
    current_wallpaper.pixels = NULL;
    current_wallpaper.width = 0;
    current_wallpaper.height = 0;
    current_wallpaper.mode = WALLPAPER_MODE_STRETCH;
    current_wallpaper.loaded = false;
    current_wallpaper.filename[0] = '\0';
    wallpaper_memory_used = 0;
}

bool wallpaper_load_bmp(const char* filename) {
    // Get file from filesystem
    const EmbeddedFile64* file = fs_get_file64(filename);
    if (!file || file->size < sizeof(BMPHeader) + sizeof(BMPInfoHeader)) {
        return false;
    }
    
    const uint8_t* data = (const uint8_t*)file->content;
    
    // Read BMP header
    BMPHeader* header = (BMPHeader*)data;
    if (header->type != 0x4D42) { // 'BM'
        return false;
    }
    
    // Read info header
    BMPInfoHeader* info = (BMPInfoHeader*)(data + sizeof(BMPHeader));
    
    // Validate format
    if (info->bits_per_pixel != 24 && info->bits_per_pixel != 32) {
        return false; // Only support 24-bit and 32-bit BMP
    }
    
    if (info->compression != 0) {
        return false; // No compression support
    }
    
    // Validate size
    uint32_t width = (uint32_t)info->width;
    uint32_t height = (uint32_t)(info->height > 0 ? info->height : -info->height);
    
    if (width > 800 || height > 600) {
        return false; // Too large
    }
    
    // Unload previous wallpaper
    wallpaper_unload();
    
    // Allocate pixel buffer
    size_t pixel_size = width * height * sizeof(uint32_t);
    uint32_t* pixels = (uint32_t*)wallpaper_alloc(pixel_size);
    if (!pixels) {
        return false;
    }
    
    // Read pixel data
    const uint8_t* pixel_data = data + header->offset;
    int bytes_per_pixel = info->bits_per_pixel / 8;
    int row_size = ((width * bytes_per_pixel + 3) / 4) * 4; // BMP rows are 4-byte aligned
    bool bottom_up = info->height > 0;
    
    for (uint32_t y = 0; y < height; y++) {
        uint32_t src_y = bottom_up ? (height - 1 - y) : y;
        const uint8_t* row = pixel_data + src_y * row_size;
        
        for (uint32_t x = 0; x < width; x++) {
            uint8_t b = row[x * bytes_per_pixel + 0];
            uint8_t g = row[x * bytes_per_pixel + 1];
            uint8_t r = row[x * bytes_per_pixel + 2];
            
            pixels[y * width + x] = RGB(r, g, b);
        }
    }
    
    // Set wallpaper
    current_wallpaper.pixels = pixels;
    current_wallpaper.width = width;
    current_wallpaper.height = height;
    current_wallpaper.loaded = true;
    str_cpy(current_wallpaper.filename, filename);
    
    return true;
}

bool wallpaper_load_raw(uint32_t* pixels, uint32_t width, uint32_t height) {
    if (!pixels || width == 0 || height == 0) {
        return false;
    }
    
    // Unload previous
    wallpaper_unload();
    
    // Allocate and copy
    size_t pixel_size = width * height * sizeof(uint32_t);
    uint32_t* new_pixels = (uint32_t*)wallpaper_alloc(pixel_size);
    if (!new_pixels) {
        return false;
    }
    
    memcpy64(new_pixels, pixels, pixel_size);
    
    current_wallpaper.pixels = new_pixels;
    current_wallpaper.width = width;
    current_wallpaper.height = height;
    current_wallpaper.loaded = true;
    str_cpy(current_wallpaper.filename, "(generated)");
    
    return true;
}

void wallpaper_set_mode(WallpaperMode mode) {
    current_wallpaper.mode = mode;
}

WallpaperMode wallpaper_get_mode(void) {
    return current_wallpaper.mode;
}

void wallpaper_draw(void) {
    if (!current_wallpaper.loaded || !current_wallpaper.pixels) {
        // No wallpaper, fill with default color
        gui_clear(RGB(0, 120, 215));
        return;
    }
    
    int screen_width = gui_get_width();
    int screen_height = gui_get_height() - 40; // Exclude taskbar
    
    switch (current_wallpaper.mode) {
        case WALLPAPER_MODE_STRETCH: {
            // Stretch to fill screen
            for (int y = 0; y < screen_height; y++) {
                for (int x = 0; x < screen_width; x++) {
                    int src_x = (x * current_wallpaper.width) / screen_width;
                    int src_y = (y * current_wallpaper.height) / screen_height;
                    uint32_t color = current_wallpaper.pixels[src_y * current_wallpaper.width + src_x];
                    gui_put_pixel(x, y, color);
                }
            }
            break;
        }
        
        case WALLPAPER_MODE_CENTER: {
            // Center on screen with background color
            gui_clear(RGB(0, 0, 0));
            int start_x = (screen_width - (int)current_wallpaper.width) / 2;
            int start_y = (screen_height - (int)current_wallpaper.height) / 2;
            
            for (uint32_t y = 0; y < current_wallpaper.height; y++) {
                for (uint32_t x = 0; x < current_wallpaper.width; x++) {
                    int px = start_x + x;
                    int py = start_y + y;
                    if (px >= 0 && px < screen_width && py >= 0 && py < screen_height) {
                        uint32_t color = current_wallpaper.pixels[y * current_wallpaper.width + x];
                        gui_put_pixel(px, py, color);
                    }
                }
            }
            break;
        }
        
        case WALLPAPER_MODE_TILE: {
            // Tile across screen
            for (int y = 0; y < screen_height; y++) {
                for (int x = 0; x < screen_width; x++) {
                    int src_x = x % current_wallpaper.width;
                    int src_y = y % current_wallpaper.height;
                    uint32_t color = current_wallpaper.pixels[src_y * current_wallpaper.width + src_x];
                    gui_put_pixel(x, y, color);
                }
            }
            break;
        }
        
        case WALLPAPER_MODE_FIT: {
            // Fit to screen maintaining aspect ratio
            gui_clear(RGB(0, 0, 0));
            
            float img_aspect = (float)current_wallpaper.width / current_wallpaper.height;
            float screen_aspect = (float)screen_width / screen_height;
            
            int draw_width, draw_height;
            int start_x, start_y;
            
            if (img_aspect > screen_aspect) {
                // Image is wider
                draw_width = screen_width;
                draw_height = (int)(screen_width / img_aspect);
                start_x = 0;
                start_y = (screen_height - draw_height) / 2;
            } else {
                // Image is taller
                draw_height = screen_height;
                draw_width = (int)(screen_height * img_aspect);
                start_x = (screen_width - draw_width) / 2;
                start_y = 0;
            }
            
            for (int y = 0; y < draw_height; y++) {
                for (int x = 0; x < draw_width; x++) {
                    int src_x = (x * current_wallpaper.width) / draw_width;
                    int src_y = (y * current_wallpaper.height) / draw_height;
                    uint32_t color = current_wallpaper.pixels[src_y * current_wallpaper.width + src_x];
                    gui_put_pixel(start_x + x, start_y + y, color);
                }
            }
            break;
        }
    }
}

void wallpaper_unload(void) {
    if (current_wallpaper.loaded) {
        wallpaper_free_all();
        current_wallpaper.pixels = NULL;
        current_wallpaper.width = 0;
        current_wallpaper.height = 0;
        current_wallpaper.loaded = false;
        current_wallpaper.filename[0] = '\0';
    }
}

bool wallpaper_is_loaded(void) {
    return current_wallpaper.loaded;
}

void wallpaper_get_info(char* buffer, int max_len) {
    if (!current_wallpaper.loaded) {
        str_cpy(buffer, "No wallpaper loaded");
        return;
    }
    
    // Format: "filename (WxH, mode)"
    str_cpy(buffer, current_wallpaper.filename);
    str_concat(buffer, " (");
    
    // Width
    char num[16];
    int w = current_wallpaper.width;
    int i = 0;
    if (w == 0) {
        num[i++] = '0';
    } else {
        int temp = w;
        int digits = 0;
        while (temp > 0) { digits++; temp /= 10; }
        for (int j = digits - 1; j >= 0; j--) {
            num[j] = '0' + (w % 10);
            w /= 10;
        }
        i = digits;
    }
    num[i] = '\0';
    str_concat(buffer, num);
    
    str_concat(buffer, "x");
    
    // Height
    int h = current_wallpaper.height;
    i = 0;
    if (h == 0) {
        num[i++] = '0';
    } else {
        int temp = h;
        int digits = 0;
        while (temp > 0) { digits++; temp /= 10; }
        for (int j = digits - 1; j >= 0; j--) {
            num[j] = '0' + (h % 10);
            h /= 10;
        }
        i = digits;
    }
    num[i] = '\0';
    str_concat(buffer, num);
    
    // Mode
    str_concat(buffer, ", ");
    switch (current_wallpaper.mode) {
        case WALLPAPER_MODE_STRETCH: str_concat(buffer, "stretch"); break;
        case WALLPAPER_MODE_CENTER: str_concat(buffer, "center"); break;
        case WALLPAPER_MODE_TILE: str_concat(buffer, "tile"); break;
        case WALLPAPER_MODE_FIT: str_concat(buffer, "fit"); break;
    }
    str_concat(buffer, ")");
}

// Built-in gradient wallpapers
void wallpaper_set_gradient_blue(void) {
    const uint32_t width = 800;
    const uint32_t height = 560;
    
    wallpaper_unload();
    
    uint32_t* pixels = (uint32_t*)wallpaper_alloc(width * height * sizeof(uint32_t));
    if (!pixels) return;
    
    for (uint32_t y = 0; y < height; y++) {
        uint8_t brightness = 20 + (y * 180) / height;
        for (uint32_t x = 0; x < width; x++) {
            pixels[y * width + x] = RGB(0, brightness / 2, brightness);
        }
    }
    
    current_wallpaper.pixels = pixels;
    current_wallpaper.width = width;
    current_wallpaper.height = height;
    current_wallpaper.loaded = true;
    current_wallpaper.mode = WALLPAPER_MODE_STRETCH;
    str_cpy(current_wallpaper.filename, "(blue gradient)");
}

void wallpaper_set_gradient_purple(void) {
    const uint32_t width = 800;
    const uint32_t height = 560;
    
    wallpaper_unload();
    
    uint32_t* pixels = (uint32_t*)wallpaper_alloc(width * height * sizeof(uint32_t));
    if (!pixels) return;
    
    for (uint32_t y = 0; y < height; y++) {
        uint8_t r = 40 + (y * 80) / height;
        uint8_t b = 80 + (y * 120) / height;
        for (uint32_t x = 0; x < width; x++) {
            pixels[y * width + x] = RGB(r, 0, b);
        }
    }
    
    current_wallpaper.pixels = pixels;
    current_wallpaper.width = width;
    current_wallpaper.height = height;
    current_wallpaper.loaded = true;
    current_wallpaper.mode = WALLPAPER_MODE_STRETCH;
    str_cpy(current_wallpaper.filename, "(purple gradient)");
}

void wallpaper_set_gradient_green(void) {
    const uint32_t width = 800;
    const uint32_t height = 560;
    
    wallpaper_unload();
    
    uint32_t* pixels = (uint32_t*)wallpaper_alloc(width * height * sizeof(uint32_t));
    if (!pixels) return;
    
    for (uint32_t y = 0; y < height; y++) {
        uint8_t g = 60 + (y * 150) / height;
        for (uint32_t x = 0; x < width; x++) {
            pixels[y * width + x] = RGB(0, g, g / 3);
        }
    }
    
    current_wallpaper.pixels = pixels;
    current_wallpaper.width = width;
    current_wallpaper.height = height;
    current_wallpaper.loaded = true;
    current_wallpaper.mode = WALLPAPER_MODE_STRETCH;
    str_cpy(current_wallpaper.filename, "(green gradient)");
}

void wallpaper_set_solid_color(uint32_t color) {
    const uint32_t width = 100;
    const uint32_t height = 100;
    
    wallpaper_unload();
    
    uint32_t* pixels = (uint32_t*)wallpaper_alloc(width * height * sizeof(uint32_t));
    if (!pixels) return;
    
    for (uint32_t i = 0; i < width * height; i++) {
        pixels[i] = color;
    }
    
    current_wallpaper.pixels = pixels;
    current_wallpaper.width = width;
    current_wallpaper.height = height;
    current_wallpaper.loaded = true;
    current_wallpaper.mode = WALLPAPER_MODE_STRETCH;
    str_cpy(current_wallpaper.filename, "(solid color)");
}