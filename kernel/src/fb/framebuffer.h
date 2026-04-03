#ifndef FB_FRAMEBUFFER_H
#define FB_FRAMEBUFFER_H

#include <stdint.h>
#include <limine.h>

void fb_init(struct limine_framebuffer *framebuffer);
void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color);
void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
void fb_clear(uint32_t color);
uint32_t fb_get_width(void);
uint32_t fb_get_height(void);

#endif
