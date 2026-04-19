#ifndef FB_FRAMEBUFFER_H
#define FB_FRAMEBUFFER_H

#include <limine.h>
#include <stdbool.h>
#include <stdint.h>
#include "../fs/vfs.h"

void fb_init(struct limine_framebuffer *framebuffer);
void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color);
void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                  uint32_t color);
void fb_clear(uint32_t color);
uint32_t fb_get_width(void);
uint32_t fb_get_height(void);
void *fb_get_base(void);
uint32_t fb_get_pitch(void);

void fb_set_backbuffer_mode(bool enabled);
bool fb_is_backbuffer_enabled(void);
void *fb_get_backbuffer(void);
void fb_swap_buffer(void);
void fb_copy_to_backbuffer(
    void); // For keeping backbuffer in sync when drawing directly

void fb_register_vfs(void);
void fb_register_device_node(const char *name, vfs_node_t *node);
vfs_node_t *fb_lookup_device(const char *name);

#endif
