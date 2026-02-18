// syscall.h - SYSCALL/SYSRET Infrastructure for AscentOS 64-bit
// Intel/AMD x86-64 SYSCALL instruction via MSR configuration
#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>

// ============================================================
// MSR Adresleri
// ============================================================
#define MSR_EFER       0xC0000080   // Extended Feature Enable Register
#define MSR_STAR       0xC0000081   // Segment selectors for SYSCALL/SYSRET
#define MSR_LSTAR      0xC0000082   // 64-bit SYSCALL entry point (RIP)
#define MSR_CSTAR      0xC0000083   // 32-bit compat mode (kullanilmiyor)
#define MSR_FMASK      0xC0000084   // RFLAGS mask (entry'de sifirlanir)

// EFER bit flags
#define EFER_SCE       (1 << 0)     // System Call Extensions
#define EFER_LME       (1 << 8)     // Long Mode Enable
#define EFER_LMA       (1 << 10)    // Long Mode Active (read-only)
#define EFER_NXE       (1 << 11)    // No-Execute Enable

// RFLAGS mask: syscall entry'de IF ve DF sifirlanir
#define SYSCALL_RFLAGS_MASK  (0x200 | 0x400)   // IF | DF

// ============================================================
// GDT Segment Selectors
//
// GDT layout:
//   0x00  Null
//   0x08  Kernel Code  (Ring 0, 64-bit, DPL=0)
//   0x10  Kernel Data  (Ring 0, DPL=0)
//   0x18  User Data    (Ring 3, DPL=3)              <- SYSRET SS = 0x1B
//   0x20  User Code    (Ring 3, 64-bit, DPL=3)      <- SYSRET CS = 0x23
//   0x28  TSS Low
//   0x30  TSS High
//
// SYSRET: CS = STAR[63:48]+16|3 = 0x23
//         SS = STAR[63:48]+8 |3 = 0x1B
// ============================================================
#define KERNEL_CS       0x08
#define KERNEL_SS       0x10
#define USER_CS_BASE    0x10    // STAR[63:48] – SYSRET hesabi icin

#define STAR_VALUE  (((uint64_t)USER_CS_BASE << 48) | ((uint64_t)KERNEL_CS << 32))

// ============================================================
// Syscall Numaralari
//
// Cagri kurali (Linux x86-64 uyumlu):
//   RAX = syscall no,  RDI = arg1,  RSI = arg2,  RDX = arg3
//   R10 = arg4,        R8  = arg5,  R9  = arg6
//   Donus: RAX = sonuc (negatif = hata kodu)
// ============================================================
#define SYS_WRITE        1   // write(fd, buf, len)         -> bytes_written | err
#define SYS_READ         2   // read(fd, buf, len)           -> bytes_read    | err
#define SYS_EXIT         3   // exit(code)                   -> noreturn
#define SYS_GETPID       4   // getpid()                     -> pid
#define SYS_YIELD        5   // yield()                      -> 0
#define SYS_SLEEP        6   // sleep(ticks)                 -> 0
#define SYS_UPTIME       7   // uptime()                     -> system_ticks
#define SYS_DEBUG        8   // debug(msg)                   -> 0
#define SYS_OPEN         9   // open(path, flags)            -> fd | err
#define SYS_CLOSE        10  // close(fd)                    -> 0  | err
#define SYS_GETPPID      11  // getppid()                    -> parent_pid
#define SYS_SBRK         12  // sbrk(increment)              -> old_brk | err
#define SYS_GETPRIORITY  13  // getpriority()                -> priority
#define SYS_SETPRIORITY  14  // setpriority(prio)            -> 0 | err
#define SYS_GETTICKS     15  // getticks() – uptime alias    -> ticks
#define SYSCALL_MAX      16

// ============================================================
// open() flags  (SYS_OPEN)
// ============================================================
#define O_RDONLY    0x0000
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_CREAT     0x0040
#define O_TRUNC     0x0200
#define O_APPEND    0x0400

// Standart fd sabitler
#define STDIN_FD    0
#define STDOUT_FD   1
#define STDERR_FD   2

// ============================================================
// Hata Kodlari
// Negatif uint64 olarak doner – kullanici tarafinda (int64_t) cast edilmeli.
// ============================================================
#define SYSCALL_OK          ((uint64_t)0)
#define SYSCALL_ERR_INVAL   ((uint64_t)-1)   // -EINVAL  : gecersiz arguman
#define SYSCALL_ERR_NOSYS   ((uint64_t)-2)   // -ENOSYS  : implemente edilmedi
#define SYSCALL_ERR_PERM    ((uint64_t)-3)   // -EPERM   : yetki yok
#define SYSCALL_ERR_NOENT   ((uint64_t)-4)   // -ENOENT  : dosya bulunamadi
#define SYSCALL_ERR_BADF    ((uint64_t)-5)   // -EBADF   : gecersiz fd
#define SYSCALL_ERR_NOMEM   ((uint64_t)-6)   // -ENOMEM  : bellek yok
#define SYSCALL_ERR_BUSY    ((uint64_t)-7)   // -EBUSY   : kaynak mesgul
#define SYSCALL_ERR_MFILE   ((uint64_t)-8)   // -EMFILE  : fd tablosu dolu
#define SYSCALL_ERR_AGAIN   ((uint64_t)-9)   // -EAGAIN  : tekrar dene

// ============================================================
// Per-task File Descriptor Tablosu
//
// Her task, task_t icinde fd_entry_t fd_table[MAX_FDS] tutar.
// 0=stdin, 1=stdout, 2=stderr otomatik olarak serial porta
// baglanir; 3..MAX_FDS-1 kullanici tarafindan open() ile acilir.
// ============================================================
#define MAX_FDS          16

#define FD_TYPE_NONE     0   // kapali / kullanilmiyor
#define FD_TYPE_SERIAL   1   // seri port (stdin/stdout/stderr)
#define FD_TYPE_FILE     2   // dosya (ileride block-device/VFS ile)
#define FD_TYPE_PIPE     3   // pipe (ileride)
#define FD_TYPE_SPECIAL  4   // /dev/* gibi ozel aygitlar

typedef struct {
    uint8_t  type;          // FD_TYPE_*
    uint8_t  flags;         // O_RDONLY, O_WRONLY vs.
    uint8_t  is_open;       // 1 = acik
    uint8_t  _pad;
    uint64_t offset;        // dosya okuma/yazma ofseti
    char     path[60];      // acik dosyanin yolu (debug / gelecek VFS)
} fd_entry_t;

// ============================================================
// Syscall Frame
// Assembly stub (syscall_entry) tarafindan yigin uzerinde olusturulur.
// Offset'ler interrupts64.asm ile eslesmeli olmak zorundadir.
// ============================================================
typedef struct {
    uint64_t rax;   // syscall number (giris) / donus degeri (cikis)  +0
    uint64_t rdi;   // arg1                                            +8
    uint64_t rsi;   // arg2                                            +16
    uint64_t rdx;   // arg3                                            +24
    uint64_t r10;   // arg4  (SYSCALL RCX clobber ettigi icin R10)    +32
    uint64_t r8;    // arg5                                            +40
    uint64_t r9;    // arg6                                            +48
    uint64_t rcx;   // SYSCALL'in kaydettigi RIP (return address)     +56
    uint64_t r11;   // SYSCALL'in kaydettigi RFLAGS                   +64
} syscall_frame_t;

// ============================================================
// Public API
// ============================================================

// MSR'lari ayarla, SYSCALL altyapisini hazirla.
// Cagri sirasi: gdt_install_user_segments() -> tss_init() -> syscall_init()
void syscall_init(void);

// SYSCALL aktif mi?
int  syscall_is_enabled(void);

// Dispatcher – assembly stub tarafindan cagirilir
void syscall_dispatch(syscall_frame_t* frame);

// Basit test rutini (kernel modunda SYSCALL tetikler)
void syscall_test(void);

// ── fd tablosu yardimci fonksiyonlari ────────────────────────
// fd_table: task_t icinde fd_entry_t[MAX_FDS] dizisinin baslangiç adresi
void        fd_table_init(fd_entry_t* table);
int         fd_alloc(fd_entry_t* table, uint8_t type, uint8_t flags,
                     const char* path);          // bos slot atar, fd doner
int         fd_free(fd_entry_t* table, int fd);  // slotu kapatiр
fd_entry_t* fd_get(fd_entry_t* table, int fd);   // gecerli entry | NULL

// ============================================================
// Low-level MSR Helpers
// ============================================================
static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t value) {
    uint32_t lo = (uint32_t)(value & 0xFFFFFFFF);
    uint32_t hi = (uint32_t)(value >> 32);
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

#endif // SYSCALL_H