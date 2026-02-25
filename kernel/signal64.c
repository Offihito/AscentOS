// signal64.c — AscentOS 64-bit Sinyal Dağıtım Altyapısı
//
// Kapsam:
//   - signal_table_init()        : Handler tablosunu sıfırla
//   - signal_send()              : Kernel içi sinyal gönder (kill de buraya gelir)
//   - signal_dispatch_pending()  : Scheduler çıkışında pending sinyalleri işle
//   - signal_do_return()         : sigreturn syscall'ı için frame restore
//   - sys_sigaction()            : SYS_SIGACTION (61)
//   - sys_sigprocmask()          : SYS_SIGPROCMASK (62)
//   - sys_sigreturn()            : SYS_SIGRETURN (63)
//   - sys_sigpending()           : SYS_SIGPENDING (64)
//   - sys_sigsuspend()           : SYS_SIGSUSPEND (65)
//
// Userspace Handler Akışı:
//
//   1. signal_send(pid, signo) → task'ın pending_sigs bit'ini set eder
//   2. Scheduler context-switch sonrası signal_dispatch_pending() çağrılır
//   3. Pending && !masked && handler!=SIG_DFL && handler!=SIG_IGN ise:
//      a. Mevcut syscall frame'i task'ın signal_saved_frame'ine kopyala
//      b. frame->rip = handler adresi, frame->rdi = signo
//      c. frame->rsp'yi trampoline adresiyle ayarla (return adresi)
//      d. Userspace'e döndüğünde doğrudan handler çalışır
//   4. Handler bitince trampoline "syscall SYS_SIGRETURN" yapar
//   5. sys_sigreturn() → signal_do_return() → kaydedilen frame'i restore et
//
// NOT: Bu kernel'de sayfa tablosu ve gerçek ring-3 yoktur; execve
// bir userspace binary çalıştırmak yerine task callback olarak
// çalışır. Handler dallanması için mevcut "task frame" mekanizması
// (syscall_frame_t içindeki rcx=RIP, r11=RFLAGS) kullanılır.

#include "signal64.h"
#include "syscall.h"    // syscall_frame_t, SYSCALL_ERR_*, SYS_KILL

// ============================================================
// External bağımlılıklar
// ============================================================
extern void  serial_print(const char* s);
extern void  serial_putchar(char c);
extern void  task_exit(void);
extern void  scheduler_yield(void);
extern void* memset64(void* d, int v, uint64_t n);
extern void* memcpy64(void* d, const void* s, uint64_t n);

// Task API — task.h üzerinden (static inline accessor'lar burada tanımlı)
#include "task.h"

// ============================================================
// Dahili yardımcılar
// ============================================================

#define KMEMSET(d,v,n)  memset64((d),(v),(uint64_t)(n))
#define KMEMCPY(d,s,n)  memcpy64((d),(s),(uint64_t)(n))

static void print_int(int v) {
    if (v < 0) { serial_putchar('-'); v = -v; }
    char buf[12]; int i = 0;
    if (v == 0) { serial_putchar('0'); return; }
    while (v > 0) { buf[i++] = '0' + (v % 10); v /= 10; }
    while (i-- > 0) serial_putchar(buf[i]);
}

// ============================================================
// Varsayılan sinyal davranışı tablosu
// ============================================================
sig_default_action_t signal_default_action(int signo) {
    switch (signo) {
    // Yoksay
    case SIGCHLD:   return SIG_ACT_IGN;
    case SIGWINCH:  return SIG_ACT_IGN;
    case SIGURG:    return SIG_ACT_IGN;
    case SIGCONT:   return SIG_ACT_CONT;

    // Durdur (maskelenemeez ama catch edilebilir olanlar)
    case SIGTSTP:   return SIG_ACT_STOP;
    case SIGTTIN:   return SIG_ACT_STOP;
    case SIGTTOU:   return SIG_ACT_STOP;
    case SIGSTOP:   return SIG_ACT_STOP;  // maskelenemeez

    // Core dump gerektiren
    case SIGQUIT:   return SIG_ACT_CORE;
    case SIGILL:    return SIG_ACT_CORE;
    case SIGTRAP:   return SIG_ACT_CORE;
    case SIGABRT:   return SIG_ACT_CORE;
    case SIGBUS:    return SIG_ACT_CORE;
    case SIGFPE:    return SIG_ACT_CORE;
    case SIGSEGV:   return SIG_ACT_CORE;
    case SIGXCPU:   return SIG_ACT_CORE;
    case SIGXFSZ:   return SIG_ACT_CORE;
    case SIGSYS:    return SIG_ACT_CORE;

    // Geri kalan her şey → TERM
    default:        return SIG_ACT_TERM;
    }
}

// ============================================================
// signal_table_init()
// Task oluşturulurken çağrılır. Tüm handler'lar SIG_DFL.
// ============================================================
void signal_table_init(signal_table_t* st) {
    KMEMSET(st, 0, sizeof(signal_table_t));
    // SIGCHLD varsayılan olarak ignored olarak başlatılabilir;
    // bash ihtiyacına göre sigaction ile değiştirir.
    // Biz POSIX'e göre SIG_DFL bırakıyoruz (bash zaten kurar).
}

// ============================================================
// signal_send()
// Kernel içinden sinyal gönder: kill() syscall'ı, IRQ handler'lar,
// ve cpu exception handler'ları buraya çağırır.
//
// Dönüş: 0=ok, -1=geçersiz / PID yok
// ============================================================
int signal_send(int pid, int signo) {
    if (signo < 1 || signo >= NSIG) return -1;

    task_t* target = task_get_by_pid(pid);
    if (!target) {
        serial_print("[SIGNAL] signal_send: PID bulunamadi pid=");
        print_int(pid);
        serial_print(" sig=");
        print_int(signo);
        serial_print("\n");
        return -1;
    }

    signal_table_t* st = task_get_signal_table(target);
    if (!st) return -1;

    // SIGKILL / SIGSTOP zaten pending'de ise tekrar set etme (idempotent)
    st->pending_sigs |= SIG_BIT(signo);

    serial_print("[SIGNAL] Delivered sig=");
    print_int(signo);
    serial_print(" -> pid=");
    print_int(pid);
    serial_print("\n");

    // SIGKILL: anında task'ı sonlandır, dispatch bekleme
    if (signo == SIGKILL) {
        task_set_stopped(target, 0);   // durdurulmuşsa çöz
        // task_terminate veya benzeri çağrılabilir;
        // burada pending bit yeterli — dispatch'e bırak.
    }

    return 0;
}

// ============================================================
// Dahili: varsayılan aksiyon uygula
// ============================================================
static void apply_default_action(task_t* cur, int signo) {
    sig_default_action_t act = signal_default_action(signo);

    switch (act) {
    case SIG_ACT_IGN:
        // Yoksay
        break;

    case SIG_ACT_STOP:
        serial_print("[SIGNAL] Task durduruluyor, sig=");
        print_int(signo);
        serial_print("\n");
        task_set_stopped(cur, 1);
        scheduler_yield();
        break;

    case SIG_ACT_CONT:
        task_set_stopped(cur, 0);
        break;

    case SIG_ACT_CORE:
        serial_print("[SIGNAL] Core dump (stub), sig=");
        print_int(signo);
        serial_print("\n");
        // Gerçek core dump yok; TERM gibi davran
        // fall-through
    case SIG_ACT_TERM:
    default:
        serial_print("[SIGNAL] Task sonlandiriliyor, sig=");
        print_int(signo);
        serial_print("\n");
        // Kayıt: exit_code = 128 + signo (bash convention)
        task_exit();
        break;
    }
}

// ============================================================
// signal_dispatch_pending()
//
// Her syscall/interrupt dönüşünde çağrılır.
// Pending & ~masked bitmask'ında bit varsa işlenir.
//
// Öncelik: en düşük numaralı sinyal önce işlenir (POSIX zorunluluğu yok
// ama bu davranış en yaygın beklentiyle örtüşür).
//
// Userspace handler mekanizması:
//   Bu kernel'de ring-3 yoktur; task'lar kernel fonksiyonu olarak çalışır.
//   Handler'ı simüle etmek için doğrudan C fonksiyon pointer'ını çağırıyoruz.
//   Gerçek bir ring-3 tasarımda frame manipülasyonu gerekir (bkz. aşağıdaki
//   "RING-3 EXTENSION" bloğu).
// ============================================================
void signal_dispatch_pending(void) {
    task_t* cur = task_get_current();
    if (!cur) return;

    signal_table_t* st = task_get_signal_table(cur);
    if (!st) return;

    // Re-entry koruması: handler içindeyken yeni sinyal işleme
    if (st->in_handler) return;

    // Teslim edilebilir sinyal bitmaskı
    sigset_t deliverable = st->pending_sigs & ~st->masked_sigs;
    if (!deliverable) return;

    // En düşük numaralı sinyali bul
    int signo = -1;
    for (int i = 1; i < NSIG; i++) {
        if (deliverable & SIG_BIT(i)) { signo = i; break; }
    }
    if (signo < 0) return;

    // Pending bit'i temizle
    st->pending_sigs &= ~SIG_BIT(signo);

    struct sigaction* sa = &st->handlers[signo - 1];

    // SIGKILL / SIGSTOP maskelenemeez ve yakalanamaz
    if (signo == SIGKILL || signo == SIGSTOP) {
        apply_default_action(cur, signo);
        return;
    }

    // SIG_IGN
    if (sa->sa_handler == SIG_IGN) {
        serial_print("[SIGNAL] Ignored sig=");
        print_int(signo);
        serial_print("\n");
        return;
    }

    // SIG_DFL
    if (sa->sa_handler == SIG_DFL) {
        apply_default_action(cur, signo);
        return;
    }

    // --- Userspace handler çağrısı ---
    //
    // Bu kernel'de userspace binary desteği olmadığı için handler
    // pointer'ı bir kernel-mode C fonksiyonudur (execve callback task'ları).
    // Sinyal maskesini uygula (handler süresi boyunca)
    sigset_t old_mask = st->masked_sigs;
    st->masked_sigs |= sa->sa_mask;
    if (!(sa->sa_flags & SA_NODEFER))
        st->masked_sigs |= SIG_BIT(signo);

    st->in_handler = 1;

    serial_print("[SIGNAL] Handler cagiriliyor, sig=");
    print_int(signo);
    serial_print("\n");

    // Handler'ı çağır: void handler(int signo)
    // Gerçek ring-3 tasarımda burada frame manipülasyonu yapılır.
    // ──────────────────────────────────────────────────────────────
    // RING-3 EXTENSION (ilerideki kernel versiyonları için):
    //
    //   syscall_frame_t* frame = task_get_saved_frame(cur);
    //   uint64_t tramp = task_get_trampoline(cur);
    //
    //   // Signal frame'i user stack'e kopyala
    //   frame->rsp -= sizeof(signal_saved_frame_t);
    //   signal_saved_frame_t* sf = (signal_saved_frame_t*)frame->rsp;
    //   *sf = (signal_saved_frame_t){ .rip=frame->rcx, .rflags=frame->r11,
    //                                 .old_mask=old_mask };
    //   // Return adresini trampoline'e yönlendir
    //   frame->rsp -= 8;
    //   *(uint64_t*)frame->rsp = tramp;
    //   // Handler'a dallan
    //   frame->rcx = (uint64_t)sa->sa_handler;
    //   frame->rdi = (uint64_t)signo;
    //
    // ──────────────────────────────────────────────────────────────
    sa->sa_handler(signo);   // Şimdilik: direkt kernel-mode çağrı

    // SA_RESETHAND: bir kerelik handler → SIG_DFL'ye dön
    if (sa->sa_flags & SA_RESETHAND) {
        sa->sa_handler = SIG_DFL;
        sa->sa_mask    = 0;
        sa->sa_flags   = 0;
    }

    st->in_handler   = 0;
    st->masked_sigs  = old_mask;
}

// ============================================================
// signal_do_return()
// Ring-3 trampoline "syscall 63 (SYS_SIGRETURN)" yaptıktan sonra
// sys_sigreturn() tarafından çağrılır.
// Kaydedilen frame'i geri yükler ve normal akışa döner.
// ============================================================
void signal_do_return(void* saved_frame_ptr) {
    // Ring-3 için: saved frame'den RIP, RFLAGS, masked_sigs restore edilir.
    // Mevcut kernel'de bu stub; trampoline geçmişte kullanıldıysa no-op.
    (void)saved_frame_ptr;

    task_t* cur = task_get_current();
    if (!cur) return;

    signal_table_t* st = task_get_signal_table(cur);
    if (st) {
        st->in_handler = 0;
        // Mask zaten dispatch_pending'de restore edildi.
        // Ring-3'te: st->masked_sigs = sf->old_mask;
    }

    serial_print("[SIGNAL] sigreturn: handler tamamlandi\n");
}


// ============================================================
// ============================================================
// SYS_SIGACTION  (syscall numarası: 61)
//
// sigaction(int signo, const struct sigaction* new_sa,
//           struct sigaction* old_sa)  -> 0 | err
//
// new_sa NULL ise sadece mevcut handler okunur (sorgu modu).
// old_sa NULL ise eski handler saklanmaz.
//
// Doğrulama:
//   - signo 1..NSIG aralığında olmalı
//   - SIGKILL / SIGSTOP yakalanıp değiştirilemez
//   - sa_handler geçerli pointer olmalı (NULL = SIG_DFL/SIG_IGN kontrolü)
// ============================================================
void sys_sigaction(syscall_frame_t* frame) {
    int  signo  = (int)(int64_t)frame->rdi;
    struct sigaction* new_sa = (struct sigaction*)frame->rsi;
    struct sigaction* old_sa = (struct sigaction*)frame->rdx;

    // Sinyal numarası geçerliliği
    if (signo < 1 || signo > NSIG) {
        frame->rax = SYSCALL_ERR_INVAL;
        return;
    }

    // SIGKILL / SIGSTOP yakalanamaz
    if (signo == SIGKILL || signo == SIGSTOP) {
        frame->rax = SYSCALL_ERR_INVAL;
        return;
    }

    task_t* cur = task_get_current();
    if (!cur) { frame->rax = SYSCALL_ERR_PERM; return; }

    signal_table_t* st = task_get_signal_table(cur);
    if (!st) { frame->rax = SYSCALL_ERR_PERM; return; }

    struct sigaction* cur_sa = &st->handlers[signo - 1];

    // Eski handler'ı kullanıcıya ver
    if (old_sa) {
        // Pointer doğrulama: NULL olmayan user ptr mı?
        uint64_t old_addr = (uint64_t)old_sa;
        if (old_addr < 0x1000 || (old_addr >> 47)) {
            frame->rax = SYSCALL_ERR_FAULT;
            return;
        }
        KMEMCPY(old_sa, cur_sa, sizeof(struct sigaction));
    }

    // Yeni handler'ı kur
    if (new_sa) {
        uint64_t new_addr = (uint64_t)new_sa;
        if (new_addr < 0x1000 || (new_addr >> 47)) {
            frame->rax = SYSCALL_ERR_FAULT;
            return;
        }

        serial_print("[SYSCALL] sigaction: sig=");
        print_int(signo);

        if (new_sa->sa_handler == SIG_IGN) {
            serial_print(" -> SIG_IGN");
            // Sinyal yoksayıldığında pending bit'i temizle
            st->pending_sigs &= ~SIG_BIT(signo);
        } else if (new_sa->sa_handler == SIG_DFL) {
            serial_print(" -> SIG_DFL");
        } else {
            serial_print(" -> handler@0x");
            // Minimal hex print (pointer)
            uint64_t p = (uint64_t)new_sa->sa_handler;
            char hbuf[9]; int hi = 0;
            const char* hx = "0123456789ABCDEF";
            for (int b = 60; b >= 0; b -= 4) {
                uint8_t nib = (p >> b) & 0xF;
                if (nib || hi > 0 || b == 0) hbuf[hi++] = hx[nib];
            }
            hbuf[hi] = '\0';
            serial_print(hbuf);
        }
        serial_print("\n");

        KMEMCPY(cur_sa, new_sa, sizeof(struct sigaction));
    }

    frame->rax = SYSCALL_OK;
}

// ============================================================
// SYS_SIGPROCMASK  (syscall numarası: 62)
//
// sigprocmask(int how, const sigset_t* set, sigset_t* oldset)
//   -> 0 | err
//
// how = SIG_BLOCK   : masked_sigs |= *set
// how = SIG_UNBLOCK : masked_sigs &= ~(*set)
// how = SIG_SETMASK : masked_sigs  = *set
//
// SIGKILL / SIGSTOP maskelenemeez → bitmaskten otomatik çıkarılır.
// ============================================================
void sys_sigprocmask(syscall_frame_t* frame) {
    int       how    = (int)(int64_t)frame->rdi;
    sigset_t* set    = (sigset_t*)frame->rsi;
    sigset_t* oldset = (sigset_t*)frame->rdx;

    task_t* cur = task_get_current();
    if (!cur) { frame->rax = SYSCALL_ERR_PERM; return; }

    signal_table_t* st = task_get_signal_table(cur);
    if (!st) { frame->rax = SYSCALL_ERR_PERM; return; }

    // Eski maskeyi döndür
    if (oldset) {
        uint64_t oa = (uint64_t)oldset;
        if (oa < 0x1000 || (oa >> 47)) {
            frame->rax = SYSCALL_ERR_FAULT;
            return;
        }
        *oldset = st->masked_sigs;
    }

    if (set) {
        uint64_t sa = (uint64_t)set;
        if (sa < 0x1000 || (sa >> 47)) {
            frame->rax = SYSCALL_ERR_FAULT;
            return;
        }

        sigset_t new_mask = *set;

        // SIGKILL + SIGSTOP hiçbir zaman maskelenmez
        new_mask &= ~(SIG_BIT(SIGKILL) | SIG_BIT(SIGSTOP));

        switch (how) {
        case SIG_BLOCK:
            st->masked_sigs |= new_mask;
            break;
        case SIG_UNBLOCK:
            st->masked_sigs &= ~new_mask;
            break;
        case SIG_SETMASK:
            st->masked_sigs = new_mask;
            break;
        default:
            frame->rax = SYSCALL_ERR_INVAL;
            return;
        }

        serial_print("[SYSCALL] sigprocmask: how=");
        print_int(how);
        serial_print(" mask=");
        print_int((int)st->masked_sigs);
        serial_print("\n");
    }

    frame->rax = SYSCALL_OK;
}

// ============================================================
// SYS_SIGRETURN  (syscall numarası: 63)
//
// Userspace trampoline tarafından signal handler bittikten sonra
// çağrılır. Kaydedilen frame'i geri yükler.
// ============================================================
void sys_sigreturn(syscall_frame_t* frame) {
    // frame->rdi: trampoline tarafından stack'e bırakılan saved_frame ptr
    void* sf = (void*)frame->rdi;
    signal_do_return(sf);
    // frame->rax değiştirilmez; SYSRET'te handler öncesi RIP'e dönülür
    frame->rax = SYSCALL_OK;
}

// ============================================================
// SYS_SIGPENDING  (syscall numarası: 64)
//
// sigpending(sigset_t* set) -> 0 | err
// Maskelenmiş ama bekleyen sinyallerin setini döndürür.
// ============================================================
void sys_sigpending(syscall_frame_t* frame) {
    sigset_t* out = (sigset_t*)frame->rdi;

    if (!out) { frame->rax = SYSCALL_ERR_INVAL; return; }
    uint64_t oa = (uint64_t)out;
    if (oa < 0x1000 || (oa >> 47)) {
        frame->rax = SYSCALL_ERR_FAULT;
        return;
    }

    task_t* cur = task_get_current();
    if (!cur) { frame->rax = SYSCALL_ERR_PERM; return; }

    signal_table_t* st = task_get_signal_table(cur);
    if (!st) { frame->rax = SYSCALL_ERR_PERM; return; }

    // Sadece maskelenmiş VE bekleyen sinyalleri döndür
    *out = st->pending_sigs & st->masked_sigs;

    frame->rax = SYSCALL_OK;
}

// ============================================================
// SYS_SIGSUSPEND  (syscall numarası: 65)
//
// sigsuspend(const sigset_t* mask) -> her zaman -EINTR döner
//
// Geçici olarak signal maskesini mask ile değiştirir,
// herhangi bir sinyal gelene kadar uyur.
// Sinyal geldiğinde eski mask geri yüklenir ve EINTR döner.
//
// Bash'in "wait" built-in'i ve ^C bekleme döngüsü için kritik.
// ============================================================
void sys_sigsuspend(syscall_frame_t* frame) {
    const sigset_t* mask = (const sigset_t*)frame->rdi;

    if (!mask) { frame->rax = SYSCALL_ERR_INVAL; return; }
    uint64_t ma = (uint64_t)mask;
    if (ma < 0x1000 || (ma >> 47)) {
        frame->rax = SYSCALL_ERR_FAULT;
        return;
    }

    task_t* cur = task_get_current();
    if (!cur) { frame->rax = SYSCALL_ERR_PERM; return; }

    signal_table_t* st = task_get_signal_table(cur);
    if (!st) { frame->rax = SYSCALL_ERR_PERM; return; }

    // Geçici maske uygula
    sigset_t old_mask  = st->masked_sigs;
    sigset_t tmp_mask  = *mask;
    tmp_mask &= ~(SIG_BIT(SIGKILL) | SIG_BIT(SIGSTOP));
    st->masked_sigs = tmp_mask;

    serial_print("[SYSCALL] sigsuspend: mask=");
    print_int((int)tmp_mask);
    serial_print(" bekleniyor...\n");

    // Sinyal gelene kadar yieldla
    // Teslim edilebilir sinyal = pending & ~masked
    while (!(st->pending_sigs & ~st->masked_sigs)) {
        __asm__ volatile ("sti; hlt" ::: "memory");
    }

    // Eski maskeyi geri yükle
    st->masked_sigs = old_mask;

    // Bekleyen sinyali işle
    signal_dispatch_pending();

    // POSIX: sigsuspend her zaman EINTR döner
    frame->rax = SYSCALL_ERR_AGAIN;   // -EAGAIN ≈ -EINTR (stub)
}


// ============================================================
// sys_kill() GÜNCELLEMESI
//
// Mevcut sys_kill, signal_send() API'sini kullanacak şekilde
// yeniden yazılmış hali. syscall.c'deki eski sys_kill yerine bu
// fonksiyon kopyalanabilir; ya da syscall.c'de:
//
//   #include "signal64.h"
//   static void sys_kill(syscall_frame_t* frame) {
//       int pid   = (int)(int64_t)frame->rdi;
//       int signo = (int)(int64_t)frame->rsi;
//       int ret   = signal_send(pid, signo);
//       frame->rax = (ret == 0) ? SYSCALL_OK : SYSCALL_ERR_INVAL;
//   }
//
// şeklinde kısa sürümü kullanılabilir.
// ============================================================
static void sys_kill_v2(syscall_frame_t* frame) {
    int pid   = (int)(int64_t)frame->rdi;
    int signo = (int)(int64_t)frame->rsi;

    if (signo < 0 || signo > NSIG) {
        frame->rax = SYSCALL_ERR_INVAL;
        return;
    }

    // signo == 0: process varlık kontrolü (sinyal gönderme)
    if (signo == 0) {
        task_t* target = task_get_by_pid(pid);
        frame->rax = target ? SYSCALL_OK : SYSCALL_ERR_INVAL;
        return;
    }

    // pid == 0: process grubuna gönder (stub: sadece mevcut task)
    if (pid == 0) {
        task_t* cur = task_get_current();
        if (cur) {
            signal_table_t* st = task_get_signal_table(cur);
            if (st) st->pending_sigs |= SIG_BIT(signo);
        }
        frame->rax = SYSCALL_OK;
        return;
    }

    // pid == -1: tüm task'lara gönder (henüz implement edilmedi → ENOSYS)
    if (pid == -1) {
        serial_print("[SIGNAL] kill(-1): broadcast not implemented\n");
        frame->rax = SYSCALL_ERR_NOSYS;
        return;
    }

    int ret = signal_send(pid, signo);
    frame->rax = (ret == 0) ? SYSCALL_OK : SYSCALL_ERR_INVAL;
}

// ============================================================
// Unified dispatcher — syscall_dispatch()'dan çağrılır
//
// syscall.c'deki syscall_dispatch() switch bloğuna aşağıdaki
// satırlar eklenmelidir:
//
//   case SYS_SIGACTION:    sys_sigaction(frame);    break;
//   case SYS_SIGPROCMASK:  sys_sigprocmask(frame);  break;
//   case SYS_SIGRETURN:    sys_sigreturn(frame);    break;
//   case SYS_SIGPENDING:   sys_sigpending(frame);   break;
//   case SYS_SIGSUSPEND:   sys_sigsuspend(frame);   break;
//   case SYS_KILL:         sys_kill_v2(frame);      break;  // eskisini değiştir
//
// VE syscall.h SYSCALL_MAX 66'ya yükseltilmeli:
//   #define SYSCALL_MAX  66
// ============================================================
void signal_syscall_dispatch(syscall_frame_t* frame) {
    switch (frame->rax) {
    case SYS_SIGACTION:   sys_sigaction(frame);   break;
    case SYS_SIGPROCMASK: sys_sigprocmask(frame); break;
    case SYS_SIGRETURN:   sys_sigreturn(frame);   break;
    case SYS_SIGPENDING:  sys_sigpending(frame);  break;
    case SYS_SIGSUSPEND:  sys_sigsuspend(frame);  break;
    case SYS_KILL:        sys_kill_v2(frame);     break;
    default: break;
    }
}