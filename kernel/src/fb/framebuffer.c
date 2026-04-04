#include "framebuffer.h"
#include <stdbool.h>

static struct limine_framebuffer *fb;
static void *backbuffer = 0;
static bool backbuffer_enabled = false;

// Provide a large enough static buffer for the backbuffer if we want it early,
// or we can allocate it later. For now, let's use a static 8MB buffer.
// 1920 * 1080 * 4 = ~8.3MB
#define BACKBUFFER_SIZE (1920 * 1200 * 4)
static uint8_t static_backbuffer[BACKBUFFER_SIZE];

void fb_init(struct limine_framebuffer *framebuffer) {
    fb = framebuffer;
    backbuffer = static_backbuffer;
}

void fb_set_backbuffer_mode(bool enabled) {
    backbuffer_enabled = enabled;
}

void fb_swap_buffer(void) {
    if (!backbuffer) return;
    
    uint8_t *dst = (uint8_t *)fb->address;
    uint8_t *src = (uint8_t *)backbuffer;
    uint32_t fb_size = fb->height * fb->pitch;
    
    // Copy the prepared frame to the physical screen
    for (uint32_t i = 0; i < fb_size; i++) {
        dst[i] = src[i];
    }
}

void fb_copy_to_backbuffer(void) {
    if (!backbuffer) return;
    uint8_t *dst = (uint8_t *)backbuffer;
    uint8_t *src = (uint8_t *)fb->address;
    uint32_t fb_size = fb->height * fb->pitch;
    for (uint32_t i = 0; i < fb_size; i++) {
        dst[i] = src[i];
    }
}

#include "../fs/vfs.h"
#include "../fs/ramfs.h"
#include "../mm/heap.h"
#include "../lib/string.h"

static uint32_t fb_vfs_write(struct vfs_node *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    (void)node;
    if (!fb || !backbuffer) return 0;
    uint32_t fb_size = fb->height * fb->pitch;
    
    if (offset >= fb_size) return 0;
    if (offset + size > fb_size) {
        size = fb_size - offset;
    }
    
    uint8_t *dst = backbuffer_enabled ? (uint8_t *)backbuffer : (uint8_t *)fb->address;
    for (uint32_t i = 0; i < size; i++) {
        dst[offset + i] = buffer[i];
    }
    
    return size;
}

static uint32_t fb_vfs_read(struct vfs_node *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    (void)node;
    if (!fb || !backbuffer) return 0;
    uint32_t fb_size = fb->height * fb->pitch;
    
    if (offset >= fb_size) return 0;
    if (offset + size > fb_size) {
        size = fb_size - offset;
    }
    
    uint8_t *src = backbuffer_enabled ? (uint8_t *)backbuffer : (uint8_t *)fb->address;
    for (uint32_t i = 0; i < size; i++) {
        buffer[i] = src[offset + i];
    }
    
    return size;
}

void fb_register_vfs(void) {
    if (!fs_root) return;
    if (!fs_root) return;
    
    vfs_node_t *dev_dir = vfs_finddir(fs_root, "dev");
    if (dev_dir) {
        vfs_node_t *node = kmalloc(sizeof(vfs_node_t));
        memset(node, 0, sizeof(vfs_node_t));
        // strncpy
        node->name[0] = 'f';
        node->name[1] = 'b';
        node->name[2] = '0';
        node->name[3] = '\0';
        
        node->flags = FS_CHARDEV; // Character device
        node->mask = 0666;
        node->length = fb->height * fb->pitch;
        node->device = fb;
        node->read = fb_vfs_read;
        node->write = fb_vfs_write;
        
        ramfs_mount_node(dev_dir, node);
    }
}

void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (x >= fb->width || y >= fb->height) {
        return;
    }

    void *target = backbuffer_enabled ? backbuffer : fb->address;

    volatile uint32_t *pixel = (volatile uint32_t *)((uint8_t *)target + y * fb->pitch + x * 4);
    *pixel = color;
}

void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    void *target = backbuffer_enabled ? backbuffer : fb->address;
    
    for (uint32_t row = y; row < y + h && row < fb->height; row++) {
        for (uint32_t col = x; col < x + w && col < fb->width; col++) {
            volatile uint32_t *pixel = (volatile uint32_t *)((uint8_t *)target + row * fb->pitch + col * 4);
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

void *fb_get_base(void) {
    return fb->address;
}

uint32_t fb_get_pitch(void) {
    return fb->pitch;
}
