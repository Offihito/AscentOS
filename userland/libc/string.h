#ifndef LIBC_STRING_H
#define LIBC_STRING_H

// ─────────────────────────────────────────────
//  AscentOS Minimal Libc — string.h
//  String ve bellek işlem fonksiyonları
// ─────────────────────────────────────────────

#include "types.h"

static inline size_t strlen(const char* s) {
    const char* p = s;
    while (*p) p++;
    return p - s;
}

static inline void* memset(void* d, int c, size_t n) {
    unsigned char* p = d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}

static inline void* memcpy(void* d, const void* s, size_t n) {
    unsigned char* dp = d;
    const unsigned char* sp = s;
    while (n--) *dp++ = *sp++;
    return d;
}

static inline int strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

// ── Sayıdan Stringe Dönüşüm (base 10) ────────
static inline void itoa(int v, char* buf) {
    if (v == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    char tmp[12];
    int n = 0, neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    while (v) { tmp[n++] = '0' + v % 10; v /= 10; }
    if (neg) tmp[n++] = '-';
    for (int i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    buf[n] = '\0';
}

#endif // LIBC_STRING_H