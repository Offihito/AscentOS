#include <stdio.h>
#include <stdlib.h>
#include <X11/Xlib.h>

int main(int argc, char **argv) {
    Display *dpy;
    char *display_name = getenv("DISPLAY");
    
    printf("test_x11: starting\n");
    printf("DISPLAY=%s\n", display_name ? display_name : "(null)");
    
    dpy = XOpenDisplay(display_name);
    if (!dpy) {
        fprintf(stderr, "test_x11: cannot open display %s\n", display_name ? display_name : "(null)");
        return 1;
    }
    
    printf("test_x11: connected to display %s\n", DisplayString(dpy));
    printf("test_x11: screen %d, width %d, height %d\n", 
           DefaultScreen(dpy), DisplayWidth(dpy, DefaultScreen(dpy)), 
           DisplayHeight(dpy, DefaultScreen(dpy)));
    
    XCloseDisplay(dpy);
    printf("test_x11: done\n");
    return 0;
}
