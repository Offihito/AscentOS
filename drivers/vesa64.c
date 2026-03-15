// vesa64.c - VESA Framebuffer Text Mode Driver
// VGA text mode (0xB8000) yerine Multiboot2 framebuffer kullanır.
// Font: 8x16 pixel, gömülü PC Screen Font (PSF1 uyumlu bitmap)

#include <stdint.h>
#include <stddef.h>

// ============================================================================
// Multiboot2'nin sağladığı framebuffer bilgileri
// boot64_unified.asm tarafından doldurulur
// ============================================================================
extern uint64_t framebuffer_addr;
extern uint32_t framebuffer_pitch;   // satır başına byte
extern uint32_t framebuffer_width;   // pixel cinsinden genişlik
extern uint32_t framebuffer_height;  // pixel cinsinden yükseklik
extern uint8_t  framebuffer_bpp;     // bits per pixel (genellikle 32)

// Font verisi ayrı dosyaya taşındı → kernel/font8x16.c
#include "../kernel/font8x16.h"

// ============================================================================
// VGA 16 renk paleti → 32-bit RGB
// ============================================================================
static const uint32_t vga_palette[16] = {
    0xFF000000,  // 0  Siyah
    0xFF0000AA,  // 1  Mavi
    0xFF00AA00,  // 2  Yeşil
    0xFF00AAAA,  // 3  Cyan
    0xFFAA0000,  // 4  Kırmızı
    0xFFAA00AA,  // 5  Magenta
    0xFFAA5500,  // 6  Kahverengi
    0xFFAAAAAA,  // 7  Açık gri
    0xFF555555,  // 8  Koyu gri
    0xFF5555FF,  // 9  Açık mavi
    0xFF55FF55,  // 10 Açık yeşil
    0xFF55FFFF,  // 11 Açık cyan
    0xFFFF5555,  // 12 Açık kırmızı
    0xFFFF55FF,  // 13 Açık magenta
    0xFFFFFF55,  // 14 Sarı
    0xFFFFFFFF,  // 15 Beyaz
};

// ============================================================================
// İç durum
// ============================================================================
#define FONT_WIDTH  8
#define FONT_HEIGHT 16

// Scroll back buffer boyutu (satır cinsinden)
#define SCROLL_BACK_LINES 500

static uint8_t*  fb     = (uint8_t*)0;  // framebuffer başlangıç adresi
static uint32_t  fb_w   = 0;            // genişlik (pixel)
static uint32_t  fb_h   = 0;            // yükseklik (pixel)
static uint32_t  fb_p   = 0;            // pitch (satır başına byte)
static uint8_t   fb_bpp = 32;           // bit per pixel

// Karakter grid boyutu
static size_t cols = 0;  // sütun sayısı
static size_t rows = 0;  // satır sayısı

// Cursor pozisyonu
static size_t cur_col = 0;
static size_t cur_row = 0;

// Pending wrap: satır sonuna ulaşıldı ama henüz newline yapılmadı.
// Bir sonraki yazdırılabilir karakter gelince newline tetiklenir.
// Cursor hareketi komutları (CUP, CUU, CUD vb.) bu flag'i temizler.
static int pending_wrap = 0;

// Mevcut renk (VGA renk byte'ı: yüksek nibble = bg, düşük nibble = fg)
static uint8_t cur_color = 0x07;  // açık gri ön plan, siyah arka plan

// Yazılımsal cursor (alttaki pixel satırında cursor çizgisi)
static int cursor_visible = 1;

// ============================================================================
// Scroll back buffer
// Her "satır" COLS adet {char, color} ikilisi içerir.
// ============================================================================
typedef struct {
    uint8_t ch;
    uint8_t color;
    uint8_t dirty;  // 1 = yeniden çizilmeli
} Cell;

// Aktif ekran içeriği (karakter hücreleri)
// Maksimum 240x68 = ~16KB, stack'te değil BSS'de tutulur
#define MAX_COLS 240
#define MAX_ROWS 68

static Cell screen_cells[MAX_ROWS][MAX_COLS];

// Scroll back buffer: geçmiş satırlar
static Cell scroll_back[SCROLL_BACK_LINES][MAX_COLS];
static size_t scroll_back_count = 0;   // buffer'da kaç satır var
static size_t scroll_view_offset = 0;  // şu an kaç satır geri bakıyoruz

// Forward declarations
void update_cursor64(void);
void clear_screen64(void);

// ============================================================================
// I/O portları (cursor için — VESA'da yazılımsal cursor kullanıyoruz)
// ============================================================================
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// ============================================================================
// Düşük seviyeli pixel yazma
// ============================================================================

// Tek bir piksel yaz (32bpp varsayılır, 24bpp de desteklenir)
static inline void put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (x >= fb_w || y >= fb_h) return;

    uint8_t* ptr = fb + y * fb_p + x * (fb_bpp / 8);

    if (fb_bpp == 32) {
        *(uint32_t*)ptr = color;
    } else if (fb_bpp == 24) {
        ptr[0] = color & 0xFF;
        ptr[1] = (color >> 8) & 0xFF;
        ptr[2] = (color >> 16) & 0xFF;
    }
}

// ============================================================================
// Font glyph çizimi
// ============================================================================
static void draw_char_at_pixel(uint32_t px, uint32_t py,
                                char c, uint8_t color_byte) {
    uint32_t fg = vga_palette[color_byte & 0x0F];
    uint32_t bg = vga_palette[(color_byte >> 4) & 0x07];

    // Geçerli ASCII aralığı kontrolü
    uint8_t idx = (uint8_t)c;
    if (idx < 32 || idx > 127) idx = 32;  // bilinmeyen → boşluk
    const uint8_t* glyph = font8x16[idx - 32];

    for (int row = 0; row < FONT_HEIGHT; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < FONT_WIDTH; col++) {
            uint32_t pcolor = (bits & (0x80 >> col)) ? fg : bg;
            put_pixel(px + col, py + row, pcolor);
        }
    }
}

// Karakter koordinatından piksel koordinatına
static inline uint32_t char_to_px(size_t col) { return (uint32_t)(col * FONT_WIDTH);  }
static inline uint32_t char_to_py(size_t row) { return (uint32_t)(row * FONT_HEIGHT); }

// ============================================================================
// Hücre tabanlı çizim
// ============================================================================

// Ekrandaki tek bir hücreyi yeniden çiz
static void redraw_cell(size_t row, size_t col) {
    if (row >= rows || col >= cols) return;
    Cell* c = &screen_cells[row][col];
    draw_char_at_pixel(char_to_px(col), char_to_py(row), c->ch, c->color);
}

// Tüm ekranı hücrelerden yeniden çiz
static void redraw_all(void) {
    for (size_t r = 0; r < rows; r++)
        for (size_t c = 0; c < cols; c++)
            redraw_cell(r, c);
    // Cursor çiz
    update_cursor64();
}

// ============================================================================
// Cursor (yazılımsal: son satırda 2px yüksek beyaz çizgi)
// ============================================================================
void update_cursor64(void) {
    if (!cursor_visible) return;

    uint32_t px = char_to_px(cur_col);
    uint32_t py = char_to_py(cur_row);

    // Cursor: karakterin altındaki 2 piksel satırı
    uint32_t cursor_color = vga_palette[cur_color & 0x0F];
    for (int i = 0; i < 2; i++) {
        uint32_t cy = py + FONT_HEIGHT - 2 + i;
        for (int x = 0; x < FONT_WIDTH; x++) {
            put_pixel(px + x, cy, cursor_color);
        }
    }
}

// Cursor'un üstündeki hücreyi yeniden çizerek cursor'u sil
static void erase_cursor(void) {
    if (cur_row < rows && cur_col < cols)
        redraw_cell(cur_row, cur_col);
}

// ============================================================================
// Scroll
// ============================================================================

// Ekranı 1 satır yukarı kaydır, en alta boş satır ekle
static void scroll_screen_up(void) {
    // En üst satırı scroll_back buffer'a kaydet
    if (scroll_back_count < SCROLL_BACK_LINES) {
        for (size_t c = 0; c < cols; c++)
            scroll_back[scroll_back_count][c] = screen_cells[0][c];
        scroll_back_count++;
    } else {
        // Buffer doluysa en eskisini at (dairesel değil, shift yap)
        for (size_t i = 0; i < SCROLL_BACK_LINES - 1; i++)
            for (size_t c = 0; c < cols; c++)
                scroll_back[i][c] = scroll_back[i+1][c];
        for (size_t c = 0; c < cols; c++)
            scroll_back[SCROLL_BACK_LINES-1][c] = screen_cells[0][c];
    }

    // Hücreleri 1 satır yukarı kaydır
    for (size_t r = 0; r < rows - 1; r++)
        for (size_t c = 0; c < cols; c++) {
            screen_cells[r][c] = screen_cells[r+1][c];
            screen_cells[r][c].dirty = 1;
        }

    // Son satırı temizle
    for (size_t c = 0; c < cols; c++) {
        screen_cells[rows-1][c].ch    = ' ';
        screen_cells[rows-1][c].color = cur_color;
        screen_cells[rows-1][c].dirty = 1;
    }

    // Framebuffer'ı 64-bit adımlarla kaydır (hızlı memmove)
    uint32_t line_bytes = (uint32_t)(FONT_HEIGHT * fb_p);
    uint64_t* src64 = (uint64_t*)(fb + line_bytes);
    uint64_t* dst64 = (uint64_t*)fb;
    uint32_t total64 = (line_bytes * (uint32_t)(rows - 1)) / 8;
    for (uint32_t i = 0; i < total64; i++)
        dst64[i] = src64[i];
    // Son satırı arkaplan rengiyle doldur
    uint32_t bg = vga_palette[(cur_color >> 4) & 0x07];
    uint64_t bg64 = ((uint64_t)bg << 32) | bg;
    uint64_t* last64 = (uint64_t*)(fb + (uint32_t)(rows-1) * line_bytes);
    uint32_t fill64 = (fb_p * FONT_HEIGHT) / 8;
    for (uint32_t i = 0; i < fill64; i++)
        last64[i] = bg64;
}

// ============================================================================
// Yeni satır
// ============================================================================
static void do_newline(void) {
    cur_col = 0;
    cur_row++;
    if (cur_row >= rows) {
        cur_row = rows - 1;
        scroll_screen_up();
    }
}

// ============================================================================
// Dışa açık API
// ============================================================================

void init_vesa64(void) {
    fb     = (uint8_t*)(uintptr_t)framebuffer_addr;
    fb_w   = framebuffer_width;
    fb_h   = framebuffer_height;
    fb_p   = framebuffer_pitch;
    fb_bpp = framebuffer_bpp;

    // Framebuffer yoksa (VESA desteklenmiyorsa) güvenli çıkış
    if (!fb || fb_w == 0 || fb_h == 0) return;

    // Karakter grid boyutu
    cols = fb_w / FONT_WIDTH;
    rows = fb_h / FONT_HEIGHT;
    if (cols > MAX_COLS) cols = MAX_COLS;
    if (rows > MAX_ROWS) rows = MAX_ROWS;

    cur_color = 0x07;  // açık gri fg, siyah bg
    scroll_back_count  = 0;
    scroll_view_offset = 0;

    clear_screen64();
}

void clear_screen64(void) {
    // Tüm hücreleri temizle
    for (size_t r = 0; r < rows; r++) {
        for (size_t c = 0; c < cols; c++) {
            screen_cells[r][c].ch    = ' ';
            screen_cells[r][c].color = cur_color;
            screen_cells[r][c].dirty = 1;
        }
    }
    cur_row = 0;
    cur_col = 0;
    pending_wrap = 0;
    scroll_view_offset = 0;

    // Framebuffer'ı arkaplan rengiyle doldur
    uint32_t bg = vga_palette[(cur_color >> 4) & 0x07];
    uint32_t total_pixels = fb_p * fb_h;
    for (uint32_t i = 0; i < total_pixels; i += 4)
        *(uint32_t*)(fb + i) = bg;
}

void set_color64(uint8_t fg, uint8_t bg) {
    cur_color = (uint8_t)((bg & 0x07) << 4) | (fg & 0x0F);
}

void set_position64(size_t row, size_t col) {
    if (row >= rows) row = rows - 1;
    if (col >= cols) col = cols - 1;
    erase_cursor();
    pending_wrap = 0;
    cur_row = row;
    cur_col = col;
    update_cursor64();
}

void get_position64(size_t* row, size_t* col) {
    if (row) *row = cur_row;
    if (col) *col = cur_col;
}

void get_screen_size64(size_t* width, size_t* height) {
    if (width)  *width  = cols;
    if (height) *height = rows;
}

// Sadece hücre dizisini günceller, framebuffer'a yazmaz
static void putchar_cell(char c, uint8_t color) {
    if (c == '\n') { pending_wrap = 0; do_newline(); return; }
    if (c == '\r') { pending_wrap = 0; cur_col = 0; return; }
    if (c == '\t') {
        pending_wrap = 0;
        size_t next_tab = (cur_col + 4) & ~(size_t)3;
        if (next_tab >= cols) next_tab = cols - 1;
        while (cur_col < next_tab) {
            screen_cells[cur_row][cur_col].ch    = ' ';
            screen_cells[cur_row][cur_col].color = color;
            screen_cells[cur_row][cur_col].dirty = 1;
            cur_col++;
        }
        return;
    }
    if (c == '\b') {
        pending_wrap = 0;
        if (cur_col > 0) {
            cur_col--;
            screen_cells[cur_row][cur_col].ch    = ' ';
            screen_cells[cur_row][cur_col].color = color;
            screen_cells[cur_row][cur_col].dirty = 1;
        }
        return;
    }
    // Yazdırılabilir karakter: önce bekleyen wrap'i uygula
    if (pending_wrap) {
        pending_wrap = 0;
        do_newline();
    }
    screen_cells[cur_row][cur_col].ch    = c;
    screen_cells[cur_row][cur_col].color = color;
    screen_cells[cur_row][cur_col].dirty = 1;
    cur_col++;
    // Satır sonuna ulaşıldı: hemen newline yapmak yerine flag set et
    if (cur_col >= cols) {
        cur_col = cols - 1;
        pending_wrap = 1;
    }
}

// ============================================================================
// ANSI / VT100 Escape Sequence Parser
// Kilo ve diğer uygulamaların \x1b[...] kodlarını doğru işler.
//
// Desteklenen sequence'ler:
//   \x1b[H       → cursor sol-üst (1,1)
//   \x1b[r;cH    → cursor satır r, sütun c (1-indexed)
//   \x1b[r;cf    → aynı (H ile özdeş)
//   \x1b[nA/B/C/D → cursor yukarı/aşağı/sağ/sol n adım
//   \x1b[2J      → ekranı temizle
//   \x1b[K       → satır sonuna kadar sil (EL)
//   \x1b[nK      → 0=satır sonu, 1=satır başı, 2=tüm satır sil
//   \x1b[?25l    → cursor gizle
//   \x1b[?25h    → cursor göster
//   \x1b[nm      → SGR renk (30-37 fg, 40-47 bg, 0=reset, 1=bold, 7=ters)
//   \x1b[s       → cursor konumunu kaydet
//   \x1b[u       → kayıtlı cursor konumunu geri yükle
//   \x1b[6n      → cursor konumu raporu (CPR) — boş yanıt (stdin yok)
//   \x1b[nS/T    → ekranı n satır yukarı/aşağı kaydır
// ============================================================================

// Parser durum makinesi
typedef enum {
    ESC_NORMAL,      // Normal metin
    ESC_ESC,         // \x1b alındı
    ESC_CSI,         // \x1b[ alındı
    ESC_CSI_PRIV,    // \x1b[? alındı (private mod)
} EscState;

#define ESC_MAX_PARAMS 8
#define ESC_BUF_SIZE   32

static EscState  esc_state   = ESC_NORMAL;
static int       esc_params[ESC_MAX_PARAMS];
static int       esc_nparams = 0;
/* esc_buf/esc_buflen kaldırıldı - kullanılmıyor */

// Kaydedilmiş cursor pozisyonu (ESC[s / ESC[u)
static size_t saved_row = 0;
static size_t saved_col = 0;

// SGR renk durumu (kilo ANSI renkleri için)
// VGA nibble: bit[3:0]=fg, bit[6:4]=bg
static uint8_t ansi_fg = 7;   // açık gri
static uint8_t ansi_bg = 0;   // siyah

// ANSI renk kodu → VGA palette indeksi (30-37, 40-47)
static uint8_t ansi_to_vga[8] = {
    0,  // 0 = siyah
    4,  // 1 = kırmızı
    2,  // 2 = yeşil
    6,  // 3 = sarı → kahverengi
    1,  // 4 = mavi
    5,  // 5 = magenta
    3,  // 6 = cyan
    7,  // 7 = beyaz (açık gri)
};

// SGR parametrelerini işle
static void ansi_apply_sgr(void) {
    if (esc_nparams == 0) {
        // ESC[m = sıfırla
        ansi_fg = 7; ansi_bg = 0;
        cur_color = (uint8_t)((ansi_bg << 4) | ansi_fg);
        return;
    }
    for (int i = 0; i < esc_nparams; i++) {
        int p = esc_params[i];
        if (p == 0) {
            ansi_fg = 7; ansi_bg = 0;
        } else if (p == 1) {
            // bold → fg'yi parlak yap (bit3 set)
            ansi_fg |= 8;
        } else if (p == 7) {
            // reverse video
            uint8_t tmp = ansi_fg & 7;
            ansi_fg = (uint8_t)(ansi_bg | (ansi_fg & 8));
            ansi_bg = tmp;
        } else if (p == 22) {
            ansi_fg &= 7;  // bold kapat
        } else if (p == 27) {
            // reverse kapat → sadece sıfıra döneriz (basit)
            ansi_fg = 7; ansi_bg = 0;
        } else if (p == 39) {
            ansi_fg = 7;   // varsayılan fg
        } else if (p == 49) {
            ansi_bg = 0;   // varsayılan bg
        } else if (p >= 30 && p <= 37) {
            ansi_fg = (uint8_t)((ansi_fg & 8) | ansi_to_vga[p - 30]);
        } else if (p >= 40 && p <= 47) {
            ansi_bg = ansi_to_vga[p - 40];
        } else if (p >= 90 && p <= 97) {
            // bright fg (aixterm)
            ansi_fg = (uint8_t)(8 | ansi_to_vga[p - 90]);
        } else if (p >= 100 && p <= 107) {
            // bright bg (aixterm)
            ansi_bg = ansi_to_vga[p - 100];
        }
        // diğer parametreler (underline vb.) yok sayılır
    }
    cur_color = (uint8_t)((ansi_bg << 4) | (ansi_fg & 0x0F));
}

// Satırı belirli bir bölge için temizle (EL yardımcısı)
static void erase_line_range(size_t from_col, size_t to_col) {
    if (from_col > to_col) return;
    if (to_col >= cols) to_col = cols - 1;
    for (size_t c = from_col; c <= to_col; c++) {
        screen_cells[cur_row][c].ch    = ' ';
        screen_cells[cur_row][c].color = cur_color;
        screen_cells[cur_row][c].dirty = 1;
    }
}

// CSI sequence tamamlandı — komutu işle
static void ansi_dispatch(char cmd) {
    int p0 = (esc_nparams > 0) ? esc_params[0] : 0;
    int p1 = (esc_nparams > 1) ? esc_params[1] : 0;

    switch (cmd) {
    // ── Cursor hareketi ─────────────────────────────────────────────────
    case 'H':   // CUP: \x1b[r;cH  (1-indexed, default 1;1)
    case 'f': { // HVP: özdeş
        int row = (p0 > 0 ? p0 : 1) - 1;
        int col = (p1 > 0 ? p1 : 1) - 1;
        erase_cursor();
        pending_wrap = 0;
        cur_row = (size_t)(row < 0 ? 0 : (row >= (int)rows ? (int)rows-1 : row));
        cur_col = (size_t)(col < 0 ? 0 : (col >= (int)cols ? (int)cols-1 : col));
        break;
    }
    case 'A': { // CUU: cursor up
        int n = p0 > 0 ? p0 : 1;
        erase_cursor();
        pending_wrap = 0;
        cur_row = (size_t)(cur_row >= (size_t)n ? cur_row - n : 0);
        break;
    }
    case 'B': { // CUD: cursor down
        int n = p0 > 0 ? p0 : 1;
        erase_cursor();
        pending_wrap = 0;
        cur_row += n;
        if (cur_row >= rows) cur_row = rows - 1;
        break;
    }
    case 'C': { // CUF: cursor right
        int n = p0 > 0 ? p0 : 1;
        erase_cursor();
        pending_wrap = 0;
        cur_col += n;
        if (cur_col >= cols) cur_col = cols - 1;
        break;
    }
    case 'D': { // CUB: cursor left
        int n = p0 > 0 ? p0 : 1;
        erase_cursor();
        pending_wrap = 0;
        cur_col = (size_t)(cur_col >= (size_t)n ? cur_col - n : 0);
        break;
    }
    case 's': // cursor kaydet
        saved_row = cur_row;
        saved_col = cur_col;
        break;
    case 'u': // cursor geri yükle
        erase_cursor();
        pending_wrap = 0;
        cur_row = saved_row;
        cur_col = saved_col;
        break;
    case 'n': // CPR: cursor position report — userland read() bekleyebilir
        // VESA'da stdin yok, sessizce yoksay
        break;

    // ── Ekran temizleme ─────────────────────────────────────────────────
    case 'J':
        if (p0 == 2 || p0 == 3) {
            // ED2/ED3: tüm ekranı temizle + framebuffer'ı siyahla doldur
            clear_screen64();
            // SGR renk sıfırla (kilo temizleme öncesi renk değiştirebilir)
        } else if (p0 == 0) {
            // ED0: cursor'dan ekran sonuna kadar temizle
            erase_line_range(cur_col, cols - 1);
            for (size_t r = cur_row + 1; r < rows; r++) {
                for (size_t c = 0; c < cols; c++) {
                    screen_cells[r][c].ch    = ' ';
                    screen_cells[r][c].color = cur_color;
                    screen_cells[r][c].dirty = 1;
                }
            }
        } else if (p0 == 1) {
            // ED1: ekran başından cursor'a kadar temizle
            for (size_t r = 0; r < cur_row; r++) {
                for (size_t c = 0; c < cols; c++) {
                    screen_cells[r][c].ch    = ' ';
                    screen_cells[r][c].color = cur_color;
                    screen_cells[r][c].dirty = 1;
                }
            }
            erase_line_range(0, cur_col);
        }
        break;

    case 'K': // EL: satır silme
        if (p0 == 0) {
            // cursor'dan satır sonuna kadar sil
            erase_line_range(cur_col, cols - 1);
        } else if (p0 == 1) {
            // satır başından cursor'a kadar sil
            erase_line_range(0, cur_col);
        } else if (p0 == 2) {
            // tüm satırı sil
            erase_line_range(0, cols - 1);
        }
        break;

    // ── Scroll ──────────────────────────────────────────────────────────
    case 'S': { // scroll up
        int n = p0 > 0 ? p0 : 1;
        for (int i = 0; i < n; i++) scroll_screen_up();
        // dirty zaten scroll_screen_up içinde set edildi
        break;
    }
    case 'T': { // scroll down (terminal pek kullanmaz ama eksiksiz olsun)
        // Basit: yoksay
        break;
    }

    // ── SGR renk ────────────────────────────────────────────────────────
    case 'm':
        ansi_apply_sgr();
        break;

    // ── Private mod (ESC[?) ─────────────────────────────────────────────
    case 'l': // private DEC: \x1b[?25l = cursor gizle
        if (esc_state == ESC_CSI_PRIV && p0 == 25)
            cursor_visible = 0;
        break;
    case 'h': // private DEC: \x1b[?25h = cursor göster
        if (esc_state == ESC_CSI_PRIV && p0 == 25)
            cursor_visible = 1;
        break;

    // ── Bilinmeyen — sessizce yoksay ────────────────────────────────────
    default:
        break;
    }
}

// Parser'ı sıfırla
static void ansi_reset(void) {
    esc_state   = ESC_NORMAL;
    esc_nparams = 0;
    for (int i = 0; i < ESC_MAX_PARAMS; i++) esc_params[i] = 0;
}

// Tek ham byte'ı parser'a besle.
// Normal metin ise doğrudan putchar_cell'e gönderir.
// Escape sequence bitmişse ansi_dispatch çağırır.
static void ansi_feed(char c) {
    switch (esc_state) {

    case ESC_NORMAL:
        if (c == '\x1b') {
            esc_state = ESC_ESC;
        } else {
            putchar_cell(c, cur_color);
        }
        break;

    case ESC_ESC:
        if (c == '[') {
            esc_state   = ESC_CSI;
            esc_nparams = 0;
                    for (int i = 0; i < ESC_MAX_PARAMS; i++) esc_params[i] = 0;
        } else if (c == 'c') {
            // RIS: full reset
            clear_screen64();
            ansi_fg = 7; ansi_bg = 0;
            cur_color = 0x07;
            ansi_reset();
        } else {
            // Tanınmayan: ESC + char → yoksay, normal moda dön
            ansi_reset();
        }
        break;

    case ESC_CSI:
        if (c == '?') {
            // Private mod: ESC[?25l gibi
            esc_state = ESC_CSI_PRIV;
        } else if (c >= '0' && c <= '9') {
            // Sayısal parametre
            if (esc_nparams == 0) esc_nparams = 1;
            esc_params[esc_nparams - 1] =
                esc_params[esc_nparams - 1] * 10 + (c - '0');
        } else if (c == ';') {
            // Parametre ayırıcı
            if (esc_nparams < ESC_MAX_PARAMS) esc_nparams++;
        } else if (c >= 0x40 && c <= 0x7E) {
            // Final byte — sequence tamamlandı
            // parametre yok → p0=0 (default)
            ansi_dispatch(c);
            ansi_reset();
        } else {
            // Geçersiz → iptal
            ansi_reset();
        }
        break;

    case ESC_CSI_PRIV:
        if (c >= '0' && c <= '9') {
            if (esc_nparams == 0) esc_nparams = 1;
            esc_params[esc_nparams - 1] =
                esc_params[esc_nparams - 1] * 10 + (c - '0');
        } else if (c == ';') {
            if (esc_nparams < ESC_MAX_PARAMS) esc_nparams++;
        } else if (c >= 0x40 && c <= 0x7E) {
            ansi_dispatch(c);
            ansi_reset();
        } else {
            ansi_reset();
        }
        break;
    }
}

// ============================================================================
// Batch write desteği
// Userland write() syscall'ı tek seferde büyük buffer gönderir.
// Her byte için ayrı redraw yapmak hem yavaş hem artefakt bırakır.
// vesa_write_buf() bütün buffer'ı önce cell dizisine işler,
// sonra tek seferde framebuffer'a yazar.
// ============================================================================

// Kernel'ın sys_write implementasyonu bu fonksiyonu çağırmalı
// (putchar64 yerine) — eğer erişim yoksa print_str64 de aynı işi yapar.
void vesa_write_buf(const char* buf, int len) {
    if (!buf || len <= 0) return;
    erase_cursor();
    scroll_view_offset = 0;
    for (int i = 0; i < len; i++)
        ansi_feed(buf[i]);
    // Tüm ekranı yeniden çiz — dirty optimizasyonu yerine tam redraw.
    // Bu kilo gibi uygulamalarda ANSI sequence sonrası tutarlı görüntü sağlar.
    for (size_t r = 0; r < rows; r++) {
        for (size_t c = 0; c < cols; c++) {
            redraw_cell(r, c);
            screen_cells[r][c].dirty = 0;
        }
    }
    update_cursor64();
}

// ============================================================================
// Tek karakter — ANSI parser üzerinden yaz
// ============================================================================
void putchar64(char c, uint8_t color) {
    uint8_t saved = cur_color;
    if (color != cur_color) cur_color = color;

    erase_cursor();

    EscState prev_state = esc_state;
    ansi_feed(c);

    if (esc_state == ESC_NORMAL) {
        if (prev_state != ESC_NORMAL) {
            // Escape sequence tamamlandı: tüm ekranı yeniden çiz
            for (size_t r = 0; r < rows; r++)
                for (size_t cc = 0; cc < cols; cc++) {
                    redraw_cell(r, cc);
                    screen_cells[r][cc].dirty = 0;
                }
        } else if (c == '\n' || c == '\r' || c == '\t') {
            // Sadece mevcut satırı çiz (do_newline() cur_row'u zaten güncelledi)
            for (size_t col = 0; col < cols; col++)
                redraw_cell(cur_row, col);
        } else if (c == '\b') {
            // Backspace: geri gidilen sütunu (cur_col) ve mevcut sütunu çiz
            redraw_cell(cur_row, cur_col);
            if (cur_col + 1 < cols)
                redraw_cell(cur_row, cur_col + 1);
        } else if (c >= 0x20) {
            size_t drawn_col = cur_col > 0 ? cur_col - 1 : 0;
            redraw_cell(cur_row, drawn_col);
        }
    }

    update_cursor64();
    cur_color = saved;
}

void print_str64(const char* str, uint8_t color) {
    erase_cursor();
    scroll_view_offset = 0;

    uint8_t saved = cur_color;
    if (color != cur_color) cur_color = color;

    while (*str)
        ansi_feed(*str++);

    // Tüm ekranı güncelle (scroll + escape sonrası güvenli)
    for (size_t r = 0; r < rows; r++)
        for (size_t c = 0; c < cols; c++)
            redraw_cell(r, c);
    update_cursor64();
    cur_color = saved;
}

void println64(const char* str, uint8_t color) {
    erase_cursor();
    scroll_view_offset = 0;

    uint8_t saved = cur_color;
    if (color != cur_color) cur_color = color;

    while (*str)
        ansi_feed(*str++);
    ansi_feed('\n');

    for (size_t r = 0; r < rows; r++)
        for (size_t c = 0; c < cols; c++)
            redraw_cell(r, c);
    update_cursor64();
    cur_color = saved;
}

// ============================================================================
// Scroll API
// ============================================================================

void scroll_up(size_t lines) {
    if (scroll_view_offset + lines > scroll_back_count)
        lines = scroll_back_count - scroll_view_offset;
    if (lines == 0) return;

    scroll_view_offset += lines;

    // scroll_back'ten ekrana göster
    // Önce aktif ekranı geçici olarak "aşağı" ötele, üste history göster
    // Basit yaklaşım: tüm ekranı yeniden çiz
    // -- Görüntülenen satırlar scroll_back'in sonundan scroll_view_offset önünde --
    size_t view_start_in_back = scroll_back_count - scroll_view_offset;

    // Ekranı tamamen karıştırma: sadece framebuffer'ı güncelle
    for (size_t r = 0; r < rows; r++) {
        size_t src_row_in_back = view_start_in_back + r;
        for (size_t c = 0; c < cols; c++) {
            Cell cell;
            if (src_row_in_back < scroll_back_count) {
                cell = scroll_back[src_row_in_back][c];
            } else {
                // scroll_back bitti, aktif ekrandan al
                size_t active_r = src_row_in_back - scroll_back_count;
                cell = (active_r < rows) ? screen_cells[active_r][c]
                                         : (Cell){' ', cur_color, 1};
            }
            draw_char_at_pixel(char_to_px(c), char_to_py(r), cell.ch, cell.color);
        }
    }
}

void scroll_down(size_t lines) {
    if (lines > scroll_view_offset) lines = scroll_view_offset;
    if (lines == 0) return;

    scroll_view_offset -= lines;

    if (scroll_view_offset == 0) {
        // Normal görünüme dön
        redraw_all();
    } else {
        // Aynı mantıkla yeniden çiz
        size_t view_start_in_back = scroll_back_count - scroll_view_offset;
        for (size_t r = 0; r < rows; r++) {
            size_t src_row_in_back = view_start_in_back + r;
            for (size_t c = 0; c < cols; c++) {
                Cell cell;
                if (src_row_in_back < scroll_back_count) {
                    cell = scroll_back[src_row_in_back][c];
                } else {
                    size_t active_r = src_row_in_back - scroll_back_count;
                    cell = (active_r < rows) ? screen_cells[active_r][c]
                                             : (Cell){' ', cur_color, 1};
                }
                draw_char_at_pixel(char_to_px(c), char_to_py(r), cell.ch, cell.color);
            }
        }
    }
}

void get_scroll_info64(size_t* buffer_lines, size_t* offset) {
    if (buffer_lines) *buffer_lines = scroll_back_count;
    if (offset)       *offset       = scroll_view_offset;
}

// ============================================================================
// VGA uyumluluk stub'ları
// (Eski kod bu fonksiyonları çağırıyorsa hata vermemesi için)
// ============================================================================
void reset_to_standard_mode(void) { /* VESA'da gerek yok */ }
void set_extended_text_mode(void)  { /* VESA'da gerek yok */ }

// Renk indeks dönüştürücü (dışarıdan çağrılabilir)
uint32_t vesa_color_from_vga(uint8_t idx) {
    return vga_palette[idx & 0x0F];
}