#ifndef LIBC_MATH_H
#define LIBC_MATH_H

// ─────────────────────────────────────────────
//  AscentOS Minimal Libc — math.h
//  Temel matematik fonksiyonları
//  Host bağımlılığı yok, floating point yok.
//
//  int    : abs, min, max, clamp
//  long   : labs, lmin, lmax, lclamp
//  size_t : smin, smax, sclamp
// ─────────────────────────────────────────────

#include "types.h"

// ═════════════════════════════════════════════
//  INT
// ═════════════════════════════════════════════

static inline int abs(int v) {
    return v < 0 ? -v : v;
}

static inline int min(int a, int b) {
    return a < b ? a : b;
}

static inline int max(int a, int b) {
    return a > b ? a : b;
}

// [lo, hi] aralığında tutar
static inline int clamp(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// ═════════════════════════════════════════════
//  LONG
// ═════════════════════════════════════════════

static inline long labs(long v) {
    return v < 0 ? -v : v;
}

static inline long lmin(long a, long b) {
    return a < b ? a : b;
}

static inline long lmax(long a, long b) {
    return a > b ? a : b;
}

static inline long lclamp(long v, long lo, long hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// ═════════════════════════════════════════════
//  SIZE_T (unsigned)
// ═════════════════════════════════════════════

static inline size_t smin(size_t a, size_t b) {
    return a < b ? a : b;
}

static inline size_t smax(size_t a, size_t b) {
    return a > b ? a : b;
}

static inline size_t sclamp(size_t v, size_t lo, size_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

#endif // LIBC_MATH_H