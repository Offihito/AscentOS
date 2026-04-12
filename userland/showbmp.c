#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#pragma pack(push, 1)
typedef struct {
    uint16_t bfType;
    uint32_t bfSize;
    uint16_t bfReserved1;
    uint16_t bfReserved2;
    uint32_t bfOffBits;
} BITMAPFILEHEADER;

typedef struct {
    uint32_t biSize;
    int32_t  biWidth;
    int32_t  biHeight;
    uint16_t biPlanes;
    uint16_t biBitCount;
    uint32_t biCompression;
    uint32_t biSizeImage;
    int32_t  biXPelsPerMeter;
    int32_t  biYPelsPerMeter;
    uint32_t biClrUsed;
    uint32_t biClrImportant;
} BITMAPINFOHEADER;
#pragma pack(pop)

static int fb_fd = -1;
static uint8_t *fb_mem = NULL;
static uint32_t fb_width = 0;
static uint32_t fb_height = 0;
static uint32_t fb_pitch = 0;

static int detect_fb_dimensions(int fd) {
    off_t fb_size = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);

    if (fb_size <= 0) {
        fb_width  = 1024;
        fb_height = 768;
        fb_pitch  = fb_width * 4;
        return 0;
    }

    static const struct { uint32_t w, h; } resolutions[] = {
        { 640,  480 }, { 800,  600 }, { 1024, 768 }, { 1280, 720 },
        { 1280, 800 }, { 1280, 1024 }, { 1920, 1080 }, { 0, 0 }
    };

    for (int i = 0; resolutions[i].w != 0; i++) {
        uint32_t w = resolutions[i].w;
        uint32_t h = resolutions[i].h;
        if ((off_t)(w * h * 4) == fb_size) {
            fb_width  = w;
            fb_height = h;
            fb_pitch  = w * 4;
            return 1;
        }
    }

    // Guess
    for (uint32_t w = 640; w <= 4096; w += 64) {
        if (fb_size % (w * 4) == 0) {
            uint32_t h = (uint32_t)(fb_size / (w * 4));
            if (h >= 480 && h <= 2160) {
                fb_width  = w;
                fb_height = h;
                fb_pitch  = w * 4;
                return 1;
            }
        }
    }

    fb_width = 1024; fb_height = 768; fb_pitch = 1024 * 4;
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: %s <file.bmp>\n", argv[0]);
        return 1;
    }

    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) {
        perror("open bmp");
        return 1;
    }

    BITMAPFILEHEADER file_header;
    BITMAPINFOHEADER info_header;

    if (read(fd, &file_header, sizeof(file_header)) != sizeof(file_header)) {
        printf("Error: Failed to read file header\n");
        close(fd); return 1;
    }

    if (file_header.bfType != 0x4D42) {
        printf("Error: Not a BMP file (0x%X)\n", file_header.bfType);
        close(fd); return 1;
    }

    if (read(fd, &info_header, sizeof(info_header)) != sizeof(info_header)) {
        printf("Error: Failed to read info header\n");
        close(fd); return 1;
    }

    if (info_header.biBitCount != 24 && info_header.biBitCount != 32) {
        printf("Error: Unsupported BPP (%d). Only 24 and 32 are supported.\n", info_header.biBitCount);
        close(fd); return 1;
    }

    if (info_header.biCompression != 0) {
        printf("Error: Compressed BMPs not supported.\n");
        close(fd); return 1;
    }

    int32_t img_w = info_header.biWidth;
    int32_t img_h = info_header.biHeight;
    int is_bottom_up = (img_h > 0);
    if (img_h < 0) img_h = -img_h;

    printf("Image: %dx%d, %d-bit\n", (int)img_w, (int)img_h, (int)info_header.biBitCount);

    fb_fd = open("/dev/fb0", O_RDWR);
    if (fb_fd < 0) {
        perror("open /dev/fb0");
        close(fd); return 1;
    }

    detect_fb_dimensions(fb_fd);
    printf("Screen: %ux%u\n", fb_width, fb_height);

    size_t fb_size = (size_t)fb_pitch * fb_height;
    fb_mem = (uint8_t *)malloc(fb_size);
    if (!fb_mem) {
        printf("Out of memory for backbuffer\n");
        close(fb_fd); close(fd); return 1;
    }

    // Read current framebuffer to preserve background if image is small? 
    // Usually we just clear or overlay.
    memset(fb_mem, 0, fb_size);

    int start_x = (int)(fb_width - img_w) / 2;
    int start_y = (int)(fb_height - img_h) / 2;
    if (start_x < 0) start_x = 0;
    if (start_y < 0) start_y = 0;

    int bytes_per_pixel = info_header.biBitCount / 8;
    int row_size = (img_w * bytes_per_pixel + 3) & ~3;
    uint8_t *row_buf = (uint8_t *)malloc(row_size);

    lseek(fd, file_header.bfOffBits, SEEK_SET);

    for (int y = 0; y < img_h; y++) {
        if (read(fd, row_buf, row_size) != row_size) break;

        int screen_y;
        if (is_bottom_up) screen_y = start_y + (img_h - 1 - y);
        else screen_y = start_y + y;

        if (screen_y < 0 || screen_y >= (int)fb_height) continue;

        for (int x = 0; x < img_w; x++) {
            int screen_x = start_x + x;
            if (screen_x < 0 || screen_x >= (int)fb_width) continue;

            uint8_t *pixel_ptr = &row_buf[x * bytes_per_pixel];
            uint32_t color = 0;
            if (bytes_per_pixel == 3) {
                color = (0xFF << 24) | (pixel_ptr[2] << 16) | (pixel_ptr[1] << 8) | pixel_ptr[0];
            } else if (bytes_per_pixel == 4) {
                color = (pixel_ptr[3] << 24) | (pixel_ptr[2] << 16) | (pixel_ptr[1] << 8) | pixel_ptr[0];
            }

            uint32_t *fb_pixel = (uint32_t *)(fb_mem + screen_y * fb_pitch + screen_x * 4);
            *fb_pixel = color;
        }
    }

    lseek(fb_fd, 0, SEEK_SET);
    write(fb_fd, fb_mem, fb_size);

    free(row_buf);
    free(fb_mem);
    close(fb_fd);
    close(fd);

    return 0;
}
