#include "framebuffer.h"
#include "../console/console.h"
#include "../console/klog.h"
#include "../drivers/input/keyboard.h"
#include "../fs/ramfs.h"
#include "../fs/vfs.h"
#include "../mm/heap.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../syscalls/syscall.h"
#include "lib/string.h"
#include "terminal.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ── Device Node Registry ────────────────────────────────────────────────────
// Keeps track of character device nodes so they persist across lookups
#define MAX_DEVICES 32

typedef struct {
  char name[64];
  vfs_node_t *node;
} device_entry_t;

static device_entry_t device_registry[MAX_DEVICES];
static int device_count = 0;

// Register a device node in the registry
void fb_register_device_node(const char *name, vfs_node_t *node) {
  // Check if device already exists and update it
  for (int i = 0; i < device_count; i++) {
    if (strcmp(device_registry[i].name, name) == 0) {
      device_registry[i].node = node;
      return;
    }
  }
  // Add new entry
  if (device_count >= MAX_DEVICES)
    return;
  strncpy(device_registry[device_count].name, name, 63);
  device_registry[device_count].name[63] = '\0';
  device_registry[device_count].node = node;
  device_count++;
}

// Look up a device in the registry
vfs_node_t *fb_lookup_device(const char *name) {
  for (int i = 0; i < device_count; i++) {
    if (strcmp(device_registry[i].name, name) == 0) {
      return device_registry[i].node;
    }
  }
  return NULL;
}

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
bool fb_is_backbuffer_enabled(void) { return backbuffer_enabled; }
void *fb_get_backbuffer(void) { return backbuffer; }

void fb_swap_buffer(void) {
  if (!backbuffer)
    return;
  uint32_t fb_size = fb->height * fb->pitch;
  memcpy((uint8_t *)fb->address, (uint8_t *)backbuffer, fb_size);
}

void fb_copy_to_backbuffer(void) {
  if (!backbuffer)
    return;
  uint32_t fb_size = fb->height * fb->pitch;
  memcpy((uint8_t *)backbuffer, (uint8_t *)fb->address, fb_size);
}

// ── /dev/fb0 VFS node ────────────────────────────────────────────────────────

static uint32_t fb_vfs_write(struct vfs_node *node, uint32_t offset,
                             uint32_t size, uint8_t *buffer) {
  (void)node;
  if (!fb || !backbuffer)
    return 0;
  uint32_t fb_size = fb->height * fb->pitch;

  if (offset >= fb_size)
    return 0;
  if (offset + size > fb_size)
    size = fb_size - offset;

  // Write directly to framebuffer, bypass backbuffer to avoid console
  // interference
  memcpy((uint8_t *)fb->address + offset, buffer, size);

  return size;
}

// ── Framebuffer mmap: map physical fb memory directly into user space
// ───────── This allows apps like Doom to write directly without syscalls per
// frame.
#define FB_MMAP_PROT_READ 0x1
#define FB_MMAP_PROT_WRITE 0x2
#define FB_MMAP_PROT_EXEC 0x4
#define FB_MMAP_MAP_SHARED 0x01
#define FB_MMAP_MAP_PRIVATE 0x02
#define FBIOGET_VSCREENINFO 0x4600
#define FBIOPUT_VSCREENINFO 0x4601
#define FBIOGET_FSCREENINFO 0x4602
#define FBIOPAN_DISPLAY 0x4606

static uint64_t fb_vfs_mmap(struct vfs_node *node, uint64_t length,
                            uint64_t prot, uint64_t flags) {
  (void)node;
  if (!fb)
    return (uint64_t)-1; // MAP_FAILED

  uint32_t fb_size = fb->height * fb->pitch;

  // Length must not exceed framebuffer size
  if (length == 0 || length > fb_size) {
    klog_puts("[FB_MMAP] Error: invalid length\n");
    return (uint64_t)-1;
  }

  // Must be shared mapping for direct framebuffer access
  if (!(flags & FB_MMAP_MAP_SHARED)) {
    klog_puts("[FB_MMAP] Error: only MAP_SHARED supported for framebuffer\n");
    return (uint64_t)-1;
  }

// Allocate a virtual address range in user space
// Use the mmap bump allocator from sys_mm.c
#define PAGE_SIZE 4096
#define PAGE_ALIGN_UP(x) (((x) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))

  uint64_t aligned_len = PAGE_ALIGN_UP(length);
  uint64_t vaddr = mm_alloc_mmap_region(aligned_len);
  if (vaddr == 0) {
    klog_puts("[FB_MMAP] Error: mmap region exhausted\n");
    return (uint64_t)-1;
  }

  // Build page flags from prot
  uint64_t page_flags = PAGE_FLAG_PRESENT | PAGE_FLAG_USER;
  if (prot & FB_MMAP_PROT_WRITE)
    page_flags |= PAGE_FLAG_RW;
  if (!(prot & FB_MMAP_PROT_EXEC))
    page_flags |= PAGE_FLAG_NX;

  // Map each page of the framebuffer directly (physical -> user virtual)
  // fb->address from Limine is a virtual address in HHDM - convert to physical
  uint64_t *pml4 = vmm_get_active_pml4();
  uint64_t fb_phys = (uint64_t)fb->address - pmm_get_hhdm_offset();
  uint64_t num_pages = aligned_len / PAGE_SIZE;

  for (uint64_t i = 0; i < num_pages; i++) {
    uint64_t phys_page = fb_phys + i * PAGE_SIZE;
    uint64_t virt_page = vaddr + i * PAGE_SIZE;

    if (!vmm_map_page(pml4, virt_page, phys_page, page_flags)) {
      klog_puts("[FB_MMAP] Error: vmm_map_page failed\n");
      return (uint64_t)-1;
    }
  }

  klog_puts("[FB_MMAP] Mapped framebuffer ");
  klog_uint64(aligned_len);
  klog_puts(" bytes at ");
  klog_uint64(vaddr);
  klog_puts(" (phys ");
  klog_uint64(fb_phys);
  klog_puts(")\n");

  return vaddr;
}

static uint32_t fb_vfs_read(struct vfs_node *node, uint32_t offset,
                            uint32_t size, uint8_t *buffer) {
  (void)node;
  if (!fb || !backbuffer)
    return 0;
  uint32_t fb_size = fb->height * fb->pitch;

  if (offset >= fb_size)
    return 0;
  if (offset + size > fb_size)
    size = fb_size - offset;

  uint8_t *src =
      backbuffer_enabled ? (uint8_t *)backbuffer : (uint8_t *)fb->address;
  memcpy(buffer, src + offset, size);

  return size;
}

// ── /dev/console VFS node ────────────────────────────────────────────────────

static uint8_t canon_buffer[1024];
static uint32_t canon_len = 0;
static uint32_t canon_pos = 0;

static void console_vfs_open(vfs_node_t *node) { (void)node; }
static void console_vfs_close(vfs_node_t *node) { (void)node; }

static uint32_t console_vfs_read(struct vfs_node *node, uint32_t offset,
                                 uint32_t size, uint8_t *buffer) {
  (void)node;
  (void)offset;
  if (size == 0)
    return 0;

  if (console_termios.c_lflag & ICANON) {
    // If we have data in the canon buffer, return it first
    if (canon_pos < canon_len) {
      uint32_t to_copy = canon_len - canon_pos;
      if (to_copy > size)
        to_copy = size;
      memcpy(buffer, canon_buffer + canon_pos, to_copy);
      canon_pos += to_copy;
      if (canon_pos == canon_len) {
        canon_pos = 0;
        canon_len = 0;
      }
      return to_copy;
    }

    // Otherwise, collect a new line
    canon_len = 0;
    canon_pos = 0;

    while (1) {
      char c = keyboard_get_char();

      // ICRNL: Map CR to NL on input
      if (c == '\r' && (console_termios.c_iflag & ICRNL))
        c = '\n';

      // Handle erasing (Backspace or Delete)
      if (c == '\b' || c == 0x7F) {
        if (canon_len > 0) {
          canon_len--;
          if (console_termios.c_lflag & ECHO) {
            console_putchar('\b');
          }
        }
        continue;
      }

      // Buffer the character
      if (canon_len < sizeof(canon_buffer)) {
        canon_buffer[canon_len++] = c;
        if (console_termios.c_lflag & ECHO) {
          console_putchar(c);
        }
      }

      // If it's a newline, we have a complete line
      if (c == '\n')
        break;
    }

    // Return as much as requested from the newly collected line
    uint32_t to_copy = canon_len;
    if (to_copy > size)
      to_copy = size;
    memcpy(buffer, canon_buffer, to_copy);
    canon_pos = to_copy;

    if (canon_pos == canon_len) {
      canon_pos = 0;
      canon_len = 0;
    }
    return to_copy;
  } else {
    // Non-canonical mode (raw-ish)
    uint32_t count = 0;
    while (count < size) {
      char c = keyboard_get_char();

      // ICRNL: Map CR to NL on input
      if (c == '\r' && (console_termios.c_iflag & ICRNL))
        c = '\n';

      // ECHO: Echo input characters
      if (console_termios.c_lflag & ECHO) {
        console_putchar(c);
      }

      buffer[count++] = (uint8_t)c;

      if (!keyboard_has_char())
        break;
    }
    return count;
  }
}

static int console_vfs_poll(struct vfs_node *node, int events) {
  (void)node;
  int revents = 0;
  if (events & POLLIN) {
    if (keyboard_has_char()) {
      revents |= POLLIN;
    }
  }
  if (events & POLLOUT) {
    revents |= POLLOUT; // Console is always ready to write
  }
  return revents;
}

// Use console_write_batch so that each VFS write() call results in exactly
// ONE backbuffer swap — eliminating per-character flicker for apps like kilo.
static uint32_t console_vfs_write(struct vfs_node *node, uint32_t offset,
                                  uint32_t size, uint8_t *buffer) {
  (void)node;
  (void)offset;

  if (console_termios.c_oflag & ONLCR) {
    // ONLCR: Map NL to CR-NL on output
    for (uint32_t i = 0; i < size; i++) {
      if (buffer[i] == '\n') {
        console_putchar('\r');
      }
      console_putchar(buffer[i]);
    }
  } else {
    console_write_batch((const char *)buffer, size);
  }
  return size;
}

// /dev/null - discard all writes, return EOF on read
static uint32_t null_vfs_read(struct vfs_node *node, uint32_t offset,
                              uint32_t size, uint8_t *buffer) {
  (void)node;
  (void)offset;
  (void)size;
  (void)buffer;
  return 0; // EOF
}

static uint32_t null_vfs_write(struct vfs_node *node, uint32_t offset,
                               uint32_t size, uint8_t *buffer) {
  (void)node;
  (void)offset;
  (void)buffer;
  return size; // Discard but report success
}

// /dev/zero - return zeros on read, discard writes
static uint32_t zero_vfs_read(struct vfs_node *node, uint32_t offset,
                              uint32_t size, uint8_t *buffer) {
  (void)node;
  (void)offset;
  memset(buffer, 0, size);
  return size;
}

static uint32_t zero_vfs_write(struct vfs_node *node, uint32_t offset,
                               uint32_t size, uint8_t *buffer) {
  (void)node;
  (void)offset;
  (void)buffer;
  return size; // Discard but report success
}

// ── Helper for device registration ──────────────────────────────────────────
// Character devices are always created as virtual in-memory nodes, not
// persisted to ext2
static void setup_chardev(
    vfs_node_t *dev_dir, const char *name,
    uint32_t (*read_fn)(struct vfs_node *, uint32_t, uint32_t, uint8_t *),
    uint32_t (*write_fn)(struct vfs_node *, uint32_t, uint32_t, uint8_t *),
    void (*open_fn)(struct vfs_node *), void (*close_fn)(struct vfs_node *),
    int (*poll_fn)(struct vfs_node *, int),
    int (*ioctl_fn)(struct vfs_node *, uint32_t, uint64_t), void *device,
    uint32_t length) {
  (void)dev_dir; // Not needed - we use the device registry

  // Create virtual device node
  vfs_node_t *node = kmalloc(sizeof(vfs_node_t));
  if (!node)
    return;

  memset(node, 0, sizeof(vfs_node_t));
  strcpy(node->name, name);
  node->flags = FS_CHARDEV;
  node->mask = 0666;
  node->length = length;
  node->device = device;
  node->read = read_fn;
  node->write = write_fn;
  node->open = open_fn;
  node->close = close_fn;
  node->poll = poll_fn;
  node->ioctl = ioctl_fn;

  // Register in device registry for persistent lookups
  // This ensures device callbacks are always available,
  // avoiding issues with filesystem lookups
  fb_register_device_node(name, node);
}

uint16_t fb_get_bpp(void) { return fb ? fb->bpp : 0; }

static int fb_ioctl(struct vfs_node *node, uint32_t request, uint64_t arg) {
  (void)node;
  if (!fb)
    return -1;

  switch (request) {
  case FBIOGET_VSCREENINFO: {
    struct fb_var_screeninfo *var = (struct fb_var_screeninfo *)arg;
    memset(var, 0, sizeof(struct fb_var_screeninfo));
    var->xres = (uint32_t)fb->width;
    var->yres = (uint32_t)fb->height;
    var->xres_virtual = (uint32_t)(fb->pitch / (fb->bpp / 8));
    var->yres_virtual = (uint32_t)fb->height;
    var->bits_per_pixel = (uint32_t)fb->bpp;

    var->red.length = fb->red_mask_size;
    var->red.offset = fb->red_mask_shift;
    var->green.length = fb->green_mask_size;
    var->green.offset = fb->green_mask_shift;
    var->blue.length = fb->blue_mask_size;
    var->blue.offset = fb->blue_mask_shift;
    
    // For 32-bit framebuffer, explicitly specify the 8-bit transparency/alpha padding
    if (fb->bpp == 32) {
      var->transp.length = 8;
      // Assume alpha is the remaining byte not used by RGB
      uint32_t used_mask = ((1 << fb->red_mask_size) - 1) << fb->red_mask_shift;
      used_mask |= ((1 << fb->green_mask_size) - 1) << fb->green_mask_shift;
      used_mask |= ((1 << fb->blue_mask_size) - 1) << fb->blue_mask_shift;
      
      if ((used_mask & 0xFF000000) == 0) var->transp.offset = 24;
      else if ((used_mask & 0x000000FF) == 0) var->transp.offset = 0;
      else var->transp.offset = 24; // Default fallback
    }
    
    return 0;
  }
  case FBIOGET_FSCREENINFO: {
    struct fb_fix_screeninfo *fix = (struct fb_fix_screeninfo *)arg;
    memset(fix, 0, sizeof(struct fb_fix_screeninfo));
    strcpy(fix->id, "ascentos-fb");
    fix->smem_start = (unsigned long)fb->address - pmm_get_hhdm_offset();
    fix->smem_len = (uint32_t)(fb->height * fb->pitch);
    fix->line_length = (uint32_t)fb->pitch;
    fix->visual = 2; // FB_VISUAL_TRUECOLOR
    return 0;
  }
  case FBIOPUT_VSCREENINFO: {
    // X11 often calls this to probe or 'confirm' mode switching.
    // Since we don't support dynamic resolution or bpp switching,
    // we MUST firmly overwrite the user's struct with our fixed
    // hardware configuration and return success. This informs the
    // fbdev driver of the clamped/enforced parameters.
    struct fb_var_screeninfo *var = (struct fb_var_screeninfo *)arg;
    
    var->xres = (uint32_t)fb->width;
    var->yres = (uint32_t)fb->height;
    var->xres_virtual = (uint32_t)(fb->pitch / (fb->bpp / 8));
    var->yres_virtual = (uint32_t)fb->height;
    var->bits_per_pixel = (uint32_t)fb->bpp;

    var->red.length = fb->red_mask_size;
    var->red.offset = fb->red_mask_shift;
    var->green.length = fb->green_mask_size;
    var->green.offset = fb->green_mask_shift;
    var->blue.length = fb->blue_mask_size;
    var->blue.offset = fb->blue_mask_shift;
    
    if (fb->bpp == 32) {
      var->transp.length = 8;
      uint32_t used_mask = ((1 << fb->red_mask_size) - 1) << fb->red_mask_shift;
      used_mask |= ((1 << fb->green_mask_size) - 1) << fb->green_mask_shift;
      used_mask |= ((1 << fb->blue_mask_size) - 1) << fb->blue_mask_shift;
      if ((used_mask & 0xFF000000) == 0) var->transp.offset = 24;
      else if ((used_mask & 0x000000FF) == 0) var->transp.offset = 0;
      else var->transp.offset = 24;
    }

    return 0;
  }
  case FBIOPAN_DISPLAY: {
    // Scrolling/Panning - return success but do nothing (fallback to software)
    return 0;
  }
  default:
    return -1;
  }
}

// ── /dev/tty0 VT ioctls for Xfbdev/Xorg ─────────────────────────────────────
// Linux VT ioctl numbers
#define VT_OPENQRY 0x5600
#define VT_GETMODE 0x5601
#define VT_SETMODE 0x5602
#define VT_GETSTATE 0x5603
#define VT_ACTIVATE 0x5606
#define VT_WAITACTIVE 0x5607
#define VT_DISALLOCATE 0x5608
#define KDGETMODE 0x4B33
#define KDSETMODE 0x4B3A
#define KDGKBMODE 0x4B44
#define KDSKBMODE 0x4B45
#define VT_RELDISP 0x5605

#define K_RAW 0x00
#define K_XLATE 0x01
#define K_MEDIUMRAW 0x02

struct vt_stat {
  uint16_t v_active; // Active VT
  uint16_t v_signal; // Signal to send
  uint16_t v_state;  // VT bitmask of open VTs
};

struct vt_mode {
  char mode;    // VT mode (VT_AUTO, VT_PROCESS)
  char waitv;   // if set, wait for release
  short relsig; // signal to send on release
  short acqsig; // signal to send on acquire
  short frsig;  // unused
};

static int current_kd_mode = KD_TEXT;
static int current_kb_mode = K_XLATE;

static int tty0_ioctl(struct vfs_node *node, uint32_t request, uint64_t arg) {
  (void)node;

  switch (request) {
  case VT_OPENQRY: {
    // Return next available VT number
    int *vt = (int *)arg;
    if (vt)
      *vt = 1;
    return 0;
  }
  case VT_GETSTATE: {
    struct vt_stat *vs = (struct vt_stat *)arg;
    if (!vs)
      return -14;
    vs->v_active = 1; // VT 1 is active
    vs->v_signal = 0;
    vs->v_state = 0x02; // VT 1 is open (bit 1)
    return 0;
  }
  case VT_GETMODE: {
    struct vt_mode *vm = (struct vt_mode *)arg;
    if (!vm)
      return -14;
    vm->mode = 0; // VT_AUTO
    vm->waitv = 0;
    vm->relsig = 0;
    vm->acqsig = 0;
    vm->frsig = 0;
    return 0;
  }
  case VT_SETMODE: {
    // Accept mode change silently (VT_AUTO/VT_PROCESS)
    return 0;
  }
  case VT_ACTIVATE: {
    // Single-console OS — just succeed
    return 0;
  }
  case VT_WAITACTIVE: {
    // We're always on VT 1, so always active
    return 0;
  }
  case VT_RELDISP: {
    return 0;
  }
  case VT_DISALLOCATE: {
    // Single-console OS — nothing to deallocate, just succeed
    return 0;
  }
  case KDGETMODE: {
    int *mode = (int *)arg;
    if (mode)
      *mode = current_kd_mode;
    return 0;
  }
  case KDSETMODE: {
    current_kd_mode = (int)arg;
    return 0;
  }
  case KDGKBMODE: {
    int *mode = (int *)arg;
    if (mode)
      *mode = current_kb_mode;
    return 0;
  }
  case KDSKBMODE: {
    current_kb_mode = (int)arg;
    return 0;
  }
  default:
    return -25; // ENOTTY
  }
}

// ── Registration ─────────────────────────────────────────────────────────────

void fb_register_vfs(void) {
  if (!fs_root)
    return;

  vfs_node_t *dev_dir = vfs_finddir(fs_root, "dev");
  if (!dev_dir)
    return;

  // /dev/fb0 - special handling for framebuffer size
  vfs_node_t *fb_node = kmalloc(sizeof(vfs_node_t));
  if (fb_node) {
    memset(fb_node, 0, sizeof(vfs_node_t));
    strcpy(fb_node->name, "fb0");
    fb_node->flags = FS_CHARDEV;
    fb_node->mask = 0666;
    fb_node->length = fb->height * fb->pitch;
    fb_node->device = fb;
    fb_node->read = fb_vfs_read;
    fb_node->write = fb_vfs_write;
    fb_node->mmap = fb_vfs_mmap;
    fb_node->ioctl = fb_ioctl;

    // Register in device registry (no filesystem manipulation needed)
    fb_register_device_node("fb0", fb_node);
  }

  // /dev/console
  setup_chardev(dev_dir, "console", console_vfs_read, console_vfs_write,
                console_vfs_open, console_vfs_close, console_vfs_poll, 0, 0, 0);

  // /dev/tty (alias to console for now)
  setup_chardev(dev_dir, "tty", console_vfs_read, console_vfs_write,
                console_vfs_open, console_vfs_close, console_vfs_poll, 0, 0, 0);

  // /dev/stdin
  setup_chardev(dev_dir, "stdin", console_vfs_read, 0, console_vfs_open,
                console_vfs_close, console_vfs_poll, 0, 0, 0);

  // /dev/stdout
  setup_chardev(dev_dir, "stdout", 0, console_vfs_write, console_vfs_open,
                console_vfs_close, 0, 0, 0, 0);

  // /dev/stderr
  setup_chardev(dev_dir, "stderr", 0, console_vfs_write, console_vfs_open,
                console_vfs_close, 0, 0, 0, 0);

  // /dev/null
  setup_chardev(dev_dir, "null", null_vfs_read, null_vfs_write, 0, 0, 0, 0, 0,
                0);

  // /dev/zero
  setup_chardev(dev_dir, "zero", zero_vfs_read, zero_vfs_write, 0, 0, 0, 0, 0,
                0);

  // /dev/tty0 — virtual terminal device for Xfbdev/Xorg VT management
  setup_chardev(dev_dir, "tty0", console_vfs_read, console_vfs_write,
                console_vfs_open, console_vfs_close, console_vfs_poll,
                tty0_ioctl, 0, 0);

  // /dev/tty1-tty7 — individual virtual terminals (X server opens tty1)
  setup_chardev(dev_dir, "tty1", console_vfs_read, console_vfs_write,
                console_vfs_open, console_vfs_close, console_vfs_poll,
                tty0_ioctl, 0, 0);
  setup_chardev(dev_dir, "tty2", console_vfs_read, console_vfs_write,
                console_vfs_open, console_vfs_close, console_vfs_poll,
                tty0_ioctl, 0, 0);
  setup_chardev(dev_dir, "tty3", console_vfs_read, console_vfs_write,
                console_vfs_open, console_vfs_close, console_vfs_poll,
                tty0_ioctl, 0, 0);

  // /dev/apm_bios (Power management probes in X11)
  setup_chardev(dev_dir, "apm_bios", zero_vfs_read, zero_vfs_write, 0, 0, 0, 0,
                0, 0);

  // /dev/misc/apm_bios
  vfs_node_t *misc_dir = vfs_finddir(dev_dir, "misc");
  if (misc_dir) {
    setup_chardev(misc_dir, "apm_bios", zero_vfs_read, zero_vfs_write, 0, 0, 0,
                  0, 0, 0);
  }

  // Note: don't free dev_dir - it still points to a valid VFS node
}

// ── Direct drawing primitives
// ─────────────────────────────────────────────────

void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color) {
  if (x >= fb->width || y >= fb->height)
    return;

  void *target = backbuffer_enabled ? backbuffer : fb->address;
  volatile uint32_t *pixel =
      (volatile uint32_t *)((uint8_t *)target + y * fb->pitch + x * 4);
  *pixel = color;
}

void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                  uint32_t color) {
  void *target = backbuffer_enabled ? backbuffer : fb->address;

  for (uint32_t row = y; row < y + h && row < fb->height; row++) {
    uint32_t *line = (uint32_t *)((uint8_t *)target + row * fb->pitch + x * 4);
    for (uint32_t col = 0; col < w && (x + col) < fb->width; col++)
      line[col] = color;
  }
}

void fb_clear(uint32_t color) {
  fb_fill_rect(0, 0, fb->width, fb->height, color);
}

uint32_t fb_get_width(void) { return fb->width; }
uint32_t fb_get_height(void) { return fb->height; }
int fb_get_kd_mode(void) { return current_kd_mode; }
void *fb_get_base(void) { return fb->address; }
uint32_t fb_get_pitch(void) { return fb->pitch; }