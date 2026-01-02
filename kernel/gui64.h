// ============================================================================
// gui64.h - VESA Framebuffer GUI Library with Double Buffering
// ============================================================================
#ifndef GUI64_H
#define GUI64_H

#include <stdint.h>
#include <stdbool.h>

extern uint64_t framebuffer_addr;
extern uint32_t framebuffer_pitch, framebuffer_width, framebuffer_height;
extern uint8_t framebuffer_bpp;

typedef uint32_t Color;

#define COLOR_BLACK 0x000000
#define COLOR_WHITE 0xFFFFFF
#define COLOR_RED 0xFF0000
#define COLOR_GREEN 0x00FF00
#define COLOR_BLUE 0x0000FF
#define COLOR_GRAY 0x808080
#define COLOR_DARK_GRAY 0x404040

#define RGB(r,g,b) ((Color)((r)<<16|(g)<<8|(b)))
#define GET_RED(c) (((c)>>16)&0xFF)
#define GET_GREEN(c) (((c)>>8)&0xFF)
#define GET_BLUE(c) ((c)&0xFF)

// CRITICAL: Double buffering functions
void gui_begin_frame(void);  // Start drawing to back buffer
void gui_end_frame(void);    // Copy back buffer to screen

void gui_init(void);
void gui_clear(Color color);
void gui_put_pixel(int x, int y, Color color);
Color gui_get_pixel(int x, int y);
void gui_draw_line(int x1, int y1, int x2, int y2, Color color);
void gui_fill_rect(int x, int y, int w, int h, Color color);
void gui_draw_char(int x, int y, char c, Color fg, Color bg);
void gui_draw_string(int x, int y, const char* str, Color fg, Color bg);
void gui_draw_cursor(int x, int y);
Color gui_blend_colors(Color fg, Color bg, uint8_t alpha);
Color gui_darken_color(Color color, float factor);
int gui_get_width(void);
int gui_get_height(void);
bool gui_is_valid_coord(int x, int y);

typedef struct {
    int x, y, width, height;
    char title[64];
    Color border_color, bg_color;
    bool visible;
} Window;

void gui_draw_window(const Window* win);

// Clock functions
void gui_draw_clock(int x, int y, uint8_t hours, uint8_t minutes, uint8_t seconds);
void gui_get_rtc_time(uint8_t* hours, uint8_t* minutes, uint8_t* seconds);

#endif