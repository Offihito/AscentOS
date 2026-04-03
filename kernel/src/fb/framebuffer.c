#include "framebuffer.h"

static struct limine_framebuffer *fb;

void fb_init(struct limine_framebuffer *framebuffer) {
    fb = framebuffer;
}

void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (x >= fb->width || y >= fb->height) {
        return;
    }

    volatile uint32_t *pixel = (volatile uint32_t *)((uint8_t *)fb->address + y * fb->pitch + x * 4);
    *pixel = color;
}

void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    for (uint32_t row = y; row < y + h && row < fb->height; row++) {
        for (uint32_t col = x; col < x + w && col < fb->width; col++) {
            volatile uint32_t *pixel = (volatile uint32_t *)((uint8_t *)fb->address + row * fb->pitch + col * 4);
            *pixel = color;
        }
    }
}

void fb_clear(uint32_t color) {
    fb_fill_rect(0, 0, fb->width, fb->height, color);
}

uint32_t fb_get_width(void) {
    return fb->width;
}

uint32_t fb_get_height(void) {
    return fb->height;
}
