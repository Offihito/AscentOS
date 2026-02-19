#ifndef LIBC_TYPES_H
#define LIBC_TYPES_H

// ─────────────────────────────────────────────
//  AscentOS Minimal Libc — types.h
//  Temel veri tipleri ve sabitler
// ─────────────────────────────────────────────

typedef unsigned long  size_t;
typedef long           ssize_t;
typedef int            pid_t;

#define NULL  ((void*)0)

#define STDIN  0
#define STDOUT 1
#define STDERR 2

#endif // LIBC_TYPES_H