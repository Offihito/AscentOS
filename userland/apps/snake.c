// ═══════════════════════════════════════════════════════════════════════════
//  AscentOS — snake.c  (Yılan Oyunu v1.1)
//
//  v1.0 → v1.1 düzeltmeleri:
//    - nb_getchar(): her frame'de fcntl çağırmak yerine başlangıçta
//      stdin'i kalıcı O_NONBLOCK yap. ESC dizi okuma artık güvenli.
//    - game_delay(): yield sayma yerine SYS_GETTICKS ile gerçek
//      tick-tabanlı bekleme. Scheduler hızından bağımsız, sabit frame süresi.
//    - draw_board(): tek büyük write buffer'ı — her karakter için ayrı
//      write() yok, performans çok daha iyi.
//    - VMIN=0 VTIME=0: raw mode non-blocking read için doğru ayar.
//
//  Syscall bağımlılıkları: read, write, fcntl, ioctl, exit, yield, getticks
// ═══════════════════════════════════════════════════════════════════════════

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

// ── Inline syscall'lar ────────────────────────────────────────────────────

static inline void yield_cpu(void) {
    __asm__ volatile (
        "movq $24, %%rax\n\t"
        "syscall\n\t"
        ::: "rax", "rcx", "r11", "memory"
    );
}

// SYS_GETTICKS = 404
static inline long get_ticks(void) {
    long ret;
    __asm__ volatile (
        "movq $404, %%rax\n\t"
        "syscall\n\t"
        : "=a"(ret)
        :: "rcx", "r11", "memory"
    );
    return ret;
}

// SYS_FCNTL = 72
static inline long _fcntl(int fd, int cmd, long arg) {
    long ret;
    __asm__ volatile (
        "movq $72, %%rax\n\t"
        "syscall\n\t"
        : "=a"(ret)
        : "D"((long)fd), "S"((long)cmd), "d"(arg)
        : "rcx", "r11", "memory"
    );
    return ret;
}

// SYS_IOCTL = 16
static inline long _ioctl(int fd, unsigned long req, void* arg) {
    long ret;
    __asm__ volatile (
        "movq $16, %%rax\n\t"
        "syscall\n\t"
        : "=a"(ret)
        : "D"((long)fd), "S"((long)req), "d"((long)arg)
        : "rcx", "r11", "memory"
    );
    return ret;
}

// ── termios ───────────────────────────────────────────────────────────────
#define TCGETS  0x5401
#define TCSETS  0x5402
#define ECHO    0x00000008
#define ICANON  0x00000002

struct termios {
    unsigned int  c_iflag;
    unsigned int  c_oflag;
    unsigned int  c_cflag;
    unsigned int  c_lflag;
    unsigned char c_line;
    unsigned char c_cc[19];
    unsigned int  c_ispeed;
    unsigned int  c_ospeed;
};

static struct termios orig_termios;
static int raw_active = 0;

static void enable_raw_mode(void) {
    _ioctl(STDIN_FILENO, TCGETS, &orig_termios);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    raw.c_cc[6] = 0;   // VMIN  = 0
    raw.c_cc[5] = 0;   // VTIME = 0
    _ioctl(STDIN_FILENO, TCSETS, &raw);
    // Kalici O_NONBLOCK
    long fl = _fcntl(STDIN_FILENO, 3 /*F_GETFL*/, 0);
    _fcntl(STDIN_FILENO, 4 /*F_SETFL*/, fl | 0x800 /*O_NONBLOCK*/);
    raw_active = 1;
}

static void disable_raw_mode(void) {
    if (!raw_active) return;
    long fl = _fcntl(STDIN_FILENO, 3, 0);
    _fcntl(STDIN_FILENO, 4, fl & ~0x800L);
    _ioctl(STDIN_FILENO, TCSETS, &orig_termios);
    raw_active = 0;
}

// ── ANSI ─────────────────────────────────────────────────────────────────
#define CLR_RESET   "\033[0m"
#define CLR_BOLD    "\033[1m"
#define CLR_GREEN   "\033[32m"
#define CLR_BGREEN  "\033[92m"
#define CLR_RED     "\033[31m"
#define CLR_YELLOW  "\033[33m"
#define CLR_CYAN    "\033[36m"
#define CLR_WHITE   "\033[37m"

static void clrscr(void)      { write(1, "\033[2J\033[H", 7); }
static void cursor_hide(void) { write(1, "\033[?25l", 6); }
static void cursor_show(void) { write(1, "\033[?25h", 6); }

// ── Cizim buffer — tum frame tek write() ─────────────────────────────────
#define DBUF_SIZE (16 * 1024)
static char dbuf[DBUF_SIZE];
static int  dbuf_pos;

static void db_reset(void) { dbuf_pos = 0; }
static void db_flush(void) { write(1, dbuf, dbuf_pos); dbuf_pos = 0; }

static void db_puts(const char* s) {
    int l = strlen(s);
    if (dbuf_pos + l < DBUF_SIZE) { memcpy(dbuf + dbuf_pos, s, l); dbuf_pos += l; }
}
static void db_putc(char c) {
    if (dbuf_pos < DBUF_SIZE - 1) dbuf[dbuf_pos++] = c;
}
static void db_move(int row, int col) {
    char tmp[24];
    int n = snprintf(tmp, sizeof(tmp), "\033[%d;%dH", row, col);
    if (dbuf_pos + n < DBUF_SIZE) { memcpy(dbuf + dbuf_pos, tmp, n); dbuf_pos += n; }
}

// ── Oyun sabitleri ────────────────────────────────────────────────────────
#define BOARD_W      40
#define BOARD_H      20
#define MAX_SNAKE    (BOARD_W * BOARD_H)
#define BOARD_ROW     2
#define BOARD_COL     2
#define FRAME_MS_BASE 200
#define FRAME_MS_MIN   60

#define DIR_UP    0
#define DIR_DOWN  1
#define DIR_LEFT  2
#define DIR_RIGHT 3

typedef struct { int x; int y; } Point;

static Point snake[MAX_SNAKE];
static int   snake_len;
static int   dir;
static Point food;
static int   score;
static int   game_over;

// ── RNG ──────────────────────────────────────────────────────────────────
static unsigned long rng = 98765;
static int rng_range(int lo, int hi) {
    rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
    return lo + (int)((unsigned int)(rng >> 33) % (unsigned int)(hi - lo + 1));
}

static void place_food(void) {
    int ok;
    do {
        ok = 1;
        food.x = rng_range(0, BOARD_W - 1);
        food.y = rng_range(0, BOARD_H - 1);
        for (int i = 0; i < snake_len; i++)
            if (snake[i].x == food.x && snake[i].y == food.y) { ok = 0; break; }
    } while (!ok);
}

static void game_init(void) {
    snake_len = 3; dir = DIR_RIGHT; score = 0; game_over = 0;
    int sx = BOARD_W / 2, sy = BOARD_H / 2;
    for (int i = 0; i < snake_len; i++) { snake[i].x = sx - i; snake[i].y = sy; }
    place_food();
}

// ── Cizim ────────────────────────────────────────────────────────────────
static void draw_board(void) {
    db_reset();
    // Baslik
    db_move(1, BOARD_COL);
    db_puts(CLR_BOLD CLR_CYAN "  AscentOS Snake v1.1" CLR_RESET "   " CLR_YELLOW);
    char tmp[32]; snprintf(tmp, sizeof(tmp), "Skor: %d   ", score);
    db_puts(tmp); db_puts(CLR_RESET);
    // Ust duvar
    db_move(BOARD_ROW, BOARD_COL);
    db_puts(CLR_WHITE "+");
    for (int x = 0; x < BOARD_W; x++) db_putc('-');
    db_puts("+" CLR_RESET);
    // Satirlar
    for (int y = 0; y < BOARD_H; y++) {
        db_move(BOARD_ROW + 1 + y, BOARD_COL);
        db_puts(CLR_WHITE "|" CLR_RESET);
        for (int x = 0; x < BOARD_W; x++) {
            if (x == snake[0].x && y == snake[0].y) {
                db_puts(CLR_BGREEN "O" CLR_RESET);
            } else {
                int body = 0;
                for (int i = 1; i < snake_len; i++)
                    if (snake[i].x == x && snake[i].y == y) { body = 1; break; }
                if (body)               db_puts(CLR_GREEN "o" CLR_RESET);
                else if (x == food.x && y == food.y) db_puts(CLR_RED "*" CLR_RESET);
                else                    db_putc(' ');
            }
        }
        db_puts(CLR_WHITE "|" CLR_RESET);
    }
    // Alt duvar
    db_move(BOARD_ROW + BOARD_H + 1, BOARD_COL);
    db_puts(CLR_WHITE "+");
    for (int x = 0; x < BOARD_W; x++) db_putc('-');
    db_puts("+" CLR_RESET);
    // Yardim
    db_move(BOARD_ROW + BOARD_H + 2, BOARD_COL);
    db_puts(CLR_CYAN "  WASD / ok tuslari: yon  |  q: cikis  " CLR_RESET);
    db_flush();
}

static void draw_gameover(void) {
    int mr = BOARD_ROW + BOARD_H / 2, mc = BOARD_COL + BOARD_W / 2 - 12;
    db_reset();
    db_move(mr - 1, mc); db_puts(CLR_BOLD CLR_RED "  *** OYUN BITTI ***  " CLR_RESET "          ");
    db_move(mr,     mc);
    char tmp[48]; snprintf(tmp, sizeof(tmp), CLR_YELLOW "  Skor: %-5d" CLR_RESET "              ", score);
    db_puts(tmp);
    db_move(mr + 1, mc); db_puts(CLR_CYAN "  [r] Tekrar  [q] Cikis  " CLR_RESET "       ");
    db_flush();
}

// ── Input ─────────────────────────────────────────────────────────────────
static int nb_getchar(void) {
    unsigned char c = 0;
    return (read(STDIN_FILENO, &c, 1) == 1) ? (int)c : -1;
}

// handle_input: main loop icine tasindi

// ── Oyun adimi ────────────────────────────────────────────────────────────
static void game_step(void) {
    Point h = snake[0];
    switch (dir) {
    case DIR_UP:    h.y--; break;
    case DIR_DOWN:  h.y++; break;
    case DIR_LEFT:  h.x--; break;
    case DIR_RIGHT: h.x++; break;
    }
    if (h.x < 0 || h.x >= BOARD_W || h.y < 0 || h.y >= BOARD_H) { game_over = 1; return; }
    for (int i = 0; i < snake_len; i++)
        if (snake[i].x == h.x && snake[i].y == h.y) { game_over = 1; return; }
    int ate = (h.x == food.x && h.y == food.y);
    if (ate && snake_len < MAX_SNAKE) snake_len++;
    int lim = snake_len;
    for (int i = lim - 1; i > 0; i--) snake[i] = snake[i-1];
    snake[0] = h;
    if (ate) { score += 10; place_food(); }
}

// ── main ──────────────────────────────────────────────────────────────────
int main(void) {
    enable_raw_mode();
    cursor_hide();
    clrscr();
    game_init();

    // getticks sagligi kontrolu
    long t0 = get_ticks();
    int ticks_ok = (t0 > 0);

    long next_step = t0;   // bir sonraki game_step zamani

    while (1) {
        // ── Frame suresi hesapla ──────────────────────────────────────────
        int fms = FRAME_MS_BASE - (score / 50) * 10;
        if (fms < FRAME_MS_MIN) fms = FRAME_MS_MIN;

        // ── Frame suresi boyunca input oku, adim zamanini bekle ───────────
        if (ticks_ok) {
            while (get_ticks() < next_step) {
                // Input: yön degistir veya q/r yakala
                int c = nb_getchar();
                if (c == 'q' || c == 'Q') goto quit;
                if (c == 'r' || c == 'R') { game_init(); clrscr(); next_step = get_ticks(); break; }
                if (c == '\033') {
                    int c2 = nb_getchar();
                    if (c2 == '[') {
                        int c3 = nb_getchar();
                        if      (c3 == 'A') c = 'w';
                        else if (c3 == 'B') c = 's';
                        else if (c3 == 'C') c = 'd';
                        else if (c3 == 'D') c = 'a';
                    }
                }
                switch (c) {
                case 'w': case 'W': if (dir != DIR_DOWN)  dir = DIR_UP;    break;
                case 's': case 'S': if (dir != DIR_UP)    dir = DIR_DOWN;  break;
                case 'a': case 'A': if (dir != DIR_RIGHT) dir = DIR_LEFT;  break;
                case 'd': case 'D': if (dir != DIR_LEFT)  dir = DIR_RIGHT; break;
                }
                yield_cpu();
            }
            next_step = get_ticks() + (long)fms;
        } else {
            // getticks yok — yield fallback ile bekle, input oku
            int iters = fms * 8;
            for (int i = 0; i < iters; i++) {
                int c = nb_getchar();
                if (c == 'q' || c == 'Q') goto quit;
                if (c == 'r' || c == 'R') { game_init(); clrscr(); break; }
                if (c == '\033') {
                    int c2 = nb_getchar();
                    if (c2 == '[') {
                        int c3 = nb_getchar();
                        if      (c3 == 'A') c = 'w';
                        else if (c3 == 'B') c = 's';
                        else if (c3 == 'C') c = 'd';
                        else if (c3 == 'D') c = 'a';
                    }
                }
                switch (c) {
                case 'w': case 'W': if (dir != DIR_DOWN)  dir = DIR_UP;    break;
                case 's': case 'S': if (dir != DIR_UP)    dir = DIR_DOWN;  break;
                case 'a': case 'A': if (dir != DIR_RIGHT) dir = DIR_LEFT;  break;
                case 'd': case 'D': if (dir != DIR_LEFT)  dir = DIR_RIGHT; break;
                }
                yield_cpu();
            }
        }

        // ── Adim at ve ciz ────────────────────────────────────────────────
        if (game_over) {
            draw_board();
            draw_gameover();
            while (1) {
                int c = nb_getchar();
                if (c == 'q' || c == 'Q') goto quit;
                if (c == 'r' || c == 'R') {
                    game_init(); clrscr();
                    next_step = get_ticks();
                    break;
                }
                yield_cpu();
            }
            continue;
        }

        game_step();
        draw_board();

        if (game_over) {
            draw_gameover();
            while (1) {
                int c = nb_getchar();
                if (c == 'q' || c == 'Q') goto quit;
                if (c == 'r' || c == 'R') {
                    game_init(); clrscr();
                    next_step = get_ticks();
                    break;
                }
                yield_cpu();
            }
        }
    }

quit:
    cursor_show();
    disable_raw_mode();
    clrscr();
    printf(CLR_CYAN "AscentOS Snake - Final Skor: %d\n" CLR_RESET, score);
    printf("Iyi gunler!\n");
    fflush(stdout);
    exit(0);
    return 0;
}