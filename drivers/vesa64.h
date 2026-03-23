#pragma once
#include <stdint.h>
#include <stddef.h>

// Colors (RGB)
typedef uint32_t VesaColor;

#define VESA_BLACK          0x00000000
#define VESA_BLUE           0x000000AA
#define VESA_GREEN          0x0000AA00
#define VESA_CYAN           0x0000AAAA
#define VESA_RED            0x00AA0000
#define VESA_MAGENTA        0x00AA00AA
#define VESA_BROWN          0x00AA5500
#define VESA_LIGHT_GRAY     0x00AAAAAA
#define VESA_DARK_GRAY      0x00555555
#define VESA_LIGHT_BLUE     0x005555FF
#define VESA_LIGHT_GREEN    0x0055FF55
#define VESA_LIGHT_CYAN     0x0055FFFF
#define VESA_LIGHT_RED      0x00FF5555
#define VESA_LIGHT_MAGENTA  0x00FF55FF
#define VESA_YELLOW         0x00FFFF55
#define VESA_WHITE          0x00FFFFFF


VesaColor vesa_color_from_vga(uint8_t vga_color_index);


#define FONT_WIDTH   8    
#define FONT_HEIGHT  16   

// Function declarations
void init_vesa64(void);

void clear_screen64(void);

void putchar64(char c, uint8_t color);

void print_str64(const char* str, uint8_t color);

void println64(const char* str, uint8_t color);

void set_color64(uint8_t fg, uint8_t bg);

void set_position64(size_t row, size_t col);
void get_position64(size_t* row, size_t* col);

void get_screen_size64(size_t* width, size_t* height);

void scroll_up(size_t lines);
void scroll_down(size_t lines);
void get_scroll_info64(size_t* buffer_lines, size_t* offset);

void update_cursor64(void);

void vesa_write_buf(const char* buf, int len);

void reset_to_standard_mode(void);
void set_extended_text_mode(void);
