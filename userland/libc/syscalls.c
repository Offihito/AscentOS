// ═══════════════════════════════════════════════════════════════════════════
//  AscentOS — Userland Syscall Stubs
//  Dosya: userland/libc/syscalls.c
//
//  Bu dosya kernel'daki syscall.c dispatch tablosuyla TAMAMEN senkronize
//  olacak şekilde güncellenmiştir (v1 → v15 tüm syscall'lar dahil).
//
//  newlib'in reentrant wrapper'ları (örn. _write_r, _sbrk_r) bu
//  underscore'suz isimlere (write, sbrk, ...) bağlanır.
//
//  ÇAĞRI KURALI (Linux x86-64 uyumlu):
//    RAX = syscall no
//    RDI = arg1, RSI = arg2, RDX = arg3
//    R10 = arg4, R8  = arg5, R9  = arg6
//    Dönüş: RAX (negatif = hata)
// ═══════════════════════════════════════════════════════════════════════════

// ── Minimum tip tanımları (host header yok) ──────────────────────────────
typedef long           ssize_t;
typedef unsigned long  size_t;
typedef int            pid_t;
typedef unsigned int   mode_t;
typedef long           off_t;
typedef long           ptrdiff_t;

// ── AscentOS Syscall Numaraları (syscall.h ile birebir aynı) ─────────────
// v1-v2: Temel I/O ve süreç
#define SYS_WRITE         1
#define SYS_READ          0
#define SYS_EXIT         60
#define SYS_GETPID       39
#define SYS_YIELD        24
#define SYS_SLEEP       406
#define SYS_UPTIME       99
#define SYS_DEBUG       103
#define SYS_OPEN          2
#define SYS_CLOSE         3
#define SYS_GETPPID     110
#define SYS_SBRK        405
#define SYS_GETPRIORITY 140
#define SYS_SETPRIORITY 141
#define SYS_GETTICKS    404
// v3: Bellek ve süreç
#define SYS_MMAP          9
#define SYS_MUNMAP       11
#define SYS_BRK          12
#define SYS_FORK         57
#define SYS_EXECVE       59
#define SYS_WAITPID      61
#define SYS_PIPE         22
#define SYS_DUP2         33
// v4: Dosya I/O
#define SYS_LSEEK         8
#define SYS_FSTAT         5
#define SYS_IOCTL        16
// v5: Çoğullama
#define SYS_SELECT       23
#define SYS_POLL          7
// v6: Newlib uyumu
#define SYS_KILL         62
#define SYS_GETTIMEOFDAY 96
// v8: Dosya sorgulama
#define SYS_STAT          4
#define SYS_ACCESS       21
// v7: Dizin
#define SYS_GETCWD       79
#define SYS_CHDIR        80
// v9: Dizin okuma
#define SYS_GETDENTS     78
#define SYS_OPENDIR     402
#define SYS_CLOSEDIR    403
// v10: Sinyal
#define SYS_SIGACTION    13
#define SYS_SIGPROCMASK  14
#define SYS_SIGRETURN    15
#define SYS_SIGPENDING  127
#define SYS_SIGSUSPEND  130
// v11: fd yönetimi
#define SYS_FCNTL        72
#define SYS_DUP          32
// v12: Process group & session (bash iş kontrolü)
#define SYS_SETPGID     109
#define SYS_GETPGID     121
#define SYS_SETSID      112
#define SYS_TCSETPGRP   400
#define SYS_TCGETPGRP   401
// v13: Sistem bilgisi
#define SYS_UNAME        63
// v14: Dosya sistemi yazma (bash mkdir/rm/mv)
#define SYS_MKDIR        83
#define SYS_RMDIR        84
#define SYS_UNLINK       87
#define SYS_RENAME       82
// v15: Kullanıcı kimliği / zamanlama / sinyal yığını
#define SYS_GETUID      102
#define SYS_GETEUID     107
#define SYS_GETGID      104
#define SYS_GETEGID     108
#define SYS_NANOSLEEP    35
#define SYS_SIGALTSTACK 131
// v16: clock, alarm, truncate, rlimit, lstat, link, umask, symlink, readlink, chmod, mprotect, pipe2, times, getgroups
#define SYS_CLOCK_GETTIME 228
#define SYS_CLOCK_GETRES  229
#define SYS_ALARM        37
#define SYS_FTRUNCATE    77
#define SYS_TRUNCATE     76
#define SYS_GETRLIMIT    97
#define SYS_SETRLIMIT   160
#define SYS_LSTAT         6
#define SYS_LINK         86
#define SYS_TIMES       100
#define SYS_UMASK        95
#define SYS_SYMLINK      88
#define SYS_READLINK     89
#define SYS_CHMOD        90
#define SYS_MPROTECT     10
#define SYS_PIPE2       293
#define SYS_GETGROUPS    106
// v27: musl libc başlatma ve gelişmiş syscall'lar
#define SYS_FUTEX         202
#define SYS_GETRANDOM     318
#define SYS_ARCH_PRCTL    158
#define SYS_CLONE          56
#define SYS_SET_TID_ADDRESS 218
#define SYS_SET_ROBUST_LIST 273
#define SYS_WRITEV         20
#define SYS_MADVISE        28
#define SYS_EXIT_GROUP    231
#define SYS_OPENAT        257
#define SYS_NEWFSTATAT    262
#define SYS_PRLIMIT64     302

// ── open() flags ─────────────────────────────────────────────────────────
#define O_RDONLY    0x00
#define O_WRONLY    0x01
#define O_RDWR      0x02
#define O_CREAT     0x40
#define O_TRUNC     0x200
#define O_APPEND    0x400
#define O_NONBLOCK  0x800
#define O_CLOEXEC   0x80000

// ── errno storage ────────────────────────────────────────────────────────
// NEDEN weak DEĞİL:
//
// musl'ün __errno_location() %fs:pthread_self()->errno_val kullanır.
// Context switch assembly'si "mov fs, 0" yaptığında Intel/AMD MSR_FS_BASE'i
// sıfırlar. Sonra SET_ERRNO_RET → musl __errno_location() → %fs:0 →
// adres 0 okur → BIOS IVT verisi (0xF000FF53...) → non-canonical pointer
// → *errno_ptr = x → #GP. Bu kernel panığin tam sebebidir.
//
// Çözüm: syscalls.o objesi musl.a'dan önce link edildiğinde
// (Makefile'daki standart sıra budur) strong sembolümüz musl'ünkünü gölgeler.
// musl.a içindeki __errno_location linker tarafından zaten çözülmüş sembol
// olarak atlanır — çakışma hatası vermez.
// Böylece SET_ERRNO_RET her zaman güvenli static _errno_storage'a yazar.
//
// DİKKAT: musl.a, syscalls.o'dan ÖNCE link ediliyorsa "-z muldefs" ekle.
static int _errno_storage = 0;

int *__errno_location(void) { return &_errno_storage; }

// errno'yu oku (musl errno.h makrosuyla uyumlu)
static inline int _get_errno(void) { return _errno_storage; }

// ── Linux uyumlu errno set makrosu ───────────────────────────────────────
#define SET_ERRNO(ret)       do { if ((ret) < 0) *__errno_location() = (int)(-(ret)); } while(0)
#define SET_ERRNO_RET(ret, errval) \
    do { if ((ret) < 0) { *__errno_location() = (int)(-(ret)); return (errval); } } while(0)

// ── access() modları ─────────────────────────────────────────────────────
#define F_OK   0
#define R_OK   4
#define W_OK   2
#define X_OK   1

// ── lseek() whence ───────────────────────────────────────────────────────
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

// ── waitpid() seçenekleri ────────────────────────────────────────────────
#define WNOHANG   0x01
#define WUNTRACED 0x02

// waitpid() status decode makroları
#define WIFEXITED(s)    (((s) & 0xFF) == 0)
#define WEXITSTATUS(s)  (((s) >> 8) & 0xFF)
#define WIFSIGNALED(s)  (((s) & 0x7F) != 0 && ((s) & 0x7F) != 0x7F)
#define WTERMSIG(s)     ((s) & 0x7F)
#define WIFSTOPPED(s)   (((s) & 0xFF) == 0x7F)
#define WSTOPSIG(s)     (((s) >> 8) & 0xFF)

// ── Sinyal numaraları ────────────────────────────────────────────────────
#define SIGHUP    1
#define SIGINT    2
#define SIGQUIT   3
#define SIGILL    4
#define SIGABRT   6
#define SIGFPE    8
#define SIGKILL   9
#define SIGSEGV  11
#define SIGPIPE  13
#define SIGALRM  14
#define SIGTERM  15
#define SIGCHLD  17
#define SIGCONT  18
#define SIGSTOP  19
#define SIGTSTP  20
#define SIGTTIN  21
#define SIGTTOU  22
#define SIGWINCH 28

// ── sigaction flags ──────────────────────────────────────────────────────
#define SA_RESTART    0x10000000
#define SA_NOCLDSTOP  0x00000001
#define SA_SIGINFO    0x00000004
#define SA_RESETHAND  0x80000000
#define SA_NODEFER    0x40000000

// ── sigprocmask how ──────────────────────────────────────────────────────
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

// ── fcntl komutları ──────────────────────────────────────────────────────
#define F_DUPFD   0
#define F_GETFD   1
#define F_SETFD   2
#define F_GETFL   3
#define F_SETFL   4
#define FD_CLOEXEC 1

// ── MAP flags ────────────────────────────────────────────────────────────
#define PROT_NONE  0x00
#define PROT_READ  0x01
#define PROT_WRITE 0x02
#define PROT_EXEC  0x04
#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20
#define MAP_ANON      MAP_ANONYMOUS
#define MAP_FIXED     0x10
#define MAP_FAILED    ((void*)(long)-1)

// ── Temel tipler (newlib yokken) ─────────────────────────────────────────
typedef unsigned int  uid_t;
typedef unsigned int  gid_t;
typedef unsigned long dev_t;
typedef unsigned long ino_t;
typedef unsigned int  nlink_t;

// ── stat yapısı ──────────────────────────────────────────────────────────
struct stat {
    dev_t  st_dev;
    ino_t  st_ino;
    mode_t st_mode;
    nlink_t st_nlink;
    uid_t  st_uid;
    gid_t  st_gid;
    dev_t  st_rdev;
    off_t  st_size;
    long   st_blksize;
    long   st_blocks;
    long   st_atime;
    long   st_mtime;
    long   st_ctime;
};

// stat mode bitleri (bash dosya tipi kontrolü için)
#define S_IFMT   0xF000
#define S_IFSOCK 0xC000
#define S_IFLNK  0xA000
#define S_IFREG  0x8000
#define S_IFBLK  0x6000
#define S_IFDIR  0x4000
#define S_IFCHR  0x2000
#define S_IFIFO  0x1000
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)

// ── sigset_t ─────────────────────────────────────────────────────────────
typedef unsigned int sigset_t;

// ── sigaction yapısı ─────────────────────────────────────────────────────
typedef void (*sighandler_t)(int);
#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)
#define SIG_ERR ((sighandler_t)-1)

struct sigaction {
    sighandler_t sa_handler;
    sigset_t     sa_mask;
    unsigned int sa_flags;
    unsigned long sa_restorer;
};

// ── timeval ──────────────────────────────────────────────────────────────
struct timeval {
    long tv_sec;
    long tv_usec;
};

// ── timespec (nanosleep için) ─────────────────────────────────────────────
struct timespec {
    long tv_sec;
    long tv_nsec;
};

// ── utsname (uname için) ──────────────────────────────────────────────────
#define UTS_LEN 65
struct utsname {
    char sysname[UTS_LEN];
    char nodename[UTS_LEN];
    char release[UTS_LEN];
    char version[UTS_LEN];
    char machine[UTS_LEN];
};

// ── sigaltstack (sigaltstack için) ────────────────────────────────────────
#define SS_ONSTACK  1
#define SS_DISABLE  2
#define MINSIGSTKSZ 2048

typedef struct {
    void  *ss_sp;
    int    ss_flags;
    size_t ss_size;
} stack_t;

// ── termios (bash için gerekli - ioctl TCGETS/TCSETS) ────────────────────
#define NCCS 19
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

// c_lflag bitleri
#define ISIG    0x0001
#define ICANON  0x0002
#define ECHO    0x0008
#define ECHOE   0x0010
#define ECHOK   0x0020
#define ECHONL  0x0040
#define NOFLSH  0x0080
#define TOSTOP  0x0100

// c_iflag bitleri
#define BRKINT  0x0002
#define ICRNL   0x0100
#define INPCK   0x0010
#define ISTRIP  0x0020
#define IXON    0x0400

// c_oflag bitleri
#define OPOST   0x0001

// c_cc indeksleri
#define VEOF   4
#define VEOL   5
#define VERASE 2
#define VINTR  0
#define VKILL  3
#define VMIN   6
#define VQUIT  1
#define VSUSP  10
#define VTIME  7

// ioctl istekleri
#define TCGETS      0x5401
#define TCSETS      0x5402
#define TCSETSW     0x5403
#define TCSETSF     0x5404
#define TIOCGWINSZ  0x5413
#define TIOCSWINSZ  0x5414
#define TIOCGPGRP   0x540F
#define TIOCSPGRP   0x5410

// winsize yapısı (TIOCGWINSZ için)
struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

// ── dirent (getdents için) ────────────────────────────────────────────────
struct dirent {
    unsigned long  d_ino;
    unsigned long  d_off;
    unsigned short d_reclen;
    unsigned char  d_type;
    char           d_name[256];
};

#define DT_UNKNOWN 0
#define DT_REG     8
#define DT_DIR     4

// ═══════════════════════════════════════════════════════════════════════════
//  RAW SYSCALL HELPER'LAR
// ═══════════════════════════════════════════════════════════════════════════

static inline long _sc0(long nr) {
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(nr)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline long _sc1(long nr, long a1) {
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(nr), "D"(a1)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline long _sc2(long nr, long a1, long a2) {
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(nr), "D"(a1), "S"(a2)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline long _sc3(long nr, long a1, long a2, long a3) {
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(nr), "D"(a1), "S"(a2), "d"(a3)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline long _sc4(long nr, long a1, long a2, long a3, long a4) {
    long ret;
    register long _r10 __asm__("r10") = a4;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(_r10)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline long _sc6(long nr, long a1, long a2, long a3,
                         long a4, long a5, long a6) {
    long ret;
    register long _r10 __asm__("r10") = a4;
    register long _r8  __asm__("r8")  = a5;
    register long _r9  __asm__("r9")  = a6;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(nr), "D"(a1), "S"(a2), "d"(a3),
          "r"(_r10), "r"(_r8), "r"(_r9)
        : "rcx", "r11", "memory"
    );
    return ret;
}

// ═══════════════════════════════════════════════════════════════════════════
//  BÖLÜM 1: TEMEL I/O
// ═══════════════════════════════════════════════════════════════════════════

ssize_t write(int fd, const void *buf, size_t len) {
    long ret = _sc3(SYS_WRITE, (long)fd, (long)buf, (long)len);
    if (ret < 0) { *__errno_location() = (int)(-ret); return -1; }
    return (ssize_t)ret;
}

ssize_t read(int fd, void *buf, size_t len) {
    long ret = _sc3(SYS_READ, (long)fd, (long)buf, (long)len);
    SET_ERRNO_RET(ret, -1);
    return (ssize_t)ret;
}

// ═══════════════════════════════════════════════════════════════════════════
//  BÖLÜM 2: DOSYA İŞLEMLERİ
// ═══════════════════════════════════════════════════════════════════════════

int open(const char *path, int flags, ...) {
    // Bilinmeyen flag bitlerini maskele (newlib O_CLOEXEC=0x80000 vb. ekleyebilir)
    int clean_flags = flags & (O_RDONLY | O_WRONLY | O_RDWR |
                                O_CREAT  | O_TRUNC  | O_APPEND |
                                O_NONBLOCK | O_CLOEXEC);
    unsigned int mode = 0644;
    long ret = _sc3(SYS_OPEN, (long)path, (long)clean_flags, (long)mode);
    if (ret < 0) {
        // Kernel artık Linux değerleri döndürüyor: -ret == Linux errno
        *__errno_location() = (int)(-ret);
        return -1;
    }
    return (int)ret;
}

int close(int fd) {
    long ret = _sc1(SYS_CLOSE, (long)fd);
    SET_ERRNO_RET(ret, -1);
    return (int)ret;
}

off_t lseek(int fd, off_t offset, int whence) {
    long ret = _sc3(SYS_LSEEK, (long)fd, (long)offset, (long)whence);
    SET_ERRNO_RET(ret, (off_t)-1);
    return (off_t)ret;
}

int stat(const char *path, struct stat *buf) {
    long ret = _sc2(SYS_STAT, (long)path, (long)buf);
    SET_ERRNO_RET(ret, -1);
    return (int)ret;
}

int fstat(int fd, struct stat *buf) {
    long ret = _sc2(SYS_FSTAT, (long)fd, (long)buf);
    SET_ERRNO_RET(ret, -1);
    return (int)ret;
}

int lstat(const char *path, struct stat *buf) {
    long ret = _sc2(SYS_LSTAT, (long)path, (long)buf);
    SET_ERRNO_RET(ret, -1);
    return (int)ret;
}

int access(const char *path, int mode) {
    long ret = _sc2(SYS_ACCESS, (long)path, (long)mode);
    SET_ERRNO_RET(ret, -1);
    return (int)ret;
}

int ioctl(int fd, unsigned long req, void *arg) {
    long ret = _sc3(SYS_IOCTL, (long)fd, (long)req, (long)arg);
    SET_ERRNO_RET(ret, -1);
    return (int)ret;
}

int fcntl(int fd, int cmd, long arg) {
    long ret = _sc3(SYS_FCNTL, (long)fd, (long)cmd, (long)arg);
    SET_ERRNO_RET(ret, -1);
    return (int)ret;
}

int dup(int fd) {
    long ret = _sc1(SYS_DUP, (long)fd);
    SET_ERRNO_RET(ret, -1);
    return (int)ret;
}

int dup2(int oldfd, int newfd) {
    long ret = _sc2(SYS_DUP2, (long)oldfd, (long)newfd);
    SET_ERRNO_RET(ret, -1);
    return (int)ret;
}

int pipe(int fd[2]) {
    long ret = _sc1(SYS_PIPE, (long)fd);
    SET_ERRNO_RET(ret, -1);
    return (int)ret;
}

// pipe2: O_CLOEXEC bayrağını atomik destekler
int pipe2(int fd[2], int flags) {
    long ret = _sc2(SYS_PIPE2, (long)fd, (long)flags);
    SET_ERRNO_RET(ret, -1);
    return (int)ret;
}

// ═══════════════════════════════════════════════════════════════════════════
//  BÖLÜM 3: DİZİN İŞLEMLERİ
// ═══════════════════════════════════════════════════════════════════════════

char *getcwd(char *buf, size_t size) {
    long r = _sc2(SYS_GETCWD, (long)buf, (long)size);
    return (r == 0) ? buf : (char *)0;
}

int chdir(const char *path) {
    long ret = _sc1(SYS_CHDIR, (long)path);
    SET_ERRNO_RET(ret, -1);
    return (int)ret;
}

int mkdir(const char *path, mode_t mode) {
    long ret = _sc2(SYS_MKDIR, (long)path, (long)mode);
    SET_ERRNO_RET(ret, -1);
    return (int)ret;
}

int rmdir(const char *path) {
    long ret = _sc1(SYS_RMDIR, (long)path);
    SET_ERRNO_RET(ret, -1);
    return (int)ret;
}

__attribute__((noinline))
int unlink(const char *path) {
    volatile char _stack_guard[5120];
    _stack_guard[0] = 0; _stack_guard[5119] = 0;
    (void)_stack_guard;
    long ret;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"((long)87), "D"((long)path)
        : "rcx", "r11", "memory"
    );
    if (ret < 0) { *__errno_location() = (int)(-ret); return -1; }
    return (int)ret;
}

__attribute__((noinline))
int rename(const char *old, const char *newpath) {
    // Büyük stack guard: kernel sys_rename ~4KB stack kullanıyor.
    // 5KB buffer ile wrapper'ın return adresi overflow bölgesinin dışında kalır.
    volatile char _stack_guard[5120];
    _stack_guard[0] = 0; _stack_guard[5119] = 0;  // touch pages
    (void)_stack_guard;
    long ret;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"((long)82), "D"((long)old), "S"((long)newpath)
        : "rcx", "r11", "memory"
    );
    if (ret < 0) { *__errno_location() = (int)(-ret); return -1; }
    return (int)ret;
}

// ── getdents (opendir/readdir/closedir temel altyapısı) ──────────────────

// Basit DIR handle (opaque pointer newlib'in dirent.h'ı için)
typedef struct {
    int    fd;
    int    offset;
    int    buf_count;
    struct dirent buf[64];
} DIR_IMPL;

// Basit malloc (sbrk tabanlı) — newlib bağlı değilse
extern void *sbrk(long incr);

static void *_simple_malloc(size_t n) {
    void *p = sbrk((long)n);
    return (p == (void*)-1) ? (void*)0 : p;
}

void *opendir(const char *path) {
    DIR_IMPL *d = (DIR_IMPL*)_simple_malloc(sizeof(DIR_IMPL));
    if (!d) return (void*)0;
    int fd = (int)_sc1(SYS_OPENDIR, (long)path);
    if (fd < 0) return (void*)0;
    d->fd = fd;
    d->offset = 0;
    d->buf_count = (int)_sc3(SYS_GETDENTS, (long)fd, (long)d->buf, (long)(sizeof(d->buf)));
    if (d->buf_count < 0) d->buf_count = 0;
    return (void*)d;
}

struct dirent *readdir(void *dirp) {
    DIR_IMPL *d = (DIR_IMPL*)dirp;
    if (!d) return (struct dirent*)0;
    if (d->offset >= d->buf_count) return (struct dirent*)0;
    return &d->buf[d->offset++];
}

int closedir(void *dirp) {
    DIR_IMPL *d = (DIR_IMPL*)dirp;
    if (!d) return -1;
    return (int)_sc1(SYS_CLOSEDIR, (long)d->fd);
}

// ═══════════════════════════════════════════════════════════════════════════
//  BÖLÜM 4: SÜREÇ YÖNETİMİ
// ═══════════════════════════════════════════════════════════════════════════

__attribute__((noreturn))
void _exit(int code) {
    _sc1(SYS_EXIT, (long)code);
    __builtin_unreachable();
}

// ── Minimal atexit implementasyonu ───────────────────────────────────────────
// Musl libc.a'da atexit cross-link sırasında erişilemiyor.
// Kilo gibi uygulamalar atexit(disableRawMode) çağırır — terminal
// restore edilmezse kernel_termios raw modda kalır ve shell bozulur.
#define ATEXIT_MAX 32
static void (*_atexit_funcs[ATEXIT_MAX])(void);
static int   _atexit_count = 0;

int atexit(void (*func)(void)) {
    if (_atexit_count >= ATEXIT_MAX) return -1;
    _atexit_funcs[_atexit_count++] = func;
    return 0;
}

// __cxa_atexit: C++ ve bazı musl iç çağrıları bu ismi kullanır.
// arg olarak func'ı çağırır, dtor_handle yoksayılır.
int __cxa_atexit(void (*func)(void*), void* arg, void* dso_handle) {
    (void)dso_handle;
    // Basit wrap: arg=NULL ise void(void) olarak çağır
    if (!arg) return atexit((void(*)(void))func);
    // arg varsa şimdilik yoksay (kilo kullanmıyor)
    return 0;
}

__attribute__((noreturn))
void exit(int code) {
    // atexit handler'larını ters sırayla çağır (LIFO — POSIX gereksinimi)
    for (int i = _atexit_count - 1; i >= 0; i--) {
        if (_atexit_funcs[i]) _atexit_funcs[i]();
    }
    _exit(code);
}

pid_t getpid(void) {
    return (pid_t)_sc0(SYS_GETPID);
}

pid_t getppid(void) {
    return (pid_t)_sc0(SYS_GETPPID);
}

pid_t fork(void) {
    long ret;
    __asm__ volatile (
        "mov %%rsp, %%rdi\n\t"
        "mov %1,    %%rax\n\t"
        "syscall"
        : "=a"(ret)
        : "i"((long)SYS_FORK)
        : "rcx", "r11", "rdi", "memory"
    );
    SET_ERRNO(ret);
    return (pid_t)ret;
}

int execve(const char *path, char *const argv[], char *const envp[]) {
    long ret = _sc3(SYS_EXECVE, (long)path, (long)argv, (long)envp);
    SET_ERRNO_RET(ret, -1);
    return (int)ret;
}

int execv(const char *path, char *const argv[]) {
    return execve(path, argv, (char *const[]){(char*)0});
}

int execvp(const char *file, char *const argv[]) {
    // Tam yol değilse PATH'te ara — basit implementasyon
    if (file[0] == '/') return execve(file, argv, (char *const[]){(char*)0});
    // PATH araması: /bin ve /usr/bin'i dene
    static const char *paths[] = { "/bin/", "/usr/bin/", "./", (char*)0 };
    static char buf[256];
    for (int i = 0; paths[i]; i++) {
        // buf = paths[i] + file
        int j = 0;
        const char *p = paths[i];
        while (*p && j < 200) buf[j++] = *p++;
        p = file;
        while (*p && j < 254) buf[j++] = *p++;
        buf[j] = '\0';
        if (access(buf, X_OK) == 0)
            return execve(buf, argv, (char *const[]){(char*)0});
    }
    return execve(file, argv, (char *const[]){(char*)0});
}

pid_t waitpid(pid_t pid, int *status, int opts) {
    long ret = _sc3(SYS_WAITPID, (long)pid, (long)status, (long)opts);
    SET_ERRNO(ret);
    return (pid_t)ret;
}

pid_t wait(int *status) {
    return waitpid(-1, status, 0);
}

// ── Process Group & Session ───────────────────────────────────────────────

int setpgid(pid_t pid, pid_t pgid) {
    long ret = _sc2(SYS_SETPGID, (long)pid, (long)pgid);
    SET_ERRNO_RET(ret, -1);
    return (int)ret;
}

pid_t getpgid(pid_t pid) {
    long ret = _sc1(SYS_GETPGID, (long)pid);
    SET_ERRNO(ret);
    return (pid_t)ret;
}

pid_t getpgrp(void) {
    return getpgid(0);
}

pid_t setsid(void) {
    long ret = _sc0(SYS_SETSID);
    SET_ERRNO(ret);
    return (pid_t)ret;
}

pid_t getsid(pid_t pid) {
    // AscentOS'ta getsid ayrı bir syscall yok; PGID olarak döndür
    return getpgid(pid);
}

int tcsetpgrp(int fd, pid_t pgrp) {
    long ret = _sc2(SYS_TCSETPGRP, (long)fd, (long)pgrp);
    SET_ERRNO_RET(ret, -1);
    return (int)ret;
}

pid_t tcgetpgrp(int fd) {
    long ret = _sc1(SYS_TCGETPGRP, (long)fd);
    SET_ERRNO(ret);
    return (pid_t)ret;
}

// ── kill / pause ──────────────────────────────────────────────────────────

int kill(pid_t pid, int sig) {
    long ret = _sc2(SYS_KILL, (long)pid, (long)sig);
    SET_ERRNO_RET(ret, -1);
    return (int)ret;
}

int raise(int sig) {
    return kill(getpid(), sig);
}

unsigned int alarm(unsigned int seconds) {
    return (unsigned int)_sc1(SYS_ALARM, (long)seconds);
}

int pause(void) {
    // Herhangi bir sinyal gelene kadar bekle
    // AscentOS'ta sigsuspend(0) ile uygulayabiliriz
    sigset_t empty = 0;
    return (int)_sc1(SYS_SIGSUSPEND, (long)&empty);
}

// ═══════════════════════════════════════════════════════════════════════════
//  BÖLÜM 5: SİNYAL YÖNETİMİ
// ═══════════════════════════════════════════════════════════════════════════

int sigaction(int signo, const struct sigaction *new_sa,
              struct sigaction *old_sa) {
    long ret = _sc3(SYS_SIGACTION, (long)signo, (long)new_sa, (long)old_sa);
    SET_ERRNO_RET(ret, -1);
    return (int)ret;
}

int sigprocmask(int how, const sigset_t *set, sigset_t *oldset) {
    long ret = _sc3(SYS_SIGPROCMASK, (long)how, (long)set, (long)oldset);
    SET_ERRNO_RET(ret, -1);
    return (int)ret;
}

int sigpending(sigset_t *set) {
    long ret = _sc1(SYS_SIGPENDING, (long)set);
    SET_ERRNO_RET(ret, -1);
    return (int)ret;
}

int sigsuspend(const sigset_t *mask) {
    long ret = _sc1(SYS_SIGSUSPEND, (long)mask);
    SET_ERRNO(ret);   // sigsuspend her zaman -1 döner (EINTR beklenir)
    return -1;
}

// signal() — basit sarıcı
sighandler_t signal(int signo, sighandler_t handler) {
    struct sigaction sa, old;
    sa.sa_handler = handler;
    sa.sa_mask    = 0;
    sa.sa_flags   = SA_RESTART;
    sa.sa_restorer = 0;
    if (sigaction(signo, &sa, &old) < 0) return SIG_ERR;
    return old.sa_handler;
}

// sigset operasyonları (bash bunları doğrudan çağırır)
int sigemptyset(sigset_t *set) { *set = 0; return 0; }
int sigfillset(sigset_t *set)  { *set = ~0u; return 0; }
int sigaddset(sigset_t *set, int sig) {
    if (sig < 1 || sig > 31) return -1;
    *set |= (1u << sig);
    return 0;
}
int sigdelset(sigset_t *set, int sig) {
    if (sig < 1 || sig > 31) return -1;
    *set &= ~(1u << sig);
    return 0;
}
int sigismember(const sigset_t *set, int sig) {
    if (sig < 1 || sig > 31) return -1;
    return (*set & (1u << sig)) ? 1 : 0;
}

// ═══════════════════════════════════════════════════════════════════════════
//  BÖLÜM 6: TERMINAL (termios)
// ═══════════════════════════════════════════════════════════════════════════

int tcgetattr(int fd, struct termios *t) {
    return ioctl(fd, TCGETS, (void*)t);
}

int tcsetattr(int fd, int action, const struct termios *t) {
    unsigned long req;
    if      (action == 0) req = TCSETS;   // TCSANOW
    else if (action == 1) req = TCSETSW;  // TCSADRAIN
    else                  req = TCSETSF;  // TCSAFLUSH
    return ioctl(fd, req, (void*)t);
}

int isatty(int fd) {
    struct termios t;
    return (tcgetattr(fd, &t) == 0) ? 1 : 0;
}

int ttyname_r(int fd, char *buf, size_t buflen) {
    (void)fd;
    if (!buf || buflen < 9) return -1;
    // Tek bir terminal: /dev/tty0
    const char *name = "/dev/tty0";
    size_t i = 0;
    while (name[i] && i < buflen - 1) { buf[i] = name[i]; i++; }
    buf[i] = '\0';
    return 0;
}

// ═══════════════════════════════════════════════════════════════════════════
//  BÖLÜM 7: BELLEK YÖNETİMİ
// ═══════════════════════════════════════════════════════════════════════════

// sbrk — Linux x86-64 ABI uyumlu implementasyon
// ──────────────────────────────────────────────────────────────────────────
// Linux'ta sbrk(3) bir libc sarmalayıcısıdır; altında her zaman brk(2)
// (syscall 12) çağrılır. Önceki implementasyon kernel'dan bağımsız özel
// bir _heap_ptr tutuyordu: bu nedenle brk(0) ve sbrk(0) hiçbir zaman
// aynı değeri döndürmüyordu (B4 testi NG).
//
// Yeni implementasyon:
//   sbrk(0)  → brk(0) syscall'ı ile mevcut break'i sorgula
//   sbrk(+N) → break'i N byte büyüt; ESKİ break döner (POSIX)
//   sbrk(-N) → break'i N byte küçült; ESKİ break döner
//   Hata     → (void*)-1  (malloc bunu kontrol eder)
//
// Önemli: Linux brk(2) başarısızlıkta -errno DEĞİL, mevcut break'i döner.
// Başarısızlığı "istek edilen adres != dönen adres" karşılaştırmasıyla
// tespit ediyoruz.
// ──────────────────────────────────────────────────────────────────────────
void *sbrk(long incr) {
    // Adım 1: Mevcut break'i sorgula (brk(0) asla hata vermez)
    void *cur = (void *)_sc1(SYS_BRK, 0);

    if (incr == 0)
        return cur;

    // Adım 2: Hedef adresi hesapla ve kernel'a iste
    void *target = (char *)cur + incr;
    void *result = (void *)_sc1(SYS_BRK, (long)target);

    // Adım 3: Başarı kontrolü — Linux break'i hareket ettiremediyse
    // istenen adresi değil, mevcut break'i döndürür.
    if (result != target)
        return (void *)-1;   // ENOMEM; malloc (void*)-1 ile başarısızlığı algılar

    // Başarı: ESKİ break'i döndür (POSIX/Linux sbrk semantiği)
    return cur;
}

// ── Userland mmap pool ────────────────────────────────────────────────────
// musl mallocng mmap(MAP_ANONYMOUS) ile büyük chunk'lar alır (65KB+).
// Kernel sys_mmap kmalloc tabanlı yüksek adres döndürür (~0x180000+).
// mcmodel=small ile derlenen Lua bu adresi 32-bit pointer'a sığdıramaz → #GP.
//
// Çözüm: Hem 4KB hem de büyük istekleri statik BSS pool'dan karşıla.
// Pool adresleri BSS'te olduğundan mcmodel=small aralığında (<2GB) kalır.
//
// İki pool:
//   _mmap_pool_small : 512 × 4KB  = 2MB  (musl mutex/errno küçük alloc'lar)
//   _mmap_pool_large : 32  × 64KB = 2MB  (musl mallocng büyük chunk'lar)

#define MMAP_POOL_PAGES     512
#define MMAP_PAGE_SIZE      4096

#define MMAP_LARGE_PAGES    32
#define MMAP_LARGE_SIZE     (65536)   // 64KB — musl mallocng chunk boyutu

static unsigned char _mmap_pool[MMAP_POOL_PAGES][MMAP_PAGE_SIZE]
    __attribute__((aligned(MMAP_PAGE_SIZE)));
static unsigned long _mmap_used[(MMAP_POOL_PAGES + 63) / 64];

static unsigned char _mmap_large[MMAP_LARGE_PAGES][MMAP_LARGE_SIZE]
    __attribute__((aligned(MMAP_PAGE_SIZE)));
static unsigned long _mmap_large_used[(MMAP_LARGE_PAGES + 63) / 64];

static inline int _pool_slot_used(int i) {
    return (_mmap_used[i / 64] >> (i % 64)) & 1;
}
static inline void _pool_slot_set(int i, int v) {
    if (v) _mmap_used[i / 64] |=  (1UL << (i % 64));
    else   _mmap_used[i / 64] &= ~(1UL << (i % 64));
}

static void *_pool_alloc(void) {
    for (int i = 0; i < MMAP_POOL_PAGES; i++) {
        if (!_pool_slot_used(i)) {
            _pool_slot_set(i, 1);
            unsigned char *p = _mmap_pool[i];
            for (int j = 0; j < MMAP_PAGE_SIZE; j++) p[j] = 0;
            return (void*)p;
        }
    }
    return (void*)0;
}

static int _pool_contains(void *addr) {
    unsigned char *p    = (unsigned char*)addr;
    unsigned char *base = _mmap_pool[0];
    unsigned char *end  = _mmap_pool[MMAP_POOL_PAGES - 1] + MMAP_PAGE_SIZE;
    return (p >= base && p < end);
}

static int _pool_free(void *addr) {
    unsigned char *p    = (unsigned char*)addr;
    unsigned char *base = _mmap_pool[0];
    size_t offset = (size_t)(p - base);
    int slot = (int)(offset / MMAP_PAGE_SIZE);
    if (slot < 0 || slot >= MMAP_POOL_PAGES) return -1;
    _pool_slot_set(slot, 0);
    return 0;
}

// ── Büyük blok pool (64KB slot'lar) ──────────────────────────────────────
static inline int _large_slot_used(int i) {
    return (_mmap_large_used[i / 64] >> (i % 64)) & 1;
}
static inline void _large_slot_set(int i, int v) {
    if (v) _mmap_large_used[i / 64] |=  (1UL << (i % 64));
    else   _mmap_large_used[i / 64] &= ~(1UL << (i % 64));
}

// len için kaç slot gerekli (yukarı yuvarla)
static void *_large_alloc(size_t len) {
    int slots_needed = (int)((len + MMAP_LARGE_SIZE - 1) / MMAP_LARGE_SIZE);
    // Ardışık boş slot bul
    for (int i = 0; i <= MMAP_LARGE_PAGES - slots_needed; i++) {
        int ok = 1;
        for (int j = 0; j < slots_needed; j++)
            if (_large_slot_used(i + j)) { ok = 0; break; }
        if (ok) {
            for (int j = 0; j < slots_needed; j++)
                _large_slot_set(i + j, 1);
            unsigned char *p = _mmap_large[i];
            // Sıfırla
            size_t total = (size_t)slots_needed * MMAP_LARGE_SIZE;
            for (size_t k = 0; k < total; k++) p[k] = 0;
            return (void*)p;
        }
    }
    return (void*)0;  // Pool dolu
}

static int _large_contains(void *addr) {
    unsigned char *p    = (unsigned char*)addr;
    unsigned char *base = _mmap_large[0];
    unsigned char *end  = _mmap_large[MMAP_LARGE_PAGES - 1] + MMAP_LARGE_SIZE;
    return (p >= base && p < end);
}

static int _large_free(void *addr, size_t len) {
    unsigned char *p    = (unsigned char*)addr;
    unsigned char *base = _mmap_large[0];
    if (p < base) return -1;
    size_t offset = (size_t)(p - base);
    int slot = (int)(offset / MMAP_LARGE_SIZE);
    if (slot < 0 || slot >= MMAP_LARGE_PAGES) return -1;
    int slots = (int)((len + MMAP_LARGE_SIZE - 1) / MMAP_LARGE_SIZE);
    for (int j = 0; j < slots && (slot + j) < MMAP_LARGE_PAGES; j++)
        _large_slot_set(slot + j, 0);
    return 0;
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off) {
    if (addr == (void*)0
        && (flags & MAP_ANONYMOUS)
        && !(flags & MAP_FIXED)
        && fd == -1)
    {
        // 4KB istekler: küçük pool
        if (len <= MMAP_PAGE_SIZE) {
            void *p = _pool_alloc();
            if (p) return p;
        }
        // 4KB..2MB arası istekler: büyük pool
        // musl mallocng ~65KB chunk'lar istiyor; pool'dan karşıla
        if (len <= (size_t)(MMAP_LARGE_PAGES * MMAP_LARGE_SIZE)) {
            void *p = _large_alloc(len);
            if (p) return p;
        }
        // Pool dolu veya çok büyük → kernel'a düş
    }
    long ret = _sc6(SYS_MMAP,
        (long)addr, (long)len, (long)prot,
        (long)flags, (long)fd, (long)off);
    if (ret < 0) { *__errno_location() = (int)(-ret); return MAP_FAILED; }
    return (void*)ret;
}

int munmap(void *addr, size_t len) {
    if (_pool_contains(addr)) {
        _pool_free(addr);
        return 0;
    }
    if (_large_contains(addr)) {
        _large_free(addr, len);
        return 0;
    }
    long ret = _sc2(SYS_MUNMAP, (long)addr, (long)len);
    if (ret < 0) { *__errno_location() = (int)(-ret); return -1; }
    return 0;
}

void *brk_syscall(void *addr) {
    return (void*)_sc1(SYS_BRK, (long)addr);
}

// ═══════════════════════════════════════════════════════════════════════════
//  BÖLÜM 8: ZAMAN
// ═══════════════════════════════════════════════════════════════════════════

int gettimeofday(struct timeval *tv, void *tz) {
    (void)tz;
    return (int)_sc2(SYS_GETTIMEOFDAY, (long)tv, 0);
}

unsigned int sleep(unsigned int seconds) {
    // 1 saniye ≈ 1000 tick varsayımı (timer 1ms ise)
    _sc1(SYS_SLEEP, (long)(seconds * 1000));
    return 0;
}

int usleep(unsigned int usec) {
    // mikrosaniye → tick (kaba)
    unsigned int ticks = usec / 1000;
    if (ticks == 0) ticks = 1;
    _sc1(SYS_SLEEP, (long)ticks);
    return 0;
}

// ═══════════════════════════════════════════════════════════════════════════
//  BÖLÜM 9: ÇEŞİTLİ (newlib stub'ları)
// ═══════════════════════════════════════════════════════════════════════════

// getenv/setenv: bash environment desteği için basit implementasyon
// Gerçek env dizisini execve ile alıyoruz; burada basit stub
extern char **environ;  // crt0 tarafından başlatılmalı

char *getenv(const char *name) {
    if (!environ || !name) return (char*)0;
    size_t nlen = 0;
    while (name[nlen]) nlen++;
    for (char **ep = environ; *ep; ep++) {
        size_t i = 0;
        while ((*ep)[i] && (*ep)[i] != '=' && i < nlen) i++;
        if (i == nlen && (*ep)[i] == '=')
            return *ep + nlen + 1;
    }
    return (char*)0;
}

// uid/gid: gerçek syscall'lar
uid_t getuid(void)  { return (uid_t)_sc0(SYS_GETUID); }
uid_t geteuid(void) { return (uid_t)_sc0(SYS_GETEUID); }
gid_t getgid(void)  { return (gid_t)_sc0(SYS_GETGID); }
gid_t getegid(void) { return (gid_t)_sc0(SYS_GETEGID); }

// symlink/link/readlink: FAT32'de yok
int symlink(const char *target, const char *linkpath) {
    long ret = _sc2(SYS_SYMLINK, (long)target, (long)linkpath);
    SET_ERRNO_RET(ret, -1);
    return (int)ret;
}
__attribute__((noinline))
int link(const char *oldpath, const char *newpath) {
    volatile char _stack_guard[5120];
    _stack_guard[0] = 0; _stack_guard[5119] = 0;
    (void)_stack_guard;
    long ret;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"((long)86), "D"((long)oldpath), "S"((long)newpath)
        : "rcx", "r11", "memory"
    );
    if (ret < 0) { *__errno_location() = (int)(-ret); return -1; }
    return (int)ret;
}
ssize_t readlink(const char *path, char *buf, size_t size) {
    long ret = _sc3(SYS_READLINK, (long)path, (long)buf, (long)size);
    SET_ERRNO_RET(ret, -1);
    return (ssize_t)ret;
}

// chmod/chown: tek kullanıcılı sistem
int chmod(const char *path, mode_t mode) {
    long ret = _sc2(SYS_CHMOD, (long)path, (long)mode);
    SET_ERRNO_RET(ret, -1);
    return (int)ret;
}
int chown(const char *p, uid_t u, gid_t g) { (void)p;(void)u;(void)g; return 0; }
int fchmod(int fd, mode_t m)               { (void)fd;(void)m; return 0; }

// times/getrusage: süreç zamanı
typedef struct { long tms_utime, tms_stime, tms_cutime, tms_cstime; } tms_t;
long times(tms_t *buf) {
    return (long)_sc1(SYS_TIMES, (long)buf);
}

// select: zaten kernel'da var
typedef struct { unsigned int fds_bits[1]; } fd_set_k;
int select(int nfds, fd_set_k *rd, fd_set_k *wr, fd_set_k *ex, struct timeval *tv) {
    return (int)_sc6(SYS_SELECT, (long)nfds, (long)rd, (long)wr, (long)ex, (long)tv, 0);
}

// getlogin: root
char *getlogin(void) { return "root"; }

// getpwuid_r / getgrgid_r: stub
// (bash user/group lookup için; tek kullanıcılı sistemde basit dön)

// ctermid: controlling terminal
char *ctermid(char *s) {
    static char buf[] = "/dev/tty0";
    if (s) {
        int i = 0;
        while (buf[i]) { s[i] = buf[i]; i++; }
        s[i] = '\0';
        return s;
    }
    return buf;
}

// posix_spawn: fork+execve sarıcısı (bash bunu kullanabilir)
// Basit implementasyon: posix_spawn_file_actions yoksayılır
int posix_spawn(pid_t *pid, const char *path,
                void *file_actions, void *attr,
                char *const argv[], char *const envp[]) {
    (void)file_actions; (void)attr;
    pid_t child = fork();
    if (child < 0) return child;
    if (child == 0) {
        execve(path, argv, envp ? envp : (char *const[]){(char*)0});
        _exit(127);
    }
    if (pid) *pid = child;
    return 0;
}

// ═══════════════════════════════════════════════════════════════════════════
//  BÖLÜM 10: SİSTEM BİLGİSİ (v13)
// ═══════════════════════════════════════════════════════════════════════════

int uname(struct utsname *buf) {
    return (int)_sc1(SYS_UNAME, (long)buf);
}

// ═══════════════════════════════════════════════════════════════════════════
//  BÖLÜM 12: ZAMANLAMA VE SİNYAL YIĞINI (v15)
// ═══════════════════════════════════════════════════════════════════════════

int nanosleep(const struct timespec *req, struct timespec *rem) {
    long ret = _sc2(SYS_NANOSLEEP, (long)req, (long)rem);
    SET_ERRNO_RET(ret, -1);
    return (int)ret;
}

int sigaltstack(const stack_t *ss, stack_t *old_ss) {
    long ret = _sc2(SYS_SIGALTSTACK, (long)ss, (long)old_ss);
    SET_ERRNO_RET(ret, -1);
    return (int)ret;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * ASCENTOS_STUBS_INJECTED — Bash 4.4 port için gerekli stub semboller
 * Bu blok bash-build.sh tarafından otomatik eklendi.
 * Düzeltmeler: hosted header yok, duplicate fonksiyonlar kaldırıldı.
 * ═══════════════════════════════════════════════════════════════════════════ */

// ═══════════════════════════════════════════════════════════════════════════
//  BÖLÜM 13: CLOCK (v16)
// ═══════════════════════════════════════════════════════════════════════════

// POSIX saat ID'leri
#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1

int clock_gettime(int clockid, struct timespec *tp) {
    return (int)_sc2(SYS_CLOCK_GETTIME, (long)clockid, (long)tp);
}

int clock_getres(int clockid, struct timespec *res) {
    return (int)_sc2(SYS_CLOCK_GETRES, (long)clockid, (long)res);
}

// ═══════════════════════════════════════════════════════════════════════════
//  BÖLÜM 14: TRUNCATE (v16)
// ═══════════════════════════════════════════════════════════════════════════

int ftruncate(int fd, off_t length) {
    long ret = _sc2(SYS_FTRUNCATE, (long)fd, (long)length);
    SET_ERRNO_RET(ret, -1);
    return (int)ret;
}

int truncate(const char *path, off_t length) {
    long ret = _sc2(SYS_TRUNCATE, (long)path, (long)length);
    SET_ERRNO_RET(ret, -1);
    return (int)ret;
}

// ═══════════════════════════════════════════════════════════════════════════
//  BÖLÜM 15: RLIMIT (v16)
// ═══════════════════════════════════════════════════════════════════════════

// POSIX rlimit kaynak ID'leri
#define RLIMIT_CPU    0   // CPU zamanı (saniye)
#define RLIMIT_FSIZE  1   // Maksimum dosya boyutu
#define RLIMIT_DATA   2   // Maksimum data segment boyutu
#define RLIMIT_STACK  3   // Maksimum stack boyutu
#define RLIMIT_CORE   4   // Maksimum core dosyası boyutu
#define RLIMIT_NOFILE 7   // Açık dosya sayısı limiti
#define RLIMIT_AS     9   // Sanal adres alanı limiti

#define RLIM_INFINITY ((unsigned long)-1)

typedef struct {
    unsigned long rlim_cur;   // Soft limit
    unsigned long rlim_max;   // Hard limit (ceiling)
} rlimit_t;

int getrlimit(int resource, rlimit_t *rlim) {
    long ret = _sc2(SYS_GETRLIMIT, (long)resource, (long)rlim);
    SET_ERRNO_RET(ret, -1);
    return (int)ret;
}

int setrlimit(int resource, const rlimit_t *rlim) {
    long ret = _sc2(SYS_SETRLIMIT, (long)resource, (long)rlim);
    SET_ERRNO_RET(ret, -1);
    return (int)ret;
}

// ═══════════════════════════════════════════════════════════════════════════
//  BÖLÜM 16: UMASK & MPROTECT (v16)
// ═══════════════════════════════════════════════════════════════════════════

mode_t umask(mode_t mask) {
    return (mode_t)_sc1(SYS_UMASK, (long)mask);
}

int mprotect(void *addr, size_t len, int prot) {
    long ret = _sc3(SYS_MPROTECT, (long)addr, (long)len, (long)prot);
    SET_ERRNO_RET(ret, -1);
    return (int)ret;
}

// ═══════════════════════════════════════════════════════════════════════════
//  BÖLÜM 17: GRUP KİMLİĞİ (v16)
// ═══════════════════════════════════════════════════════════════════════════

// getgroups: sürecin ek grup listesini döndürür
// Tek kullanıcılı AscentOS'ta genellikle 0 grup döner.
int getgroups(int size, gid_t list[]) {
    long ret = _sc2(SYS_GETGROUPS, (long)size, (long)list);
    SET_ERRNO_RET(ret, -1);
    return (int)ret;
}

// ═══════════════════════════════════════════════════════════════════════════
//  BÖLÜM 18: FUTEX (v24)
// ═══════════════════════════════════════════════════════════════════════════

// futex işlem sabitleri (Linux uyumlu)
#define FUTEX_WAIT          0
#define FUTEX_WAKE          1
#define FUTEX_REQUEUE       3
#define FUTEX_CMP_REQUEUE   4
#define FUTEX_PRIVATE_FLAG  128
#define FUTEX_WAIT_PRIVATE  (FUTEX_WAIT | FUTEX_PRIVATE_FLAG)
#define FUTEX_WAKE_PRIVATE  (FUTEX_WAKE | FUTEX_PRIVATE_FLAG)

long futex(int *uaddr, int op, int val, const struct timespec *timeout,
           int *uaddr2, int val3) {
    long ret = _sc6(SYS_FUTEX, (long)uaddr, (long)op, (long)val,
                    (long)timeout, (long)uaddr2, (long)val3);
    SET_ERRNO(ret);
    return ret;
}

// ═══════════════════════════════════════════════════════════════════════════
//  BÖLÜM 19: GETRANDOM (v24)
// ═══════════════════════════════════════════════════════════════════════════

#define GRND_NONBLOCK 0x0001
#define GRND_RANDOM   0x0002

ssize_t getrandom(void *buf, size_t buflen, unsigned int flags) {
    long ret = _sc3(SYS_GETRANDOM, (long)buf, (long)buflen, (long)flags);
    SET_ERRNO_RET(ret, -1);
    return (ssize_t)ret;
}

// ═══════════════════════════════════════════════════════════════════════════
//  BÖLÜM 20: ARCH_PRCTL (v25)
// ═══════════════════════════════════════════════════════════════════════════

#define ARCH_SET_GS   0x1001
#define ARCH_SET_FS   0x1002
#define ARCH_GET_FS   0x1003
#define ARCH_GET_GS   0x1004
#define ARCH_GET_CPUID 0x1011
#define ARCH_SET_CPUID 0x1012

int arch_prctl(int code, unsigned long addr) {
    long ret = _sc2(SYS_ARCH_PRCTL, (long)code, (long)addr);
    SET_ERRNO_RET(ret, -1);
    return (int)ret;
}

// ═══════════════════════════════════════════════════════════════════════════
//  BÖLÜM 21: CLONE (v25)
// ═══════════════════════════════════════════════════════════════════════════

// clone() bayrakları (Linux uyumlu)
#define CLONE_VM           0x00000100
#define CLONE_FS           0x00000200
#define CLONE_FILES        0x00000400
#define CLONE_SIGHAND      0x00000800
#define CLONE_THREAD       0x00010000
#define CLONE_SETTLS       0x00080000
#define CLONE_PARENT_SETTID  0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_CHILD_SETTID   0x01000000

// Not: clone() çağrı kuralı Linux x86-64'e özgüdür.
// child_stack, ptid, ctid, tls argümanları sırasıyla
// RSI, RDX, R10, R8 registerlarına gider.
long clone(unsigned long flags, void *child_stack,
           int *ptid, int *ctid, unsigned long tls) {
    long ret = _sc6(SYS_CLONE, (long)flags, (long)child_stack,
                    (long)ptid, (long)ctid, (long)tls, 0);
    SET_ERRNO(ret);
    return ret;
}

// ═══════════════════════════════════════════════════════════════════════════
//  BÖLÜM 22: SET_TID_ADDRESS & SET_ROBUST_LIST (v26)
// ═══════════════════════════════════════════════════════════════════════════

#define ROBUST_LIST_HEAD_SIZE 24

long set_tid_address(int *tidptr) {
    return _sc1(SYS_SET_TID_ADDRESS, (long)tidptr);
}

int set_robust_list(void *head, size_t len) {
    long ret = _sc2(SYS_SET_ROBUST_LIST, (long)head, (long)len);
    SET_ERRNO_RET(ret, -1);
    return (int)ret;
}

// ═══════════════════════════════════════════════════════════════════════════
//  BÖLÜM 23: WRITEV (v27)
// ═══════════════════════════════════════════════════════════════════════════

// iovec yapısı
struct iovec {
    void   *iov_base;
    size_t  iov_len;
};

ssize_t writev(int fd, const struct iovec *iov, int iovcnt) {
    long ret = _sc3(SYS_WRITEV, (long)fd, (long)iov, (long)iovcnt);
    SET_ERRNO_RET(ret, -1);
    return (ssize_t)ret;
}

// ═══════════════════════════════════════════════════════════════════════════
//  BÖLÜM 24: MADVISE (v27)
// ═══════════════════════════════════════════════════════════════════════════

#define MADV_NORMAL     0
#define MADV_RANDOM     1
#define MADV_SEQUENTIAL 2
#define MADV_WILLNEED   3
#define MADV_DONTNEED   4
#define MADV_FREE       8

int madvise(void *addr, size_t len, int advice) {
    // AscentOS kernel madvise'ı henüz implemente etmemiş olabilir.
    // musl free() → madvise(MADV_FREE) çağırır; kernel patlarsa tüm
    // uygulama çöker. Güvenli stub: sessizce başarılı dön.
    (void)addr; (void)len; (void)advice;
    return 0;
}

// ═══════════════════════════════════════════════════════════════════════════
//  BÖLÜM 25: EXIT_GROUP (v27)
// ═══════════════════════════════════════════════════════════════════════════

__attribute__((noreturn))
void exit_group(int code) {
    _sc1(SYS_EXIT_GROUP, (long)code);
    __builtin_unreachable();
}

// ═══════════════════════════════════════════════════════════════════════════
//  BÖLÜM 26: OPENAT & NEWFSTATAT (v27)
// ═══════════════════════════════════════════════════════════════════════════

// openat() dirfd sabitleri
#define AT_FDCWD             (-100)
#define AT_SYMLINK_NOFOLLOW  0x100
#define AT_REMOVEDIR         0x200
#define AT_EMPTY_PATH        0x1000

int openat(int dirfd, const char *path, int flags, ...) {
    unsigned int mode = 0644;
    long ret = _sc4(SYS_OPENAT, (long)dirfd, (long)path, (long)flags, (long)mode);
    SET_ERRNO_RET(ret, -1);
    return (int)ret;
}

int newfstatat(int dirfd, const char *path, struct stat *buf, int flags) {
    long ret = _sc4(SYS_NEWFSTATAT, (long)dirfd, (long)path, (long)buf, (long)flags);
    SET_ERRNO_RET(ret, -1);
    return (int)ret;
}

// fstatat: newfstatat için alias (POSIX ismi)
int fstatat(int dirfd, const char *path, struct stat *buf, int flags) {
    return newfstatat(dirfd, path, buf, flags);
}

// ═══════════════════════════════════════════════════════════════════════════
//  BÖLÜM 27: PRLIMIT64 (v27)
// ═══════════════════════════════════════════════════════════════════════════

typedef struct {
    unsigned long rlim_cur;
    unsigned long rlim_max;
} rlimit64_t;

int prlimit64(pid_t pid, int resource,
              const rlimit64_t *new_limit, rlimit64_t *old_limit) {
    long ret = _sc4(SYS_PRLIMIT64, (long)pid, (long)resource,
                    (long)new_limit, (long)old_limit);
    SET_ERRNO_RET(ret, -1);
    return (int)ret;
}

// prlimit: prlimit64 için takma ad (musl bu ismi kullanır)
int prlimit(pid_t pid, int resource,
            const rlimit64_t *new_limit, rlimit64_t *old_limit) {
    return prlimit64(pid, resource, new_limit, old_limit);
}

// ═══════════════════════════════════════════════════════════════════════════
//  BÖLÜM 29: DOOM / GRAFİK SYSCALL'LARI (v29)
//
//  SYS_FB_INFO (407): Framebuffer bilgisini kernel'dan alır.
//  SYS_KB_RAW  (408): Klavye raw scancode modunu açar/kapatır.
// ═══════════════════════════════════════════════════════════════════════════

#define SYS_FB_INFO  407
#define SYS_KB_RAW   408
#define SYS_FB_BLIT  409
#define SYS_KB_READ  410
#define SYS_SB16_PLAY 411

// fb_info_t — kernel fb_info_t ile birebir aynı olmalı
// Kernel syscall.h: TÜM FIELD'LAR uint64_t
typedef struct {
    unsigned long long addr;    // Framebuffer fiziksel/lineer adresi — offset 0
    unsigned long long width;   // Pixel cinsinden genişlik            — offset 8
    unsigned long long height;  // Pixel cinsinden yükseklik           — offset 16
    unsigned long long pitch;   // Satır başına byte                   — offset 24
    unsigned long long bpp;     // Bit per pixel (genellikle 32)       — offset 32
} ascent_fb_info_t;             // toplam: 40 byte

// ascent_fb_blit_t — kernel ascent_fb_blit_t ile birebir aynı olmalı
// UYARI: Kernel syscall.h'da TÜM FIELD'LAR uint64_t (8 byte).
// uint32_t kullanmak offset uyumsuzluğuna ve sessiz veri kaybına yol açar!
typedef struct {
    unsigned long long src_pixels;  // user-space uint32_t* (Doom XRGB8888) — offset 0
    unsigned long long src_w;       // kaynak genişlik  (320)               — offset 8
    unsigned long long src_h;       // kaynak yükseklik (200)               — offset 16
    unsigned long long dst_x;       // hedef x ofseti                       — offset 24
    unsigned long long dst_y;       // hedef y ofseti                       — offset 32
    unsigned long long scale;       // 1, 2 veya 3                          — offset 40
} ascent_fb_blit_t;                 // toplam: 48 byte — kernel ile birebir

// fb_info: framebuffer bilgisini doldurur
// Dönüş: 0 başarı, -1 hata (errno ayarlanır)
int fb_info(ascent_fb_info_t* out) {
    long ret = _sc1(SYS_FB_INFO, (long)out);
    SET_ERRNO_RET(ret, -1);
    return (int)ret;
}

// kb_raw: raw klavye modu aç (1) / kapat (0)
// Dönüş: 0 başarı, -1 hata
int kb_raw(int enable) {
    long ret = _sc1(SYS_KB_RAW, (long)enable);
    SET_ERRNO_RET(ret, -1);
    return (int)ret;
}

// fb_blit: Doom screen buffer'ını kernel VRAM'ına kopyalat
// Dönüş: 0 başarı, -1 hata
int fb_blit(ascent_fb_blit_t* req) {
    long ret = _sc1(SYS_FB_BLIT, (long)req);

// ── SYS_SB16_PLAY (411): SB16 PCM calmak icin ──────────────────────────────
// buf      : PCM verisi (user-space, max 4096 byte)
// len      : byte sayisi (max 4096, SB16 DMA kisiti)
// rate_hz  : ornekleme hizi (4000-44100 Hz)
// fmt      : 0=8bit_mono 1=8bit_stereo 2=16bit_mono 3=16bit_stereo
// Donus    : 0=basari, -19=ENODEV (SB16 yok), -22=EINVAL, -5=EIO
#define SYS_SB16_PLAY 411
// __attribute__((used)): -Wunused-function uyarısını önler.
// wav_player.c bu fonksiyonu inline syscall ile çağırıyor;
// gerekirse doğrudan bu stub da kullanılabilir.
int __attribute__((used)) sb16_play(const void* buf, int len, int rate_hz, int fmt) {
    // errno.h gerektirmemek için -EINVAL değerini doğrudan döndür
    if (!buf || len <= 0 || len > 4096) return -22; // EINVAL, SET_ERRNO_RET bypass
    long ret;
    register long r10 __asm__("r10") = (long)fmt;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "0"(411L), "D"((long)buf), "S"((long)len), "d"((long)rate_hz), "r"(r10)
        : "rcx", "r11", "memory");
    SET_ERRNO_RET(ret, -1);
    return (int)ret;
}

    SET_ERRNO_RET(ret, -1);
    return (int)ret;
}

// ── errno notu ────────────────────────────────────────────────────────────
// __errno_location strong sembol olarak yukarıda tanımlıdır.
// syscalls.o musl.a'dan önce link edildiğinde musl'ün archive versiyonu atlanır.
// Tüm SET_ERRNO_RET çağrıları FS_BASE bağımsız _errno_storage'a yazar.