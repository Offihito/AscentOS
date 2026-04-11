#ifndef SCANCODE_H
#define SCANCODE_H

#include <stdint.h>
#include <stdbool.h>
#include <sys/ioctl.h>

// Scancode constants
#define SCANCODE_ESC        0x01
#define SCANCODE_1          0x02
#define SCANCODE_W          0x11
#define SCANCODE_A          0x1E
#define SCANCODE_S          0x1F
#define SCANCODE_D          0x20
#define SCANCODE_Q          0x10
#define SCANCODE_UP         0x48
#define SCANCODE_DOWN       0x50
#define SCANCODE_LEFT       0x4B
#define SCANCODE_RIGHT      0x4D
#define SCANCODE_SPACE      0x39

// Scancode event structure
typedef struct {
    uint8_t scancode;      // The raw scancode (without release bit)
    uint8_t is_extended;   // 1 if this was an extended scancode (E0 prefix)
    uint8_t is_release;    // 1 if this is a key release, 0 for key press
} scancode_event_t;

// IOCTL commands
#define KBDSCANMODE_GET 0x5470    // Get scancode mode state: ioctl(fd, KBDSCANMODE_GET, &int_mode)
#define KBDSCANMODE_SET 0x5471    // Set scancode mode: ioctl(fd, KBDSCANMODE_SET, 1 or 0)
#define KBDSCANCODE_READ 0x5472   // Read a scancode event: ioctl(fd, KBDSCANCODE_READ, &scancode_event_t)

// Helper functions
static inline int keyboard_set_scancode_mode(int fd, bool enabled) {
    return ioctl(fd, KBDSCANMODE_SET, enabled ? 1 : 0);
}

static inline int keyboard_get_scancode_mode(int fd) {
    int mode;
    int ret = ioctl(fd, KBDSCANMODE_GET, &mode);
    if (ret < 0) return ret;
    return mode;
}

static inline int keyboard_read_scancode(int fd, scancode_event_t *event) {
    if (!event) return -1;
    return ioctl(fd, KBDSCANCODE_READ, event);
}

#endif // SCANCODE_H
