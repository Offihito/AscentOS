// syscall.h - SYSCALL/SYSRET Infrastructure for AscentOS 64-bit
// Intel/AMD x86-64 SYSCALL instruction via MSR configuration
//
// v3 Yeni Eklemeler:
//   SYS_MMAP    (16) – bellek haritalama (anonim, MAP_ANON)
//   SYS_MUNMAP  (17) – haritalanmış bölgeyi serbest bırak
//   SYS_BRK     (18) – program break'i set et (sbrk'nin modern kardeşi)
//   SYS_FORK    (19) – mevcut task'ı kopyala (copy-on-write yok; tam kopya)
//   SYS_EXECVE  (20) – yeni program yükle ve çalıştır (stub / gelecek VFS)
//   SYS_WAITPID (21) – çocuk işlem bitmesini bekle
//   SYS_PIPE    (22) – tek yönlü pipe oluştur, fd[0]=okuma fd[1]=yazma
//   SYS_DUP2    (23) – eski fd'yi newfd üzerine kopyala (atomik)

#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>

// ============================================================
// MSR Adresleri
// ============================================================
#define MSR_EFER       0xC0000080   // Extended Feature Enable Register
#define MSR_STAR       0xC0000081   // Segment selectors for SYSCALL/SYSRET
#define MSR_LSTAR      0xC0000082   // 64-bit SYSCALL entry point (RIP)
#define MSR_CSTAR      0xC0000083   // 32-bit compat mode (kullanılmıyor)
#define MSR_FMASK      0xC0000084   // RFLAGS mask (entry'de sıfırlanır)

// EFER bit flags
#define EFER_SCE       (1 << 0)     // System Call Extensions
#define EFER_LME       (1 << 8)     // Long Mode Enable
#define EFER_LMA       (1 << 10)    // Long Mode Active (read-only)
#define EFER_NXE       (1 << 11)    // No-Execute Enable

// RFLAGS mask: syscall entry'de IF ve DF sıfırlanır
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
// ============================================================
#define KERNEL_CS       0x08
#define KERNEL_SS       0x10
#define USER_CS_BASE    0x10    // STAR[63:48] – SYSRET hesabı için

#define STAR_VALUE  (((uint64_t)USER_CS_BASE << 48) | ((uint64_t)KERNEL_CS << 32))

// ============================================================
// Syscall Numaraları
//
// Çağrı kuralı (Linux x86-64 uyumlu):
//   RAX = syscall no,  RDI = arg1,  RSI = arg2,  RDX = arg3
//   R10 = arg4,        R8  = arg5,  R9  = arg6
//   Dönüş: RAX = sonuç (negatif = hata kodu)
// ============================================================

// ── Mevcut syscall'lar (v1/v2) ───────────────────────────────
#define SYS_WRITE        1   // write(fd, buf, len)           -> bytes_written | err
#define SYS_READ         2   // read(fd, buf, len)             -> bytes_read    | err
#define SYS_EXIT         3   // exit(code)                     -> noreturn
#define SYS_GETPID       4   // getpid()                       -> pid
#define SYS_YIELD        5   // yield()                        -> 0
#define SYS_SLEEP        6   // sleep(ticks)                   -> 0
#define SYS_UPTIME       7   // uptime()                       -> system_ticks
#define SYS_DEBUG        8   // debug(msg)                     -> 0
#define SYS_OPEN         9   // open(path, flags)              -> fd | err
#define SYS_CLOSE        10  // close(fd)                      -> 0  | err
#define SYS_GETPPID      11  // getppid()                      -> parent_pid
#define SYS_SBRK         12  // sbrk(increment)                -> old_brk | err
#define SYS_GETPRIORITY  13  // getpriority()                  -> priority
#define SYS_SETPRIORITY  14  // setpriority(prio)              -> 0 | err
#define SYS_GETTICKS     15  // getticks()                     -> ticks

// ── Yeni syscall'lar (v3) ─────────────────────────────────────
#define SYS_MMAP         16  // mmap(addr,len,prot,flags,fd,off)-> mapped_addr | err
#define SYS_MUNMAP       17  // munmap(addr, len)              -> 0 | err
#define SYS_BRK          18  // brk(addr)                      -> new_brk | err
#define SYS_FORK         19  // fork()                         -> child_pid | 0 | err
#define SYS_EXECVE       20  // execve(path, argv, envp)       -> err (başarıda dönmez)
#define SYS_WAITPID      21  // waitpid(pid, *status, options) -> waited_pid | err
#define SYS_PIPE         22  // pipe(fd[2])                    -> 0 | err
#define SYS_DUP2         23  // dup2(oldfd, newfd)             -> newfd | err

#define SYSCALL_MAX      24

// ============================================================
// mmap() prot flags  (SYS_MMAP)
// ============================================================
#define PROT_NONE    0x00   // Erişim yok
#define PROT_READ    0x01   // Okuma
#define PROT_WRITE   0x02   // Yazma
#define PROT_EXEC    0x04   // Çalıştırma

// ============================================================
// mmap() map flags  (SYS_MMAP)
//
// Şu an yalnızca MAP_ANONYMOUS desteklenir; MAP_FILE, MAP_SHARED
// gelecek VFS entegrasyonu için ayrılmıştır.
// ============================================================
#define MAP_SHARED       0x01   // Paylaşımlı haritalama (ileride)
#define MAP_PRIVATE      0x02   // Özel kopya (ileride copy-on-write)
#define MAP_ANONYMOUS    0x20   // Dosya değil; bellekten tahsis et
#define MAP_ANON         MAP_ANONYMOUS
#define MAP_FIXED        0x10   // addr tam bu adrese haritalansın
#define MAP_FAILED       ((void*)(uint64_t)-1)   // hata dönüş değeri

// ============================================================
// waitpid() options  (SYS_WAITPID)
// ============================================================
#define WNOHANG          0x01   // Bloklanmadan döner; PID bitmemişse 0
#define WUNTRACED        0x02   // Durdurulmuş çocukları da raporla (stub)

// waitpid status decode makroları
#define WIFEXITED(s)     (((s) & 0xFF) == 0)
#define WEXITSTATUS(s)   (((s) >> 8) & 0xFF)
#define WIFSIGNALED(s)   (((s) & 0x7F) != 0 && ((s) & 0x7F) != 0x7F)
#define WTERMSIG(s)      ((s) & 0x7F)

// ============================================================
// open() flags  (SYS_OPEN)
// ============================================================
#define O_RDONLY    0x0000
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_CREAT     0x0040
#define O_TRUNC     0x0200
#define O_APPEND    0x0400

// Standart fd sabitleri
#define STDIN_FD    0
#define STDOUT_FD   1
#define STDERR_FD   2

// ============================================================
// Hata Kodları
// Negatif uint64 olarak döner; kullanıcı tarafında (int64_t) cast edilmeli.
// ============================================================
#define SYSCALL_OK          ((uint64_t)0)
#define SYSCALL_ERR_INVAL   ((uint64_t)-1)   // -EINVAL  : geçersiz argüman
#define SYSCALL_ERR_NOSYS   ((uint64_t)-2)   // -ENOSYS  : implemente edilmedi
#define SYSCALL_ERR_PERM    ((uint64_t)-3)   // -EPERM   : yetki yok
#define SYSCALL_ERR_NOENT   ((uint64_t)-4)   // -ENOENT  : dosya bulunamadı
#define SYSCALL_ERR_BADF    ((uint64_t)-5)   // -EBADF   : geçersiz fd
#define SYSCALL_ERR_NOMEM   ((uint64_t)-6)   // -ENOMEM  : bellek yok
#define SYSCALL_ERR_BUSY    ((uint64_t)-7)   // -EBUSY   : kaynak meşgul
#define SYSCALL_ERR_MFILE   ((uint64_t)-8)   // -EMFILE  : fd tablosu dolu
#define SYSCALL_ERR_AGAIN   ((uint64_t)-9)   // -EAGAIN  : tekrar dene
#define SYSCALL_ERR_CHILD   ((uint64_t)-10)  // -ECHILD  : çocuk yok / bulunamadı
#define SYSCALL_ERR_FAULT   ((uint64_t)-11)  // -EFAULT  : geçersiz adres
#define SYSCALL_ERR_NOSPC   ((uint64_t)-12)  // -ENOSPC  : alan yok (pipe tamponu)
#define SYSCALL_ERR_RANGE   ((uint64_t)-13)  // -ERANGE  : değer aralık dışı

// ============================================================
// Per-task File Descriptor Tablosu
//
// Her task, task_t içinde fd_entry_t fd_table[MAX_FDS] tutar.
// 0=stdin, 1=stdout, 2=stderr otomatik olarak serial porta
// bağlanır; 3..MAX_FDS-1 kullanıcı tarafından açılır.
//
// v3: MAX_FDS 32'ye çıkarıldı (pipe + dup2 için ek slot gerekti).
// ============================================================
#define MAX_FDS          32

#define FD_TYPE_NONE     0   // kapalı / kullanılmıyor
#define FD_TYPE_SERIAL   1   // seri port (stdin/stdout/stderr)
#define FD_TYPE_FILE     2   // dosya (ileride block-device/VFS ile)
#define FD_TYPE_PIPE     3   // pipe ucu (pipe_buf_t'ye işaret eder)
#define FD_TYPE_SPECIAL  4   // /dev/* gibi özel aygıtlar

// ============================================================
// Pipe Tamponu
//
// Her pipe() çağrısı bir pipe_buf_t ayırır ve paylaşılan
// tampon adresini hem okuma hem yazma fd'sine yazar.
// Referans sayacı 0'a düştüğünde tampon serbest bırakılır.
// ============================================================
#define PIPE_BUF_SIZE    4096

typedef struct pipe_buf {
    uint8_t  data[PIPE_BUF_SIZE];
    uint32_t read_pos;       // Bir sonraki okunacak byte
    uint32_t write_pos;      // Bir sonraki yazılacak konum
    uint32_t bytes_avail;    // Tamponda bekleyen byte sayısı
    uint32_t ref_count;      // Kaç fd bu tampona bağlı (max 2)
    uint8_t  write_closed;   // Yazma ucu kapalıysa 1 (EOF sinyali)
    uint8_t  read_closed;    // Okuma ucu kapalıysa 1
    uint8_t  _pad[2];
} pipe_buf_t;

typedef struct {
    uint8_t     type;        // FD_TYPE_*
    uint8_t     flags;       // O_RDONLY, O_WRONLY vb.
    uint8_t     is_open;     // 1 = açık
    uint8_t     _pad;
    uint64_t    offset;      // dosya okuma/yazma ofseti
    char        path[52];    // açık dosyanın yolu (debug / gelecek VFS)
    pipe_buf_t* pipe;        // FD_TYPE_PIPE ise tampon; diğer türler için NULL
} fd_entry_t;

// ============================================================
// Syscall Frame
// Assembly stub (syscall_entry) tarafından yığın üzerinde oluşturulur.
// Offsetler interrupts64.asm ile eşleşmeli.
// ============================================================
typedef struct {
    uint64_t rax;   // syscall number (giriş) / dönüş değeri (çıkış)  +0
    uint64_t rdi;   // arg1                                            +8
    uint64_t rsi;   // arg2                                            +16
    uint64_t rdx;   // arg3                                            +24
    uint64_t r10;   // arg4  (SYSCALL RCX clobber ettiği için R10)    +32
    uint64_t r8;    // arg5                                            +40
    uint64_t r9;    // arg6                                            +48
    uint64_t rcx;   // SYSCALL'in kaydettiği RIP (return address)     +56
    uint64_t r11;   // SYSCALL'in kaydettiği RFLAGS                   +64
} syscall_frame_t;

// ============================================================
// Public API
// ============================================================

// MSR'ları ayarla, SYSCALL altyapısını hazırla.
void syscall_init(void);

// SYSCALL aktif mi?
int  syscall_is_enabled(void);

// Dispatcher – assembly stub tarafından çağrılır
void syscall_dispatch(syscall_frame_t* frame);

// Basit test rutini
void syscall_test(void);

// ── fd tablosu yardımcı fonksiyonları ──────────────────────────
void        fd_table_init(fd_entry_t* table);
int         fd_alloc(fd_entry_t* table, uint8_t type, uint8_t flags,
                     const char* path);
int         fd_alloc_pipe(fd_entry_t* table, uint8_t rw_flags,
                          pipe_buf_t* pbuf);   // pipe için özel ayırıcı
int         fd_free(fd_entry_t* table, int fd);
fd_entry_t* fd_get(fd_entry_t* table, int fd);

// ── pipe tampon ayırma/serbest bırakma ──────────────────────────
pipe_buf_t* pipe_buf_alloc(void);
void        pipe_buf_release(pipe_buf_t* pb);  // ref_count--; 0'da kfree

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