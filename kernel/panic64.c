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
// 8×8 yedek font (ASCII 0x20–0x7F)
// Harici font erişimi yoksa panic buradaki kendi fontunu kullanır.
// ============================================================================
static const uint8_t pfont[96][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // ' '
    {0x18,0x18,0x18,0x18,0x18,0x00,0x18,0x00}, // '!'
    {0x6C,0x6C,0x6C,0x00,0x00,0x00,0x00,0x00}, // '"'
    {0x6C,0x6C,0xFE,0x6C,0xFE,0x6C,0x6C,0x00}, // '#'
    {0x18,0x7E,0xC0,0x7C,0x06,0xFC,0x18,0x00}, // '$'
    {0x00,0xC6,0xCC,0x18,0x30,0x66,0xC6,0x00}, // '%'
    {0x38,0x6C,0x38,0x76,0xDC,0xCC,0x76,0x00}, // '&'
    {0x30,0x30,0x60,0x00,0x00,0x00,0x00,0x00}, // '\''
    {0x18,0x30,0x60,0x60,0x60,0x30,0x18,0x00}, // '('
    {0x60,0x30,0x18,0x18,0x18,0x30,0x60,0x00}, // ')'
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, // '*'
    {0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00}, // '+'
    {0x00,0x00,0x00,0x00,0x00,0x30,0x30,0x60}, // ','
    {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00}, // '-'
    {0x00,0x00,0x00,0x00,0x00,0x30,0x30,0x00}, // '.'
    {0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00}, // '/'
    {0x7C,0xCE,0xDE,0xF6,0xE6,0xC6,0x7C,0x00}, // '0'
    {0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00}, // '1'
    {0x7C,0xC6,0x06,0x1C,0x70,0xC6,0xFE,0x00}, // '2'
    {0x7C,0xC6,0x06,0x3C,0x06,0xC6,0x7C,0x00}, // '3'
    {0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x1E,0x00}, // '4'
    {0xFE,0xC0,0xFC,0x06,0x06,0xC6,0x7C,0x00}, // '5'
    {0x3C,0x60,0xC0,0xFC,0xC6,0xC6,0x7C,0x00}, // '6'
    {0xFE,0xC6,0x0C,0x18,0x30,0x30,0x30,0x00}, // '7'
    {0x7C,0xC6,0xC6,0x7C,0xC6,0xC6,0x7C,0x00}, // '8'
    {0x7C,0xC6,0xC6,0x7E,0x06,0x0C,0x78,0x00}, // '9'
    {0x00,0x30,0x30,0x00,0x30,0x30,0x00,0x00}, // ':'
    {0x00,0x30,0x30,0x00,0x30,0x30,0x60,0x00}, // ';'
    {0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0x00}, // '<'
    {0x00,0x00,0x7E,0x00,0x7E,0x00,0x00,0x00}, // '='
    {0x60,0x30,0x18,0x0C,0x18,0x30,0x60,0x00}, // '>'
    {0x7C,0xC6,0x0C,0x18,0x18,0x00,0x18,0x00}, // '?'
    {0x7C,0xC6,0xDE,0xDE,0xDE,0xC0,0x7E,0x00}, // '@'
    {0x38,0x6C,0xC6,0xFE,0xC6,0xC6,0xC6,0x00}, // 'A'
    {0xFC,0x66,0x66,0x7C,0x66,0x66,0xFC,0x00}, // 'B'
    {0x3C,0x66,0xC0,0xC0,0xC0,0x66,0x3C,0x00}, // 'C'
    {0xF8,0x6C,0x66,0x66,0x66,0x6C,0xF8,0x00}, // 'D'
    {0xFE,0x62,0x68,0x78,0x68,0x62,0xFE,0x00}, // 'E'
    {0xFE,0x62,0x68,0x78,0x68,0x60,0xF0,0x00}, // 'F'
    {0x3C,0x66,0xC0,0xC0,0xCE,0x66,0x3E,0x00}, // 'G'
    {0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0x00}, // 'H'
    {0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, // 'I'
    {0x1E,0x0C,0x0C,0x0C,0xCC,0xCC,0x78,0x00}, // 'J'
    {0xE6,0x66,0x6C,0x78,0x6C,0x66,0xE6,0x00}, // 'K'
    {0xF0,0x60,0x60,0x60,0x62,0x66,0xFE,0x00}, // 'L'
    {0xC6,0xEE,0xFE,0xD6,0xC6,0xC6,0xC6,0x00}, // 'M'
    {0xC6,0xE6,0xF6,0xDE,0xCE,0xC6,0xC6,0x00}, // 'N'
    {0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00}, // 'O'
    {0xFC,0x66,0x66,0x7C,0x60,0x60,0xF0,0x00}, // 'P'
    {0x7C,0xC6,0xC6,0xC6,0xD6,0xDE,0x7C,0x00}, // 'Q'
    {0xFC,0x66,0x66,0x7C,0x6C,0x66,0xE6,0x00}, // 'R'
    {0x7C,0xC6,0x60,0x38,0x0C,0xC6,0x7C,0x00}, // 'S'
    {0x7E,0x5A,0x18,0x18,0x18,0x18,0x3C,0x00}, // 'T'
    {0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00}, // 'U'
    {0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x10,0x00}, // 'V'
    {0xC6,0xC6,0xD6,0xD6,0xFE,0xEE,0x6C,0x00}, // 'W'
    {0xC6,0x6C,0x38,0x38,0x38,0x6C,0xC6,0x00}, // 'X'
    {0x66,0x66,0x66,0x3C,0x18,0x18,0x3C,0x00}, // 'Y'
    {0xFE,0xCC,0x18,0x30,0x60,0xC6,0xFE,0x00}, // 'Z'
    {0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0x00}, // '['
    {0x80,0xC0,0x60,0x30,0x18,0x0C,0x06,0x00}, // '\'
    {0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00}, // ']'
    {0x10,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00}, // '^'
    {0x00,0x00,0x00,0x00,0x00,0x00,0xFE,0x00}, // '_'
    {0x30,0x18,0x0C,0x00,0x00,0x00,0x00,0x00}, // '`'
    {0x00,0x00,0x78,0x0C,0x7C,0xCC,0x76,0x00}, // 'a'
    {0xE0,0x60,0x7C,0x66,0x66,0x66,0x7C,0x00}, // 'b'
    {0x00,0x00,0x7C,0xC6,0xC0,0xC6,0x7C,0x00}, // 'c'
    {0x1C,0x0C,0x7C,0xCC,0xCC,0xCC,0x76,0x00}, // 'd'
    {0x00,0x00,0x7C,0xC6,0xFE,0xC0,0x7C,0x00}, // 'e'
    {0x1C,0x36,0x30,0x78,0x30,0x30,0x78,0x00}, // 'f'
    {0x00,0x00,0x76,0xCC,0xCC,0x7C,0x0C,0x78}, // 'g'
    {0xE0,0x60,0x6C,0x76,0x66,0x66,0xE6,0x00}, // 'h'
    {0x18,0x00,0x38,0x18,0x18,0x18,0x3C,0x00}, // 'i'
    {0x06,0x00,0x06,0x06,0x06,0x66,0x66,0x3C}, // 'j'
    {0xE0,0x60,0x66,0x6C,0x78,0x6C,0xE6,0x00}, // 'k'
    {0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, // 'l'
    {0x00,0x00,0xEC,0xFE,0xD6,0xD6,0xC6,0x00}, // 'm'
    {0x00,0x00,0xDC,0x66,0x66,0x66,0x66,0x00}, // 'n'
    {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7C,0x00}, // 'o'
    {0x00,0x00,0xDC,0x66,0x66,0x7C,0x60,0xF0}, // 'p'
    {0x00,0x00,0x76,0xCC,0xCC,0x7C,0x0C,0x1E}, // 'q'
    {0x00,0x00,0xDC,0x76,0x60,0x60,0xF0,0x00}, // 'r'
    {0x00,0x00,0x7C,0xC0,0x7C,0x06,0xFC,0x00}, // 's'
    {0x30,0x30,0xFC,0x30,0x30,0x36,0x1C,0x00}, // 't'
    {0x00,0x00,0xCC,0xCC,0xCC,0xCC,0x76,0x00}, // 'u'
    {0x00,0x00,0xC6,0xC6,0xC6,0x6C,0x38,0x00}, // 'v'
    {0x00,0x00,0xC6,0xD6,0xD6,0xFE,0x6C,0x00}, // 'w'
    {0x00,0x00,0xC6,0x6C,0x38,0x6C,0xC6,0x00}, // 'x'
    {0x00,0x00,0xC6,0xC6,0xC6,0x7E,0x06,0x7C}, // 'y'
    {0x00,0x00,0xFE,0x4C,0x18,0x32,0xFE,0x00}, // 'z'
    {0x0E,0x18,0x18,0x70,0x18,0x18,0x0E,0x00}, // '{'
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00}, // '|'
    {0x70,0x18,0x18,0x0E,0x18,0x18,0x70,0x00}, // '}'
    {0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00}, // '~'
    {0x00,0x10,0x38,0x6C,0xC6,0xFE,0x00,0x00}, // 0x7F
};

#define PFONT_W 8
#define PFONT_H 8

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

// Karakter çiz (scale: 1 veya 2 desteklenir)
static void draw_char_s(uint32_t px, uint32_t py, char ch,
                         uint32_t fg, uint32_t bg, int scale) {
    uint8_t idx = (uint8_t)ch;
    if (idx < 0x20 || idx > 0x7F) idx = 0x20;
    const uint8_t* g = pfont[idx - 0x20];
    for (int row=0; row<PFONT_H; row++)
        for (int sr=0; sr<scale; sr++) {
            uint8_t bits = g[row];
            for (int col=0; col<PFONT_W; col++)
                for (int sc=0; sc<scale; sc++)
                    ppix(px + col*scale + sc,
                         py + row*scale + sr,
                         (bits & (0x80 >> col)) ? fg : bg);
        }
}

static uint32_t draw_str_s(uint32_t px, uint32_t py, const char* s,
                             uint32_t fg, uint32_t bg, int scale) {
    uint32_t cx = px;
    while (*s) { draw_char_s(cx, py, *s, fg, bg, scale); cx += PFONT_W*scale; s++; }
    return cx;
}

static size_t pslen(const char* s) { size_t n=0; while(s[n])n++; return n; }

static void draw_str_centered(uint32_t cy, uint32_t area_x, uint32_t area_w,
                               const char* s, uint32_t fg, uint32_t bg, int scale) {
    uint32_t tw = (uint32_t)pslen(s) * PFONT_W * scale;
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
// CR2
// ============================================================================
static uint64_t read_cr2(void) {
    uint64_t v; __asm__ volatile("mov %%cr2,%0":"=r"(v)); return v;
}

// ============================================================================
// Serial dump
// ============================================================================
static void serial_dump(const exception_frame_t* f) {
    char buf[20];
    serial_print("\r\n============================================================\r\n");
    serial_print("*** KERNEL PANIC — EXCEPTION #");
    dec64(f->isr_num, buf); serial_print(buf);
    if (f->isr_num < 32) { serial_print(" "); serial_print(exc_names[f->isr_num]); }
    serial_print(" ***\r\n------------------------------------------------------------\r\n");

#define SD(lbl,val) do { hex64(val,buf); serial_print("  " lbl "  "); serial_print(buf); serial_print("\r\n"); } while(0)
    SD("RIP   ", f->rip);   SD("RSP   ", f->rsp);
    SD("CS    ", f->cs);    SD("SS    ", f->ss);
    SD("RFLAGS", f->rflags);SD("ERRCODE", f->err_code);
    if (f->isr_num == 14)   { SD("CR2   ", read_cr2()); }
    serial_print("  --- GPR ---\r\n");
    SD("RAX   ", f->rax);   SD("RBX   ", f->rbx);
    SD("RCX   ", f->rcx);   SD("RDX   ", f->rdx);
    SD("RSI   ", f->rsi);   SD("RDI   ", f->rdi);
    SD("RBP   ", f->rbp);
    SD("R8    ", f->r8);    SD("R9    ", f->r9);
    SD("R10   ", f->r10);   SD("R11   ", f->r11);
    SD("R12   ", f->r12);   SD("R13   ", f->r13);
    SD("R14   ", f->r14);   SD("R15   ", f->r15);
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
        uint64_t cr2 = read_cr2();
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

    // Double Fault özel not
    if (f->isr_num == 8) {
        draw_str_centered(cy, PX, PW,
            "DOUBLE FAULT — Likely: bad stack, bad IDT, or RSP misalign",
            0x00FF8888u, 0x00280000u, 1);
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