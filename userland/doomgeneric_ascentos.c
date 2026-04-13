// doomgeneric_ascentos.c — AscentOS platform backend for doomgeneric
// Uses /dev/fb0 (framebuffer), scancode keyboard, /dev/dsp (SB16 OSS audio)

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <stdbool.h>
#include <time.h>
#include <sys/syscall.h>

#include "doomgeneric.h"
#include "doomkeys.h"

// ── Framebuffer ────────────────────────────────────────────────────────────
static int fb_fd = -1;
static uint8_t *fb_backbuffer = NULL;
static uint32_t fb_width = 0;
static uint32_t fb_height = 0;
static uint32_t fb_pitch = 0;
static uint32_t fb_bpp = 32;

// Centering offset for DOOM's 640×400 output
static uint32_t fb_x_offset = 0;
static uint32_t fb_y_offset = 0;

// ── Keyboard (scancode interface) ──────────────────────────────────────────
#define KBDSCANMODE_SET   0x5471
#define KBDSCANCODE_READ  0x5472

typedef struct {
    uint8_t scancode;
    uint8_t is_extended;
    uint8_t is_release;
} scancode_event_t;

// ── Key queue ──────────────────────────────────────────────────────────────
#define KEYQUEUE_SIZE 64
static unsigned short s_KeyQueue[KEYQUEUE_SIZE];
static unsigned int s_KeyQueueReadIndex = 0;
static unsigned int s_KeyQueueWriteIndex = 0;

static void addKeyToQueue(int pressed, unsigned char doomKey) {
    s_KeyQueue[s_KeyQueueWriteIndex] = (pressed << 8) | doomKey;
    s_KeyQueueWriteIndex++;
    s_KeyQueueWriteIndex %= KEYQUEUE_SIZE;
}

// ── PS/2 Set 1 scancode → DOOM key mapping ────────────────────────────────
// Based on doomgeneric_soso.c working implementation
static unsigned char scancode_to_doom(uint8_t sc, bool extended) {
    // Extended scancodes (0xE0 prefix)
    if (extended) {
        switch (sc) {
        case 0x48: return KEY_UPARROW;
        case 0x50: return KEY_DOWNARROW;
        case 0x4B: return KEY_LEFTARROW;
        case 0x4D: return KEY_RIGHTARROW;
        case 0x1C: return KEY_ENTER;     // numpad enter
        case 0x35: return '/';           // numpad divide
        case 0x38: return KEY_RALT;      // right alt
        case 0x1D: return KEY_FIRE;      // right ctrl -> fire (like left ctrl)
        default:   return 0;
        }
    }

    switch (sc) {
    // Number row
    case 0x02: return '1';
    case 0x03: return '2';
    case 0x04: return '3';
    case 0x05: return '4';
    case 0x06: return '5';
    case 0x07: return '6';
    case 0x08: return '7';
    case 0x09: return '8';
    case 0x0A: return '9';
    case 0x0B: return '0';
    case 0x0C: return '-';
    case 0x0D: return '=';
    // QWERTY row (uppercase for DOOM key bindings)
    case 0x10: return 'Q';
    case 0x11: return 'W';
    case 0x12: return 'E';
    case 0x13: return 'R';
    case 0x14: return 'T';
    case 0x15: return 'Y';
    case 0x16: return 'U';
    case 0x17: return 'I';
    case 0x18: return 'O';
    case 0x19: return 'P';
    case 0x1A: return '[';
    case 0x1B: return ']';
    // Home row (uppercase for DOOM key bindings)
    case 0x1E: return 'A';
    case 0x1F: return 'S';
    case 0x20: return 'D';
    case 0x21: return 'F';
    case 0x22: return 'G';
    case 0x23: return 'H';
    case 0x24: return 'J';
    case 0x25: return 'K';
    case 0x26: return 'L';
    case 0x27: return ';';
    case 0x28: return '\'';
    // Bottom row (uppercase for DOOM key bindings)
    case 0x2C: return 'Z';
    case 0x2D: return 'X';
    case 0x2E: return 'C';
    case 0x2F: return 'V';
    case 0x30: return 'B';
    case 0x31: return 'N';
    case 0x32: return 'M';
    case 0x33: return ',';
    case 0x34: return '.';
    // Special keys
    case 0x01: return KEY_ESCAPE;
    case 0x0E: return KEY_BACKSPACE;
    case 0x0F: return KEY_TAB;
    case 0x1C: return KEY_ENTER;
    case 0x39: return KEY_USE;        // space -> use (open doors)
    case 0x29: return '`';
    case 0x2B: return '\\';
    // Modifiers - DOOM uses these for gameplay!
    case 0x2A: return KEY_RSHIFT;     // left shift -> run
    case 0x36: return KEY_RSHIFT;     // right shift -> run
    case 0x1D: return KEY_FIRE;       // left ctrl -> fire
    case 0x38: return KEY_RALT;       // left alt -> strafe
    // Function keys
    case 0x3B: return KEY_F1;
    case 0x3C: return KEY_F2;
    case 0x3D: return KEY_F3;
    case 0x3E: return KEY_F4;
    case 0x3F: return KEY_F5;
    case 0x40: return KEY_F6;
    case 0x41: return KEY_F7;
    case 0x42: return KEY_F8;
    case 0x43: return KEY_F9;
    case 0x44: return KEY_F10;
    case 0x57: return KEY_F11;
    case 0x58: return KEY_F12;
    // Lock keys
    case 0x3A: return KEY_CAPSLOCK;
    case 0x45: return KEY_NUMLOCK;
    case 0x46: return KEY_SCRLCK;
    default:   return 0;
    }
}

// ── Detect framebuffer dimensions from /dev/fb0 node size ──────────────────
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
        { 640,  480 }, { 800,  600 }, { 1024, 600 }, { 1024, 768 },
        { 1152, 864 }, { 1280, 720 }, { 1280, 768 }, { 1280, 800 },
        { 1280, 960 }, { 1280,1024 }, { 1360, 768 }, { 1366, 768 },
        { 1400,1050 }, { 1440, 900 }, { 1600, 900 }, { 1600,1200 },
        { 1680,1050 }, { 1920, 800 }, { 1920,1080 }, { 1920,1200 },
        { 2560,1080 }, { 2560,1440 }, { 3840,2160 }, { 0, 0 }
    };

    for (int i = 0; resolutions[i].w != 0; i++) {
        uint32_t w = resolutions[i].w;
        uint32_t h = resolutions[i].h;
        uint32_t pitch = w * 4;
        if ((off_t)(pitch * h) == fb_size) {
            fb_width  = w;
            fb_height = h;
            fb_pitch  = pitch;
            return 1;
        }
    }

    // Fallback: guess from size
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

    fb_width  = 1024;
    fb_height = 768;
    fb_pitch  = fb_width * 4;
    return 0;
}

// ── DG_Init ────────────────────────────────────────────────────────────────
void DG_Init(void) {
    // Open framebuffer
    fb_fd = open("/dev/fb0", O_RDWR);
    if (fb_fd < 0) {
        printf("DOOM: Cannot open /dev/fb0\n");
        exit(1);
    }

    detect_fb_dimensions(fb_fd);

    // Calculate centering offsets
    if (fb_width >= DOOMGENERIC_RESX)
        fb_x_offset = (fb_width - DOOMGENERIC_RESX) / 2;
    if (fb_height >= DOOMGENERIC_RESY)
        fb_y_offset = (fb_height - DOOMGENERIC_RESY) / 2;

    // Allocate backbuffer (full screen, cleared to black)
    size_t fb_size = (size_t)fb_pitch * fb_height;
    fb_backbuffer = (uint8_t *)malloc(fb_size);
    if (!fb_backbuffer) {
        printf("DOOM: Cannot allocate framebuffer backbuffer (%zu bytes)\n", fb_size);
        exit(1);
    }
    memset(fb_backbuffer, 0, fb_size);

    printf("DOOM: Framebuffer %ux%u (pitch=%u), DOOM centered at (%u,%u)\n",
           fb_width, fb_height, fb_pitch, fb_x_offset, fb_y_offset);

    // Enable scancode mode on stdin for raw keyboard input
    if (ioctl(0, KBDSCANMODE_SET, 1) < 0) {
        printf("DOOM: Warning: Failed to enable scancode mode\n");
    }

    // Make stdin non-blocking
    int flags = fcntl(0, F_GETFL, 0);
    fcntl(0, F_SETFL, flags | O_NONBLOCK);
}

// ── Handle keyboard input via scancode interface ───────────────────────────
static void handleKeyInput(void) {
    scancode_event_t event;

    while (ioctl(0, KBDSCANCODE_READ, &event) > 0) {
        unsigned char doomKey = scancode_to_doom(event.scancode, event.is_extended);
        if (doomKey == 0) continue;

        int pressed = !event.is_release;
        addKeyToQueue(pressed, doomKey);
    }
}

// ── DG_DrawFrame ───────────────────────────────────────────────────────────
void DG_DrawFrame(void) {
    if (!fb_backbuffer) return;

    // Write DOOM rows directly from DG_ScreenBuffer to /dev/fb0
    // Skips the full-screen memset and backbuffer intermediate copy.
    // I/O reduced from fb_width*fb_height*4 to 640*400*4 per frame.
    size_t row_bytes = DOOMGENERIC_RESX * (fb_bpp / 8);
    for (int y = 0; y < DOOMGENERIC_RESY; y++) {
        off_t offset = (off_t)((y + fb_y_offset) * fb_pitch +
                               fb_x_offset * (fb_bpp / 8));
        lseek(fb_fd, offset, SEEK_SET);
        write(fb_fd, (uint8_t *)DG_ScreenBuffer + y * row_bytes, row_bytes);
    }

    // Process keyboard input
    handleKeyInput();
}

// ── DG_SleepMs ─────────────────────────────────────────────────────────────
void DG_SleepMs(uint32_t ms) {
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    syscall(SYS_nanosleep, &ts, NULL);
}

// ── DG_GetTicksMs ──────────────────────────────────────────────────────────
uint32_t DG_GetTicksMs(void) {
    struct timespec ts;
    syscall(SYS_clock_gettime, 1, &ts); // CLOCK_MONOTONIC = 1
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

// ── DG_GetKey ──────────────────────────────────────────────────────────────
int DG_GetKey(int *pressed, unsigned char *key) {
    if (s_KeyQueueReadIndex == s_KeyQueueWriteIndex) {
        return 0;  // queue empty
    }

    unsigned short keyData = s_KeyQueue[s_KeyQueueReadIndex];
    s_KeyQueueReadIndex++;
    s_KeyQueueReadIndex %= KEYQUEUE_SIZE;

    *pressed = keyData >> 8;
    *key     = keyData & 0xFF;
    return 1;
}

// ── DG_SetWindowTitle ──────────────────────────────────────────────────────
void DG_SetWindowTitle(const char *title) {
    (void)title;  // no window manager, no title
}

// ── main ───────────────────────────────────────────────────────────────────
int main(int argc, char **argv) {
    doomgeneric_Create(argc, argv);

    while (1) {
        doomgeneric_Tick();
    }

    return 0;
}
