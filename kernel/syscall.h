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
#define MSR_FMASK      0xC0000084   // RFLAGS mask (entry'de sıfırlanır)

// EFER bit flags
#define EFER_SCE       (1 << 0)     // System Call Extensions

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

// ── Linux x86-64 Syscall Numaraları ──────────────────────────
//
// Numaralar Linux x86-64 ABI ile birebir uyumludur.
// Kaynak: arch/x86/entry/syscalls/syscall_64.tbl (Linux 6.x)
//
// AscentOS'a özgü syscall'lar (Linux'ta karşılığı olmayanlar)
// 400+ aralığında tutulur; hiçbir zaman Linux numaralarıyla çakışmaz.
// ─────────────────────────────────────────────────────────────

// ── Temel I/O ────────────────────────────────────────────────
#define SYS_READ          0   // read(fd, buf, len)               -> bytes_read    | err
#define SYS_WRITE         1   // write(fd, buf, len)              -> bytes_written | err
#define SYS_OPEN          2   // open(path, flags)                -> fd | err
#define SYS_CLOSE         3   // close(fd)                        -> 0  | err
#define SYS_STAT          4   // stat(path, *stat_buf)            -> 0 | err
#define SYS_FSTAT         5   // fstat(fd, *stat)                 -> 0 | err
#define SYS_LSTAT         6   // lstat(path, *stat_buf)           -> 0 | err  (symlink meta)
#define SYS_POLL          7   // poll(fds, nfds, timeout_ms)      -> nready | err
#define SYS_LSEEK         8   // lseek(fd, offset, whence)        -> new_offset | err
#define SYS_MMAP          9   // mmap(addr,len,prot,flags,fd,off) -> mapped_addr | err
#define SYS_MPROTECT     10   // mprotect(addr, len, prot)        -> 0 | err
#define SYS_MUNMAP       11   // munmap(addr, len)                -> 0 | err
#define SYS_BRK          12   // brk(addr)                        -> new_brk | err
#define SYS_SIGACTION    13   // sigaction(signo, *new_sa, *old)  -> 0 | err
#define SYS_SIGPROCMASK  14   // sigprocmask(how, *set, *oldset)  -> 0 | err
#define SYS_SIGRETURN    15   // sigreturn() — trampoline tarafından çağrılır
#define SYS_IOCTL        16   // ioctl(fd, request, arg)          -> 0 | err
#define SYS_ACCESS       21   // access(path, mode)               -> 0 | err
#define SYS_PIPE         22   // pipe(fd[2])                      -> 0 | err
#define SYS_SELECT       23   // select(nfds,rd,wr,ex,tv)         -> nready | err
#define SYS_SCHED_YIELD  24   // sched_yield() / AscentOS yield() -> 0  [Linux: sched_yield]
#define SYS_YIELD        SYS_SCHED_YIELD  // takma ad
#define SYS_DUP          32   // dup(fd)                          -> newfd | err
#define SYS_DUP2         33   // dup2(oldfd, newfd)               -> newfd | err
#define SYS_NANOSLEEP    35   // nanosleep(*req, *rem)            -> 0 | err
#define SYS_SLEEP        406  // sleep(ticks) — AscentOS özel     -> 0 [AscentOS özel]
#define SYS_ALARM        37   // alarm(seconds)                   -> kalan_sn
#define SYS_GETPID       39   // getpid()                         -> pid
#define SYS_FORK         57   // fork()                           -> child_pid | 0 | err
#define SYS_EXECVE       59   // execve(path, argv, envp)         -> err (dönmez)
#define SYS_EXIT         60   // exit(code)                       -> noreturn
#define SYS_WAITPID      61   // wait4(pid,*st,opt,*ru) → waitpid -> waited_pid | err
#define SYS_KILL         62   // kill(pid, sig)                   -> 0 | err
#define SYS_UNAME        63   // uname(*utsname_t)                -> 0 | err
#define SYS_FCNTL        72   // fcntl(fd, cmd, arg)              -> val | err
#define SYS_TRUNCATE     76   // truncate(path, length)           -> 0 | err
#define SYS_FTRUNCATE    77   // ftruncate(fd, length)            -> 0 | err
#define SYS_GETCWD       79   // getcwd(buf, size)                -> buf | NULL
#define SYS_CHDIR        80   // chdir(path)                      -> 0 | err
#define SYS_RENAME       82   // rename(oldpath, newpath)         -> 0 | err
#define SYS_MKDIR        83   // mkdir(path, mode)                -> 0 | err
#define SYS_RMDIR        84   // rmdir(path)                      -> 0 | err
#define SYS_LINK         86   // link(oldpath, newpath)           -> 0 | err
#define SYS_UNLINK       87   // unlink(path)                     -> 0 | err
#define SYS_SYMLINK      88   // symlink(target, linkpath)        -> 0 | err
#define SYS_READLINK     89   // readlink(path, buf, size)        -> nbytes | err
#define SYS_CHMOD        90   // chmod(path, mode)                -> 0 | err
#define SYS_GETUID       102  // getuid()                         -> uid
#define SYS_SYSLOG       103  // syslog / AscentOS debug(msg)     -> 0
#define SYS_DEBUG        SYS_SYSLOG  // takma ad
#define SYS_GETGID       104  // getgid()                         -> gid
#define SYS_SETPGID      109  // setpgid(pid, pgid)               -> 0 | err
#define SYS_TIMES        100  // times(*tms)                      -> ticks | err
#define SYS_GETPPID      110  // getppid()                        -> parent_pid
#define SYS_GETPGRP      111  // getpgrp() (takma ad: getpgid(0)) -> pgid
#define SYS_SETSID       112  // setsid()                         -> new_sid | err
#define SYS_GETGROUPS    115  // getgroups(size, list[])          -> ngroups | err
#define SYS_SETGROUPS    116  // setgroups (gelecekte)
#define SYS_GETEUID      107  // geteuid()                        -> euid
#define SYS_GETEGID      108  // getegid()                        -> egid
#define SYS_SIGALTSTACK  131  // sigaltstack(*ss, *old_ss)        -> 0 | err
#define SYS_GETDENTS     78   // getdents(dirfd, buf, count)      -> nbytes | err
#define SYS_GETTIMEOFDAY 96   // gettimeofday(*tv, *tz)           -> 0 | err
#define SYS_GETRLIMIT    97   // getrlimit(resource, *rlimit)     -> 0 | err
#define SYS_SETRLIMIT    160  // setrlimit(resource, *rlimit)     -> 0 | err
#define SYS_SYSINFO      99   // sysinfo / AscentOS uptime()      -> system_ticks  [Linux: sysinfo]
#define SYS_UPTIME       SYS_SYSINFO  // takma ad
#define SYS_UMASK        95   // umask(mode)                      -> old_mask
#define SYS_SIGPENDING   127  // sigpending(*set)                 -> 0 | err
#define SYS_SIGSUSPEND   130  // sigsuspend(*mask)                -> 0 | err
#define SYS_PIPE2        293  // pipe2(pipefd[2], flags)          -> 0 | err
#define SYS_CLOCK_GETTIME 228 // clock_gettime(clockid, *timespec)-> 0 | err
#define SYS_CLOCK_GETRES  229 // clock_getres(clockid, *timespec) -> 0 | err
#define SYS_GETPGID      121  // getpgid(pid)                     -> pgid | err
#define SYS_FUTEX        202  // futex(uaddr,op,val,timeout,uaddr2,val3) -> 0|waiters|err  [Linux 202]
#define SYS_GETRANDOM    318  // getrandom(buf, buflen, flags)    -> bytes_read | err       [Linux 318]
#define SYS_ARCH_PRCTL    158 // arch_prctl(code, addr)           -> 0 | err                [Linux 158]
#define SYS_CLONE          56 // clone(flags,stack,ptid,ctid,tls) -> child_pid | 0 | err    [Linux 56]
#define SYS_SET_TID_ADDRESS 218 // set_tid_address(tidptr)        -> tid                    [Linux 218]
#define SYS_SET_ROBUST_LIST 273 // set_robust_list(head, len)     -> 0 | err                [Linux 273]

// ── musl libc başlatma için gerekli syscall'lar ───────────────────────────
#define SYS_WRITEV        20  // writev(fd, iov, iovcnt)          -> bytes_written | err    [Linux 20]
#define SYS_MADVISE       28  // madvise(addr, len, advice)       -> 0 | err                [Linux 28]
#define SYS_EXIT_GROUP   231  // exit_group(code)                 -> noreturn               [Linux 231]
#define SYS_OPENAT       257  // openat(dirfd, path, flags, mode) -> fd | err               [Linux 257]
#define SYS_NEWFSTATAT   262  // newfstatat(dirfd,path,*stat,flg) -> 0 | err                [Linux 262]
#define SYS_PRLIMIT64    302  // prlimit64(pid,res,*new,*old)     -> 0 | err                [Linux 302]

// tcsetpgrp / tcgetpgrp — Linux'ta ioctl(TIOCSPGRP/TIOCGPGRP) ile yapılır;
// AscentOS convenience syscall'ları yüksek numarada tutulur.
#define SYS_TCSETPGRP    400  // tcsetpgrp(fd, pgrp)              -> 0 | err   [AscentOS özel]
#define SYS_TCGETPGRP    401  // tcgetpgrp(fd)                    -> pgrp | err [AscentOS özel]

// opendir / closedir — Linux'ta userspace libc wrapper'ı; AscentOS kernel'da tutuyor.
#define SYS_OPENDIR      402  // opendir(path)                    -> dirfd  | err [AscentOS özel]
#define SYS_CLOSEDIR     403  // closedir(dirfd)                  -> 0      | err [AscentOS özel]

// AscentOS'a özgü: doğrudan zamanlayıcı tick'i
#define SYS_GETTICKS     404  // getticks()                       -> ticks        [AscentOS özel]

// AscentOS'a özgü: görev önceliği
#define SYS_GETPRIORITY  140  // getpriority(which, who)          -> priority  [Linux 140 ile aynı!]
#define SYS_SETPRIORITY  141  // setpriority(which, who, prio)    -> 0 | err   [Linux 141 ile aynı!]

// sbrk — Linux'ta yoktur (brk() ile yapılır); newlib uyumu için AscentOS özel alanda.
#define SYS_SBRK         405  // sbrk(increment)                  -> old_brk | err [AscentOS özel]

// clock_gettime() / clock_getres() clockid değerleri
#define CLOCK_REALTIME            0   // Sistem saati (epoch'tan itibaren)
#define CLOCK_MONOTONIC           1   // Monoton saat (önyüklemeden itibaren, geriye gitmez)
#define CLOCK_PROCESS_CPUTIME_ID  2   // Bu process'in CPU süresi (stub: nanosleep gibi)
#define CLOCK_THREAD_CPUTIME_ID   3   // Bu thread'in CPU süresi  (stub)

// ============================================================
// futex() — Linux x86-64 uyumlu futex işlem sabitleri  (SYS_FUTEX = 202)
//
// Temel kullanım: futex(uaddr, FUTEX_WAIT, val, timeout, NULL, 0)
//                 futex(uaddr, FUTEX_WAKE, nwake, NULL, NULL, 0)
// ============================================================
#define FUTEX_WAIT          0    // *uaddr == val ise uyut
#define FUTEX_WAKE          1    // En fazla val kadar thread'i uyandır
#define FUTEX_FD            2    // (eski, kullanımdan kalktı)
#define FUTEX_REQUEUE       3    // Kuyruğu başka bir futex'e taşı
#define FUTEX_CMP_REQUEUE   4    // Koşullu requeue
#define FUTEX_WAKE_OP       5    // Atomik OP + wake

#define FUTEX_PRIVATE_FLAG  128  // Sadece bu process'in thread'leri için
#define FUTEX_CLOCK_REALTIME 256 // timeout için CLOCK_REALTIME kullan

// Yaygın bileşik işlemler
#define FUTEX_WAIT_PRIVATE  (FUTEX_WAIT | FUTEX_PRIVATE_FLAG)
#define FUTEX_WAKE_PRIVATE  (FUTEX_WAKE | FUTEX_PRIVATE_FLAG)

// ============================================================
// getrandom() — Linux x86-64 uyumlu rastgele veri sabitleri  (SYS_GETRANDOM = 318)
//
// Kullanım: getrandom(buf, buflen, flags) -> bytes_read | err
// ============================================================
#define GRND_NONBLOCK  0x0001   // Bloklamak yerine EAGAIN döndür
#define GRND_RANDOM    0x0002   // /dev/random kalite entropisi kullan

// ============================================================
// writev() — scatter/gather I/O  (SYS_WRITEV = 20)
// ============================================================
typedef struct {
    void*    iov_base;   // Tampon başlangıcı
    uint64_t iov_len;    // Tampon uzunluğu
} iovec_t;

// ============================================================
// madvise() — bellek kullanım tavsiyesi  (SYS_MADVISE = 28)
// musl malloc/free tarafından çağrılır; çoğu tavsiye stub olarak 0 döner.
// ============================================================
#define MADV_NORMAL       0   // Varsayılan davranış
#define MADV_RANDOM       1   // Rastgele erişim bekleniyor
#define MADV_SEQUENTIAL   2   // Sıralı erişim bekleniyor
#define MADV_WILLNEED     3   // Yakında kullanılacak — prefetch
#define MADV_DONTNEED     4   // Artık gerekmez — serbest bırak
#define MADV_FREE         8   // Sayfa içeriği önemsiz (Linux 4.5+)
#define MADV_HUGEPAGE    14   // THP tercih et
#define MADV_NOHUGEPAGE  15   // THP kullanma

// ============================================================
// openat() — dizin-göreceli dosya açma  (SYS_OPENAT = 257)
// musl open() wrapper'ı her zaman openat(AT_FDCWD, ...) çağırır.
// ============================================================
#define AT_FDCWD          (-100)  // Geçerli çalışma dizinini temsil eder
#define AT_SYMLINK_NOFOLLOW 0x100 // Sembolik bağları takip etme
#define AT_REMOVEDIR      0x200   // rmdir gibi davran (unlinkat için)
#define AT_EACCESS        0x200   // faccessat: gerçek yerine etkin UID kullan

// ============================================================
// newfstatat() — dizin-göreceli stat  (SYS_NEWFSTATAT = 262)
// musl stat() wrapper'ı bunu çağırır.
// ============================================================
#define AT_EMPTY_PATH     0x1000  // boş path → dirfd'nin kendisini stat et

// ============================================================
// prlimit64() — işlem kaynak limiti al/set  (SYS_PRLIMIT64 = 302)
// musl getrlimit/setrlimit yerine bunu tercih eder.
// ============================================================
typedef struct {
    uint64_t rlim_cur;   // Soft limit
    uint64_t rlim_max;   // Hard limit
} rlimit64_t;

// ============================================================
// set_tid_address — musl __init_tp() tarafından ilk çağrılan syscall  (SYS_SET_TID_ADDRESS = 218)
//
// musl, thread pointer (TP) kurulumu sırasında kernel'e clear_child_tid
// adresini bildirir. Thread çıkışında kernel *tidptr = 0 yazar ve
// futex_wake ile bekleyenleri uyandırır.
//
// AscentOS implementasyonu:
//   - tidptr task_t içinde saklanır
//   - Dönüş: mevcut task'ın TID (PID) değeri
// ============================================================

// ============================================================
// set_robust_list — pthreads mutex recovery  (SYS_SET_ROBUST_LIST = 273)
//
// musl her thread için kernel'e robust futex listesi başını bildirir.
// Thread anormal çıkışında kernel listedeki futex'leri otomatik release eder.
//
// AscentOS implementasyonu: liste adresi kaydedilir, 0 döner.
// (Gerçek robust futex işlemi gelecek; şimdilik kayıt + ENOSYS yerine 0.)
// ============================================================
#define ROBUST_LIST_HEAD_SIZE  24   // sizeof(struct robust_list_head) — 64-bit

// ============================================================
// arch_prctl() — x86-64'e özgü işlem/thread kontrol  (SYS_ARCH_PRCTL = 158)
//
// Temel kullanım: arch_prctl(ARCH_SET_FS, addr) → FS.base = addr
//                 arch_prctl(ARCH_GET_FS, *addr) → *addr = FS.base
// ============================================================
#define ARCH_SET_GS        0x1001   // GS.base yaz
#define ARCH_SET_FS        0x1002   // FS.base yaz  (TLS pointer — en yaygın kullanım)
#define ARCH_GET_FS        0x1003   // FS.base oku
#define ARCH_GET_GS        0x1004   // GS.base oku
#define ARCH_GET_CPUID     0x1011   // CPUID izni sorgula
#define ARCH_SET_CPUID     0x1012   // CPUID iznini aç/kapat

// ============================================================
// clone() — Linux x86-64 uyumlu thread/process oluşturma  (SYS_CLONE = 56)
//
// Kullanım: clone(flags, child_stack, ptid, ctid, tls) -> child_pid | 0 | err
// AscentOS stub: CLONE_THREAD içermeyen çağrılar → fork() gibi davranır.
// CLONE_THREAD içerenlerde TLS kurulumu yapılır, scheduler'a yeni task eklenir.
// ============================================================
#define CLONE_VM           0x00000100  // VM'i paylaş (thread)
#define CLONE_FS           0x00000200  // Dosya sistemi bilgisini paylaş
#define CLONE_FILES        0x00000400  // fd tablosunu paylaş
#define CLONE_SIGHAND      0x00000800  // Sinyal handler'larını paylaş
#define CLONE_PTRACE       0x00002000  // Trace'i miras al
#define CLONE_VFORK        0x00004000  // Parent MM kilidi (vfork semantiği)
#define CLONE_PARENT       0x00008000  // Aynı parent'ı paylaş
#define CLONE_THREAD       0x00010000  // Aynı thread grubuna gir
#define CLONE_NEWNS        0x00020000  // Yeni mount namespace
#define CLONE_SYSVSEM      0x00040000  // SysV semaphore listesini paylaş
#define CLONE_SETTLS       0x00080000  // TLS'i R8'deki değerle kur (FS.base)
#define CLONE_PARENT_SETTID 0x00100000 // Parent'ta *ptid = child_tid
#define CLONE_CHILD_CLEARTID 0x00200000 // Child çıkışında *ctid = 0 + futex_wake
#define CLONE_DETACHED     0x00400000  // (kullanımdan kalktı)
#define CLONE_UNTRACED     0x00800000  // CLONE_PTRACE zorlanamaz
#define CLONE_CHILD_SETTID 0x01000000  // Child'da *ctid = child_tid
#define CLONE_NEWCGROUP    0x02000000  // Yeni cgroup namespace
#define CLONE_NEWUTS       0x04000000  // Yeni UTS namespace
#define CLONE_NEWIPC       0x08000000  // Yeni IPC namespace
#define CLONE_NEWUSER      0x10000000  // Yeni kullanıcı namespace
#define CLONE_NEWPID       0x20000000  // Yeni PID namespace
#define CLONE_NEWNET       0x40000000  // Yeni ağ namespace
#define CLONE_IO           0x80000000  // I/O context kopyala

// ============================================================
// getrlimit() / setrlimit() — kaynak limit yapısı ve sabitleri
// ============================================================
typedef struct {
    uint64_t rlim_cur;   // Soft limit (uyarı eşiği)
    uint64_t rlim_max;   // Hard limit (üst tavan)
} rlimit_t;

#define RLIM_INFINITY    ((uint64_t)-1)   // Sınırsız

// resource sabitleri (Linux x86-64 uyumlu numaralar)
#define RLIMIT_CPU        0   // CPU süresi (saniye)
#define RLIMIT_FSIZE      1   // Maksimum dosya boyutu (byte)
#define RLIMIT_DATA       2   // Veri segmenti boyutu
#define RLIMIT_STACK      3   // Stack boyutu
#define RLIMIT_CORE       4   // Core dump boyutu
#define RLIMIT_RSS        5   // Resident set size
#define RLIMIT_NPROC      6   // Maksimum process sayısı
#define RLIMIT_NOFILE     7   // Maksimum açık fd sayısı  ← bash bunu sorgular
#define RLIMIT_MEMLOCK    8   // Kilitlenebilir bellek
#define RLIMIT_AS         9   // Sanal adres alanı
#define RLIMIT_LOCKS     10   // Dosya kilitleri
#define RLIMIT_SIGPENDING 11  // Bekleyen sinyal sayısı
#define RLIMIT_MSGQUEUE  12   // POSIX mesaj kuyruğu bayt sayısı
#define RLIMIT_NICE      13   // nice değeri tavanı
#define RLIMIT_RTPRIO    14   // Gerçek zamanlı öncelik tavanı
#define RLIMIT_NLIMITS   15   // Toplam kaynak sayısı

// ============================================================
// times() — POSIX işlem süresi yapısı  (SYS_TIMES)
// Tüm alanlar clock tick cinsindendir (CLK_TCK = 100 Hz varsayılan).
// newlib _times() ve bash 'time' builtin bu struct'ı kullanır.
// ============================================================
typedef struct {
    uint64_t tms_utime;   // Kullanıcı modu CPU süresi
    uint64_t tms_stime;   // Kernel modu CPU süresi
    uint64_t tms_cutime;  // Beklenmiş çocukların kullanıcı süresi
    uint64_t tms_cstime;  // Beklenmiş çocukların kernel süresi
} tms_t;

#define CLK_TCK  100   // Saniyedeki tick sayısı (getconf CLK_TCK uyumlu)

// access() mod bitleri
#define F_OK   0   // dosya var mı?
#define R_OK   4   // okuma izni var mı?
#define W_OK   2   // yazma izni var mı?
#define X_OK   1   // çalıştırma izni var mı?

// ── Ağ / Socket syscall'ları  (Linux x86-64 numaraları) ─────────────────────
#define SYS_SOCKET       41   // socket(domain, type, protocol)          -> fd | err
#define SYS_CONNECT      42   // connect(fd, *addr, addrlen)             -> 0  | err
#define SYS_ACCEPT       43   // accept(fd, *addr, *addrlen)             -> fd | err
#define SYS_SENDTO       44   // sendto(fd,buf,len,flags,addr,addrlen)   -> sent | err
#define SYS_RECVFROM     45   // recvfrom(fd,buf,len,flags,addr,*alen)   -> recv | err
#define SYS_SENDMSG      46   // sendmsg(fd, *msghdr, flags)             -> sent | err
#define SYS_RECVMSG      47   // recvmsg(fd, *msghdr, flags)             -> recv | err
#define SYS_SHUTDOWN     48   // shutdown(fd, how)                       -> 0  | err
#define SYS_BIND         49   // bind(fd, *addr, addrlen)                -> 0  | err
#define SYS_LISTEN       50   // listen(fd, backlog)                     -> 0  | err
#define SYS_GETSOCKNAME  51   // getsockname(fd, *addr, *len)            -> 0  | err
#define SYS_GETPEERNAME  52   // getpeername(fd, *addr, *len)            -> 0  | err
#define SYS_SETSOCKOPT   54   // setsockopt(fd,lvl,opt,*val,len)         -> 0  | err
#define SYS_GETSOCKOPT   55   // getsockopt(fd,lvl,opt,*val,*len)        -> 0  | err

// AscentOS özel syscall'lar 400-409 aralığında; Linux syscall'ları max ~450'de bitiyor.
#define SYS_FB_INFO      407  // fb_info(fb_info_t* out)        -> 0 | err  [AscentOS özel]
#define SYS_KB_RAW       408  // kb_raw(int enable)             -> 0 | err  [AscentOS özel]
#define SYS_FB_BLIT      409  // fb_blit(ascent_fb_blit_t* req) -> 0 | err  [AscentOS özel]
#define SYSCALL_MAX      410  // AscentOS özel alan üst sınırı

// ============================================================
// fb_info_t — SYS_FB_INFO dönüş struct'ı
// Kernel → user-space framebuffer bilgisi aktarımı.
// vesa64.c extern'leriyle birebir eşleşir.
// ============================================================
// v4: Tüm field'lar uint64_t — hizalama garantili, mixed-type sorunları yok
typedef struct {
    uint64_t addr;      // Framebuffer fiziksel/lineer adresi
    uint64_t width;     // Pixel cinsinden genişlik
    uint64_t height;    // Pixel cinsinden yükseklik
    uint64_t pitch;     // Satır başına byte
    uint64_t bpp;       // Bit per pixel (genellikle 32)
} fb_info_t;

// ============================================================
// ascent_fb_blit_t — SYS_FB_BLIT istek struct'ı  (v4: tüm field'lar uint64_t)
// User-space Doom buffer'ını kernel VRAM'ına kopyalatır.
// ============================================================
typedef struct {
    uint64_t src_pixels;  // user-space uint32_t* (Doom XRGB8888 buffer)
    uint64_t src_w;       // kaynak genişlik  (tipik: 320)
    uint64_t src_h;       // kaynak yükseklik (tipik: 200)
    uint64_t dst_x;       // hedef x ofseti
    uint64_t dst_y;       // hedef y ofseti
    uint64_t scale;       // büyütme çarpanı: 1, 2 veya 3
} ascent_fb_blit_t;

// ============================================================
// kill() sinyal numaraları (POSIX alt kümesi)
// signal64.h dahil edilmemişse buradaki tanımlar geçerlidir;
// signal64.h dahil edilmişse oradaki kapsamlı NSIG=32 tanımları kullanılır.
// ============================================================
#ifndef SIGNAL64_H
#define SIGTERM   15   // Nazikçe sonlandır
#define SIGKILL    9   // Zorla sonlandır (yakalanamaz)
#define SIGINT     2   // Klavye interrupt (^C)
#define SIGHUP     1   // Hang-up
#define SIGPIPE   13   // Yazma ucuna kimse okumuyorsa
#define SIGCHLD   17   // Çocuk durumu değişti
#define SIGALRM   14   // Zamanlayıcı
#define SIGUSR1   10   // Kullanıcı tanımlı 1
#define SIGUSR2   12   // Kullanıcı tanımlı 2
#define SIGWINCH  28   // Terminal pencere boyutu değişti (readline/bash için)
#endif /* SIGNAL64_H */

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
// v5: MAP_FILE + MAP_SHARED/MAP_PRIVATE artık dosya haritalama
//     için destekleniyor. fd açık bir dosyaya işaret etmeli;
//     offset 512 byte sektör sınırına hizalanmış olmalı.
//     Gerçek sayfa tablosu yok; içerik heap'e okunur (flat copy).
// ============================================================
#define MAP_SHARED       0x01   // Paylaşımlı haritalama (dosya: yazma geri yazar)
#define MAP_PRIVATE      0x02   // Özel kopya (copy-on-write; dosya değişmez)
#define MAP_ANONYMOUS    0x20   // Dosya değil; bellekten tahsis et
#define MAP_ANON         MAP_ANONYMOUS
#define MAP_FIXED        0x10   // addr tam bu adrese haritalansın
#define MAP_FILE         0x00   // Dosya haritalama (MAP_ANONYMOUS yokken varsayılan)
#define MAP_FAILED       ((void*)(uint64_t)-1)   // hata dönüş değeri

// ============================================================
// select() yapıları ve sabitleri  (SYS_SELECT)
// ============================================================
#define FD_SETSIZE   32   // MAX_FDS ile eşleşmeli

typedef struct {
    uint32_t fds_bits[FD_SETSIZE / 32];  // Her bit bir fd'yi temsil eder
} fd_set_t;

// fd_set makroları
#define FD_ZERO(s)       ((s)->fds_bits[0] = 0)
#define FD_SET(fd, s)    ((s)->fds_bits[0] |=  (1u << (fd)))
#define FD_CLR(fd, s)    ((s)->fds_bits[0] &= ~(1u << (fd)))
#define FD_ISSET(fd, s)  ((s)->fds_bits[0] &   (1u << (fd)))

// timeval: select() zaman aşımı
typedef struct {
    int64_t tv_sec;    // Saniye
    int64_t tv_usec;   // Mikrosaniye
} timeval_t;

// ============================================================
// poll() yapıları ve sabitleri  (SYS_POLL)
// ============================================================

// pollfd: tek bir fd'nin izleme isteği
typedef struct {
    int32_t  fd;        // İzlenecek fd (-1 = atla)
    int16_t  events;    // İstenen olaylar (POLLIN | POLLOUT | …)
    int16_t  revents;   // Gerçekleşen olaylar (kernel doldurur)
} pollfd_t;

// poll olayları (events / revents bitleri)
#define POLLIN       0x0001   // Okunabilir veri var
#define POLLPRI      0x0002   // Yüksek öncelikli veri (OOB)
#define POLLOUT      0x0004   // Yazma bloklanmayacak
#define POLLERR      0x0008   // Hata durumu (sadece revents)
#define POLLHUP      0x0010   // Bağlantı kapandı (sadece revents)
#define POLLNVAL     0x0020   // Geçersiz fd (sadece revents)
#define POLLRDHUP    0x2000   // Karşı taraf kapandı (pipe EOF)


#define WNOHANG          0x01   // Bloklanmadan döner; PID bitmemişse 0
#define WUNTRACED        0x02   // Durdurulmuş çocukları da raporla
#define WCONTINUED       0x08   // SIGCONT ile devam eden çocukları raporla (bash job control)

// waitpid status decode makroları
#define WIFEXITED(s)     (((s) & 0xFF) == 0)
#define WEXITSTATUS(s)   (((s) >> 8) & 0xFF)
#define WIFSIGNALED(s)   (((s) & 0x7F) != 0 && ((s) & 0x7F) != 0x7F)
#define WTERMSIG(s)      ((s) & 0x7F)
#define WIFSTOPPED(s)    (((s) & 0xFF) == 0x7F)                  // SIGTSTP/SIGSTOP ile durdu
#define WSTOPSIG(s)      (((s) >> 8) & 0xFF)                     // Durduran sinyal numarası
#define WIFCONTINUED(s)  ((s) == 0xFFFF)                         // SIGCONT ile devam etti

// ============================================================
// lseek() whence değerleri  (SYS_LSEEK)
// ============================================================
#define SEEK_SET    0   // Dosyanın başından
#define SEEK_CUR    1   // Mevcut konumdan
#define SEEK_END    2   // Dosyanın sonundan

// ============================================================
// ioctl() istekleri  (SYS_IOCTL)
//
// Terminal (termios) istekleri:
//   TCGETS  – mevcut termios ayarlarını al
//   TCSETS  – termios ayarlarını hemen uygula
//   TCSETSW – termios ayarlarını çıktı boşalınca uygula
//   TCSETSF – termios ayarlarını uygula, giriş tamponunu temizle
// ============================================================
#define TCGETS       0x5401
#define TCSETS       0x5402
#define TCSETSW      0x5403
#define TCSETSF      0x5404

// Diğer yaygın ioctl istekleri
#define TIOCGWINSZ   0x5413   // Terminal pencere boyutunu sorgula
#define TIOCSWINSZ   0x5414   // Terminal pencere boyutunu ayarla
#define FIONREAD     0x541B   // Okunabilir byte sayısını döndür
#define TIOCGPGRP    0x540F   // Ön plan süreç grubunu al
#define TIOCSPGRP    0x5410   // Ön plan süreç grubunu ayarla
#define TIOCSCTTY    0x540E   // Controlling terminal olarak ata (setsid() sonrası)
#define TIOCNOTTY    0x5422   // Controlling terminal'den ayrıl
#define TIOCSTI      0x5412   // Terminale karakter enjekte et (nadiren)

// tcsetattr() when sabitleri — TCSETS/TCSETSW/TCSETSF ile eşleşir
#define TCSANOW      0        // Hemen uygula           → TCSETS
#define TCSADRAIN    1        // Output boşalınca uygula → TCSETSW
#define TCSAFLUSH    2        // Flush + uygula          → TCSETSF

// ============================================================
// termios yapısı  (POSIX.1 uyumlu)
//
// c_iflag – giriş modu bayrakları
// c_oflag – çıkış modu bayrakları
// c_cflag – kontrol modu bayrakları
// c_lflag – yerel modu bayrakları
// c_cc    – kontrol karakterleri (NCCS adet)
// ============================================================
#define NCCS        19  // Kontrol karakter dizisinin boyutu

typedef struct {
    uint32_t c_iflag;       // Giriş modu
    uint32_t c_oflag;       // Çıkış modu
    uint32_t c_cflag;       // Kontrol modu
    uint32_t c_lflag;       // Yerel mod
    uint8_t  c_line;        // Satır disiplini
    uint8_t  c_cc[NCCS];   // Kontrol karakterleri
    uint32_t c_ispeed;      // Giriş baud hızı
    uint32_t c_ospeed;      // Çıkış baud hızı
} termios_t;

// c_iflag bitleri
#define IGNBRK   0x0001   // Break sinyalini yoksay
#define BRKINT   0x0002   // Break'te SIGINT
#define IGNPAR   0x0004   // Parite hatalarını yoksay
#define PARMRK   0x0008   // Parite hatalarını işaretle
#define INPCK    0x0010   // Giriş parite kontrolünü etkinleştir
#define ISTRIP   0x0020   // 8. biti sıyır
#define INLCR    0x0040   // NL'yi CR'ye dönüştür
#define IGNCR    0x0080   // CR'yi yoksay
#define ICRNL    0x0100   // CR'yi NL'ye dönüştür (varsayılan açık)
#define IXON     0x0400   // XON/XOFF çıkış akış kontrolü
#define IXOFF    0x1000   // XON/XOFF giriş akış kontrolü
#define IXANY    0x0800   // Herhangi bir karakter akışı yeniden başlatır

// c_oflag bitleri
#define OPOST    0x0001   // Çıkış işlemeyi etkinleştir
#define ONLCR    0x0004   // NL'yi CR+NL'ye dönüştür

// c_cflag bitleri (baud hızı maskeleri hariç)
#define CS5      0x0000   // 5 bit karakter boyutu
#define CS6      0x0010   // 6 bit
#define CS7      0x0020   // 7 bit
#define CS8      0x0030   // 8 bit (varsayılan)
#define CSIZE    0x0030   // Karakter boyutu maskesi
#define CSTOPB   0x0040   // 2 durdurma biti; aksi halde 1
#define CREAD    0x0080   // Alıcıyı etkinleştir
#define PARENB   0x0100   // Pariteyi etkinleştir
#define PARODD   0x0200   // Tek parite; aksi halde çift
#define HUPCL    0x0400   // Son kapat'ta hat düşür
#define CLOCAL   0x0800   // Modem durum satırlarını yoksay

// c_lflag bitleri – terminal davranışı
#define ISIG     0x0001   // Sinyaller üret (SIGINT, SIGQUIT…)
#define ICANON   0x0002   // Kanonik mod (satır bazlı okuma)
#define ECHO     0x0008   // Giriş karakterlerini yankıla
#define ECHOE    0x0010   // Silme karakterini ERASE olarak yankıla
#define ECHOK    0x0020   // KILL karakterinde NL yankıla
#define ECHONL   0x0040   // ICANON açıkken NL'yi yankıla
#define NOFLSH   0x0080   // Sinyal sonrası tamponu temizleme
#define TOSTOP   0x0100   // Arkaplan yazma girişimlerinde SIGTTOU
#define ECHOCTL  0x0200   // Kontrol karakterlerini ^X biçiminde göster (^C, ^Z vb.)
#define IEXTEN   0x8000   // Genişletilmiş giriş işlemeyi etkinleştir

// c_cc dizisi indeksleri (kontrol karakterleri)
#define VINTR    0    // Interrupt  (^C)
#define VQUIT    1    // Quit       (^\)
#define VERASE   2    // Erase      (^H / Backspace)
#define VKILL    3    // Kill line  (^U)
#define VEOF     4    // EOF        (^D)
#define VTIME    5    // Min zaman  (non-canon)
#define VMIN     6    // Min karakter sayısı (non-canon)
#define VSWTC    7    // Switch (genellikle \0)
#define VSTART   8    // Start akış (^Q)
#define VSTOP    9    // Stop akış  (^S)
#define VSUSP    10   // Suspend    (^Z)
#define VEOL     11   // EOL        (ek satır sonu)
#define VREPRINT 12   // Reprint    (^R)
#define VDISCARD 13   // Discard    (^O)
#define VWERASE  14   // Word erase (^W)
#define VLNEXT   15   // Literal    (^V)
#define VEOL2    16   // EOL2       (ek satır sonu 2)

// Yaygın terminal baud hız sabitleri (c_ispeed / c_ospeed)
#define B0       0
#define B9600    9600
#define B19200   19200
#define B38400   38400
#define B57600   57600
#define B115200  115200

// ============================================================
// winsize yapısı  (TIOCGWINSZ / TIOCSWINSZ)
// ============================================================
typedef struct {
    uint16_t ws_row;    // Satır sayısı
    uint16_t ws_col;    // Sütun sayısı
    uint16_t ws_xpixel; // Yatay piksel (genellikle 0)
    uint16_t ws_ypixel; // Dikey piksel  (genellikle 0)
} winsize_t;

// ============================================================
// utsname yapısı  (SYS_UNAME)
//
// bash bu bilgiyi $MACHTYPE, $HOSTTYPE, $OSTYPE env değişkenlerine
// ve PS1 içindeki \s (shell adı) / \v (versiyon) escape'lerine yazar.
// ============================================================
#define UTS_LEN     65   // Her alan max 64 karakter + null terminator

typedef struct {
    char sysname [UTS_LEN];   // İşletim sistemi adı  → "AscentOS"
    char nodename[UTS_LEN];   // Hostname             → "ascent"
    char release [UTS_LEN];   // Kernel sürümü        → "1.0.0"
    char version [UTS_LEN];   // Build bilgisi        → "#1 SMP ..."
    char machine [UTS_LEN];   // Donanım mimarisi     → "x86_64"
} utsname_t;

// ============================================================
// timespec yapısı  (SYS_NANOSLEEP, SYS_CLOCK_GETTIME)
// ============================================================
typedef struct {
    int64_t tv_sec;    // Saniye
    int64_t tv_nsec;   // Nanosaniye (0..999_999_999)
} timespec_t;

// ============================================================
// Socket — adres ailesi ve tür sabitleri
// ============================================================
#define AF_UNSPEC     0
#define AF_UNIX       1    // Unix domain socket  (/tmp/.X11-unix/X0 vb.)
#define AF_LOCAL      AF_UNIX
#define AF_INET       2    // IPv4
#define AF_INET6      10   // IPv6

#define SOCK_STREAM   1    // Bağlantı odaklı (TCP / Unix stream)
#define SOCK_DGRAM    2    // Bağlantısız (UDP / Unix datagram)
#define SOCK_NONBLOCK 0x800   // O_NONBLOCK ile aynı değer
#define SOCK_CLOEXEC  0x80000 // O_CLOEXEC ile aynı değer

// shutdown() how değerleri
#define SHUT_RD   0   // Okuma tarafını kapat
#define SHUT_WR   1   // Yazma tarafını kapat
#define SHUT_RDWR 2   // Her ikisini kapat

// setsockopt / getsockopt seviye
#define SOL_SOCKET  1

// SOL_SOCKET seçenek isimleri
#define SO_DEBUG        1
#define SO_REUSEADDR    2
#define SO_TYPE         3
#define SO_ERROR        4
#define SO_SNDBUF       7
#define SO_RCVBUF       8
#define SO_KEEPALIVE    9
#define SO_RCVTIMEO     20
#define SO_SNDTIMEO     21
#define SO_PASSCRED     16   // Unix socket: kimlik bilgisi gönder
#define SO_PEERCRED     17   // Unix socket: karşı tarafın kimliği

// ── sockaddr yapıları ─────────────────────────────────────
typedef struct {
    uint16_t sa_family;   // AF_* değeri
    char     sa_data[14]; // Adres verisi (aile bağımlı)
} sockaddr_t;

// Unix domain socket adresi
#define UNIX_PATH_MAX  108
typedef struct {
    uint16_t sun_family;             // AF_UNIX
    char     sun_path[UNIX_PATH_MAX]; // Socket dosya yolu
} sockaddr_un_t;

// IPv4 socket adresi
typedef struct {
    uint16_t sin_family;  // AF_INET
    uint16_t sin_port;    // Port (network byte order)
    uint32_t sin_addr;    // IPv4 adresi
    uint8_t  sin_zero[8]; // Dolgu
} sockaddr_in_t;

// msghdr — sendmsg/recvmsg için
typedef struct {
    void*    msg_name;       // Hedef adres (bağlantısız için)
    uint32_t msg_namelen;    // Adres uzunluğu
    iovec_t* msg_iov;        // Scatter/gather tampon dizisi
    uint64_t msg_iovlen;     // iov eleman sayısı
    void*    msg_control;    // Yardımcı veri (SCM_RIGHTS vb.)
    uint64_t msg_controllen; // Yardımcı veri uzunluğu
    int32_t  msg_flags;      // Alınan mesaj bayrakları
} msghdr_t;

// ============================================================
// stack_t yapısı  (SYS_SIGALTSTACK)
//
// bash SIGSEGV handler'ı için alternate signal stack kurar.
// ss_flags: SS_ONSTACK (1) = şu an alt-stack'te, SS_DISABLE (2) = devre dışı
// ============================================================
#define SS_ONSTACK   1
#define SS_DISABLE   2
#define MINSIGSTKSZ  2048    // Minimum alternate stack boyutu
#define SIGSTKSZ     8192    // Önerilen alternate stack boyutu

typedef struct {
    void*    ss_sp;      // Stack başlangıç adresi
    uint32_t ss_flags;   // SS_ONSTACK | SS_DISABLE
    uint64_t ss_size;    // Stack boyutu (byte)
} stack_t;

// ============================================================
// stat yapısı  (SYS_FSTAT)
//
// POSIX stat(2) alt kümesi; dosya meta verilerini tutar.
// ============================================================
typedef struct {
    uint32_t st_dev;        // Aygıt numarası
    uint32_t st_ino;        // İnode numarası
    uint32_t st_mode;       // Dosya türü ve izinleri
    uint32_t st_nlink;      // Hard link sayısı
    uint32_t st_uid;        // Sahibin kullanıcı kimliği
    uint32_t st_gid;        // Sahibin grup kimliği
    uint32_t st_rdev;       // Aygıt kimliği (özel dosyalar)
    uint64_t st_size;       // Toplam boyut (byte)
    uint32_t st_blksize;    // Dosya sistemi I/O'su için blok boyutu
    uint32_t st_blocks;     // Tahsis edilen 512B blok sayısı
    uint32_t st_atime;      // Son erişim zamanı
    uint32_t st_mtime;      // Son değiştirme zamanı
    uint32_t st_ctime;      // Son durum değişikliği zamanı
    uint32_t _pad;
} stat_t;

// st_mode sabitleri
#define S_IFMT   0170000  // Dosya türü maskesi
#define S_IFREG  0100000  // Düzenli dosya
#define S_IFDIR  0040000  // Dizin
#define S_IFCHR  0020000  // Karakter aygıtı
#define S_IFIFO  0010000  // FIFO / pipe
#define S_IRUSR  0000400  // Sahibi okuyabilir
#define S_IWUSR  0000200  // Sahibi yazabilir
#define S_IXUSR  0000100  // Sahibi çalıştırabilir
#define S_IRGRP  0000040  // Grup okuyabilir
#define S_IWGRP  0000020  // Grup yazabilir
#define S_IROTH  0000004  // Diğerleri okuyabilir
#define S_IWOTH  0000002  // Diğerleri yazabilir


#define O_RDONLY    0x0000
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_CREAT     0x0040
#define O_TRUNC     0x0200
#define O_APPEND    0x0400
#define O_NONBLOCK  0x0800
#define O_CLOEXEC   0x80000

// ── fcntl komutları ──────────────────────────────────────────────
#define F_DUPFD         0   // fd kopyala (>= arg olan ilk bos fde)
#define F_GETFD         1   // fd bayraklarini al
#define F_SETFD         2   // fd bayraklarini ayarla
#define F_GETFL         3   // dosya durum bayraklarini al
#define F_SETFL         4   // dosya durum bayraklarini ayarla
#define F_DUPFD_CLOEXEC 1030 // F_DUPFD + FD_CLOEXEC atomik

// ── fd bayraklari ───────────────────────────────────────────────
#define FD_CLOEXEC      1   // exec() sonrasi fdi kapat

// Standart fd sabitleri
#define STDIN_FD    0
#define STDOUT_FD   1
#define STDERR_FD   2

// ============================================================
// Hata Kodları  ── Linux x86-64 ABI ile BİREBİR UYUMLU
//
// Değerler Linux kernel'inin include/uapi/asm-generic/errno-base.h
// ve errno.h dosyalarından alınmıştır.  Negatif uint64 olarak döner;
// kullanıcı tarafında (int64_t) cast edilmeli.
//
// syscalls.c artık hiçbir dönüşüm yapmaz:
//   if (ret < 0) { errno = (int)(-ret); return -1; }
// ============================================================
#define SYSCALL_OK          ((uint64_t)0)
#define SYSCALL_ERR_PERM    ((uint64_t)-1)   // -EPERM   : yetki yok
#define SYSCALL_ERR_NOENT   ((uint64_t)-2)   // -ENOENT  : dosya bulunamadı
#define SYSCALL_ERR_SRCH    ((uint64_t)-3)   // -ESRCH   : süreç bulunamadı
#define SYSCALL_ERR_INTR    ((uint64_t)-4)   // -EINTR   : sinyal ile kesildi
#define SYSCALL_ERR_IO      ((uint64_t)-5)   // -EIO     : I/O hatası
#define SYSCALL_ERR_AGAIN   ((uint64_t)-11)  // -EAGAIN  : tekrar dene  (=EWOULDBLOCK)
#define SYSCALL_ERR_NOMEM   ((uint64_t)-12)  // -ENOMEM  : bellek yok
#define SYSCALL_ERR_FAULT   ((uint64_t)-14)  // -EFAULT  : geçersiz adres
#define SYSCALL_ERR_BUSY    ((uint64_t)-16)  // -EBUSY   : kaynak meşgul
#define SYSCALL_ERR_NODEV   ((uint64_t)-19)  // -ENODEV  : aygıt yok
#define SYSCALL_ERR_INVAL   ((uint64_t)-22)  // -EINVAL  : geçersiz argüman
#define SYSCALL_ERR_MFILE   ((uint64_t)-24)  // -EMFILE  : fd tablosu dolu
#define SYSCALL_ERR_NOSPC   ((uint64_t)-28)  // -ENOSPC  : alan yok
#define SYSCALL_ERR_RANGE   ((uint64_t)-34)  // -ERANGE  : değer aralık dışı
#define SYSCALL_ERR_NOSYS   ((uint64_t)-38)  // -ENOSYS  : implemente edilmedi
#define SYSCALL_ERR_CHILD   ((uint64_t)-10)  // -ECHILD  : çocuk yok / bulunamadı
#define SYSCALL_ERR_BADF    ((uint64_t)-9)   // -EBADF   : geçersiz fd
#define SYSCALL_ERR_PIPE    ((uint64_t)-32)  // -EPIPE   : broken pipe
#define SYSCALL_ERR_NAMETOOLONG ((uint64_t)-36) // -ENAMETOOLONG

// ── Takma adlar (geriye dönük uyumluluk için; eski sabitleri kullanan
//    kernel kodunu kırmaz, sadece değerleri artık Linux ile örtüşür) ───────
#define SYSCALL_ERR_WOULDBLOCK  SYSCALL_ERR_AGAIN  // -EWOULDBLOCK == -EAGAIN

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
    uint8_t     fd_flags;    // FD_CLOEXEC vb. (fcntl F_GETFD/F_SETFD)
    uint16_t    flags;       // O_RDONLY, O_WRONLY vb. — uint16_t: O_TRUNC(0x200) sigmaz!
    uint8_t     is_open;     // 1 = acik
    uint8_t     _pad[3];
    uint64_t    offset;      // dosya okuma/yazma ofseti
    char        path[52];    // açık dosyanın yolu (debug / gelecek VFS)
    pipe_buf_t* pipe;        // FD_TYPE_PIPE ise tampon; diğer türler için NULL
} fd_entry_t;

// ============================================================
// Syscall Frame
// Assembly stub (syscall_entry) tarafından yığın üzerinde oluşturulur.
// Offsetler interrupts64.asm ile eşleşmeli.
//
// DÜZELTİLMİŞ: user_rsp alanı eklendi (+72).
// sys_execve bu alana yeni user stack pointer'ını yazar.
// Assembly stub SYSRET öncesi bu değeri kontrol eder:
//   - 0 ise: orijinal user RSP'ye dön (normal syscall davranışı)
//   - 0 dışı: bu adresi RSP olarak kullan (execve sonrası yeni stack)
// ============================================================
typedef struct {
    uint64_t rax;       // syscall number (giriş) / dönüş değeri (çıkış)  +0
    uint64_t rdi;       // arg1                                            +8
    uint64_t rsi;       // arg2                                            +16
    uint64_t rdx;       // arg3                                            +24
    uint64_t r10;       // arg4  (SYSCALL RCX clobber ettiği için R10)    +32
    uint64_t r8;        // arg5                                            +40
    uint64_t r9;        // arg6                                            +48
    uint64_t rcx;       // SYSCALL'in kaydettiği RIP (return address)     +56
    uint64_t r11;       // SYSCALL'in kaydettiği RFLAGS                   +64
    uint64_t user_rsp;  // execve tarafından override edilir (0 = kullanma) +72
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

// ── Alarm yardımcı API (scheduler entegrasyonu için) ───────────────
// scheduler her tick'te alarm_is_active() kontrolü yaparak
// deadline geçtiyse task'a SIGALRM inject eder.
uint8_t  alarm_is_active(void);
uint64_t alarm_get_deadline(void);
void     alarm_clear(void);

// ── fd tablosu yardımcı fonksiyonları ──────────────────────────
void        fd_table_init(fd_entry_t* table);
int         fd_alloc(fd_entry_t* table, uint8_t type, uint16_t flags,
                     const char* path);
int         fd_alloc_pipe(fd_entry_t* table, uint8_t rw_flags,
                          pipe_buf_t* pbuf);   // pipe için özel ayırıcı
int         fd_free(fd_entry_t* table, int fd);
fd_entry_t* fd_get(fd_entry_t* table, int fd);

// ── pipe tampon ayırma/serbest bırakma ──────────────────────────
pipe_buf_t* pipe_buf_alloc(void);
void        pipe_buf_release(pipe_buf_t* pb);  // ref_count--; 0'da kfree

// ── Doom / grafik syscall'lar (v29) ─────────────────────────
// sys_fb_info: vesa64.c extern'lerinden fb bilgisini user-space'e aktarır.
// sys_kb_raw:  klavye raw scancode modunu açar/kapatır (PS/2 varsa).
// sys_fb_blit: Doom screen buffer'ını kernel VRAM'ına nearest-neighbor ile kopyalar.
//              User-space fiziksel adrese hiç dokunmaz; tüm yazma Ring-0'da olur.
void sys_fb_info (syscall_frame_t* frame);
void sys_kb_raw  (syscall_frame_t* frame);
void sys_fb_blit (syscall_frame_t* frame);

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