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
#define SYS_WRITE        1
#define SYS_READ         2
#define SYS_EXIT         3
#define SYS_GETPID       4
#define SYS_YIELD        5
#define SYS_SLEEP        6
#define SYS_UPTIME       7
#define SYS_DEBUG        8
#define SYS_OPEN         9
#define SYS_CLOSE        10
#define SYS_GETPPID      11
#define SYS_SBRK         12
#define SYS_GETPRIORITY  13
#define SYS_SETPRIORITY  14
#define SYS_GETTICKS     15
// v3: Bellek ve süreç
#define SYS_MMAP         16
#define SYS_MUNMAP       17
#define SYS_BRK          18
#define SYS_FORK         19
#define SYS_EXECVE       20
#define SYS_WAITPID      21
#define SYS_PIPE         22
#define SYS_DUP2         23
// v4: Dosya I/O
#define SYS_LSEEK        24
#define SYS_FSTAT        25
#define SYS_IOCTL        26
// v5: Çoğullama
#define SYS_SELECT       27
#define SYS_POLL         28
// v6: Newlib uyumu
#define SYS_KILL         29
#define SYS_GETTIMEOFDAY 30
// v8: Dosya sorgulama
#define SYS_STAT         31
#define SYS_ACCESS       42
// v7: Dizin
#define SYS_GETCWD       43
#define SYS_CHDIR        44
// v9: Dizin okuma
#define SYS_GETDENTS     58
#define SYS_OPENDIR      59
#define SYS_CLOSEDIR     60
// v10: Sinyal
#define SYS_SIGACTION    61
#define SYS_SIGPROCMASK  62
#define SYS_SIGRETURN    63
#define SYS_SIGPENDING   64
#define SYS_SIGSUSPEND   65
// v11: fd yönetimi
#define SYS_FCNTL        66
#define SYS_DUP          67
// v12: Process group & session (bash iş kontrolü)
#define SYS_SETPGID      68
#define SYS_GETPGID      69
#define SYS_SETSID       70
#define SYS_TCSETPGRP    71
#define SYS_TCGETPGRP    72
// v13: Sistem bilgisi
#define SYS_UNAME        73
// v14: Dosya sistemi yazma (bash mkdir/rm/mv)
#define SYS_MKDIR        80
#define SYS_RMDIR        81
#define SYS_UNLINK       82
#define SYS_RENAME       83
// v15: Kullanıcı kimliği / zamanlama / sinyal yığını
#define SYS_GETUID       84
#define SYS_GETEUID      85
#define SYS_GETGID       86
#define SYS_GETEGID      87
#define SYS_NANOSLEEP    88
#define SYS_SIGALTSTACK  89
// v16: clock, alarm, truncate, rlimit, lstat, link, umask, symlink, readlink, chmod, mprotect, pipe2, times, getgroups
#define SYS_CLOCK_GETTIME 90
#define SYS_CLOCK_GETRES  91
#define SYS_ALARM         92
#define SYS_FTRUNCATE     93
#define SYS_TRUNCATE      94
#define SYS_GETRLIMIT     95
#define SYS_SETRLIMIT     96
#define SYS_LSTAT         97
#define SYS_LINK          98
#define SYS_TIMES         99
#define SYS_UMASK        100
#define SYS_SYMLINK      101
#define SYS_READLINK     102
#define SYS_CHMOD        103
#define SYS_MPROTECT     104
#define SYS_PIPE2        105
#define SYS_GETGROUPS    106

// ── open() flags ─────────────────────────────────────────────────────────
#define O_RDONLY    0x00
#define O_WRONLY    0x01
#define O_RDWR      0x02
#define O_CREAT     0x40
#define O_TRUNC     0x200
#define O_APPEND    0x400
#define O_NONBLOCK  0x800
#define O_CLOEXEC   0x80000

// errno forward declaration (tanım dosyanın sonunda)
extern int _errno_val;

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
    register long _rax __asm__("rax") = nr;
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "r"(_rax)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline long _sc1(long nr, long a1) {
    register long _rax __asm__("rax") = nr;
    register long _rdi __asm__("rdi") = a1;
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "r"(_rax), "r"(_rdi)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline long _sc2(long nr, long a1, long a2) {
    register long _rax __asm__("rax") = nr;
    register long _rdi __asm__("rdi") = a1;
    register long _rsi __asm__("rsi") = a2;
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "r"(_rax), "r"(_rdi), "r"(_rsi)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline long _sc3(long nr, long a1, long a2, long a3) {
    register long _rax __asm__("rax") = nr;
    register long _rdi __asm__("rdi") = a1;
    register long _rsi __asm__("rsi") = a2;
    register long _rdx __asm__("rdx") = a3;
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "r"(_rax), "r"(_rdi), "r"(_rsi), "r"(_rdx)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline long _sc4(long nr, long a1, long a2, long a3, long a4) {
    long ret;
    register long _rax __asm__("rax") = nr;
    register long _rdi __asm__("rdi") = a1;
    register long _rsi __asm__("rsi") = a2;
    register long _rdx __asm__("rdx") = a3;
    register long _r10 __asm__("r10") = a4;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "r"(_rax), "r"(_rdi), "r"(_rsi), "r"(_rdx), "r"(_r10)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline long _sc6(long nr, long a1, long a2, long a3,
                         long a4, long a5, long a6) {
    long ret;
    register long _rax __asm__("rax") = nr;
    register long _rdi __asm__("rdi") = a1;
    register long _rsi __asm__("rsi") = a2;
    register long _rdx __asm__("rdx") = a3;
    register long _r10 __asm__("r10") = a4;
    register long _r8  __asm__("r8")  = a5;
    register long _r9  __asm__("r9")  = a6;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "r"(_rax), "r"(_rdi), "r"(_rsi), "r"(_rdx), "r"(_r10), "r"(_r8), "r"(_r9)
        : "rcx", "r11", "memory"
    );
    return ret;
}

// ═══════════════════════════════════════════════════════════════════════════
//  BÖLÜM 1: TEMEL I/O
// ═══════════════════════════════════════════════════════════════════════════

ssize_t write(int fd, const void *buf, size_t len) {
    long ret = _sc3(SYS_WRITE, (long)fd, (long)buf, (long)len);
    if (ret < 0) { _errno_val = (ret == -9) ? 9 : 22; return -1; }
    return (ssize_t)ret;
}

ssize_t read(int fd, void *buf, size_t len) {
    return (ssize_t)_sc3(SYS_READ, (long)fd, (long)buf, (long)len);
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
        // Kernel AscentOS hata kodlarını (-1..-14) errno değerlerine çevir
        // SYSCALL_ERR_INVAL=-1→EINVAL, SYSCALL_ERR_NOENT=-4→ENOENT vb.
        int e = (int)(-ret);
        switch (e) {
            case 1:  _errno_val = 22; break; // EINVAL
            case 2:  _errno_val = 38; break; // ENOSYS
            case 3:  _errno_val = 1;  break; // EPERM
            case 4:  _errno_val = 2;  break; // ENOENT
            case 5:  _errno_val = 9;  break; // EBADF
            case 6:  _errno_val = 12; break; // ENOMEM
            case 8:  _errno_val = 24; break; // EMFILE
            default: _errno_val = 22; break; // EINVAL
        }
        return -1;
    }
    return (int)ret;
}

int close(int fd) {
    return (int)_sc1(SYS_CLOSE, (long)fd);
}

off_t lseek(int fd, off_t offset, int whence) {
    return (off_t)_sc3(SYS_LSEEK, (long)fd, (long)offset, (long)whence);
}

int stat(const char *path, struct stat *buf) {
    return (int)_sc2(SYS_STAT, (long)path, (long)buf);
}

int fstat(int fd, struct stat *buf) {
    return (int)_sc2(SYS_FSTAT, (long)fd, (long)buf);
}

int lstat(const char *path, struct stat *buf) {
    return (int)_sc2(SYS_LSTAT, (long)path, (long)buf);
}

int access(const char *path, int mode) {
    return (int)_sc2(SYS_ACCESS, (long)path, (long)mode);
}

int ioctl(int fd, unsigned long req, void *arg) {
    return (int)_sc3(SYS_IOCTL, (long)fd, (long)req, (long)arg);
}

int fcntl(int fd, int cmd, long arg) {
    return (int)_sc3(SYS_FCNTL, (long)fd, (long)cmd, (long)arg);
}

int dup(int fd) {
    return (int)_sc1(SYS_DUP, (long)fd);
}

int dup2(int oldfd, int newfd) {
    return (int)_sc2(SYS_DUP2, (long)oldfd, (long)newfd);
}

int pipe(int fd[2]) {
    return (int)_sc1(SYS_PIPE, (long)fd);
}

// pipe2: O_CLOEXEC bayrağını atomik destekler
int pipe2(int fd[2], int flags) {
    return (int)_sc2(SYS_PIPE2, (long)fd, (long)flags);
}

// ═══════════════════════════════════════════════════════════════════════════
//  BÖLÜM 3: DİZİN İŞLEMLERİ
// ═══════════════════════════════════════════════════════════════════════════

char *getcwd(char *buf, size_t size) {
    long r = _sc2(SYS_GETCWD, (long)buf, (long)size);
    return (r == 0) ? buf : (char *)0;
}

int chdir(const char *path) {
    return (int)_sc1(SYS_CHDIR, (long)path);
}

int mkdir(const char *path, mode_t mode) {
    return (int)_sc2(SYS_MKDIR, (long)path, (long)mode);
}

int rmdir(const char *path) {
    return (int)_sc1(SYS_RMDIR, (long)path);
}

int unlink(const char *path) {
    return (int)_sc1(SYS_UNLINK, (long)path);
}

int rename(const char *old, const char *newpath) {
    return (int)_sc2(SYS_RENAME, (long)old, (long)newpath);
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

__attribute__((noreturn))
void exit(int code) {
    // TODO: atexit handler'larını çağır (newlib bağlıysa newlib halleder)
    _exit(code);
}

pid_t getpid(void) {
    return (pid_t)_sc0(SYS_GETPID);
}

pid_t getppid(void) {
    return (pid_t)_sc0(SYS_GETPPID);
}

pid_t fork(void) {
    register long _rax __asm__("rax") = SYS_FORK;
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "r"(_rax)
        : "rcx", "r11", "memory"
    );
    return (pid_t)ret;
}

int execve(const char *path, char *const argv[], char *const envp[]) {
    return (int)_sc3(SYS_EXECVE, (long)path, (long)argv, (long)envp);
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
    return (pid_t)_sc3(SYS_WAITPID, (long)pid, (long)status, (long)opts);
}

pid_t wait(int *status) {
    return waitpid(-1, status, 0);
}

// ── Process Group & Session ───────────────────────────────────────────────

int setpgid(pid_t pid, pid_t pgid) {
    return (int)_sc2(SYS_SETPGID, (long)pid, (long)pgid);
}

pid_t getpgid(pid_t pid) {
    return (pid_t)_sc1(SYS_GETPGID, (long)pid);
}

pid_t getpgrp(void) {
    return getpgid(0);
}

pid_t setsid(void) {
    return (pid_t)_sc0(SYS_SETSID);
}

pid_t getsid(pid_t pid) {
    // AscentOS'ta getsid ayrı bir syscall yok; PGID olarak döndür
    return getpgid(pid);
}

int tcsetpgrp(int fd, pid_t pgrp) {
    return (int)_sc2(SYS_TCSETPGRP, (long)fd, (long)pgrp);
}

pid_t tcgetpgrp(int fd) {
    return (pid_t)_sc1(SYS_TCGETPGRP, (long)fd);
}

// ── kill / pause ──────────────────────────────────────────────────────────

int kill(pid_t pid, int sig) {
    return (int)_sc2(SYS_KILL, (long)pid, (long)sig);
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
    return (int)_sc3(SYS_SIGACTION, (long)signo, (long)new_sa, (long)old_sa);
}

int sigprocmask(int how, const sigset_t *set, sigset_t *oldset) {
    return (int)_sc3(SYS_SIGPROCMASK, (long)how, (long)set, (long)oldset);
}

int sigpending(sigset_t *set) {
    return (int)_sc1(SYS_SIGPENDING, (long)set);
}

int sigsuspend(const sigset_t *mask) {
    return (int)_sc1(SYS_SIGSUSPEND, (long)mask);
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

// sbrk: newlib malloc/free bu fonksiyona bağlanır
/* AscentOS: ASCENTOS_HEAP_PATCHED -- __heap_start fallback */
#ifdef DASCENTOS_STATIC_HEAP
/* user.ld olmadan derleme: statik 4MB heap tamponu */
static char _static_heap[4 * 1024 * 1024];
static char *_heap_ptr = _static_heap;
#else
extern char __heap_start;
static char *_heap_ptr = 0;
#endif

void *sbrk(long incr) {
#ifndef DASCENTOS_STATIC_HEAP
    if (!_heap_ptr) _heap_ptr = &__heap_start;
#endif
    char *prev = _heap_ptr;
    _heap_ptr += incr;
    return (void*)prev;
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off) {
    return (void*)_sc6(SYS_MMAP,
        (long)addr, (long)len, (long)prot,
        (long)flags, (long)fd, (long)off);
}

int munmap(void *addr, size_t len) {
    return (int)_sc2(SYS_MUNMAP, (long)addr, (long)len);
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
    return (int)_sc2(SYS_SYMLINK, (long)target, (long)linkpath);
}
int link(const char *oldpath, const char *newpath) {
    return (int)_sc2(SYS_LINK, (long)oldpath, (long)newpath);
}
ssize_t readlink(const char *path, char *buf, size_t size) {
    return (ssize_t)_sc3(SYS_READLINK, (long)path, (long)buf, (long)size);
}

// chmod/chown: tek kullanıcılı sistem
int chmod(const char *path, mode_t mode) {
    return (int)_sc2(SYS_CHMOD, (long)path, (long)mode);
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
    return (int)_sc2(SYS_NANOSLEEP, (long)req, (long)rem);
}

int sigaltstack(const stack_t *ss, stack_t *old_ss) {
    return (int)_sc2(SYS_SIGALTSTACK, (long)ss, (long)old_ss);
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
    return (int)_sc2(SYS_FTRUNCATE, (long)fd, (long)length);
}

int truncate(const char *path, off_t length) {
    return (int)_sc2(SYS_TRUNCATE, (long)path, (long)length);
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
    return (int)_sc2(SYS_GETRLIMIT, (long)resource, (long)rlim);
}

int setrlimit(int resource, const rlimit_t *rlim) {
    return (int)_sc2(SYS_SETRLIMIT, (long)resource, (long)rlim);
}

// ═══════════════════════════════════════════════════════════════════════════
//  BÖLÜM 16: UMASK & MPROTECT (v16)
// ═══════════════════════════════════════════════════════════════════════════

mode_t umask(mode_t mask) {
    return (mode_t)_sc1(SYS_UMASK, (long)mask);
}

int mprotect(void *addr, size_t len, int prot) {
    return (int)_sc3(SYS_MPROTECT, (long)addr, (long)len, (long)prot);
}

// ═══════════════════════════════════════════════════════════════════════════
//  BÖLÜM 17: GRUP KİMLİĞİ (v16)
// ═══════════════════════════════════════════════════════════════════════════

// getgroups: sürecin ek grup listesini döndürür
// Tek kullanıcılı AscentOS'ta genellikle 0 grup döner.
int getgroups(int size, gid_t list[]) {
    return (int)_sc2(SYS_GETGROUPS, (long)size, (long)list);
}

// ── errno emülasyonu ──────────────────────────────────────────────────────
// Kernel negatif hata kodu döndürür; newlib errno'yu bu değerden alır.
// newlib'in _errno() fonksiyonu global errno'ya pointer döndürür.
int _errno_val = 0;
int *__errno_location(void) { return &_errno_val; }
// newlib bazı platformlarda bunu kullanır:
int *_errno(void) { return &_errno_val; }