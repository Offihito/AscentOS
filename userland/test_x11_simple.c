#include <stdio.h>
#include <stdlib.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

int main(int argc, char **argv) {
    Display *dpy;
    Window win;
    GC gc;
    XEvent event;
    int screen;
    
    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "Cannot open display\n");
        return 1;
    }
    
    screen = DefaultScreen(dpy);
    win = XCreateSimpleWindow(dpy, RootWindow(dpy, screen),
                              100, 100, 400, 400, 1,
                              BlackPixel(dpy, screen),
                              WhitePixel(dpy, screen));
    
    XSelectInput(dpy, win, ExposureMask | KeyPressMask);
    XMapWindow(dpy, win);
    
    gc = XCreateGC(dpy, win, 0, NULL);
    XSetForeground(dpy, gc, BlackPixel(dpy, screen));
    
    printf("Window created, entering event loop\n");
    fflush(stdout);
    
    while (1) {
        XNextEvent(dpy, &event);
        if (event.type == Expose) {
            XFillRectangle(dpy, win, gc, 50, 50, 100, 100);
            XDrawString(dpy, win, gc, 150, 150, "Hello AscentOS!", 15);
        }
        if (event.type == KeyPress)
            break;
    }
    
    XCloseDisplay(dpy);
    return 0;
}
