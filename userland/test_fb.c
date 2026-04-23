#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define FBIOGET_VSCREENINFO 0x4600
#define FBIOGET_FSCREENINFO 0x4602

struct fb_bitfield {
  uint32_t offset;
  uint32_t length;
  uint32_t msb_right;
};

struct fb_var_screeninfo {
  uint32_t xres;
  uint32_t yres;
  uint32_t xres_virtual;
  uint32_t yres_virtual;
  uint32_t xoffset;
  uint32_t yoffset;
  uint32_t bits_per_pixel;
  uint32_t grayscale;
  struct fb_bitfield red;
  struct fb_bitfield green;
  struct fb_bitfield blue;
  struct fb_bitfield transp;
  uint32_t nonstd;
  uint32_t activate;
  uint32_t height;
  uint32_t width;
  uint32_t accel_flags;
  uint32_t pixclock;
  uint32_t left_margin;
  uint32_t right_margin;
  uint32_t upper_margin;
  uint32_t lower_margin;
  uint32_t hsync_len;
  uint32_t vsync_len;
  uint32_t sync;
  uint32_t vmode;
  uint32_t rotate;
  uint32_t colorspace;
  uint32_t reserved[4];
};

struct fb_fix_screeninfo {
  char id[16];
  unsigned long smem_start;
  uint32_t smem_len;
  uint32_t type;
  uint32_t type_aux;
  uint32_t visual;
  uint16_t xpanstep;
  uint16_t ypanstep;
  uint16_t ywrapstep;
  uint32_t line_length;
  unsigned long mmio_start;
  uint32_t mmio_len;
  uint32_t accel;
  uint16_t capabilities;
  uint16_t reserved[2];
};

int main() {
  int fd = open("/dev/fb0", O_RDWR);
  if (fd < 0) {
    perror("open /dev/fb0");
    return 1;
  }

  struct fb_var_screeninfo var;
  if (ioctl(fd, FBIOGET_VSCREENINFO, &var) < 0) {
    perror("ioctl FBIOGET_VSCREENINFO");
    return 1;
  }

  printf("Fixed Screen Info: (Not actually printed yet, checking var first)\n");
  printf("Resolution: %ux%u @ %u bpp\n", var.xres, var.yres,
         var.bits_per_pixel);
  printf("Color Offsets: R:%u G:%u B:%u\n", var.red.offset, var.green.offset,
         var.blue.offset);
  printf("Color Lengths: R:%u G:%u B:%u\n", var.red.length, var.green.length,
         var.blue.length);

  struct fb_fix_screeninfo fix;
  if (ioctl(fd, FBIOGET_FSCREENINFO, &fix) < 0) {
    perror("ioctl FBIOGET_FSCREENINFO");
    return 1;
  }

  printf("ID: %s\n", fix.id);
  printf("SMem Start: 0x%lx\n", fix.smem_start);
  printf("SMem Len: %u\n", fix.smem_len);
  printf("Line Length: %u\n", fix.line_length);

  // Now let's DRAW something to prove it works!
  printf("\nDrawing a test pattern to proof-of-concept graphics...\n");

  // Map the framebuffer into our address space
  uint32_t *fb = (uint32_t *)mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE,
                                  MAP_SHARED, fd, 0);
  if (fb == MAP_FAILED) {
    perror("mmap /dev/fb0");
    return 1;
  }

  // Draw a colorful gradient at the top
  for (uint32_t y = 0; y < 100; y++) {
    for (uint32_t x = 0; x < var.xres; x++) {
      // Create a gradient based on X position
      uint32_t r = (x * 255) / var.xres;
      uint32_t g = (y * 255) / 100;
      uint32_t b = 128;

      uint32_t color = (r << var.red.offset) | (g << var.green.offset) |
                       (b << var.blue.offset);
      fb[y * (fix.line_length / 4) + x] = color;
    }
  }

  printf(
      "Done! You should see a colorful gradient at the top of the screen.\n");

  munmap(fb, fix.smem_len);
  close(fd);
  return 0;
}
