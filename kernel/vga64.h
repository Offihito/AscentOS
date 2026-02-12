// vga64.h
void init_vga64(void);
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
void reset_to_standard_mode(void);
void set_extended_text_mode(void);  // Keep for optional extended mode use