// panic64.c — AscentOS Kernel Panic Ekranı (VESA Framebuffer)
// CPU exception yakalandığında VESA framebuffer üzerinde kırmızı panic ekranı
// gösterir + serial porta tam register dump yapar.
//
// KURULUM (3 adım):
//   1. panic64.c → projeye ekle
//   2. interrupts64.asm patch'ini uygula (aşağıda açıklandı)
//   3. Makefile'a panic64.o ekle
//
// BAĞIMLILIK: boot64_unified.asm'deki framebuffer_* değişkenleri
//   (framebuffer_addr, framebuffer_pitch, framebuffer_width,
//    framebuffer_height, framebuffer_bpp)

#include <stdint.h>
#include <stddef.h>
#include "cpu64.h"   // cpu_get_cr2() ve diğer CPU yardımcıları
#include "font8x16.h" // 8x16 font

// ============================================================================
// Framebuffer — boot64_unified.asm tarafından doldurulur
// ============================================================================
extern uint64_t  framebuffer_addr;
extern uint32_t  framebuffer_pitch;
extern uint32_t  framebuffer_width;
extern uint32_t  framebuffer_height;
extern uint8_t   framebuffer_bpp;

// ============================================================================
// Serial (kernel64.c'de tanımlı)
// ============================================================================
extern void serial_print(const char* s);

// ============================================================================
// Stack sınır değişkenleri — task.c'den doldurulur.
// Panic öncesinde task_switch() veya task_create() içinden set edilmeli:
//
//   g_panic_user_stack_base = task->user_stack_base;
//   g_panic_user_stack_top  = task->user_stack_top;
//   g_panic_kern_stack_base = task->kernel_stack_base;
//   g_panic_kern_stack_top  = task->kernel_stack_top;
//
// panic64.h'a extern bildirimleri ekle, task.c'de #include "panic64.h" yap.
// ============================================================================

// ============================================================================
// Renkler (0x00RRGGBB — framebuffer little-endian 32bpp varsayımı)
// ============================================================================
#define COL_BG        0x00200000u   // Koyu kırmızı arka plan
#define COL_TITLE_BG  0x00AA0000u   // Parlak kırmızı başlık şeridi
#define COL_TITLE_FG  0x00FFFFFFu   // Beyaz başlık yazısı
#define COL_LABEL     0x00FF8800u   // Turuncu: register etiketi
#define COL_VALUE     0x0055FFFFu   // Cyan: register değeri
#define COL_ERR_NAME  0x00FFFFAAu   // Sarı: exception adı
#define COL_HINT      0x0088FF88u   // Yeşil: alt ipucu satırı
#define COL_BORDER    0x00FF4444u   // Kırmızı panel çerçevesi
#define COL_RIP_FG    0x00FFFFFFu   // Beyaz: RIP değeri (önemli)
#define COL_DIVIDER   0x00662222u   // Koyu kırmızı ayırıcı çizgi

// ============================================================================
// exception_frame_t
// interrupts64.asm'deki isr_panic_common push sırası ile BİREBİR eşleşmeli.
//
// Stack düzeni (düşük adresten yükseğe):
//   r15, r14, r13, r12, r11, r10, r9, r8   (ilk 8 × 8 = 64 byte)
//   rbp, rdi, rsi, rdx, rcx, rbx, rax      (7 × 8 = 56 byte)
//   err_code  (pseudo 0 veya CPU'nun push ettiği)
//   isr_num
//   --- CPU iretq frame ---
//   rip, cs, rflags, rsp*, ss*
//   (* sadece privilege change'de geçerli)
// ============================================================================
// Stack push sırası (isr_panic_common):
//   push rax, rbx, rcx, rdx, rsi, rdi, rbp, r8..r15
//   → r15 en düşük adreste (struct başı)
// Sonra asm: push 0 (err_code), push isr_num
//   → isr_num en düşük adres, err_code üstünde
//   YANİ: struct'ta isr_num önce, err_code sonra gelir!
typedef struct __attribute__((packed)) {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t isr_num;   // ← push %1 (son push = en düşük adres)
    uint64_t err_code;  // ← push 0 veya CPU err_code (önce push)
    // CPU iretq frame
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} exception_frame_t;

// ============================================================================
// Exception isimleri
// ============================================================================
static const char* const exc_names[32] = {
    "#DE Divide Error",           "#DB Debug",
    "#NMI Non-Maskable Int",      "#BP Breakpoint",
    "#OF Overflow",               "#BR Bound Range",
    "#UD Invalid Opcode",         "#NM Device Not Available",
    "#DF DOUBLE FAULT",           "#09 Coprocessor Overrun",
    "#TS Invalid TSS",            "#NP Segment Not Present",
    "#SS Stack-Segment Fault",    "#GP General Protection",
    "#PF Page Fault",             "#15 Reserved",
    "#MF x87 FP Error",           "#AC Alignment Check",
    "#MC Machine Check",          "#XF SIMD FP Exception",
    "#VE Virtualization",         "#CP Control Protection",
    "#16", "#17", "#18", "#19",   "#1A", "#1B",
    "#HV Hypervisor Injection",   "#VC VMM Communication",
    "#SX Security Exception",     "#1F Reserved",
};

// ============================================================================
// Font tanımı — font8x16.h'dan gelir
// ============================================================================
#define FONT_W 8
#define FONT_H 16

// ============================================================================
// Düşük seviyeli araçlar
// ============================================================================
static inline void ppix(uint32_t x, uint32_t y, uint32_t color) {
    if (!framebuffer_addr || x >= framebuffer_width || y >= framebuffer_height) return;
    uint8_t* p = (uint8_t*)(uintptr_t)framebuffer_addr
                 + y * framebuffer_pitch
                 + x * (framebuffer_bpp / 8);
    if (framebuffer_bpp == 32)      *(uint32_t*)p = color;
    else if (framebuffer_bpp == 24) { p[0]=color&0xFF; p[1]=(color>>8)&0xFF; p[2]=(color>>16)&0xFF; }
}

static void fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t c) {
    for (uint32_t dy=0; dy<h; dy++)
        for (uint32_t dx=0; dx<w; dx++)
            ppix(x+dx, y+dy, c);
}

static void draw_hline(uint32_t x, uint32_t y, uint32_t w, uint32_t c) {
    for (uint32_t i=0; i<w; i++) ppix(x+i, y, c);
}

static void draw_border(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t c) {
    for (uint32_t i=0; i<w; i++) { ppix(x+i,y,c); ppix(x+i,y+h-1,c); }
    for (uint32_t i=0; i<h; i++) { ppix(x,y+i,c); ppix(x+w-1,y+i,c); }
}

// Karakter çiz (8x16 font, scale: 1 veya 2 desteklenir)
static void draw_char_s(uint32_t px, uint32_t py, char ch,
                         uint32_t fg, uint32_t bg, int scale) {
    uint8_t idx = (uint8_t)ch;
    if (idx < 0x20 || idx > 0x7F) idx = 0x20;
    const uint8_t* g = font8x16[idx - 0x20];  // font8x16.h'dan gelen font
    
    for (int row = 0; row < FONT_H; row++) {
        uint8_t bits = g[row];
        for (int sr = 0; sr < scale; sr++) {
            for (int col = 0; col < FONT_W; col++) {
                uint8_t bit = (bits >> (7 - col)) & 1;
                for (int sc = 0; sc < scale; sc++) {
                    ppix(px + col * scale + sc,
                         py + row * scale + sr,
                         bit ? fg : bg);
                }
            }
        }
    }
}

static uint32_t draw_str_s(uint32_t px, uint32_t py, const char* s,
                             uint32_t fg, uint32_t bg, int scale) {
    uint32_t cx = px;
    while (*s) { 
        draw_char_s(cx, py, *s, fg, bg, scale); 
        cx += FONT_W * scale; 
        s++; 
    }
    return cx;
}

static size_t pslen(const char* s) { 
    size_t n = 0; 
    while(s[n]) n++; 
    return n; 
}

static void draw_str_centered(uint32_t cy, uint32_t area_x, uint32_t area_w,
                               const char* s, uint32_t fg, uint32_t bg, int scale) {
    uint32_t tw = (uint32_t)pslen(s) * FONT_W * scale;
    uint32_t x  = area_x + (tw < area_w ? (area_w - tw)/2 : 0);
    draw_str_s(x, cy, s, fg, bg, scale);
}

// ============================================================================
// Sayı → string dönüşümleri
// ============================================================================
static void hex64(uint64_t v, char* out) {
    static const char H[] = "0123456789ABCDEF";
    out[0]='0'; out[1]='x';
    for (int i=0; i<16; i++) out[2+i] = H[(v>>(60-i*4))&0xF];
    out[18] = '\0';
}
static void dec64(uint64_t v, char* out) {
    if (!v) { out[0]='0'; out[1]='\0'; return; }
    char t[21]; int i=0;
    while (v) { t[i++]='0'+(int)(v%10); v/=10; }
    int j=0; while(--i>=0) out[j++]=t[i]; out[j]='\0';
}
static void strcat_p(char* dst, const char* src) {
    while (*dst) dst++;
    while (*src) *dst++ = *src++;
    *dst = '\0';
}

// ============================================================================
// Serial dump
// ============================================================================
// ============================================================================
// Kernel/user stack sınırlarını dışarıdan bildir (task.c'de doldurulur).
// Sıfır bırakılırsa overflow kontrolü atlanır.
// ============================================================================
uint64_t g_panic_user_stack_base = 0;   // task->user_stack_base
uint64_t g_panic_user_stack_top  = 0;   // task->user_stack_top
uint64_t g_panic_kern_stack_base = 0;   // task->kernel_stack_base
uint64_t g_panic_kern_stack_top  = 0;   // task->kernel_stack_top

// #GP err_code: segment selector (sıfırsa segmentsiz genel koruma hatası)
static void print_gp_hint(uint64_t err_code, uint64_t rip, uint64_t rsp,
                           uint64_t fs_base) {
    serial_print("  --- #GP TANI ---\r\n");

    // err_code != 0 → belirli bir segment ihlali
    if (err_code & ~1ULL) {
        serial_print("  Segment selector=");
        char b[20]; hex64(err_code & ~1ULL, b); serial_print(b);
        serial_print((err_code & 1) ? " [EXT]\r\n" : " [IDT/LDT/GDT]\r\n");
    } else {
        serial_print("  err_code=0: genel koruma (segment yok)\r\n");
    }

    // FS_BASE sıfırsa musl __pthread_self → adres 0 → #GP en yaygın neden
    if (fs_base == 0 || fs_base < 0x1000ULL) {
        serial_print("  >> OLASI NEDEN: FS_BASE=0 → musl pthread_self() %fs:0 okuyor\r\n");
        serial_print("     → task_restore_fs_base() timer/syscall dönüşünde çağrıldı mı?\r\n");
    }

    // RIP NULL veya çok düşükse NULL dereference'tan gelmiş olabilir
    if (rip < 0x1000ULL) {
        serial_print("  >> OLASI NEDEN: RIP<0x1000 → NULL fonksiyon pointer çağrısı\r\n");
    }

    // RSP hizalaması: CALL öncesi 16-byte hizalı olmalı, sonra 8 kayar
    if (rsp & 0xF) {
        serial_print("  >> OLASI NEDEN: RSP hizasız (SSE/AVX talimatı veya ABI ihlali)\r\n");
        char b[20]; hex64(rsp, b);
        serial_print("     RSP="); serial_print(b); serial_print("\r\n");
    }
}

// #PF err_code bitlerini oku
static void print_pf_hint(uint64_t err_code, uint64_t cr2,
                           uint64_t user_base, uint64_t user_top) {
    serial_print("  --- #PF TANI ---\r\n");
    char b[20];
    serial_print("  CR2="); hex64(cr2, b); serial_print(b); serial_print("\r\n");
    serial_print("  Flags:");
    if (!(err_code & 1)) serial_print(" NOT_PRESENT");
    else                 serial_print(" PROTECTION");
    if (err_code & 2)    serial_print(" WRITE");
    else                 serial_print(" READ");
    if (err_code & 4)    serial_print(" USER");
    if (err_code & 8)    serial_print(" RSVD_BIT");
    if (err_code & 16)   serial_print(" INSTR_FETCH");
    serial_print("\r\n");

    // Kullanıcı stack'inin hemen altına düştüyse → stack overflow
    if (user_base && user_top && cr2 < user_base && cr2 >= user_base - 65536ULL) {
        serial_print("  >> STACK OVERFLOW: CR2 user stack tabaninin hemen altinda!\r\n");
        serial_print("     user_stack_base="); hex64(user_base, b);
        serial_print(b); serial_print("\r\n");
        serial_print("     Cozum: USER_STACK_SIZE artirin veya Doom rekürsyonunu azaltin.\r\n");
    } else if (cr2 < 0x1000ULL) {
        serial_print("  >> NULL pointer dereference (CR2<0x1000)\r\n");
    }
}

// Stack overflow tespiti: RSP stack sınırlarını aştı mı?
static void check_stack_overflow(uint64_t rsp,
                                  uint64_t kern_base, uint64_t kern_top,
                                  uint64_t user_base, uint64_t user_top) {
    serial_print("  --- STACK OVERFLOW KONTROLU ---\r\n");
    char b[20];

    int checked = 0;

    if (kern_base && kern_top) {
        checked = 1;
        serial_print("  Kernel stack: [");
        hex64(kern_base, b); serial_print(b); serial_print(" — ");
        hex64(kern_top,  b); serial_print(b); serial_print("]\r\n");

        if (rsp < kern_base) {
            serial_print("  !! KERNEL STACK OVERFLOW: RSP taşmış! RSP=");
            hex64(rsp, b); serial_print(b);
            serial_print("  altinda="); hex64(kern_base - rsp, b);
            serial_print(b); serial_print(" byte\r\n");
        } else if (rsp >= kern_base && rsp < kern_base + 512) {
            serial_print("  !! KERNEL STACK KRITIK: RSP tabana cok yakin (<512 byte)\r\n");
        } else if (rsp >= kern_base && rsp <= kern_top) {
            uint64_t used = kern_top - rsp;
            serial_print("  Kernel stack kullanimiı: ");
            hex64(used, b); serial_print(b); serial_print(" byte\r\n");
        } else {
            serial_print("  RSP kernel stack disinda. RSP=");
            hex64(rsp, b); serial_print(b); serial_print("\r\n");
        }
    }

    if (user_base && user_top) {
        checked = 1;
        serial_print("  User stack:   [");
        hex64(user_base, b); serial_print(b); serial_print(" — ");
        hex64(user_top,  b); serial_print(b); serial_print("]\r\n");

        if (rsp < user_base) {
            serial_print("  !! USER STACK OVERFLOW: RSP tasmiş! RSP=");
            hex64(rsp, b); serial_print(b);
            serial_print("  altinda="); hex64(user_base - rsp, b);
            serial_print(b); serial_print(" byte\r\n");
            serial_print("  >> Cozum: task_create_user() icerisinde USER_STACK_SIZE degerini artirin.\r\n");
        } else if (rsp >= user_base && rsp < user_base + 4096) {
            serial_print("  !! USER STACK KRITIK: RSP tabana cok yakin (<4KB)\r\n");
        } else if (rsp >= user_base && rsp <= user_top) {
            uint64_t used = user_top - rsp;
            serial_print("  User stack kullanimi: ");
            hex64(used, b); serial_print(b); serial_print(" byte\r\n");
        }
    }

    if (!checked)
        serial_print("  Stack sinirlari bilinmiyor (g_panic_*_stack_* set edilmemis)\r\n");
}

static void serial_dump(const exception_frame_t* f) {
    char buf[20];
    serial_print("\r\n============================================================\r\n");
    serial_print("*** KERNEL PANIC — EXCEPTION #");
    dec64(f->isr_num, buf); serial_print(buf);
    if (f->isr_num < 32) { serial_print(" "); serial_print(exc_names[f->isr_num]); }
    serial_print(" ***\r\n------------------------------------------------------------\r\n");

#define SD(lbl,val) do { hex64(val,buf); serial_print("  " lbl "  "); serial_print(buf); serial_print("\r\n"); } while(0)
    SD("RIP    ", f->rip);    SD("RSP    ", f->rsp);
    SD("CS     ", f->cs);     SD("SS     ", f->ss);
    SD("RFLAGS ", f->rflags); SD("ERRCODE", f->err_code);

    // CPL
    {
        int ring = (int)(f->cs & 3);
        serial_print("  CPL    = Ring-");
        buf[0] = '0' + ring; buf[1] = '\r'; buf[2] = '\n'; buf[3] = 0;
        serial_print(buf);
        if (ring == 0) serial_print("  *** Panic kernel (Ring-0) modunda ***\r\n");
        else           serial_print("  *** Panic kullanici (Ring-3) modunda ***\r\n");
    }

    // FS_BASE — erken oku, hem GP hem genel tana icin kullanilir
    uint64_t fs_base = 0;
    {
        uint32_t _lo = 0, _hi = 0;
        __asm__ volatile("rdmsr" : "=a"(_lo),"=d"(_hi) : "c"(0xC0000100u));
        fs_base = ((uint64_t)_hi << 32) | _lo;
        SD("FS_BASE", fs_base);
        if (fs_base == 0)
            serial_print("  *** FS_BASE=0: musl pthread_self() -> #GP ***\r\n");
        else if (fs_base < 0x1000ULL)
            serial_print("  *** FS_BASE<0x1000: gecersiz TLS ***\r\n");
    }

    // Exception'a ozel tani
    if (f->isr_num == 13)
        print_gp_hint(f->err_code, f->rip, f->rsp, fs_base);

    if (f->isr_num == 14) {
        uint64_t cr2 = cpu_get_cr2();
        print_pf_hint(f->err_code, cr2,
                      g_panic_user_stack_base, g_panic_user_stack_top);
    }

    if (f->isr_num == 12)
        serial_print("  *** #SS: stack overflow veya gecersiz SS segment ***\r\n");

    if (f->isr_num == 8)
        serial_print("  *** #DF: kotu stack / IDT hatasi / RSP hizasizligi ***\r\n");

    // Stack overflow kontrolu
    check_stack_overflow(f->rsp,
                         g_panic_kern_stack_base, g_panic_kern_stack_top,
                         g_panic_user_stack_base, g_panic_user_stack_top);

    // GPR tablosu
    serial_print("  --- GPR ---\r\n");
    SD("RAX   ", f->rax);   SD("RBX   ", f->rbx);
    SD("RCX   ", f->rcx);   SD("RDX   ", f->rdx);
    SD("RSI   ", f->rsi);   SD("RDI   ", f->rdi);
    SD("RBP   ", f->rbp);
    SD("R8    ", f->r8);    SD("R9    ", f->r9);
    SD("R10   ", f->r10);   SD("R11   ", f->r11);
    SD("R12   ", f->r12);   SD("R13   ", f->r13);
    SD("R14   ", f->r14);   SD("R15   ", f->r15);

    // Stack walk — RSP'den 24 slot
    serial_print("  --- STACK WALK (RSP) ---\r\n");
    {
        uint64_t* sp = (uint64_t*)(uintptr_t)f->rsp;
        if ((uint64_t)(uintptr_t)sp >= 0x1000ULL &&
            (uint64_t)(uintptr_t)sp <  0xFFFFFFFFFFFFULL) {
            for (int i = 0; i < 24; i++) {
                serial_print("  [RSP+");
                buf[0] = '0' + (char)((i*8)/100%10);
                buf[1] = '0' + (char)((i*8)/ 10%10);
                buf[2] = '0' + (char)((i*8)    %10);
                buf[3] = ']'; buf[4] = ' '; buf[5] = 0;
                serial_print(buf);
                hex64(sp[i], buf); serial_print(buf);
                serial_print("\r\n");
            }
        } else {
            serial_print("  RSP gecersiz aralikta, walk atlanıyor\r\n");
        }
    }

    // Frame pointer zinciri (GCC -fno-omit-frame-pointer ile anlamlı)
    serial_print("  --- FRAME POINTER CHAIN ---\r\n");
    {
        uint64_t rbp = f->rbp;
        for (int depth = 0; depth < 16 && rbp != 0; depth++) {
            if (rbp < 0x1000ULL || (rbp & 7)) break;
            uint64_t saved_rbp = ((uint64_t*)(uintptr_t)rbp)[0];
            uint64_t ret_addr  = ((uint64_t*)(uintptr_t)rbp)[1];
            buf[0] = '#'; buf[1] = '0' + (depth/10); buf[2] = '0' + (depth%10);
            buf[3] = ' '; buf[4] = 0;
            serial_print("  "); serial_print(buf);
            serial_print("ret="); hex64(ret_addr,  buf); serial_print(buf);
            serial_print("  rbp="); hex64(saved_rbp, buf); serial_print(buf);
            serial_print("\r\n");
            rbp = saved_rbp;
        }
    }

    serial_print("============================================================\r\n");
    serial_print("  System HALTED.\r\n");
#undef SD
}

// ============================================================================
// ANA PANİK FONKSİYONU — interrupts64.asm'den çağrılır
// ============================================================================
void kernel_panic_handler(exception_frame_t* f) {
    serial_dump(f);

    if (!framebuffer_addr || !framebuffer_width || !framebuffer_height) goto halt;

    uint32_t SW = framebuffer_width;
    uint32_t SH = framebuffer_height;

    // Panel
    uint32_t PW = (SW > 760) ? 760 : SW;
    uint32_t PH = (SH > 490) ? 490 : SH - 20;
    uint32_t PX = (SW - PW) / 2;
    uint32_t PY = (SH - PH) / 2;

    // Ekran → koyu kırmızı
    fill_rect(0, 0, SW, SH, COL_BG);

    // Panel arka plan + çerçeve
    fill_rect(PX, PY, PW, PH, 0x00280000u);
    draw_border(PX,   PY,   PW,   PH,   COL_BORDER);
    draw_border(PX+2, PY+2, PW-4, PH-4, 0x00882222u);

    // ── Başlık şeridi ──────────────────────────────────────────────────────
    uint32_t TH = 28;
    fill_rect(PX+3, PY+3, PW-6, TH, COL_TITLE_BG);
    draw_str_centered(PY+6, PX, PW, "*** KERNEL PANIC ***", COL_TITLE_FG, COL_TITLE_BG, 2);

    uint32_t cy = PY + TH + 10;

    // Exception adı + numarası
    {
        char etitle[64] = "Exception #";
        char ibuf[8]; dec64(f->isr_num, ibuf);
        strcat_p(etitle, ibuf);
        strcat_p(etitle, "  ");
        strcat_p(etitle, (f->isr_num < 32) ? exc_names[f->isr_num] : "Unknown");
        draw_str_centered(cy, PX, PW, etitle, COL_ERR_NAME, 0x00280000u, 1);
    }
    cy += 14;

    // Ayırıcı
    draw_hline(PX+10, cy, PW-20, COL_DIVIDER); cy += 8;

    // ── Önemli register'lar ────────────────────────────────────────────────
    uint32_t CX1 = PX + 18;
    uint32_t CX2 = PX + PW/2 + 10;
    char buf[20];

#define LABEL_W 56   // etiket genişliği (piksel)
#define VAL_W   144  // "0x..." genişliği

    // RIP — beyaz, öne çıkar
    draw_str_s(CX1, cy, "RIP:", COL_LABEL, 0x00280000u, 1);
    hex64(f->rip, buf);
    draw_str_s(CX1 + LABEL_W, cy, buf, COL_RIP_FG, 0x00280000u, 1);
    draw_str_s(CX2, cy, "CS: ", COL_LABEL, 0x00280000u, 1);
    hex64(f->cs, buf);
    draw_str_s(CX2 + LABEL_W, cy, buf, COL_VALUE, 0x00280000u, 1);
    cy += 12;

    draw_str_s(CX1, cy, "RSP:", COL_LABEL, 0x00280000u, 1);
    hex64(f->rsp, buf);
    draw_str_s(CX1 + LABEL_W, cy, buf, COL_VALUE, 0x00280000u, 1);
    draw_str_s(CX2, cy, "SS: ", COL_LABEL, 0x00280000u, 1);
    hex64(f->ss, buf);
    draw_str_s(CX2 + LABEL_W, cy, buf, COL_VALUE, 0x00280000u, 1);
    cy += 12;

    draw_str_s(CX1, cy, "RFLAGS:", COL_LABEL, 0x00280000u, 1);
    hex64(f->rflags, buf);
    draw_str_s(CX1 + 64, cy, buf, COL_VALUE, 0x00280000u, 1);
    draw_str_s(CX2, cy, "ERRCODE:", COL_LABEL, 0x00280000u, 1);
    hex64(f->err_code, buf);
    draw_str_s(CX2 + 68, cy, buf, COL_VALUE, 0x00280000u, 1);
    cy += 14;

    // Page Fault özel bilgi
    if (f->isr_num == 14) {
        uint64_t cr2 = cpu_get_cr2();
        draw_str_s(CX1, cy, "CR2 (fault addr):", 0x00FFAA55u, 0x00280000u, 1);
        hex64(cr2, buf);
        draw_str_s(CX1 + 144, cy, buf, COL_RIP_FG, 0x00280000u, 1);
        cy += 12;

        draw_str_s(CX1, cy, "#PF: ", COL_LABEL, 0x00280000u, 1);
        uint32_t fx = CX1 + 44;
        uint64_t ec = f->err_code;
        if (ec & 1)  { draw_str_s(fx, cy, "[PROT]",        0x00FF8888u, 0x00280000u, 1); fx += 56; }
        else          { draw_str_s(fx, cy, "[NOT_PRESENT]", 0x00FF8888u, 0x00280000u, 1); fx += 104; }
        if (ec & 2)  { draw_str_s(fx, cy, "[WRITE]",  0x00FFAA00u, 0x00280000u, 1); fx += 56; }
        else          { draw_str_s(fx, cy, "[READ]",   0x00FFAA00u, 0x00280000u, 1); fx += 48; }
        if (ec & 4)  { draw_str_s(fx, cy, "[USER]",   0x00FFFF00u, 0x00280000u, 1); fx += 48; }
        if (ec & 16) { draw_str_s(fx, cy, "[IF]",     0x00FF5555u, 0x00280000u, 1); fx += 32; }
        cy += 14;
    }

    // Stack-Segment Fault özel not (#SS = 12)
    if (f->isr_num == 12) {
        draw_str_centered(cy, PX, PW,
            "#SS STACK FAULT — Stack overflow or bad SS segment",
            0x00FF8888u, 0x00280000u, 1);
        cy += 12;
    }

    // #GP özel not
    if (f->isr_num == 13) {
        if (f->err_code == 0) {
            uint32_t _lo=0,_hi=0;
            __asm__ volatile("rdmsr"::"c"(0xC0000100u),"a"(_lo),"d"(_hi));
            // FS_BASE okunamaz burada (clobber), sadece genel mesaj yeterli
            draw_str_centered(cy, PX, PW,
                "#GP err_code=0: NULL func ptr / bad RSP align / FS_BASE=0",
                0x00FF8888u, 0x00280000u, 1);
        } else {
            draw_str_centered(cy, PX, PW,
                "#GP: Segment ihlali — selector bilgisi serial logda",
                0x00FF8888u, 0x00280000u, 1);
        }
        cy += 12;
        draw_str_centered(cy, PX, PW,
            "Bkz: serial log -> #GP TANI bolumu",
            0x00FFAA55u, 0x00280000u, 1);
        cy += 12;
    }

    // Stack overflow özeti (user stack taştıysa)
    if (g_panic_user_stack_base && f->rsp < g_panic_user_stack_base) {
        draw_str_centered(cy, PX, PW,
            "!! USER STACK OVERFLOW !! RSP stack tabaninin altinda",
            0x00FF4444u, 0x00280000u, 1);
        cy += 12;
        draw_str_centered(cy, PX, PW,
            "Cozum: USER_STACK_SIZE artirin (task_create_user)",
            0x00FFAA55u, 0x00280000u, 1);
        cy += 12;
    }
    if (g_panic_kern_stack_base && f->rsp < g_panic_kern_stack_base) {
        draw_str_centered(cy, PX, PW,
            "!! KERNEL STACK OVERFLOW !! RSP kernel stack tabaninin altinda",
            0x00FF4444u, 0x00280000u, 1);
        cy += 12;
    }

    // Double Fault özel not (#DF = 8)
    if (f->isr_num == 8) {
        draw_str_centered(cy, PX, PW,
            "DOUBLE FAULT — Likely: bad stack, bad IDT, or RSP misalign",
            0x00FF8888u, 0x00280000u, 1);
        cy += 12;
        // #DF err_code her zaman 0'dır; RSP değeri taşmış stack'i işaret eder
        draw_str_centered(cy, PX, PW,
            "Tip: Add IST entry to IDT #SS/#DF to survive stack overflow",
            0x00FFAA55u, 0x00280000u, 1);
        cy += 12;
    }

    // Ayırıcı
    draw_hline(PX+10, cy, PW-20, COL_DIVIDER); cy += 6;

    // ── GPR tablosu (2 sütun) ─────────────────────────────────────────────
    draw_str_centered(cy, PX, PW, "General Purpose Registers",
                      0x00AAAAFF, 0x00280000u, 1);
    cy += 12;

    typedef struct { const char* n; uint64_t v; } R;
    R regs[15] = {
        {"RAX:", f->rax}, {"RBX:", f->rbx}, {"RCX:", f->rcx}, {"RDX:", f->rdx},
        {"RSI:", f->rsi}, {"RDI:", f->rdi}, {"RBP:", f->rbp},
        {"R8: ", f->r8},  {"R9: ", f->r9},  {"R10:", f->r10}, {"R11:", f->r11},
        {"R12:", f->r12}, {"R13:", f->r13}, {"R14:", f->r14}, {"R15:", f->r15},
    };

    for (int i=0; i<15; i+=2) {
        draw_str_s(CX1,       cy, regs[i].n,   COL_LABEL, 0x00280000u, 1);
        hex64(regs[i].v, buf);
        draw_str_s(CX1 + 36,  cy, buf, COL_VALUE, 0x00280000u, 1);
        if (i+1 < 15) {
            draw_str_s(CX2,      cy, regs[i+1].n, COL_LABEL, 0x00280000u, 1);
            hex64(regs[i+1].v, buf);
            draw_str_s(CX2 + 36, cy, buf, COL_VALUE, 0x00280000u, 1);
        }
        cy += 11;
    }

    // Alt ipucu
    cy = PY + PH - 20;
    draw_str_centered(cy, PX, PW,
        "System HALTED. See serial output (COM1) for full dump.",
        COL_HINT, 0x00280000u, 1);

halt:
    __asm__ volatile("cli\n1: hlt\njmp 1b\n");
}