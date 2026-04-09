#include "framebuffer.h"
#include "lib/string.h"
#include "../fs/ramfs.h"
#include "../fs/vfs.h"
#include "../mm/heap.h"
#include "../console/console.h"
#include "../drivers/input/keyboard.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static struct limine_framebuffer *fb;
static void *backbuffer = 0;
static bool backbuffer_enabled = false;

// 1920 * 1200 * 4 = ~9.2 MB — enough for any common resolution
#define BACKBUFFER_SIZE (1920 * 1200 * 4)
static uint8_t static_backbuffer[BACKBUFFER_SIZE];

void fb_init(struct limine_framebuffer *framebuffer) {
  fb = framebuffer;
  backbuffer = static_backbuffer;
  // Initialize backbuffer to match current framebuffer state
  uint32_t fb_size = fb->height * fb->pitch;
  memcpy((uint8_t *)backbuffer, (uint8_t *)fb->address, fb_size);
}

void fb_set_backbuffer_mode(bool enabled) { backbuffer_enabled = enabled; }

void fb_swap_buffer(void) {
  if (!backbuffer) return;
  uint32_t fb_size = fb->height * fb->pitch;
  memcpy((uint8_t *)fb->address, (uint8_t *)backbuffer, fb_size);
}

void fb_copy_to_backbuffer(void) {
  if (!backbuffer) return;
  uint32_t fb_size = fb->height * fb->pitch;
  memcpy((uint8_t *)backbuffer, (uint8_t *)fb->address, fb_size);
}

// ── /dev/fb0 VFS node ────────────────────────────────────────────────────────

static uint32_t fb_vfs_write(struct vfs_node *node, uint32_t offset,
                             uint32_t size, uint8_t *buffer) {
  (void)node;
  if (!fb || !backbuffer) return 0;
  uint32_t fb_size = fb->height * fb->pitch;

  if (offset >= fb_size) return 0;
  if (offset + size > fb_size)
    size = fb_size - offset;

  // Write directly to framebuffer, bypass backbuffer to avoid console interference
  memcpy((uint8_t *)fb->address + offset, buffer, size);

  return size;
}

static uint32_t fb_vfs_read(struct vfs_node *node, uint32_t offset,
                            uint32_t size, uint8_t *buffer) {
  (void)node;
  if (!fb || !backbuffer) return 0;
  uint32_t fb_size = fb->height * fb->pitch;

  if (offset >= fb_size) return 0;
  if (offset + size > fb_size)
    size = fb_size - offset;

  uint8_t *src =
      backbuffer_enabled ? (uint8_t *)backbuffer : (uint8_t *)fb->address;
  memcpy(buffer, src + offset, size);

  return size;
}

// ── /dev/console VFS node ────────────────────────────────────────────────────

static void console_vfs_open(vfs_node_t *node)  { (void)node; }
static void console_vfs_close(vfs_node_t *node) { (void)node; }

static uint32_t console_vfs_read(struct vfs_node *node, uint32_t offset,
                                 uint32_t size, uint8_t *buffer) {
  (void)node;
  (void)offset;
  if (size == 0) return 0;

  // Non-blocking: return 0 if no input available
  if (!keyboard_has_char()) return 0;

  uint32_t count = 0;
  while (count < size && keyboard_has_char())
    buffer[count++] = keyboard_get_char();

  return count;
}

// Use console_write_batch so that each VFS write() call results in exactly
// ONE backbuffer swap — eliminating per-character flicker for apps like kilo.
static uint32_t console_vfs_write(struct vfs_node *node, uint32_t offset,
                                  uint32_t size, uint8_t *buffer) {
  (void)node;
  (void)offset;
  console_write_batch((const char *)buffer, size);
  return size;
}

// ── Registration ─────────────────────────────────────────────────────────────

void fb_register_vfs(void) {
  if (!fs_root) return;

  vfs_node_t *dev_dir = vfs_finddir(fs_root, "dev");
  if (!dev_dir) return;

  // /dev/fb0
  vfs_node_t *fb_node = kmalloc(sizeof(vfs_node_t));
  memset(fb_node, 0, sizeof(vfs_node_t));
  fb_node->name[0] = 'f';
  fb_node->name[1] = 'b';
  fb_node->name[2] = '0';
  fb_node->name[3] = '\0';
  fb_node->flags   = FS_CHARDEV;
  fb_node->mask    = 0666;
  fb_node->length  = fb->height * fb->pitch;
  fb_node->device  = fb;
  fb_node->read    = fb_vfs_read;
  fb_node->write   = fb_vfs_write;
  ramfs_mount_node(dev_dir, fb_node);

  // /dev/console
  vfs_node_t *console_node = kmalloc(sizeof(vfs_node_t));
  memset(console_node, 0, sizeof(vfs_node_t));
  strcpy(console_node->name, "console");
  console_node->flags  = FS_CHARDEV;
  console_node->mask   = 0666;
  console_node->length = 0;
  console_node->device = NULL;
  console_node->read   = console_vfs_read;
  console_node->write  = console_vfs_write;
  console_node->open   = console_vfs_open;
  console_node->close  = console_vfs_close;
  ramfs_mount_node(dev_dir, console_node);
}

// ── Direct drawing primitives ─────────────────────────────────────────────────

void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color) {
  if (x >= fb->width || y >= fb->height) return;

  void *target = backbuffer_enabled ? backbuffer : fb->address;
  volatile uint32_t *pixel =
      (volatile uint32_t *)((uint8_t *)target + y * fb->pitch + x * 4);
  *pixel = color;
}

void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                  uint32_t color) {
  void *target = backbuffer_enabled ? backbuffer : fb->address;

  for (uint32_t row = y; row < y + h && row < fb->height; row++) {
    uint32_t *line =
        (uint32_t *)((uint8_t *)target + row * fb->pitch + x * 4);
    for (uint32_t col = 0; col < w && (x + col) < fb->width; col++)
      line[col] = color;
  }
}

void fb_clear(uint32_t color) {
  fb_fill_rect(0, 0, fb->width, fb->height, color);
}

uint32_t fb_get_width(void)  { return fb->width;   }
uint32_t fb_get_height(void) { return fb->height;  }
void    *fb_get_base(void)   { return fb->address; }
uint32_t fb_get_pitch(void)  { return fb->pitch;   }