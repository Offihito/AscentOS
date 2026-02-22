// ============================================================================
// kernel/xhello.c  —  AscentOS X11 Demo
//
// Kernel image'ına statik olarak bağlanır.
// FAT32 / elfload / disk gerektirmez.
// kernel64.c içindeki GUI loop'tan buton tıklamasıyla çağrılır.
// Newlib bağımlılığı yoktur.
// ============================================================================

#include "xlib_stub.h"

extern void serial_print(const char* str);

static int xh_strlen(const char* s) {
    int n = 0; while (s[n]) n++; return n;
}

void xhello_main(void) {
    serial_print("[xhello] starting\n");

    Display* dpy = XOpenDisplay(":0");
    if (!dpy) { serial_print("[xhello] XOpenDisplay failed\n"); return; }

    int scr = DefaultScreen(dpy);

    Window win = XCreateSimpleWindow(dpy,
        RootWindow(dpy, scr),
        160, 100, 360, 250,
        0, BlackPixel(dpy, scr), WhitePixel(dpy, scr));

    if (win == None) {
        serial_print("[xhello] XCreateSimpleWindow failed\n");
        XCloseDisplay(dpy);
        return;
    }

    XStoreName(dpy, win, "X11 Demo");
    XSelectInput(dpy, win, ExposureMask | KeyPressMask | ButtonPressMask);
    XMapWindow(dpy, win);

    XGCValues gcv;
    gcv.foreground = 0x0000cc;
    gcv.background = 0xffffff;
    GC gc = XCreateGC(dpy, win, GCForeground | GCBackground, &gcv);

    int running = 1;
    while (running) {
        XEvent ev;
        XNextEvent(dpy, &ev);

        switch (ev.type) {
        case Expose:
            if (ev.xexpose.count != 0) break;
            XClearWindow(dpy, win);

            XSetForeground(dpy, gc, 0x000066);
            const char* t = "AscentOS - X11 Stub Demo";
            XDrawString(dpy, win, gc, 10, 18, t, xh_strlen(t));

            XSetForeground(dpy, gc, 0xaaaaaa);
            XDrawLine(dpy, win, gc, 8, 26, 332, 26);

            XSetForeground(dpy, gc, 0x00aa44);
            XFillRectangle(dpy, win, gc, 14, 38, 130, 76);

            XSetForeground(dpy, gc, 0xcc2200);
            XFillRectangle(dpy, win, gc, 164, 38, 130, 76);

            XSetForeground(dpy, gc, 0xffffff);
            const char* gl = "XFillRect";
            XDrawString(dpy, win, gc, 24, 80, gl, xh_strlen(gl));
            XDrawString(dpy, win, gc, 174, 80, gl, xh_strlen(gl));

            XSetForeground(dpy, gc, 0x0000cc);
            XDrawRectangle(dpy, win, gc, 8, 32, 324, 88);

            XSetForeground(dpy, gc, 0xff8800);
            XDrawLine(dpy, win, gc, 8,   140, 332, 168);
            XDrawLine(dpy, win, gc, 332, 140, 8,   168);

            XSetForeground(dpy, gc, 0x444444);
            const char* h = "Herhangi bir tusa basin veya tiklayin";
            XDrawString(dpy, win, gc, 8, 210, h, xh_strlen(h));

            XFlush(dpy);
            serial_print("[xhello] draw OK\n");
            break;

        case KeyPress:
        case ButtonPress:
            running = 0;
            break;
        }
    }

    XFreeGC(dpy, gc);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    serial_print("[xhello] done\n");
}