// doomgeneric_x11.c — X11 platform backend for doomgeneric
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "doomgeneric.h"
#include "doomkeys.h"

// ── X11 State ──────────────────────────────────────────────────────────────
static Display *dpy = NULL;
static Window win;
static GC gc;
static XImage *ximage = NULL;
static int screen;
static Visual *visual;
static unsigned int depth;

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

// ── X Keysym → DOOM key mapping ───────────────────────────────────────────
static unsigned char keysym_to_doom(KeySym sym) {
    if (sym >= 'a' && sym <= 'z') return sym - 'a' + 'A';
    if (sym >= 'A' && sym <= 'Z') return sym;
    if (sym >= '0' && sym <= '9') return sym;

    switch (sym) {
        case XK_Escape:     return KEY_ESCAPE;
        case XK_Return:     return KEY_ENTER;
        case XK_Tab:        return KEY_TAB;
        case XK_BackSpace:  return KEY_BACKSPACE;
        case XK_space:      return KEY_USE;
        case XK_Shift_L:
        case XK_Shift_R:    return KEY_RSHIFT;
        case XK_Control_L:
        case XK_Control_R:  return KEY_FIRE;
        case XK_Alt_L:
        case XK_Alt_R:      return KEY_RALT;
        case XK_Up:         return KEY_UPARROW;
        case XK_Down:       return KEY_DOWNARROW;
        case XK_Left:       return KEY_LEFTARROW;
        case XK_Right:      return KEY_RIGHTARROW;
        case XK_F1:         return KEY_F1;
        case XK_F2:         return KEY_F2;
        case XK_F3:         return KEY_F3;
        case XK_F4:         return KEY_F4;
        case XK_F5:         return KEY_F5;
        case XK_F6:         return KEY_F6;
        case XK_F7:         return KEY_F7;
        case XK_F8:         return KEY_F8;
        case XK_F9:         return KEY_F9;
        case XK_F10:        return KEY_F10;
        case XK_F11:        return KEY_F11;
        case XK_F12:        return KEY_F12;
        default:            return 0;
    }
}

// ── DG_Init ────────────────────────────────────────────────────────────────
void DG_Init(void) {
    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "DOOM: Cannot open X11 display. Is X running?\n");
        exit(1);
    }

    screen = DefaultScreen(dpy);
    visual = DefaultVisual(dpy, screen);
    depth = DefaultDepth(dpy, screen);

    win = XCreateSimpleWindow(dpy, RootWindow(dpy, screen), 0, 0, 
                             DOOMGENERIC_RESX, DOOMGENERIC_RESY, 1,
                             BlackPixel(dpy, screen), BlackPixel(dpy, screen));

    XSelectInput(dpy, win, ExposureMask | KeyPressMask | KeyReleaseMask);
    XMapWindow(dpy, win);
    XStoreName(dpy, win, "DOOM (X11)");

    gc = XCreateGC(dpy, win, 0, NULL);

    // Create XImage for drawing - we use DOOM's buffer directly
    ximage = XCreateImage(dpy, visual, depth, ZPixmap, 0,
                         (char *)DG_ScreenBuffer, DOOMGENERIC_RESX, DOOMGENERIC_RESY, 
                         32, DOOMGENERIC_RESX * 4);
    
    // Check if we need to adjust XImage byte order
    if (XImageByteOrder(dpy) == MSBFirst) {
        // Most modern systems are LSBFirst, but let X11 handle it if needed
    }

    printf("DOOM: X11 window initialized %dx%d\n", DOOMGENERIC_RESX, DOOMGENERIC_RESY);
}

// ── DG_DrawFrame ───────────────────────────────────────────────────────────
void DG_DrawFrame(void) {
    if (!dpy) return;

    // Handle X11 Events
    while (XPending(dpy)) {
        XEvent ev;
        XNextEvent(dpy, &ev);
        if (ev.type == KeyPress || ev.type == KeyRelease) {
            KeySym sym = XLookupKeysym(&ev.xkey, 0);
            unsigned char doomKey = keysym_to_doom(sym);
            if (doomKey) {
                addKeyToQueue(ev.type == KeyPress, doomKey);
            }
        }
    }

    // Blit frame to window
    XPutImage(dpy, win, gc, ximage, 0, 0, 0, 0, DOOMGENERIC_RESX, DOOMGENERIC_RESY);
    XFlush(dpy);
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
    if (dpy) XStoreName(dpy, win, title);
}

// ── main ───────────────────────────────────────────────────────────────────
int main(int argc, char **argv) {
    doomgeneric_Create(argc, argv);

    while (1) {
        doomgeneric_Tick();
    }

    return 0;
}
