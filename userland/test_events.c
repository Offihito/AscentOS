// test_events.c — Test the evdev input subsystem
//
// Opens /dev/input/event0 (keyboard) and /dev/input/event1 (mouse),
// reads struct input_event packets, and prints them.
//
// This is the same format Xorg's evdev driver uses.

#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// Linux-compatible input_event structure (24 bytes on x86_64)
struct input_event {
    uint64_t time_sec;
    uint64_t time_usec;
    uint16_t type;
    uint16_t code;
    int32_t  value;
} __attribute__((packed));

// Event type names
static const char *ev_type_name(uint16_t type) {
    switch (type) {
        case 0x00: return "EV_SYN";
        case 0x01: return "EV_KEY";
        case 0x02: return "EV_REL";
        case 0x03: return "EV_ABS";
        default:   return "EV_???";
    }
}

// Key/button code names (subset)
static const char *key_name(uint16_t code) {
    static char buf[16];
    // Mouse buttons
    if (code == 0x110) return "BTN_LEFT";
    if (code == 0x111) return "BTN_RIGHT";
    if (code == 0x112) return "BTN_MIDDLE";

    // Common keys
    switch (code) {
        case 1:  return "KEY_ESC";
        case 14: return "KEY_BACKSPACE";
        case 15: return "KEY_TAB";
        case 28: return "KEY_ENTER";
        case 29: return "KEY_LCTRL";
        case 42: return "KEY_LSHIFT";
        case 54: return "KEY_RSHIFT";
        case 56: return "KEY_LALT";
        case 57: return "KEY_SPACE";
        case 97: return "KEY_RCTRL";
        case 100: return "KEY_RALT";
        case 103: return "KEY_UP";
        case 105: return "KEY_LEFT";
        case 106: return "KEY_RIGHT";
        case 108: return "KEY_DOWN";
        default:
            // Letters/numbers: keycodes 2-11 = 1-0, 16-25 = Q-P, etc.
            if (code >= 2 && code <= 11) {
                buf[0] = "1234567890"[code - 2];
                buf[1] = '\0';
                return buf;
            }
            if (code >= 16 && code <= 25) {
                buf[0] = "qwertyuiop"[code - 16];
                buf[1] = '\0';
                return buf;
            }
            if (code >= 30 && code <= 38) {
                buf[0] = "asdfghjkl"[code - 30];
                buf[1] = '\0';
                return buf;
            }
            if (code >= 44 && code <= 50) {
                buf[0] = "zxcvbnm"[code - 44];
                buf[1] = '\0';
                return buf;
            }
            snprintf(buf, sizeof(buf), "0x%03x", code);
            return buf;
    }
}

// Relative axis names
static const char *rel_name(uint16_t code) {
    switch (code) {
        case 0: return "REL_X";
        case 1: return "REL_Y";
        case 8: return "REL_WHEEL";
        default: return "REL_???";
    }
}

int main(void) {
    printf("=== AscentOS evdev Test ===\n");
    printf("Opening /dev/input/event0 (keyboard)...\n");

    int kbd_fd = open("/dev/input/event0", O_RDONLY);
    if (kbd_fd < 0) {
        printf("Failed to open /dev/input/event0\n");
    } else {
        printf("  Keyboard fd = %d\n", kbd_fd);
    }

    printf("Opening /dev/input/event1 (mouse)...\n");
    int mouse_fd = open("/dev/input/event1", O_RDONLY);
    if (mouse_fd < 0) {
        printf("Failed to open /dev/input/event1\n");
    } else {
        printf("  Mouse fd = %d\n", mouse_fd);
    }

    if (kbd_fd < 0 && mouse_fd < 0) {
        printf("No input devices available!\n");
        return 1;
    }

    printf("\nReading events (Ctrl+C to exit)...\n\n");

    // Set up poll descriptors
    struct pollfd fds[2];
    int nfds = 0;

    if (kbd_fd >= 0) {
        fds[nfds].fd = kbd_fd;
        fds[nfds].events = 1; // POLLIN
        nfds++;
    }
    if (mouse_fd >= 0) {
        fds[nfds].fd = mouse_fd;
        fds[nfds].events = 1; // POLLIN
        nfds++;
    }

    struct input_event ev;
    int count = 0;

    while (count < 200) {
        int ret = poll(fds, nfds, 5000); // 5 second timeout
        if (ret <= 0) {
            printf("  (waiting for input...)\n");
            continue;
        }

        for (int i = 0; i < nfds; i++) {
            if (fds[i].revents & 1) { // POLLIN
                int n = read(fds[i].fd, &ev, sizeof(ev));
                if (n == (int)sizeof(ev)) {
                    const char *dev_name = (fds[i].fd == kbd_fd) ? "KBD" : "MOUSE";

                    if (ev.type == 0x00) {
                        // SYN_REPORT — just a separator, print lightly
                        printf("  [%s] --- SYN_REPORT ---\n", dev_name);
                    } else if (ev.type == 0x01) {
                        // EV_KEY
                        printf("  [%s] %s  %s  %s\n", dev_name,
                               ev_type_name(ev.type), key_name(ev.code),
                               ev.value ? "PRESS" : "RELEASE");
                    } else if (ev.type == 0x02) {
                        // EV_REL
                        printf("  [%s] %s  %s  %d\n", dev_name,
                               ev_type_name(ev.type), rel_name(ev.code),
                               ev.value);
                    } else {
                        printf("  [%s] %s  code=%u  value=%d\n", dev_name,
                               ev_type_name(ev.type), ev.code, ev.value);
                    }
                    count++;
                }
            }
        }
    }

    printf("\n=== Done (%d events read) ===\n", count);

    if (kbd_fd >= 0) close(kbd_fd);
    if (mouse_fd >= 0) close(mouse_fd);
    return 0;
}
