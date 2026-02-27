/* ═══════════════════════════════════════════════════════════════════════════
 *  kilo — AscentOS Port  (kaynak: github.com/antirez/kilo, BSD-2-Clause)
 *  Port: AscentOS newlib 4.4 uyumlu
 *
 *  Orijinal: Salvatore Sanfilippo (antirez)
 *  Port notları:
 *    - termios: AscentOS syscalls.c'deki TCGETS/TCSETS ioctl ile çalışır
 *    - TIOCGWINSZ: AscentOS kernel'ında destekleniyor
 *    - SIGWINCH (28): sinyal tablosunda mevcut
 *    - read/write: AscentOS SYS_READ/SYS_WRITE üzerinden
 *    - Kaldırılan: POSIX özellikli platform makrolar
 *    - Eklenen: AscentOS uyumlu raw mode kurulumu
 *    - Derleme: newlib 4.4 + syscalls.c + crt0.asm
 * ═══════════════════════════════════════════════════════════════════════════ */

/* newlib ile derleme için gerekli tanımlar */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <ctype.h>

/* ── ctype fallback: -ffreestanding + newlib kombinasyonunda ctype.h
 * fonksiyonları bazen görünmez hale gelir. Güvenli inline makrolar.   */
#ifndef _KILO_CTYPE_SAFE
#define _KILO_CTYPE_SAFE
/* Sadece newlib'in sağlamadığı durum için; sağlarsa inline olarak override */
static inline int kilo_isspace(int c)  { return (c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\f'||c=='\v'); }
static inline int kilo_isdigit(int c)  { return (c>='0' && c<='9'); }
static inline int kilo_iscntrl(int c)  { return ((unsigned)c < 32 || c == 127); }
/* Sadece yukarıdaki 3 fonksiyon kilo'da kullanılıyor.
 * Eğer ctype.h bunları sağlıyorsa, aşağıdaki #define'lar onları gizler
 * ve bizimkiler kullanılır — davranış aynı, çakışma yok.             */
#undef  isspace
#define isspace  kilo_isspace
#undef  isdigit
#define isdigit  kilo_isdigit
#undef  iscntrl
#define iscntrl  kilo_iscntrl
#endif

/* ── getline: newlib ffreestanding modunda eksik olabilir ────────────────
 * POSIX.1-2008 tanımı. _POSIX_C_SOURCE 200809L set edilmişse newlib 4.4
 * normalde sağlar; ancak bazı -ffreestanding konfigürasyonlarında stdio.h
 * prototipi gizlenebilir. Güvenli fallback: kendi implementasyonumuz.    */
#ifndef KILO_GETLINE_DEFINED
#define KILO_GETLINE_DEFINED
static ssize_t kilo_getline(char **lineptr, size_t *n, FILE *stream) {
    if (!lineptr || !n || !stream) return -1;

    if (!*lineptr || *n == 0) {
        *n = 128;
        *lineptr = malloc(*n);
        if (!*lineptr) return -1;
    }

    ssize_t len = 0;
    int c;
    while ((c = fgetc(stream)) != EOF) {
        if ((size_t)(len + 2) > *n) {
            *n *= 2;
            char *tmp = realloc(*lineptr, *n);
            if (!tmp) return -1;
            *lineptr = tmp;
        }
        (*lineptr)[len++] = (char)c;
        if (c == '\n') break;
    }

    if (len == 0 && c == EOF) return -1;
    (*lineptr)[len] = '\0';
    return len;
}
/* getline'ı kendi versiyonumuza yönlendir (çakışma önlemi) */
#undef getline
#define getline kilo_getline
#endif

/* ── AscentOS termios tanımları ─────────────────────────────────────────────
 * syscalls.c'deki struct termios ile tam uyumlu.
 * newlib'in kendi termios.h'ı varsa çakışma önlemek için guard kullanıyoruz. */
#ifndef ASCENTOS_TERMIOS_DEFINED
#define ASCENTOS_TERMIOS_DEFINED

#define NCCS 19

/* Kernel'da tanımlı ioctl kodları */
#define TCGETS      0x5401
#define TCSETS      0x5402
#define TCSETSW     0x5403
#define TCSETSF     0x5404
#define TIOCGWINSZ  0x5413
#define TIOCSWINSZ  0x5414

/* c_lflag bitleri */
#ifndef ISIG
#define ISIG    0x0001
#endif
#ifndef ICANON
#define ICANON  0x0002
#endif
#ifndef ECHO
#define ECHO    0x0008
#endif
#ifndef ECHOE
#define ECHOE   0x0010
#endif
#ifndef ECHOK
#define ECHOK   0x0020
#endif
#ifndef ECHONL
#define ECHONL  0x0040
#endif

/* c_iflag bitleri */
#ifndef BRKINT
#define BRKINT  0x0002
#endif
#ifndef ICRNL
#define ICRNL   0x0100
#endif
#ifndef INPCK
#define INPCK   0x0010
#endif
#ifndef ISTRIP
#define ISTRIP  0x0020
#endif
#ifndef IXON
#define IXON    0x0400
#endif

/* c_oflag bitleri */
#ifndef OPOST
#define OPOST   0x0001
#endif

/* c_cc indeksleri */
#ifndef VEOF
#define VEOF   4
#endif
#ifndef VEOL
#define VEOL   5
#endif
#ifndef VERASE
#define VERASE 2
#endif
#ifndef VINTR
#define VINTR  0
#endif
#ifndef VKILL
#define VKILL  3
#endif
#ifndef VMIN
#define VMIN   6
#endif
#ifndef VQUIT
#define VQUIT  1
#endif
#ifndef VSUSP
#define VSUSP  10
#endif
#ifndef VTIME
#define VTIME  7
#endif

/* termios yapısı — syscalls.c ile birebir */
struct termios {
    unsigned int  c_iflag;
    unsigned int  c_oflag;
    unsigned int  c_cflag;
    unsigned int  c_lflag;
    unsigned char c_line;
    unsigned char c_cc[NCCS];
    unsigned int  c_ispeed;
    unsigned int  c_ospeed;
};

/* winsize yapısı */
struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

/* tcgetattr / tcsetattr — ioctl syscall üzerinden */
static inline long _ioctl(int fd, unsigned long req, void *arg) {
    long ret;
    register long _rax __asm__("rax") = 26; /* SYS_IOCTL */
    register long _rdi __asm__("rdi") = fd;
    register long _rsi __asm__("rsi") = (long)req;
    register long _rdx __asm__("rdx") = (long)arg;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "r"(_rax), "r"(_rdi), "r"(_rsi), "r"(_rdx)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static int tcgetattr(int fd, struct termios *t) {
    return (int)_ioctl(fd, TCGETS, t);
}

static int tcsetattr(int fd, int action, const struct termios *t) {
    unsigned long req;
    switch (action) {
    case 0: req = TCSETS;  break; /* TCSANOW  */
    case 1: req = TCSETSW; break; /* TCSADRAIN */
    case 2: req = TCSETSF; break; /* TCSAFLUSH */
    default: req = TCSETS; break;
    }
    return (int)_ioctl(fd, req, (void*)t);
}

#define TCSANOW   0
#define TCSADRAIN 1
#define TCSAFLUSH 2

#endif /* ASCENTOS_TERMIOS_DEFINED */

/* ═══════════════════════════════════════════════════════════════════════════
 *  KILO — Ana Kaynak (antirez/kilo, değiştirilmiş)
 * ═══════════════════════════════════════════════════════════════════════════ */

#define KILO_VERSION "0.0.1-ascentos"
#define KILO_TAB_STOP 8
#define KILO_QUIT_TIMES 3

#define CTRL_KEY(k) ((k) & 0x1f)

/* Editor özel tuş kodları */
enum editorKey {
    BACKSPACE = 127,
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN
};

/* Syntax highlight türleri */
enum editorHighlight {
    HL_NORMAL = 0,
    HL_COMMENT,
    HL_MLCOMMENT,
    HL_KEYWORD1,
    HL_KEYWORD2,
    HL_STRING,
    HL_NUMBER,
    HL_MATCH
};

#define HL_HIGHLIGHT_NUMBERS (1<<0)
#define HL_HIGHLIGHT_STRINGS (1<<1)

/* ── Veri yapıları ──────────────────────────────────────────────────────── */

struct editorSyntax {
    char *filetype;
    char **filematch;
    char **keywords;
    char *singleline_comment_start;
    char *multiline_comment_start;
    char *multiline_comment_end;
    int flags;
};

typedef struct erow {
    int idx;
    int size;
    int rsize;
    char *chars;
    char *render;
    unsigned char *hl;
    int hl_open_comment;
} erow;

struct editorConfig {
    int cx, cy;
    int rx;
    int rowoff;
    int coloff;
    int screenrows;
    int screencols;
    int numrows;
    erow *row;
    int dirty;
    char *filename;
    char statusmsg[80];
    time_t statusmsg_time;
    struct editorSyntax *syntax;
    struct termios orig_termios;
};

struct editorConfig E;

/* ── Syntax highlight — C/C++ ───────────────────────────────────────────── */

char *C_HL_extensions[] = { ".c", ".h", ".cpp", ".cc", ".hpp", NULL };
char *C_HL_keywords[] = {
    "switch","if","while","for","break","continue","return","else",
    "struct","union","typedef","static","enum","class","case",
    "#define","#include","#ifndef","#ifdef","#endif","#pragma",
    "int|","long|","double|","float|","char|","unsigned|","signed|",
    "void|","size_t|","ssize_t|","off_t|","pid_t|",
    NULL
};

struct editorSyntax HLDB[] = {
    {
        "c",
        C_HL_extensions,
        C_HL_keywords,
        "//", "/*", "*/",
        HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS
    },
};

#define HLDB_ENTRIES (int)(sizeof(HLDB) / sizeof(HLDB[0]))

/* ── Prototip bildirimleri ──────────────────────────────────────────────── */
void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshScreen(void);
char *editorPrompt(const char *prompt, void (*callback)(char *, int));

/* ── Terminal ───────────────────────────────────────────────────────────── */

void die(const char *s) {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    perror(s);
    exit(1);
}

void disableRawMode(void) {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
        die("tcsetattr");
}

void enableRawMode(void) {
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1)
        die("tcgetattr");
    atexit(disableRawMode);

    struct termios raw = E.orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= 0x30; /* CS8 */
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | 0x0200 /* IEXTEN */);
    raw.c_cc[VMIN]  = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
        die("tcsetattr");
}

int editorReadKey(void) {
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) die("read");
    }

    if (c == '\x1b') {
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
                if (seq[2] == '~') {
                    switch (seq[1]) {
                    case '1': return HOME_KEY;
                    case '3': return DEL_KEY;
                    case '4': return END_KEY;
                    case '5': return PAGE_UP;
                    case '6': return PAGE_DOWN;
                    case '7': return HOME_KEY;
                    case '8': return END_KEY;
                    }
                }
            } else {
                switch (seq[1]) {
                case 'A': return ARROW_UP;
                case 'B': return ARROW_DOWN;
                case 'C': return ARROW_RIGHT;
                case 'D': return ARROW_LEFT;
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
                }
            }
        } else if (seq[0] == 'O') {
            switch (seq[1]) {
            case 'H': return HOME_KEY;
            case 'F': return END_KEY;
            }
        }
        return '\x1b';
    }
    return c;
}

int getCursorPosition(int *rows, int *cols) {
    char buf[32];
    unsigned int i = 0;

    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;
    while (i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
        if (buf[i] == 'R') break;
        i++;
    }
    buf[i] = '\0';

    if (buf[0] != '\x1b' || buf[1] != '[') return -1;
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;
    return 0;
}

int getWindowSize(int *rows, int *cols) {
    struct winsize ws;
    if (_ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        /* Fallback: imleci sağ-aşağı köşeye taşı, konumu sor */
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
        return getCursorPosition(rows, cols);
    }
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
}

/* ── Syntax Highlight ───────────────────────────────────────────────────── */

int is_separator(int c) {
    return isspace((unsigned char)c) || c == '\0' ||
           strchr(",.()+-/*=~%<>[];", c) != NULL;
}

void editorUpdateSyntax(erow *row) {
    row->hl = realloc(row->hl, row->rsize);
    memset(row->hl, HL_NORMAL, row->rsize);

    if (E.syntax == NULL) return;

    char **keywords = E.syntax->keywords;
    char *scs = E.syntax->singleline_comment_start;
    char *mcs = E.syntax->multiline_comment_start;
    char *mce = E.syntax->multiline_comment_end;

    int scs_len = scs ? (int)strlen(scs) : 0;
    int mcs_len = mcs ? (int)strlen(mcs) : 0;
    int mce_len = mce ? (int)strlen(mce) : 0;

    int prev_sep = 1;
    int in_string = 0;
    int in_comment = (row->idx > 0 && E.row[row->idx - 1].hl_open_comment);

    int i = 0;
    while (i < row->rsize) {
        char c = row->render[i];
        unsigned char prev_hl = (i > 0) ? row->hl[i - 1] : HL_NORMAL;

        if (scs_len && !in_string && !in_comment) {
            if (!strncmp(&row->render[i], scs, scs_len)) {
                memset(&row->hl[i], HL_COMMENT, row->rsize - i);
                break;
            }
        }

        if (mcs_len && mce_len && !in_string) {
            if (in_comment) {
                row->hl[i] = HL_MLCOMMENT;
                if (!strncmp(&row->render[i], mce, mce_len)) {
                    memset(&row->hl[i], HL_MLCOMMENT, mce_len);
                    i += mce_len;
                    in_comment = 0;
                    prev_sep = 1;
                    continue;
                } else {
                    i++;
                    continue;
                }
            } else if (!strncmp(&row->render[i], mcs, mcs_len)) {
                memset(&row->hl[i], HL_MLCOMMENT, mcs_len);
                i += mcs_len;
                in_comment = 1;
                continue;
            }
        }

        if (E.syntax->flags & HL_HIGHLIGHT_STRINGS) {
            if (in_string) {
                row->hl[i] = HL_STRING;
                if (c == '\\' && i + 1 < row->rsize) {
                    row->hl[i + 1] = HL_STRING;
                    i += 2;
                    continue;
                }
                if (c == in_string) in_string = 0;
                i++;
                prev_sep = 1;
                continue;
            } else {
                if (c == '"' || c == '\'') {
                    in_string = c;
                    row->hl[i] = HL_STRING;
                    i++;
                    continue;
                }
            }
        }

        if (E.syntax->flags & HL_HIGHLIGHT_NUMBERS) {
            if ((isdigit((unsigned char)c) && (prev_sep || prev_hl == HL_NUMBER)) ||
                (c == '.' && prev_hl == HL_NUMBER)) {
                row->hl[i] = HL_NUMBER;
                i++;
                prev_sep = 0;
                continue;
            }
        }

        if (prev_sep) {
            int j;
            for (j = 0; keywords[j]; j++) {
                int klen = (int)strlen(keywords[j]);
                int kw2 = keywords[j][klen - 1] == '|';
                if (kw2) klen--;

                if (!strncmp(&row->render[i], keywords[j], klen) &&
                    is_separator(row->render[i + klen])) {
                    memset(&row->hl[i], kw2 ? HL_KEYWORD2 : HL_KEYWORD1, klen);
                    i += klen;
                    break;
                }
            }
            if (keywords[j] != NULL) {
                prev_sep = 0;
                continue;
            }
        }

        prev_sep = is_separator(c);
        i++;
    }

    int changed = (row->hl_open_comment != in_comment);
    row->hl_open_comment = in_comment;
    if (changed && row->idx + 1 < E.numrows)
        editorUpdateSyntax(&E.row[row->idx + 1]);
}

int editorSyntaxToColor(int hl) {
    switch (hl) {
    case HL_COMMENT:
    case HL_MLCOMMENT: return 36;
    case HL_KEYWORD1:  return 33;
    case HL_KEYWORD2:  return 32;
    case HL_STRING:    return 35;
    case HL_NUMBER:    return 31;
    case HL_MATCH:     return 34;
    default:           return 37;
    }
}

void editorSelectSyntaxHighlight(void) {
    E.syntax = NULL;
    if (E.filename == NULL) return;

    char *ext = strrchr(E.filename, '.');

    for (int j = 0; j < HLDB_ENTRIES; j++) {
        struct editorSyntax *s = &HLDB[j];
        unsigned int i = 0;
        while (s->filematch[i]) {
            int is_ext = (s->filematch[i][0] == '.');
            if ((is_ext && ext && !strcmp(ext, s->filematch[i])) ||
                (!is_ext && strstr(E.filename, s->filematch[i]))) {
                E.syntax = s;
                for (int filerow = 0; filerow < E.numrows; filerow++)
                    editorUpdateSyntax(&E.row[filerow]);
                return;
            }
            i++;
        }
    }
}

/* ── Satır İşlemleri ────────────────────────────────────────────────────── */

int editorRowCxToRx(erow *row, int cx) {
    int rx = 0;
    for (int j = 0; j < cx; j++) {
        if (row->chars[j] == '\t')
            rx += (KILO_TAB_STOP - 1) - (rx % KILO_TAB_STOP);
        rx++;
    }
    return rx;
}

int editorRowRxToCx(erow *row, int rx) {
    int cur_rx = 0;
    int cx;
    for (cx = 0; cx < row->size; cx++) {
        if (row->chars[cx] == '\t')
            cur_rx += (KILO_TAB_STOP - 1) - (cur_rx % KILO_TAB_STOP);
        cur_rx++;
        if (cur_rx > rx) return cx;
    }
    return cx;
}

void editorUpdateRow(erow *row) {
    int tabs = 0;
    for (int j = 0; j < row->size; j++)
        if (row->chars[j] == '\t') tabs++;

    free(row->render);
    row->render = malloc(row->size + tabs * (KILO_TAB_STOP - 1) + 1);

    int idx = 0;
    for (int j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') {
            row->render[idx++] = ' ';
            while (idx % KILO_TAB_STOP != 0) row->render[idx++] = ' ';
        } else {
            row->render[idx++] = row->chars[j];
        }
    }
    row->render[idx] = '\0';
    row->rsize = idx;

    editorUpdateSyntax(row);
}

void editorInsertRow(int at, char *s, size_t len) {
    if (at < 0 || at > E.numrows) return;

    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
    memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));
    for (int j = at + 1; j <= E.numrows; j++) E.row[j].idx++;

    E.row[at].idx = at;
    E.row[at].size = (int)len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';

    E.row[at].rsize = 0;
    E.row[at].render = NULL;
    E.row[at].hl = NULL;
    E.row[at].hl_open_comment = 0;
    editorUpdateRow(&E.row[at]);

    E.numrows++;
    E.dirty++;
}

void editorFreeRow(erow *row) {
    free(row->render);
    free(row->chars);
    free(row->hl);
}

void editorDelRow(int at) {
    if (at < 0 || at >= E.numrows) return;
    editorFreeRow(&E.row[at]);
    memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
    for (int j = at; j < E.numrows - 1; j++) E.row[j].idx--;
    E.numrows--;
    E.dirty++;
}

void editorRowInsertChar(erow *row, int at, int c) {
    if (at < 0 || at > row->size) at = row->size;
    row->chars = realloc(row->chars, row->size + 2);
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
    row->size++;
    row->chars[at] = c;
    editorUpdateRow(row);
    E.dirty++;
}

void editorRowAppendString(erow *row, char *s, size_t len) {
    row->chars = realloc(row->chars, row->size + len + 1);
    memcpy(&row->chars[row->size], s, len);
    row->size += len;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
    E.dirty++;
}

void editorRowDelChar(erow *row, int at) {
    if (at < 0 || at >= row->size) return;
    memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
    row->size--;
    editorUpdateRow(row);
    E.dirty++;
}

/* ── Editör İşlemleri ───────────────────────────────────────────────────── */

void editorInsertChar(int c) {
    if (E.cy == E.numrows) editorInsertRow(E.numrows, "", 0);
    editorRowInsertChar(&E.row[E.cy], E.cx, c);
    E.cx++;
}

void editorInsertNewline(void) {
    if (E.cx == 0) {
        editorInsertRow(E.cy, "", 0);
    } else {
        erow *row = &E.row[E.cy];
        editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
        row = &E.row[E.cy];
        row->size = E.cx;
        row->chars[row->size] = '\0';
        editorUpdateRow(row);
    }
    E.cy++;
    E.cx = 0;
}

void editorDelChar(void) {
    if (E.cy == E.numrows) return;
    if (E.cx == 0 && E.cy == 0) return;

    erow *row = &E.row[E.cy];
    if (E.cx > 0) {
        editorRowDelChar(row, E.cx - 1);
        E.cx--;
    } else {
        E.cx = E.row[E.cy - 1].size;
        editorRowAppendString(&E.row[E.cy - 1], row->chars, row->size);
        editorDelRow(E.cy);
        E.cy--;
    }
}

/* ── Dosya I/O ──────────────────────────────────────────────────────────── */

char *editorRowsToString(int *buflen) {
    int totlen = 0;
    for (int j = 0; j < E.numrows; j++)
        totlen += E.row[j].size + 1;
    *buflen = totlen;

    char *buf = malloc(totlen);
    char *p = buf;
    for (int j = 0; j < E.numrows; j++) {
        memcpy(p, E.row[j].chars, E.row[j].size);
        p += E.row[j].size;
        *p = '\n';
        p++;
    }
    return buf;
}

void editorOpen(char *filename) {
    free(E.filename);
    E.filename = strdup(filename);

    editorSelectSyntaxHighlight();

    /* O_RDONLY = 0 — fopen kullanma, newlib O_CLOEXEC ekliyor */
    int fd = open(filename, 0, 0);
    if (fd < 0) {
        editorSetStatusMessage("Yeni dosya: %s", filename);
        return;
    }

    char *filebuf = malloc(1024 * 1024);
    if (!filebuf) { close(fd); return; }

    int total = 0, n;
    while (total < 1024 * 1024 - 1) {
        n = (int)read(fd, filebuf + total, (size_t)(1024 * 1024 - 1 - total));
        if (n <= 0) break;
        total += n;
    }
    close(fd);
    filebuf[total] = '\0';

    char *p = filebuf;
    while (p < filebuf + total) {
        char *nl = p;
        while (nl < filebuf + total && *nl != '\n') nl++;
        int linelen = (int)(nl - p);
        if (linelen > 0 && p[linelen - 1] == '\r') linelen--;
        editorInsertRow(E.numrows, p, linelen);
        p = nl + 1;
    }
    free(filebuf);
    E.dirty = 0;
}

void editorSave(void) {
    if (E.filename == NULL) {
        E.filename = editorPrompt("Kaydet: %s (ESC = iptal)", NULL);
        if (E.filename == NULL) {
            editorSetStatusMessage("Kaydet iptal edildi.");
            return;
        }
        editorSelectSyntaxHighlight();
    }

    int len;
    char *buf = editorRowsToString(&len);
    if (!buf) {
        editorSetStatusMessage("Kaydetme hatasi: bellek yok");
        return;
    }

    /* Doğrudan syscall — O_WRONLY|O_CREAT, O_TRUNC YOK!
     * O_TRUNC kernel'da auto_save tetikler ve 0-byte'lık hayalet oluşturur.
     * fs_vfs_write zaten offset=0'dan başlayarak eski içeriği ezer. */
    int fd = open(E.filename, 0x01 | 0x40, 0644);  /* O_WRONLY|O_CREAT */
    if (fd < 0) {
        free(buf);
        editorSetStatusMessage("Kaydetme hatasi: open=%d errno=%d", fd, errno);
        return;
    }

    int written = 0;
    while (written < len) {
        int n = (int)write(fd, buf + written, (size_t)(len - written));
        if (n <= 0) {
            close(fd);
            free(buf);
            editorSetStatusMessage("Kaydetme hatasi: write %d/%d", written, len);
            return;
        }
        written += n;
    }
    close(fd);
    free(buf);
    E.dirty = 0;
    editorSetStatusMessage("%d byte kaydedildi: %s", len, E.filename);
}

/* ── Bul (Arama) ────────────────────────────────────────────────────────── */

void editorFindCallback(char *query, int key) {
    static int last_match = -1;
    static int direction = 1;
    static int saved_hl_line;
    static char *saved_hl = NULL;

    if (saved_hl) {
        memcpy(E.row[saved_hl_line].hl, saved_hl, E.row[saved_hl_line].rsize);
        free(saved_hl);
        saved_hl = NULL;
    }

    if (key == '\r' || key == '\x1b') {
        last_match = -1;
        direction = 1;
        return;
    } else if (key == ARROW_RIGHT || key == ARROW_DOWN) {
        direction = 1;
    } else if (key == ARROW_LEFT || key == ARROW_UP) {
        direction = -1;
    } else {
        last_match = -1;
        direction = 1;
    }

    if (last_match == -1) direction = 1;
    int current = last_match;

    for (int i = 0; i < E.numrows; i++) {
        current += direction;
        if (current == -1) current = E.numrows - 1;
        else if (current == E.numrows) current = 0;

        erow *row = &E.row[current];
        char *match = strstr(row->render, query);
        if (match) {
            last_match = current;
            E.cy = current;
            E.cx = editorRowRxToCx(row, (int)(match - row->render));
            E.rowoff = E.numrows;

            saved_hl_line = current;
            saved_hl = malloc(row->rsize);
            memcpy(saved_hl, row->hl, row->rsize);
            memset(&row->hl[match - row->render], HL_MATCH, strlen(query));
            break;
        }
    }
}

void editorFind(void) {
    int saved_cx = E.cx;
    int saved_cy = E.cy;
    int saved_coloff = E.coloff;
    int saved_rowoff = E.rowoff;

    char *query = editorPrompt("Ara: %s (ESC/Oklar/Enter)",
                               editorFindCallback);
    if (query) {
        free(query);
    } else {
        E.cx = saved_cx;
        E.cy = saved_cy;
        E.coloff = saved_coloff;
        E.rowoff = saved_rowoff;
    }
}

/* ── Append Buffer ──────────────────────────────────────────────────────── */

struct abuf {
    char *b;
    int len;
};

#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len) {
    char *new = realloc(ab->b, ab->len + len);
    if (new == NULL) return;
    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
}

void abFree(struct abuf *ab) {
    free(ab->b);
}

/* ── Çıktı ──────────────────────────────────────────────────────────────── */

void editorScroll(void) {
    E.rx = 0;
    if (E.cy < E.numrows)
        E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);

    if (E.cy < E.rowoff) E.rowoff = E.cy;
    if (E.cy >= E.rowoff + E.screenrows) E.rowoff = E.cy - E.screenrows + 1;
    if (E.rx < E.coloff) E.coloff = E.rx;
    if (E.rx >= E.coloff + E.screencols) E.coloff = E.rx - E.screencols + 1;
}

void editorDrawRows(struct abuf *ab) {
    for (int y = 0; y < E.screenrows; y++) {
        /* Her satır için mutlak cursor konumu — \r\n yerine.
         * \n do_newline() çağırır, bu scroll_screen_up() tetikleyebilir.
         * Mutlak konumlandırma ile bu sorun tamamen ortadan kalkar.    */
        char pos[16];
        int plen = snprintf(pos, sizeof(pos), "\x1b[%d;1H", y + 1);
        abAppend(ab, pos, plen);

        abAppend(ab, "\x1b[0m", 4);   /* rengi sıfırla */

        int filerow = y + E.rowoff;
        if (filerow >= E.numrows) {
            if (E.numrows == 0 && y == E.screenrows / 3) {
                char welcome[80];
                int welcomelen = snprintf(welcome, sizeof(welcome),
                    "Kilo editor -- %s", KILO_VERSION);
                if (welcomelen > E.screencols) welcomelen = E.screencols;
                int padding = (E.screencols - welcomelen) / 2;
                if (padding) {
                    abAppend(ab, "\x1b[37m", 5);
                    abAppend(ab, "~", 1);
                    padding--;
                }
                while (padding--) abAppend(ab, " ", 1);
                abAppend(ab, welcome, welcomelen);
            } else {
                abAppend(ab, "\x1b[37m", 5);
                abAppend(ab, "~", 1);
            }
        } else {
            int len = E.row[filerow].rsize - E.coloff;
            if (len < 0) len = 0;
            if (len > E.screencols) len = E.screencols;
            char *c = &E.row[filerow].render[E.coloff];
            unsigned char *hl = &E.row[filerow].hl[E.coloff];
            int current_color = -1;
            abAppend(ab, "\x1b[37m", 5);   /* varsayılan beyaz fg */
            for (int j = 0; j < len; j++) {
                if (hl[j] == HL_NORMAL) {
                    if (current_color != -1) {
                        abAppend(ab, "\x1b[39m", 5);
                        current_color = -1;
                    }
                    abAppend(ab, &c[j], 1);
                } else {
                    int color = editorSyntaxToColor(hl[j]);
                    if (color != current_color) {
                        current_color = color;
                        char buf[16];
                        int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", color);
                        abAppend(ab, buf, clen);
                    }
                    abAppend(ab, &c[j], 1);
                }
            }
            abAppend(ab, "\x1b[39m", 5);
        }

        abAppend(ab, "\x1b[K", 3);   /* satır sonunu temizle (\r\n YOK) */
    }
}

void editorDrawStatusBar(struct abuf *ab) {
    /* Status bar: E.screenrows + 1. satır (mutlak konum) */
    char pos[16];
    int plen = snprintf(pos, sizeof(pos), "\x1b[%d;1H", E.screenrows + 1);
    abAppend(ab, pos, plen);

    abAppend(ab, "\x1b[7m", 4);
    char status[80], rstatus[80];
    int len = snprintf(status, sizeof(status), "%.20s - %d satir %s",
        E.filename ? E.filename : "[Isimsiz]", E.numrows,
        E.dirty ? "(degistirildi)" : "");
    int rlen = snprintf(rstatus, sizeof(rstatus), "%s | %d/%d",
        E.syntax ? E.syntax->filetype : "duz yazi",
        E.cy + 1, E.numrows);
    if (len > E.screencols) len = E.screencols;
    abAppend(ab, status, len);
    while (len < E.screencols) {
        if (E.screencols - len == rlen) {
            abAppend(ab, rstatus, rlen);
            break;
        }
        abAppend(ab, " ", 1);
        len++;
    }
    abAppend(ab, "\x1b[m", 3);
}

void editorDrawMessageBar(struct abuf *ab) {
    /* Mesaj satırı: E.screenrows + 2. satır (mutlak konum) */
    char pos[16];
    int plen = snprintf(pos, sizeof(pos), "\x1b[%d;1H", E.screenrows + 2);
    abAppend(ab, pos, plen);

    abAppend(ab, "\x1b[K", 3);
    int msglen = (int)strlen(E.statusmsg);
    if (msglen > E.screencols) msglen = E.screencols;
    if (msglen && time(NULL) - E.statusmsg_time < 5)
        abAppend(ab, E.statusmsg, msglen);
}

void editorRefreshScreen(void) {
    editorScroll();

    struct abuf ab = ABUF_INIT;
    abAppend(&ab, "\x1b[?25l", 6);  /* cursor gizle */
    abAppend(&ab, "\x1b[0m", 4);    /* rengi sıfırla */
    /* \x1b[H YOK — editorDrawRows her satır için mutlak konum kullanıyor */

    editorDrawRows(&ab);
    editorDrawStatusBar(&ab);
    editorDrawMessageBar(&ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH",
             (E.cy - E.rowoff) + 1,
             (E.rx - E.coloff) + 1);
    abAppend(&ab, buf, (int)strlen(buf));

    abAppend(&ab, "\x1b[?25h", 6);
    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

void editorSetStatusMessage(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_time = time(NULL);
}

/* ── Giriş ──────────────────────────────────────────────────────────────── */

char *editorPrompt(const char *prompt, void (*callback)(char *, int)) {
    size_t bufsize = 128;
    char *buf = malloc(bufsize);
    size_t buflen = 0;
    buf[0] = '\0';

    while (1) {
        editorSetStatusMessage(prompt, buf);
        editorRefreshScreen();

        int c = editorReadKey();
        if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
            if (buflen != 0) buf[--buflen] = '\0';
        } else if (c == '\x1b') {
            editorSetStatusMessage("");
            if (callback) callback(buf, c);
            free(buf);
            return NULL;
        } else if (c == '\r') {
            if (buflen != 0) {
                editorSetStatusMessage("");
                if (callback) callback(buf, c);
                return buf;
            }
        } else if (!iscntrl(c) && c < 128) {
            if (buflen == bufsize - 1) {
                bufsize *= 2;
                buf = realloc(buf, bufsize);
            }
            buf[buflen++] = c;
            buf[buflen] = '\0';
        }

        if (callback) callback(buf, c);
    }
}

void editorMoveCursor(int key) {
    erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

    switch (key) {
    case ARROW_LEFT:
        if (E.cx != 0) {
            E.cx--;
        } else if (E.cy > 0) {
            E.cy--;
            E.cx = E.row[E.cy].size;
        }
        break;
    case ARROW_RIGHT:
        if (row && E.cx < row->size) {
            E.cx++;
        } else if (row && E.cx == row->size) {
            E.cy++;
            E.cx = 0;
        }
        break;
    case ARROW_UP:
        if (E.cy != 0) E.cy--;
        break;
    case ARROW_DOWN:
        if (E.cy < E.numrows) E.cy++;
        break;
    }

    row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
    int rowlen = row ? row->size : 0;
    if (E.cx > rowlen) E.cx = rowlen;
}

void editorProcessKeypress(void) {
    static int quit_times = KILO_QUIT_TIMES;

    int c = editorReadKey();

    switch (c) {
    case '\r':
        editorInsertNewline();
        break;

    case CTRL_KEY('q'):
        if (E.dirty && quit_times > 0) {
            editorSetStatusMessage(
                "UYARI! Kaydedilmemis degisiklikler var. "
                "Cikmak icin CTRL-Q'ya %d kez daha basin.",
                quit_times);
            quit_times--;
            return;
        }
        write(STDOUT_FILENO, "\x1b[2J", 4);
        write(STDOUT_FILENO, "\x1b[H", 3);
        exit(0);
        break;

    case CTRL_KEY('s'):
        editorSave();
        break;

    case HOME_KEY:
        E.cx = 0;
        break;
    case END_KEY:
        if (E.cy < E.numrows) E.cx = E.row[E.cy].size;
        break;

    case CTRL_KEY('f'):
        editorFind();
        break;

    case BACKSPACE:
    case CTRL_KEY('h'):
    case DEL_KEY:
        if (c == DEL_KEY) editorMoveCursor(ARROW_RIGHT);
        editorDelChar();
        break;

    case PAGE_UP:
    case PAGE_DOWN:
        {
            if (c == PAGE_UP) {
                E.cy = E.rowoff;
            } else if (c == PAGE_DOWN) {
                E.cy = E.rowoff + E.screenrows - 1;
                if (E.cy > E.numrows) E.cy = E.numrows;
            }
            int times = E.screenrows;
            while (times--)
                editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
        }
        break;

    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
        editorMoveCursor(c);
        break;

    case CTRL_KEY('l'):
    case '\x1b':
        break;

    default:
        editorInsertChar(c);
        break;
    }

    quit_times = KILO_QUIT_TIMES;
}

/* ── Başlatma ───────────────────────────────────────────────────────────── */

void initEditor(void) {
    E.cx = 0;
    E.cy = 0;
    E.rx = 0;
    E.rowoff = 0;
    E.coloff = 0;
    E.numrows = 0;
    E.row = NULL;
    E.dirty = 0;
    E.filename = NULL;
    E.statusmsg[0] = '\0';
    E.statusmsg_time = 0;
    E.syntax = NULL;

    if (getWindowSize(&E.screenrows, &E.screencols) == -1) {
        /* Fallback: TIOCGWINSZ başarısız — güvenli varsayılan boyut */
        E.screenrows = 24;
        E.screencols = 80;
    }
    E.screenrows -= 2; /* durum çubuğu + mesaj */
}

/* ── Başlangıç dosya adı okuma (raw mode öncesi) ────────────────────────
 * AscentOS shell'i "exec KILO.ELF" komutunda argv[1] geçmez.
 * Bu durumda terminal ham satır okuyarak kullanıcıdan dosya adı alırız.
 * Raw mode HENÜz aktif değil → normal echo + enter çalışır.            */
static char startup_filename[256];

static void ask_filename_before_raw(void) {
    /* Basit istem: write() ile yaz, read() ile oku (newlib stdio değil,
     * raw fd — termios henüz değiştirilmedi, kernel canonical modda)   */
    const char prompt[] = "Kilo > Dosya adi (Enter = yeni): ";
    write(STDOUT_FILENO, prompt, sizeof(prompt) - 1);

    int i = 0;
    char c;
    while (i < (int)sizeof(startup_filename) - 1) {
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n <= 0) break;
        if (c == '\r' || c == '\n') break;
        if ((c == '\b' || c == 127) && i > 0) {
            i--;
            /* backspace görsel geri al */
            write(STDOUT_FILENO, "\b \b", 3);
            continue;
        }
        if (c < 32) continue;   /* diğer kontrol karakterlerini yoksay */
        startup_filename[i++] = c;
        write(STDOUT_FILENO, &c, 1); /* echo */
    }
    startup_filename[i] = '\0';
    write(STDOUT_FILENO, "\r\n", 2);
}

/* ── main ───────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    const char *filename = NULL;

    /* argv[1] varsa doğrudan kullan (Linux/execve ile doğru geçilmişse) */
    if (argc >= 2 && argv[1] && argv[1][0] != '\0') {
        filename = argv[1];
    } else {
        /* AscentOS "exec KILO.ELF" → argv boş veya eksik.
         * Raw mode öncesinde canonical modda dosya adı sor.             */
        ask_filename_before_raw();
        if (startup_filename[0] != '\0')
            filename = startup_filename;
    }

    enableRawMode();
    initEditor();

    if (filename) {
        editorOpen((char *)filename);
    }

    editorSetStatusMessage(
        "YARDIM: CTRL-S = Kaydet | CTRL-Q = Cik | CTRL-F = Bul");

    while (1) {
        editorRefreshScreen();
        editorProcessKeypress();
    }

    return 0;
}