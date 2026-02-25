// signal64.h — AscentOS 64-bit Sinyal Altyapısı
// Bash için gereken POSIX sinyal alt kümesi: sigaction, sigprocmask,
// kill, signal dağıtımı ve userspace handler çağrısı.
//
// Tasarım prensipleri:
//   - Her task, sinyal tablosu + pending/masked bitmask taşır
//   - Kernel-to-userspace geçiş: signal trampoline via saved frame
//   - Async-safe: interrupt handler içinden signal_deliver() çağrılabilir
//   - SIGKILL / SIGSTOP maskeleme/yakalama yasağı uygulanır

#ifndef SIGNAL64_H
#define SIGNAL64_H

#include <stdint.h>

// ============================================================
// Sinyal numaraları (POSIX uyumlu)
// syscall.h'daki makrolar ile çakışmıyor; bu header önce include edilmeli.
// ============================================================
#define NSIG        32      // Desteklenen toplam sinyal sayısı

#define SIGHUP       1      // Hang-up
#define SIGINT       2      // Klavye interrupt (^C)
#define SIGQUIT      3      // Klavye çıkış (^\ )
#define SIGILL       4      // Geçersiz komut
#define SIGTRAP      5      // Trace/breakpoint
#define SIGABRT      6      // Abort
#define SIGBUS       7      // Bus error (hizalama)
#define SIGFPE       8      // Floating-point exception
#define SIGKILL      9      // Zorla sonlandır (yakalanamaz/maskelenemeez)
#define SIGUSR1     10      // Kullanıcı tanımlı 1
#define SIGSEGV     11      // Segmentation fault
#define SIGUSR2     12      // Kullanıcı tanımlı 2
#define SIGPIPE     13      // Yazma ucuna okuyucu yok
#define SIGALRM     14      // Zamanlayıcı (alarm)
#define SIGTERM     15      // Nazikçe sonlandır
#define SIGSTKFLT   16      // Stack fault (x87)
#define SIGCHLD     17      // Çocuk durumu değişti (bash'in en kritik sinyali)
#define SIGCONT     18      // Durdurulmuş process'i devam ettir
#define SIGSTOP     19      // Durdur (yakalanamaz/maskelenemeez)
#define SIGTSTP     20      // Klavye durdur (^Z)
#define SIGTTIN     21      // Background process okumaya çalıştı
#define SIGTTOU     22      // Background process yazmaya çalıştı
#define SIGURG      23      // Soket urgent data
#define SIGXCPU     24      // CPU limiti aşıldı
#define SIGXFSZ     25      // Dosya boyutu limiti aşıldı
#define SIGVTALRM   26      // Sanal zamanlayıcı
#define SIGPROF     27      // Profiling zamanlayıcı
#define SIGWINCH    28      // Terminal pencere boyutu değişti
#define SIGIO       29      // I/O hazır
#define SIGPWR      30      // Güç hatası
#define SIGSYS      31      // Geçersiz syscall

// ============================================================
// Sinyal bitmask işlemleri (sigset_t = uint32_t)
// ============================================================
typedef uint32_t sigset_t;

// Sinyal numarası → bitmask dönüşümü (1-tabanlı, bit 0 = SIGHUP)
#define SIG_BIT(signo)       (1u << (signo))

// sigset_t işlemleri (POSIX sigemptyset / sigfillset / sigaddset vb.)
#define sigemptyset(s)       (*(s) = 0u)
#define sigfillset(s)        (*(s) = ~0u)
#define sigaddset(s, sig)    (*(s) |=  SIG_BIT(sig))
#define sigdelset(s, sig)    (*(s) &= ~SIG_BIT(sig))
#define sigismember(s, sig)  ((*(s) & SIG_BIT(sig)) != 0)

// ============================================================
// Özel handler değerleri (Linux uyumlu pointer değerleri)
// ============================================================
#define SIG_DFL     ((sighandler_t)0)    // Varsayılan davranış
#define SIG_IGN     ((sighandler_t)1)    // Yoksay
#define SIG_ERR     ((sighandler_t)-1)   // Hata dönüşü

// Userspace handler prototip: void handler(int signo)
typedef void (*sighandler_t)(int);

// ============================================================
// sigaction yapısı (POSIX uyumlu)
// ============================================================
#define SA_NOCLDSTOP  0x00000001  // SIGCHLD: sadece terminate/dump'ta bildir
#define SA_NOCLDWAIT  0x00000002  // SIGCHLD: çocukları zombie yapma
#define SA_SIGINFO    0x00000004  // sa_sigaction 3-argümanlı handler kullan
#define SA_RESTART    0x10000000  // EINTR yerine syscall'ı tekrarla
#define SA_NODEFER    0x40000000  // Handler içinde aynı sinyali maskele
#define SA_RESETHAND  0x80000000  // Handler ilk çağrıda SIG_DFL'ye döner

struct sigaction {
    sighandler_t sa_handler;    // Handler pointer (SIG_DFL / SIG_IGN / fn)
    sigset_t     sa_mask;       // Handler süresince ekstra maskelenenler
    uint32_t     sa_flags;      // SA_* bayrakları
    uint64_t     sa_restorer;   // Userspace trampoline adresi (sa_flags & SA_RESTORER)
};

// ============================================================
// sigprocmask() nasıl parametresi
// ============================================================
#define SIG_BLOCK     0    // pending |= set
#define SIG_UNBLOCK   1    // pending &= ~set
#define SIG_SETMASK   2    // pending = set

// ============================================================
// Syscall numaraları (syscall.h'a ek olarak buraya da yazıldı;
// çakışmayı önlemek için #ifndef koruyucusu kullanıyoruz)
// ============================================================
#ifndef SYS_SIGACTION
#define SYS_SIGACTION    61  // sigaction(signo, *new_sa, *old_sa)  -> 0 | err
#endif
#ifndef SYS_SIGPROCMASK
#define SYS_SIGPROCMASK  62  // sigprocmask(how, *set, *oldset)     -> 0 | err
#endif
#ifndef SYS_SIGRETURN
#define SYS_SIGRETURN    63  // sigreturn() — trampoline tarafından çağrılır
#endif
#ifndef SYS_SIGPENDING
#define SYS_SIGPENDING   64  // sigpending(*set)                    -> 0 | err
#endif
#ifndef SYS_SIGSUSPEND
#define SYS_SIGSUSPEND   65  // sigsuspend(*mask) — sinyal gelene kadar uyu
#endif

// ============================================================
// Per-task sinyal tablosu
//
// task_t içine gömülür. Başlangıçta tüm alanlar sıfır (SIG_DFL).
// Interrupt-safe: masked_sigs ve pending_sigs 32-bit atomic işlemlerle
// güncellenir; x86-64'te hizalı 32-bit okuma/yazma zaten atomik.
// ============================================================
typedef struct {
    struct sigaction handlers[NSIG]; // handlers[i] → sinyal i+1'in aksiyonu
    sigset_t         pending_sigs;   // Bekleyen sinyaller bitmaskı
    sigset_t         masked_sigs;    // Maskelenmiş (bloklanmış) sinyaller
    uint8_t          in_handler;     // 1 = şu an bir handler çalışıyor (re-entry engeli)
    uint8_t          _pad[3];
} signal_table_t;

// ============================================================
// Sinyal dağıtım API'si (signal64.c)
// ============================================================

// Tüm handler'ları SIG_DFL'ye başlat (task oluşturulurken çağrılır)
void signal_table_init(signal_table_t* st);

// Bir göreve sinyal gönder (kernel içi, kill() syscall'dan çağrılır)
// pid: hedef task PID'i  signo: 1..NSIG
// Dönüş: 0=ok, -1=PID yok / geçersiz sinyal
int  signal_send(int pid, int signo);

// Mevcut task'ın bekleyen sinyallerini işle.
// Scheduler'ın her context-switch dönüşünde çağrılır.
// Handler gerektiren sinyal varsa userspace frame'i değiştirerek
// handler'a dallanır; dönüşte sigreturn çağrılır.
void signal_dispatch_pending(void);

// SYS_SIGRETURN handler'ı (trampoline buraya döner)
// Kaydedilen frame'i geri yükler.
void signal_do_return(void* saved_frame);

// ============================================================
// Varsayılan sinyal davranışı (action categorisi)
// ============================================================
typedef enum {
    SIG_ACT_TERM,    // Process'i sonlandır (varsayılan çoğu sinyal)
    SIG_ACT_IGN,     // Yoksay (SIGCHLD, SIGWINCH varsayılan)
    SIG_ACT_CORE,    // Core dump + sonlandır (SIGSEGV vb.)
    SIG_ACT_STOP,    // Process'i durdur (SIGSTOP, SIGTSTP)
    SIG_ACT_CONT,    // Durdurulmuşsa devam et (SIGCONT)
} sig_default_action_t;

sig_default_action_t signal_default_action(int signo);

// ============================================================
// Syscall handler bildirimleri (syscall_dispatch'ten çağrılır)
// syscall.h'daki syscall_frame_t tipine ihtiyaç duyar;
// #ifdef koruması döngüsel include sorununu önler.
// ============================================================
#ifdef SYSCALL_H
void sys_sigaction   (syscall_frame_t* frame);  // SYS_SIGACTION  (61)
void sys_sigprocmask (syscall_frame_t* frame);  // SYS_SIGPROCMASK(62)
void sys_sigreturn   (syscall_frame_t* frame);  // SYS_SIGRETURN  (63)
void sys_sigpending  (syscall_frame_t* frame);  // SYS_SIGPENDING (64)
void sys_sigsuspend  (syscall_frame_t* frame);  // SYS_SIGSUSPEND (65)
#endif

#endif // SIGNAL64_H