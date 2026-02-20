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
//
// v4 Yeni Eklemeler:
//   SYS_LSEEK   (24) – dosya ofseti konumlandırma (SEEK_SET/CUR/END)
//   SYS_FSTAT   (25) – fd üzerinden dosya meta verisi sorgulama
//   SYS_IOCTL   (26) – terminal mod / genel aygıt kontrolü (TCGETS/TCSETS/…)
//
// v5 Yeni Eklemeler:
//   SYS_MMAP    (16) – MAP_FILE desteği eklendi (fd + offset ile dosya haritalama)
//   SYS_SELECT  (27) – I/O multiplexing; fd kümelerini zaman aşımı ile bekle
//   SYS_POLL    (28) – select'in modern alternatifi; pollfd dizisi ile bekle

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

// ── Yeni syscall'lar (v4) ─────────────────────────────────────
#define SYS_LSEEK        24  // lseek(fd, offset, whence)      -> new_offset | err
#define SYS_FSTAT        25  // fstat(fd, *stat)               -> 0 | err
#define SYS_IOCTL        26  // ioctl(fd, request, arg)        -> 0 | err

// ── Yeni syscall'lar (v5) ─────────────────────────────────────
#define SYS_SELECT       27  // select(nfds,rd,wr,ex,tv)       -> nready | err
#define SYS_POLL         28  // poll(fds, nfds, timeout_ms)    -> nready | err

// ── Yeni syscall'lar (v6 – newlib uyumu) ──────────────────────
#define SYS_KILL         29  // kill(pid, sig)                  -> 0 | err
#define SYS_GETTIMEOFDAY 30  // gettimeofday(*tv, *tz)          -> 0 | err

#define SYSCALL_MAX      31

// ============================================================
// kill() sinyal numaraları (POSIX alt kümesi)
// ============================================================
#define SIGTERM   15   // Nazikçe sonlandır
#define SIGKILL    9   // Zorla sonlandır (yakalanamaz)
#define SIGINT     2   // Klavye interrupt (^C)
#define SIGHUP     1   // Hang-up
#define SIGPIPE   13   // Yazma ucuna kimse okumuyorsa
#define SIGCHLD   17   // Çocuk durumu değişti
#define SIGALRM   14   // Zamanlayıcı
#define SIGUSR1   10   // Kullanıcı tanımlı 1
#define SIGUSR2   12   // Kullanıcı tanımlı 2

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
#define WUNTRACED        0x02   // Durdurulmuş çocukları da raporla (stub)

// waitpid status decode makroları
#define WIFEXITED(s)     (((s) & 0xFF) == 0)
#define WEXITSTATUS(s)   (((s) >> 8) & 0xFF)
#define WIFSIGNALED(s)   (((s) & 0x7F) != 0 && ((s) & 0x7F) != 0x7F)
#define WTERMSIG(s)      ((s) & 0x7F)

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