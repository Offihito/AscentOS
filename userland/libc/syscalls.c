// ═══════════════════════════════════════════════════════════════
//  AscentOS — newlib syscall stubs
//  Dosya: userland/libc/syscalls.c
//
//  ÖNEMLİ: Bu newlib versiyonu underscore'suz isimler bekler.
//  newlib içindeki _write_r, _sbrk_r gibi reentrant wrapper'lar
//  buraya underscore'suz (write, sbrk, getpid...) olarak bağlanır.
// ═══════════════════════════════════════════════════════════════

// ── Minimum tip tanımları (host header yok) ───────────────────
typedef long           ssize_t;
typedef unsigned long  size_t;
typedef int            pid_t;

// ── AscentOS Syscall Numaraları ───────────────────────────────
#define SYS_WRITE   1
#define SYS_READ    2
#define SYS_EXIT    3
#define SYS_GETPID  4
#define SYS_SBRK    7   // ← kernel'ındaki gerçek numara neyse yaz
#define SYS_FORK    19
#define SYS_WAITPID 21

// ── Raw syscall helper'lar ────────────────────────────────────

static inline long _sc1(long nr, long a1) {
    long ret;
    __asm__ volatile (
        "mov %1, %%rax\n"
        "mov %2, %%rdi\n"
        "syscall\n"
        "mov %%rax, %0\n"
        : "=r"(ret) : "r"(nr), "r"(a1)
        : "rax", "rdi", "rcx", "r11", "memory"
    );
    return ret;
}

static inline long _sc3(long nr, long a1, long a2, long a3) {
    long ret;
    __asm__ volatile (
        "mov %1, %%rax\n"
        "mov %2, %%rdi\n"
        "mov %3, %%rsi\n"
        "mov %4, %%rdx\n"
        "syscall\n"
        "mov %%rax, %0\n"
        : "=r"(ret) : "r"(nr), "r"(a1), "r"(a2), "r"(a3)
        : "rax", "rdi", "rsi", "rdx", "rcx", "r11", "memory"
    );
    return ret;
}

// ════════════════════════════════════════════════════════════════
//  I/O
// ════════════════════════════════════════════════════════════════

ssize_t write(int fd, const void* buf, size_t len) {
    return (ssize_t)_sc3(SYS_WRITE, (long)fd, (long)buf, (long)len);
}

ssize_t read(int fd, void* buf, size_t len) {
    return (ssize_t)_sc3(SYS_READ, (long)fd, (long)buf, (long)len);
}

int close(int fd) {
    (void)fd;
    return 0;
}

// ════════════════════════════════════════════════════════════════
//  PROCESS
// ════════════════════════════════════════════════════════════════

__attribute__((noreturn))
void _exit(int code) {
    _sc1(SYS_EXIT, (long)code);
    __builtin_unreachable();
}

pid_t getpid(void) {
    return (pid_t)_sc1(SYS_GETPID, 0);
}

pid_t fork(void) {
    pid_t ret;
    __asm__ volatile (
        "mov $19, %%rax\n"
        "syscall\n"
        "mov %%eax, %0\n"
        : "=r"(ret)
        :
        : "rax", "rcx", "r11", "memory"
    );
    return ret;
}

pid_t waitpid(pid_t pid, int* status, int opts) {
    return (pid_t)_sc3(SYS_WAITPID, (long)pid, (long)status, (long)opts);
}

pid_t wait(int* status) {
    return waitpid(-1, status, 0);
}

int kill(pid_t pid, int sig) {
    (void)pid; (void)sig;
    return -1;
}

// ════════════════════════════════════════════════════════════════
//  HEAP — sbrk
//
//  SBRK_MODE 0: kernel'da SYS_SBRK var (önerilen)
//               kernel incr alır, yeni heap ucunu döner
//  SBRK_MODE 1: kernel'da sbrk yok, userspace bump allocator
//               user.ld'deki __heap_start sembolünden başlar
// ════════════════════════════════════════════════════════════════

#define SBRK_MODE 1   // ← kernel'ına SYS_SBRK eklediysen 0 yap

#if SBRK_MODE == 0

void* sbrk(long incr) {
    long ret = _sc1(SYS_SBRK, incr);
    if (ret < 0) return (void*)-1;
    return (void*)ret;
}

#else

extern char __heap_start;
static char* _heap_ptr = 0;

void* sbrk(long incr) {
    if (!_heap_ptr) _heap_ptr = &__heap_start;
    char* prev = _heap_ptr;
    _heap_ptr += incr;
    return (void*)prev;
}

#endif

// ════════════════════════════════════════════════════════════════
//  FS STUB'LAR — gerçek dosya sistemi olmadığında
// ════════════════════════════════════════════════════════════════

// fstat: terminal gibi davran (S_IFCHR = 0x2000)
int fstat(int fd, void* st) {
    (void)fd;
    if (st) ((int*)st)[3] = 0x2000;
    return 0;
}

int isatty(int fd) {
    return (fd == 0 || fd == 1 || fd == 2) ? 1 : 0;
}

long lseek(int fd, long offset, int whence) {
    (void)fd; (void)offset; (void)whence;
    return -1;
}

int open(const char* path, int flags, ...) {
    (void)path; (void)flags;
    return -1;
}