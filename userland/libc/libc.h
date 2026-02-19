#ifndef LIBC_H
#define LIBC_H

// ─────────────────────────────────────────────
//  AscentOS Minimal Libc — libc.h
//
//  Şemsiye header: tüm modülleri tek seferde
//  dahil etmek için kullanılır.
//
//  Modüler kullanım (yalnızca gerekli olanı):
//    #include "libc/types.h"    → size_t, pid_t, NULL, STD*
//    #include "libc/syscall.h"  → syscall1, syscall3, SYS_*
//    #include "libc/unistd.h"   → write, read, fork, exit ...
//    #include "libc/string.h"   → strlen, memcpy, strcmp, itoa
//    #include "libc/stdio.h"    → puts, printf
//
//  Tam kullanım (eskisi gibi):
//    #include "libc/libc.h"
// ─────────────────────────────────────────────

#include "types.h"
#include "syscall.h"
#include "unistd.h"
#include "string.h"
#include "stdio.h"
#include "math.h"

#endif // LIBC_H