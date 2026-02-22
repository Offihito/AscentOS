// ============================================================================
// xlib_user.c - Userland X11 Syscall Wrapper
//
// Bu dosya newlib userland'ı ile derlenir.
// Xlib fonksiyonlarını SYS_X* syscall'larına çevirir.
// xlib_stub.c gerektirmez — sadece xlib_stub.h'ı header olarak kullanır.
//
// Derleme: USERLAND_CC ile, -Ikernel flag'iyle.
// ============================================================================

#include "../kernel/xlib_stub.h"

// ── Syscall ABI (Linux x86-64 uyumlu) ────────────────────────────────────────
// RAX = syscall no, RDI=arg1, RSI=arg2, RDX=arg3, R10=arg4, R8=arg5, R9=arg6
// Dönüş: RAX

static inline long __x11_syscall0(long nr) {
    long ret;
    __asm__ volatile ("syscall"
        : "=a"(ret) : "a"(nr)
        : "rcx","r11","memory");
    return ret;
}
static inline long __x11_syscall1(long nr, long a1) {
    long ret;
    __asm__ volatile ("syscall"
        : "=a"(ret) : "a"(nr), "D"(a1)
        : "rcx","r11","memory");
    return ret;
}
static inline long __x11_syscall2(long nr, long a1, long a2) {
    long ret;
    __asm__ volatile ("syscall"
        : "=a"(ret) : "a"(nr), "D"(a1), "S"(a2)
        : "rcx","r11","memory");
    return ret;
}
static inline long __x11_syscall3(long nr, long a1, long a2, long a3) {
    long ret;
    __asm__ volatile ("syscall"
        : "=a"(ret) : "a"(nr), "D"(a1), "S"(a2), "d"(a3)
        : "rcx","r11","memory");
    return ret;
}
static inline long __x11_syscall4(long nr, long a1, long a2, long a3, long a4) {
    long ret;
    register long r10 __asm__("r10") = a4;
    __asm__ volatile ("syscall"
        : "=a"(ret) : "a"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(r10)
        : "rcx","r11","memory");
    return ret;
}
static inline long __x11_syscall6(long nr, long a1, long a2, long a3,
                                   long a4, long a5, long a6) {
    long ret;
    register long r10 __asm__("r10") = a4;
    register long r8  __asm__("r8")  = a5;
    register long r9  __asm__("r9")  = a6;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "a"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
        : "rcx","r11","memory");
    return ret;
}

// ── Syscall numaraları (syscall.h ile aynı) ───────────────────────────────────
#define _SYS_XOPEN        31
#define _SYS_XCLOSE       32
#define _SYS_XCREATEWIN   33
#define _SYS_XDESTROYWIN  34
#define _SYS_XMAPWIN      35
#define _SYS_XUNMAPWIN    36
#define _SYS_XSTORENAME   37
#define _SYS_XFLUSH       38
#define _SYS_XSELECTINPUT 39
#define _SYS_XNEXTEVENT   40
#define _SYS_XPENDING     41
#define _SYS_XCREATEGC    42
#define _SYS_XFREEGC      43
#define _SYS_XSETFG       44
#define _SYS_XSETBG       45
#define _SYS_XFILLRECT    46
#define _SYS_XDRAWRECT    47
#define _SYS_XDRAWLINE    48
#define _SYS_XDRAWSTRING  49
#define _SYS_XCLEARWIN    50
#define _SYS_XSCREENW     51
#define _SYS_XSCREENH     52

// ── Tek Display (userland'da static pointer) ──────────────────────────────────
// Kernel bize handle olarak kendi Display*'ını döndürür;
// biz bunu uint64_t olarak saklarız ve her çağrıda geçiririz.
static Display* _u_dpy = (Display*)0;
static Screen   _u_screen;  // Yerel ekran bilgisi kopyası

// ── Aşama 1 ──────────────────────────────────────────────────────────────────

Display* XOpenDisplay(const char* display_name) {
    long h = __x11_syscall1(_SYS_XOPEN, (long)(long long)(display_name ? display_name : (const char*)0));
    if (h < 0) return (Display*)0;

    // Kernel'in Display pointer'ını sakla
    _u_dpy = (Display*)(long long)h;

    // Ekran boyutlarını kernel'den sorgula
    _u_screen.width  = (int)__x11_syscall0(_SYS_XSCREENW);
    _u_screen.height = (int)__x11_syscall0(_SYS_XSCREENH);
    _u_screen.root   = (Window)0xFFFF;
    _u_screen.root_depth = 32;

    // Display struct alanlarını doldur — kernel pointer'ı bozma
    // (kernel VA erişilemez; userland Display stub'ı tutarız)
    return _u_dpy;
}

int XCloseDisplay(Display* dpy) {
    (void)dpy;
    __x11_syscall1(_SYS_XCLOSE, 0);
    _u_dpy = (Display*)0;
    return 0;
}

Screen* XDefaultScreenOfDisplay(Display* dpy) {
    (void)dpy;
    return &_u_screen;
}
int  XDefaultScreen(Display* dpy)       { (void)dpy; return 0; }
Window XRootWindow(Display* dpy, int s) { (void)dpy; (void)s; return _u_screen.root; }
int  XDisplayWidth(Display* dpy, int s) { (void)dpy; (void)s; return _u_screen.width; }
int  XDisplayHeight(Display* dpy,int s) { (void)dpy; (void)s; return _u_screen.height; }

Window XCreateSimpleWindow(Display* dpy, Window parent,
                            int x, int y,
                            unsigned int width, unsigned int height,
                            unsigned int border_width,
                            unsigned long border,
                            unsigned long background) {
    (void)dpy; (void)parent; (void)border_width; (void)border; (void)background;
    long r;
    register long r10 __asm__("r10") = (long)y;
    register long r8  __asm__("r8")  = (long)width;
    register long r9  __asm__("r9")  = (long)height;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "a"((long)_SYS_XCREATEWIN), "D"(0L), "S"((long)x), "d"((long)y),
          "r"(r10), "r"(r8), "r"(r9)
        : "rcx","r11","memory");
    return (r < 0) ? None : (Window)r;
}

int XDestroyWindow(Display* dpy, Window w) {
    (void)dpy;
    __x11_syscall2(_SYS_XDESTROYWIN, 0, (long)w);
    return 0;
}
int XMapWindow(Display* dpy, Window w) {
    (void)dpy;
    __x11_syscall2(_SYS_XMAPWIN, 0, (long)w);
    return 0;
}
int XUnmapWindow(Display* dpy, Window w) {
    (void)dpy;
    __x11_syscall2(_SYS_XUNMAPWIN, 0, (long)w);
    return 0;
}
int XStoreName(Display* dpy, Window w, const char* name) {
    (void)dpy;
    __x11_syscall3(_SYS_XSTORENAME, 0, (long)w, (long)(long long)name);
    return 0;
}
int XFlush(Display* dpy) {
    (void)dpy;
    __x11_syscall1(_SYS_XFLUSH, 0);
    return 0;
}
int XSync(Display* dpy, Bool discard) {
    (void)discard;
    return XFlush(dpy);
}

// ── Aşama 2 ──────────────────────────────────────────────────────────────────

int XSelectInput(Display* dpy, Window w, long mask) {
    (void)dpy;
    __x11_syscall3(_SYS_XSELECTINPUT, 0, (long)w, mask);
    return 0;
}
int XNextEvent(Display* dpy, XEvent* ev) {
    (void)dpy;
    __x11_syscall2(_SYS_XNEXTEVENT, 0, (long)(long long)ev);
    return 0;
}
int XPending(Display* dpy) {
    (void)dpy;
    return (int)__x11_syscall1(_SYS_XPENDING, 0);
}
Bool XCheckWindowEvent(Display* dpy, Window w, long mask, XEvent* ev) {
    // Basit: pending varsa XNextEvent çek, eşleşiyorsa True döndür
    (void)dpy; (void)w; (void)mask;
    if (!XPending(dpy)) return False;
    XNextEvent(dpy, ev);
    return True;
}
void xlib_push_key_event(int t, unsigned kc, int x, int y) {
    (void)t; (void)kc; (void)x; (void)y; // kernel handle eder
}
void xlib_push_button_event(int t, unsigned b, int x, int y) {
    (void)t; (void)b; (void)x; (void)y;
}
void xlib_push_motion_event(int x, int y) { (void)x; (void)y; }
void xlib_push_expose_event(Window w, int x, int y, int wd, int ht) {
    (void)w; (void)x; (void)y; (void)wd; (void)ht;
}
void xlib_backend_init(void* c, void* w, void* t) { (void)c; (void)w; (void)t; }

// ── Aşama 3 ──────────────────────────────────────────────────────────────────

GC XCreateGC(Display* dpy, Drawable d, unsigned long mask, XGCValues* v) {
    (void)dpy;
    unsigned long fg = (v && (mask & GCForeground)) ? v->foreground : 0x000000UL;
    unsigned long bg = (v && (mask & GCBackground)) ? v->background : 0xFFFFFFUL;
    long r = __x11_syscall4(_SYS_XCREATEGC, 0, (long)d, (long)fg, (long)bg);
    return (r < 0) ? (GC)0 : (GC)r;
}
int XFreeGC(Display* dpy, GC gc) {
    (void)dpy;
    __x11_syscall2(_SYS_XFREEGC, 0, (long)gc);
    return 0;
}
int XSetForeground(Display* dpy, GC gc, unsigned long fg) {
    (void)dpy;
    __x11_syscall3(_SYS_XSETFG, (long)gc, 0, (long)fg);
    return 0;
}
int XSetBackground(Display* dpy, GC gc, unsigned long bg) {
    (void)dpy;
    __x11_syscall3(_SYS_XSETBG, (long)gc, 0, (long)bg);
    return 0;
}
int XFillRectangle(Display* dpy, Drawable d, GC gc,
                    int x, int y, unsigned w, unsigned h) {
    (void)dpy;
    __x11_syscall6(_SYS_XFILLRECT, (long)gc, (long)d,
                   (long)x, (long)y, (long)w, (long)h);
    return 0;
}
int XDrawRectangle(Display* dpy, Drawable d, GC gc,
                    int x, int y, unsigned w, unsigned h) {
    (void)dpy;
    __x11_syscall6(_SYS_XDRAWRECT, (long)gc, (long)d,
                   (long)x, (long)y, (long)w, (long)h);
    return 0;
}
int XDrawLine(Display* dpy, Drawable d, GC gc,
               int x1, int y1, int x2, int y2) {
    (void)dpy;
    __x11_syscall6(_SYS_XDRAWLINE, (long)gc, (long)d,
                   (long)x1, (long)y1, (long)x2, (long)y2);
    return 0;
}
int XDrawString(Display* dpy, Drawable d, GC gc,
                 int x, int y, const char* str, int len) {
    (void)dpy;
    __x11_syscall6(_SYS_XDRAWSTRING, (long)gc, (long)d,
                   (long)x, (long)y, (long)(long long)str, (long)len);
    return 0;
}
int XClearWindow(Display* dpy, Window w) {
    (void)dpy;
    __x11_syscall2(_SYS_XCLEARWIN, 0, (long)w);
    return 0;
}
int XClearArea(Display* dpy, Window w, int x, int y,
                unsigned wd, unsigned ht, Bool exp) {
    (void)exp;
    XClearWindow(dpy, w);   // Basit: tüm pencereyi temizle
    return 0;
}