#ifndef LIBC_STDIO_H
#define LIBC_STDIO_H

// ─────────────────────────────────────────────
//  AscentOS Minimal Libc — stdio.h
//  Çıktı fonksiyonları
//
//  Format specifier'lar:
//    %s  → string          %d  → signed int
//    %u  → unsigned int    %c  → char
//    %x  → hex (küçük)     %X  → hex (büyük)
//    %o  → octal           %p  → pointer
//    %%  → literal '%'
//    (#) flag: %#x → "0x...", %#o → "0..."
//
//  Fonksiyonlar:
//    putchar, puts
//    printf, fprintf, dprintf
//    sprintf, snprintf
// ─────────────────────────────────────────────

#include "types.h"
#include "string.h"
#include "unistd.h"

// ═════════════════════════════════════════════
//  İÇ YARDIMCI FONKSİYONLAR  (_ prefix)
// ═════════════════════════════════════════════

// Unsigned long → string, istenilen tabanda (2–16)
// buf en az 66 byte olmalı (base2 + null için)
static inline void _utoa(unsigned long v, char* buf, int base, int upper) {
    const char* lo = "0123456789abcdef";
    const char* hi = "0123456789ABCDEF";
    const char* digits = upper ? hi : lo;

    if (v == 0) { buf[0] = '0'; buf[1] = '\0'; return; }

    char tmp[66]; int n = 0;
    while (v) { tmp[n++] = digits[v % base]; v /= base; }
    for (int i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    buf[n] = '\0';
}

// Signed int → string (base 10)
// unsigned long kullanılır: INT_MIN (-2147483648) için -v overflow'u önler
static inline void _itoa_s(int v, char* buf) {
    if (v == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    char tmp[12]; int n = 0, neg = 0;
    unsigned long uv;
    if (v < 0) { neg = 1; uv = (unsigned long)(-(long)v); }
    else        { uv = (unsigned long)v; }
    while (uv) { tmp[n++] = '0' + (int)(uv % 10); uv /= 10; }
    if (neg) tmp[n++] = '-';
    for (int i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    buf[n] = '\0';
}

// ═════════════════════════════════════════════
//  CORE: _vdprintf  (fd + va_list)
//  Tüm fd-bazlı printf ailesi bunu kullanır.
// ═════════════════════════════════════════════

static inline void _vdprintf(int fd, const char* fmt, __builtin_va_list ap) {
    char buf[66];

    while (*fmt) {
        if (*fmt != '%') { write(fd, fmt, 1); fmt++; continue; }
        fmt++; // '%' atla

        int flag_hash = (*fmt == '#') ? (fmt++, 1) : 0;

        switch (*fmt) {
        case 's': {
            const char* s = __builtin_va_arg(ap, const char*);
            if (!s) s = "(null)";
            write(fd, s, strlen(s));
            break;
        }
        case 'd':
            _itoa_s(__builtin_va_arg(ap, int), buf);
            write(fd, buf, strlen(buf));
            break;
        case 'u':
            _utoa((unsigned long)__builtin_va_arg(ap, unsigned int), buf, 10, 0);
            write(fd, buf, strlen(buf));
            break;
        case 'x':
            if (flag_hash) write(fd, "0x", 2);
            _utoa((unsigned long)__builtin_va_arg(ap, unsigned int), buf, 16, 0);
            write(fd, buf, strlen(buf));
            break;
        case 'X':
            if (flag_hash) write(fd, "0X", 2);
            _utoa((unsigned long)__builtin_va_arg(ap, unsigned int), buf, 16, 1);
            write(fd, buf, strlen(buf));
            break;
        case 'o':
            if (flag_hash) write(fd, "0", 1);
            _utoa((unsigned long)__builtin_va_arg(ap, unsigned int), buf, 8, 0);
            write(fd, buf, strlen(buf));
            break;
        case 'p': {
            unsigned long v = (unsigned long)__builtin_va_arg(ap, void*);
            if (v == 0) { write(fd, "(nil)", 5); }
            else { write(fd, "0x", 2); _utoa(v, buf, 16, 0); write(fd, buf, strlen(buf)); }
            break;
        }
        case 'c': {
            char c = (char)__builtin_va_arg(ap, int);
            write(fd, &c, 1);
            break;
        }
        case '%': write(fd, "%", 1); break;
        default:
            write(fd, "%", 1);
            if (flag_hash) write(fd, "#", 1);
            write(fd, fmt, 1);
            break;
        }
        fmt++;
    }
}

// ═════════════════════════════════════════════
//  CORE: _vsnprintf  (buffer + va_list)
//  sprintf / snprintf bunu kullanır.
// ═════════════════════════════════════════════

static inline int _vsnprintf(char* out, size_t size,
                              const char* fmt, __builtin_va_list ap) {
    if (size == 0) return 0;

    char nbuf[66];
    size_t w = 0; // yazılan karakter sayısı

// Taşma korumalı buffer yazma
#define _W(src, len)                                     \
    do {                                                 \
        size_t _l = (len);                               \
        for (size_t _i = 0; _i < _l; _i++) {            \
            if (w + 1 >= size) goto _done;               \
            out[w++] = ((const char*)(src))[_i];         \
        }                                                \
    } while (0)

    while (*fmt) {
        if (*fmt != '%') { _W(fmt, 1); fmt++; continue; }
        fmt++;

        int flag_hash = (*fmt == '#') ? (fmt++, 1) : 0;

        switch (*fmt) {
        case 's': {
            const char* s = __builtin_va_arg(ap, const char*);
            if (!s) s = "(null)";
            _W(s, strlen(s));
            break;
        }
        case 'd':
            _itoa_s(__builtin_va_arg(ap, int), nbuf);
            _W(nbuf, strlen(nbuf));
            break;
        case 'u':
            _utoa((unsigned long)__builtin_va_arg(ap, unsigned int), nbuf, 10, 0);
            _W(nbuf, strlen(nbuf));
            break;
        case 'x':
            if (flag_hash) _W("0x", 2);
            _utoa((unsigned long)__builtin_va_arg(ap, unsigned int), nbuf, 16, 0);
            _W(nbuf, strlen(nbuf));
            break;
        case 'X':
            if (flag_hash) _W("0X", 2);
            _utoa((unsigned long)__builtin_va_arg(ap, unsigned int), nbuf, 16, 1);
            _W(nbuf, strlen(nbuf));
            break;
        case 'o':
            if (flag_hash) _W("0", 1);
            _utoa((unsigned long)__builtin_va_arg(ap, unsigned int), nbuf, 8, 0);
            _W(nbuf, strlen(nbuf));
            break;
        case 'p': {
            unsigned long v = (unsigned long)__builtin_va_arg(ap, void*);
            if (v == 0) { _W("(nil)", 5); }
            else { _W("0x", 2); _utoa(v, nbuf, 16, 0); _W(nbuf, strlen(nbuf)); }
            break;
        }
        case 'c': {
            char c = (char)__builtin_va_arg(ap, int);
            _W(&c, 1);
            break;
        }
        case '%': _W("%", 1); break;
        default:
            _W("%", 1);
            if (flag_hash) _W("#", 1);
            _W(fmt, 1);
            break;
        }
        fmt++;
    }

_done:
    out[w] = '\0';
    return (int)w;

#undef _W
}

// ═════════════════════════════════════════════
//  PUBLIC API
// ═════════════════════════════════════════════

static inline void putchar(char c) {
    write(STDOUT, &c, 1);
}

static inline void puts(const char* s) {
    write(STDOUT, s, strlen(s));
    write(STDOUT, "\n", 1);
}

// printf — stdout'a yazar
static inline void printf(const char* fmt, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    _vdprintf(STDOUT, fmt, ap);
    __builtin_va_end(ap);
}

// fprintf — istenen fd'ye yazar
static inline void fprintf(int fd, const char* fmt, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    _vdprintf(fd, fmt, ap);
    __builtin_va_end(ap);
}

// dprintf — fprintf ile özdeş, POSIX dprintf karşılığı
static inline void dprintf(int fd, const char* fmt, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    _vdprintf(fd, fmt, ap);
    __builtin_va_end(ap);
}

// snprintf — en fazla (size-1) karakter, her zaman null-terminate
static inline int snprintf(char* buf, size_t size, const char* fmt, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    int n = _vsnprintf(buf, size, fmt, ap);
    __builtin_va_end(ap);
    return n;
}

// sprintf — taşma koruması YOK, yeterli buffer kullan
static inline int sprintf(char* buf, const char* fmt, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    int n = _vsnprintf(buf, (size_t)-1, fmt, ap);
    __builtin_va_end(ap);
    return n;
}

#endif // LIBC_STDIO_H