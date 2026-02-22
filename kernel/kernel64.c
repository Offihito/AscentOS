// kernel64.c — AscentOS 64-bit Unified Kernel
// Başlangıçta TEXT terminali açar.
// Kullanıcı "gfx" yazınca GUI moduna geçer.
// Mouse + Klavye her iki modda da interrupt-driven çalışır.

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define COM1 0x3F8

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
void serial_write(char c) { while(!(inb(COM1+5)&0x20)); outb(COM1,c); }
void serial_print(const char* s) { while(*s){if(*s=='\n')serial_write('\r');serial_write(*s++);} }
void serial_putchar(char c) { serial_write(c); }

size_t strlen64(const char* s) { size_t n=0; while(s[n])n++; return n; }
int strcmp64(const char* a, const char* b) { while(*a&&*a==*b){a++;b++;} return *(uint8_t*)a-*(uint8_t*)b; }
void* memset64(void* d, int v, size_t n) { uint8_t*p=d; while(n--)*p++=(uint8_t)v; return d; }
void* memcpy64(void* d, const void* s, size_t n) { uint8_t*dp=d; const uint8_t*sp=s; while(n--)*dp++=*sp++; return d; }

static inline void sse_init(void) {
    uint64_t cr0,cr4;
    __asm__ volatile("mov %%cr0,%0":"=r"(cr0));
    cr0&=~(1ULL<<2); cr0|=(1ULL<<1); cr0&=~(1ULL<<3);
    __asm__ volatile("mov %0,%%cr0"::"r"(cr0));
    __asm__ volatile("mov %%cr4,%0":"=r"(cr4));
    cr4|=(1ULL<<9)|(1ULL<<10);
    __asm__ volatile("mov %0,%%cr4"::"r"(cr4));
    serial_print("[SSE] OK\n");
}

void uint64_to_hex(uint64_t n, char* buf) {
    const char* h="0123456789ABCDEF"; buf[0]='0'; buf[1]='x';
    for(int i=0;i<16;i++) buf[2+i]=h[(n>>(60-i*4))&0xF]; buf[18]='\0';
}
void get_cpu_info(char* v) {
    uint32_t a,b,c,d;
    __asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(0));
    *(uint32_t*)(v+0)=b; *(uint32_t*)(v+4)=d; *(uint32_t*)(v+8)=c; v[12]='\0';
}

// Multiboot2
extern uint64_t multiboot_mmap_addr;
extern uint32_t multiboot_mmap_entry_size;
extern uint32_t multiboot_mmap_total_size;
typedef struct { uint64_t base,len; uint32_t type,res; } __attribute__((packed)) mb2_mmap_t;
struct mmap_entry { unsigned long base,len; unsigned int type,acpi; };
#define MAX_MMAP 64
static struct mmap_entry parsed_mmap[MAX_MMAP];
void pmm_init(struct mmap_entry*, uint32_t);
void vmm_init(void);
void gdt_install_user_segments(void);
void tss_init(void);
void task_init(void);
void scheduler_init(void);
void syscall_init(void);

// ============================================================================
// GLOBAL MOD DEĞİŞKENLERİ
// keyboard_unified.c ve commands64.c bunları okur/yazar
// ============================================================================
volatile int kernel_mode = 0;          // 0=TEXT  1=GUI
volatile int request_gui_start = 0;    // gfx komutu 1 yapar
volatile int gui_request_new_window = 0;

// ============================================================================
// TEXT MODE arayüz (vesa64.c)
// ============================================================================
void init_vesa64(void);
void print_str64(const char* str, uint8_t color);
void println64(const char* str, uint8_t color);
void putchar64(char c, uint8_t color);
void clear_screen64(void);
void set_position64(size_t row, size_t col);
void get_position64(size_t* row, size_t* col);
void get_screen_size64(size_t* w, size_t* h);
void scroll_up(size_t lines);
void scroll_down(size_t lines);
void update_cursor64(void);
void reset_to_standard_mode(void);
void set_extended_text_mode(void);

// ============================================================================
// GUI MODE (gui64 + compositor + wm + mouse)
// ============================================================================
#include "gui64.h"
#include "mouse64.h"
#include "taskbar64.h"
#include "compositor64.h"
#include "wm64.h"

static Compositor g_compositor;
static WindowManager g_wm;
static Taskbar g_taskbar;
static int g_taskbar_layer = -1;
static bool needs_full_redraw = false;

// ============================================================================
// PMM bootstrap
// ============================================================================
static uint32_t parse_mmap(void) {
    uint32_t es=multiboot_mmap_entry_size, ts=multiboot_mmap_total_size;
    uint64_t ma=multiboot_mmap_addr;
    if(!es||!ts||!ma) return 0;
    uint32_t n=0,off=0;
    while(off+es<=ts&&n<MAX_MMAP){
        mb2_mmap_t* e=(mb2_mmap_t*)(uint64_t)(ma+off);
        parsed_mmap[n].base=(unsigned long)e->base; parsed_mmap[n].len=(unsigned long)e->len;
        parsed_mmap[n].type=(unsigned int)e->type; parsed_mmap[n].acpi=0;
        n++; off+=es;
    }
    return n;
}
static void pmm_init_from_mb(void) {
    uint32_t n=parse_mmap();
    if(n){pmm_init(parsed_mmap,n);return;}
    struct mmap_entry fb[]={{0,0x9FC00,1,0},{0x9FC00,0x400,2,0},{0xF0000,0x10000,2,0},{0x100000,0x1FF00000,1,0}};
    pmm_init(fb,4); serial_print("[MMAP] Fallback\n");
}

// ============================================================================
// TEXT modunda boot ekranı
// ============================================================================
#define VGA_GREEN       0x02
#define VGA_CYAN        0x03
#define VGA_YELLOW      0x0E
#define VGA_LIGHT_GREEN 0x0A
#define VGA_WHITE       0x0F
#define VGA_MAGENTA     0x05

static void text_boot_screen(void) {
    char cpu[13]; get_cpu_info(cpu);
    println64("===============================================================", VGA_CYAN);
    println64("===        ASCENTOS 64-bit  v1.2  Unified Kernel          ===", VGA_LIGHT_GREEN);
    println64("===============================================================", VGA_CYAN);
    print_str64("  CPU : ", VGA_GREEN); println64(cpu, VGA_YELLOW);
    println64("  PMM, VMM, GDT, TSS, Scheduler, SYSCALL hazir", VGA_GREEN);
    println64("  Klavye + Mouse interrupt-driven aktif", VGA_GREEN);
    println64("", VGA_WHITE);
    println64("  help   - tum komutlari goster", VGA_LIGHT_GREEN);
    println64("  gfx    - GUI moduna gec (pencere yoneticisi)", VGA_YELLOW);
    println64("", VGA_WHITE);
}

// ============================================================================
// GUI modunu başlat (gfx komutundan request_gui_start=1 ile tetiklenir)
// ============================================================================
void init_mouse64(void);

static void gui_enter(void) {
    serial_print("[GFX] GUI moduna geciliyor...\n");

    kernel_mode = 1;   // klavye handler artık GUI dalına gider

    int sw=gui_get_width(), sh=gui_get_height();

    compositor_init(&g_compositor, sw, sh, RGB(44,44,44));

    g_taskbar_layer=compositor_create_layer(&g_compositor,LAYER_TYPE_TASKBAR,0,sh-40,sw,40);
    if(g_taskbar_layer>=0){
        Layer* tl=&g_compositor.layers[g_taskbar_layer];
        for(int i=0;i<sw*40;i++) tl->buffer[i]=RGB(30,30,30);
        compositor_mark_layer_dirty(&g_compositor,g_taskbar_layer);
    }
    compositor_render(&g_compositor);

    taskbar_init(&g_taskbar,sw,sh);
    uint8_t h,m,s; gui_get_rtc_time(&h,&m,&s);
    g_taskbar.current_hours=h; g_taskbar.current_minutes=m; g_taskbar.current_seconds=s;
    taskbar_draw(&g_taskbar);

    wm_init(&g_wm,sw,sh);
    wm_create_window(&g_compositor,&g_wm,&g_taskbar,100,60,420,280,"AscentOS");
    compositor_render(&g_compositor);
    taskbar_draw(&g_taskbar);

    init_mouse64();
    serial_print("[GFX] GUI hazir\n");
}

// ============================================================================
// GUI ana döngüsü — gui_enter()'dan sonra çağrılır, dönmez
// ============================================================================
static void gui_loop(void) {
    int sw=gui_get_width(), sh=gui_get_height();

    MouseState mouse;
    int lmx=-1,lmy=-1;
    bool llb=false;
    int drag_win=-1, dax=0,day=0,dwx=0,dwy=0;
    Color cbuf[20*18];
    int px=-100,py=-100; bool cr=false;

    mouse_get_state(&mouse);
    for(int r=0;r<20;r++) for(int c=0;c<18;c++)
        if(gui_is_valid_coord(mouse.x+c,mouse.y+r))
            cbuf[r*18+c]=gui_get_pixel(mouse.x+c,mouse.y+r);
    gui_draw_cursor(mouse.x,mouse.y);
    px=mouse.x; py=mouse.y; cr=true;
    uint8_t last_sec=0xFF;

    while(1){
        mouse_get_state(&mouse);

        // Saat
        uint8_t h,m,s; gui_get_rtc_time(&h,&m,&s);
        if(s!=last_sec){
            uint8_t oh=g_taskbar.current_hours, om=g_taskbar.current_minutes;
            g_taskbar.current_hours=h; g_taskbar.current_minutes=m; g_taskbar.current_seconds=s;
            last_sec=s;
            taskbar_update_clock_display(&g_taskbar,oh!=h||om!=m);
        }

        // Mouse hareketi
        bool mv=(mouse.x!=lmx||mouse.y!=lmy);
        if(mv){
            if(wm_is_resizing(&g_wm)){
                if(cr&&px>=0) for(int r=0;r<20;r++) for(int c=0;c<18;c++)
                    if(gui_is_valid_coord(px+c,py+r)) gui_put_pixel(px+c,py+r,cbuf[r*18+c]);
                cr=false; px=-100; py=-100;
                wm_update_resize(&g_wm,&g_compositor,mouse.x,mouse.y);
                compositor_render_dirty(&g_compositor);
                for(int r=0;r<20;r++) for(int c=0;c<18;c++)
                    if(gui_is_valid_coord(mouse.x+c,mouse.y+r)) cbuf[r*18+c]=gui_get_pixel(mouse.x+c,mouse.y+r);
                gui_draw_cursor(mouse.x,mouse.y); px=mouse.x; py=mouse.y; cr=true;
            } else if(drag_win>=0){
                int li=wm_get_layer_index(&g_wm,drag_win);
                if(li>=0){compositor_move_layer(&g_compositor,li,dwx+(mouse.x-dax),dwy+(mouse.y-day)); compositor_render_dirty(&g_compositor); px=-100;py=-100;}
            } else if(mouse.y>=sh-40){
                int oh=g_taskbar.hovered_button;
                taskbar_handle_mouse_move(&g_taskbar,mouse.x,mouse.y);
                if(oh!=g_taskbar.hovered_button) taskbar_draw(&g_taskbar);
            }
            lmx=mouse.x; lmy=mouse.y;
        }

        // Sol tık
        if(mouse.left_button&&!llb){
            if(mouse.y>=sh-40){
                int cid=taskbar_handle_mouse_click(&g_taskbar,mouse.x,mouse.y);
                if(cid>=0){wm_restore_window(&g_compositor,&g_wm,cid); needs_full_redraw=true;}
            } else {
                int lx,ly;
                int wid=wm_get_window_at(&g_compositor,&g_wm,mouse.x,mouse.y,&lx,&ly);
                if(wid>=0){
                    int li=wm_get_layer_index(&g_wm,wid);
                    Layer* layer=(li>=0)?&g_compositor.layers[li]:NULL;
                    WMHitResult hit=layer?wm_hit_test(layer->bounds.width,layer->bounds.height,lx,ly):WMHIT_NONE;
                    if(hit>=WMHIT_RESIZE_N){
                        wm_begin_resize(&g_wm,&g_compositor,wid,hit,mouse.x,mouse.y);
                    } else if(hit==WMHIT_TITLE){
                        drag_win=wid; dax=mouse.x; day=mouse.y;
                        if(li>=0){dwx=layer->bounds.x; dwy=layer->bounds.y;}
                        wm_focus_window(&g_compositor,&g_wm,wid);
                    } else {
                        wm_handle_click(&g_compositor,&g_wm,&g_taskbar,wid,lx,ly);
                        needs_full_redraw=true;
                    }
                }
            }
        }

        if(!mouse.left_button&&llb){
            bool wr=wm_is_resizing(&g_wm);
            drag_win=-1; wm_end_resize(&g_wm);
            if(wr) needs_full_redraw=true;
        }

        // Yeni pencere isteği (N tuşu)
        if(gui_request_new_window){
            gui_request_new_window=0;
            int n=g_wm.count+1; char title[32]; int ti=0;
            title[ti++]='P'; title[ti++]='e'; title[ti++]='n'; title[ti++]='c';
            title[ti++]='e'; title[ti++]='r'; title[ti++]='e'; title[ti++]=' ';
            if(n>=10) title[ti++]='0'+(n/10); title[ti++]='0'+(n%10); title[ti]='\0';
            if(wm_create_window(&g_compositor,&g_wm,&g_taskbar,
                120+(g_wm.count*30)%200, 80+(g_wm.count*28)%140, 400,280,title)>=0)
                needs_full_redraw=true;
        }

        llb=mouse.left_button;

        // Tam yeniden çizim
        if(needs_full_redraw){
            if(cr&&px>=0) for(int r=0;r<20;r++) for(int c=0;c<18;c++)
                if(gui_is_valid_coord(px+c,py+r)) gui_put_pixel(px+c,py+r,cbuf[r*18+c]);
            compositor_render(&g_compositor); taskbar_draw(&g_taskbar);
            needs_full_redraw=false; px=-100; py=-100; cr=false;
        }

        // Cursor çiz
        if(mouse.x!=px||mouse.y!=py){
            if(cr&&px>=0) for(int r=0;r<20;r++) for(int c=0;c<18;c++)
                if(gui_is_valid_coord(px+c,py+r)) gui_put_pixel(px+c,py+r,cbuf[r*18+c]);
            for(int r=0;r<20;r++) for(int c=0;c<18;c++)
                if(gui_is_valid_coord(mouse.x+c,mouse.y+r)) cbuf[r*18+c]=gui_get_pixel(mouse.x+c,mouse.y+r);
            gui_draw_cursor(mouse.x,mouse.y); px=mouse.x; py=mouse.y; cr=true;
        }

        __asm__ volatile("pause");
    }
}

// ============================================================================
// Dış init tanımları
// ============================================================================
void init_interrupts64(void);
void init_keyboard64(void);
void init_commands64(void);
void show_prompt64(void);

// ============================================================================
// KERNEL MAIN
// ============================================================================
void kernel_main(uint64_t multiboot_info) {
    (void)multiboot_info;
    serial_print("\n=== AscentOS Unified Kernel ===\n");

    init_vesa64();
    pmm_init_from_mb();
    vmm_init();
    gdt_install_user_segments();
    tss_init();
    task_init();
    scheduler_init();
    sse_init();
    syscall_init();

    // IRQ0(timer) + IRQ1(klavye) + IRQ12(mouse) hepsi init_interrupts64 içinde
    init_interrupts64();
    init_keyboard64();
    init_commands64();

    // gui64 framebuffer ptr'yi kur (henüz ekrana çizme)
    gui_init();

    text_boot_screen();
    show_prompt64();

    // Ana döngü: TEXT modunda bekle
    // gfx komutu request_gui_start=1 yapınca GUI'ye geç
    while(1){
        if(request_gui_start){
            request_gui_start=0;
            gui_enter();
            gui_loop();   // buradan dönmez
        }
        __asm__ volatile("sti; hlt");
    }
}