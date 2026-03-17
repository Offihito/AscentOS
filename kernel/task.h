// task.h - Task Management System for AscentOS 64-bit
// Ring-0 + Ring-3 (SYSRET) + TSS + FD tablosu + Sleep destekli versiyon
//
// v2 Degisiklikleri:
//   - task_t::fd_table[MAX_FDS]  -> SYS_OPEN/CLOSE/READ/WRITE entegrasyonu
//   - task_t::parent_pid         -> SYS_GETPPID destegi
//   - task_t::wake_tick          -> SYS_SLEEP blocking implementasyonu
//   - task_t::exit_code          -> SYS_EXIT cikis kodu
//   - TASK_STATE_SLEEPING        -> zamanlayici entegrasyonu
//   - TASK_HAS_PARENT_PID define -> syscall.c konditif derleme
//   - task_create_user() artik fd_table_init() cagiriyor
//
// v3 Degisiklikleri:
//   - task_create_from_elf() artik argc/argv alıyor (SysV ABI stack kurulumu)
//   - foreground_pid global eklendi (exec sırasında shell'i bekletmek için)

#ifndef TASK_H
#define TASK_H

#include <stdint.h>
#include "syscall.h"    // fd_entry_t, MAX_FDS
#include "signal64.h"   // signal_table_t, syscall_frame_t (sinyal altyapisi)
#include "elf64.h"      // ElfImage (task_create_from_elf icin)

// ============================================================
// Task States
// ============================================================
#define TASK_STATE_READY      0   // Calismaya hazir
#define TASK_STATE_RUNNING    1   // Su anda calisiyor
#define TASK_STATE_BLOCKED    2   // I/O bekliyor (genel blok)
#define TASK_STATE_TERMINATED 3   // Sonlandi
#define TASK_STATE_SLEEPING   4   // SYS_SLEEP: wake_tick'e kadar uyu
#define TASK_STATE_ZOMBIE     5   // Sonlandi ama parent bekliyor (ileride wait())
#define TASK_STATE_STOPPED    6   // SIGSTOP/SIGTSTP ile durduruldu (SIGCONT'a kadar)

// ============================================================
// Task Privilege Levels
// ============================================================
#define TASK_PRIVILEGE_KERNEL   0   // Ring 0
#define TASK_PRIVILEGE_USER     3   // Ring 3

// ============================================================
// Stack Sizes
// ============================================================
// KERNEL_STACK_SIZE: SYSCALL/interrupt handler'in kullandığı Ring-0 stack.
// Bash 4.4 gibi karmaşık uygulamaların derin çağrı zincirleri (readline,
// yash parser, heredoc işleme vb.) kernel geçişlerinde bu stack'i tüketir.
// 2 MB güvenli alt sınırdır.
#define KERNEL_STACK_SIZE  0x200000   // 2 MB – kernel/interrupt handler icin

// USER_STACK_SIZE: Ring-3 task'in çalışma stack'i (SYSRET sonrası RSP).
// Bash 4.4: readline + history + job control + recursive fonksiyonlar
// için tipik tüketim ~4-6 MB; 8 MB tampon sağlar.
#define USER_STACK_SIZE    0x800000   // 8 MB – ring-3 task icin (Bash 4.4+)

// ============================================================
// Oncelik sinir degerleri
// ============================================================
#define TASK_PRIORITY_IDLE     0    // Idle task
#define TASK_PRIORITY_LOW      32
#define TASK_PRIORITY_NORMAL   128
#define TASK_PRIORITY_HIGH     200
#define TASK_PRIORITY_REALTIME 255

// ============================================================
// TSS (Task State Segment) - 64-bit
//
// Intel SDM Vol.3A §7.7: 104-byte 64-bit TSS format.
// CPU, Ring-3 -> Ring-0 gecisinde (SYSCALL veya interrupt) RSP0'i
// otomatik olarak RSP'ye yukler.
// tss_init() bu yapiyi doldurur ve ltr ile TR register'ini yukler.
// ============================================================
typedef struct __attribute__((packed)) {
    uint32_t reserved0;     // +0x00  Ayrilmis
    uint64_t rsp0;          // +0x04  Ring-0 stack (SYSCALL/interrupt icin)
    uint64_t rsp1;          // +0x0C  Ring-1 (kullanilmiyor)
    uint64_t rsp2;          // +0x14  Ring-2 (kullanilmiyor)
    uint64_t reserved1;     // +0x1C  Ayrilmis
    uint64_t ist1;          // +0x24  Interrupt Stack Table slot 1 (NMI/DF vs.)
    uint64_t ist2;          // +0x2C
    uint64_t ist3;          // +0x34
    uint64_t ist4;          // +0x3C
    uint64_t ist5;          // +0x44
    uint64_t ist6;          // +0x4C
    uint64_t ist7;          // +0x54
    uint64_t reserved2;     // +0x5C  Ayrilmis
    uint16_t reserved3;     // +0x64  Ayrilmis
    uint16_t iopb_offset;   // +0x66  I/O Permission Bitmap offset
} tss_t;                    // Toplam = 104 byte

// ASM tarafinda "global kernel_tss" ile tanimli
extern tss_t kernel_tss;

// ============================================================
// GDT Selectors
//
// GDT layout (boot64_unified.asm ile eslesmeli):
//   0x00  Null
//   0x08  Kernel Code  (DPL=0, L=1, 64-bit)
//   0x10  Kernel Data  (DPL=0)
//   0x18  User Data    (DPL=3)              <- SYSRET SS = 0x1B
//   0x20  User Code    (DPL=3, L=1, 64-bit) <- SYSRET CS = 0x23
//   0x28  TSS Low      (16-byte system descriptor)
//   0x30  TSS High
//
// SYSRET 64-bit (Intel SDM):
//   CS = STAR[63:48] + 16 | 3 = 0x23
//   SS = STAR[63:48] + 8  | 3 = 0x1B
// ============================================================
#define GDT_KERNEL_CODE      0x08
#define GDT_KERNEL_DATA      0x10
#define GDT_USER_DATA        0x18
#define GDT_USER_CODE        0x20
#define GDT_TSS_SELECTOR     0x28

// RPL=3 ekli selectors (user-mode segment yukleme icin)
#define GDT_USER_DATA_RPL3   (GDT_USER_DATA | 3)   // 0x1B
#define GDT_USER_CODE_RPL3   (GDT_USER_CODE | 3)   // 0x23

// ============================================================
// CPU Context
// Tum register'lari saklar. Offsetler interrupts64.asm ile
// eslesmeli olmak zorundadir.
//
// Offset haritasi:
//   +0    rax    +8    rbx    +16   rcx    +24   rdx
//   +32   rsi    +40   rdi    +48   rbp    +56   rsp
//   +64   r8     +72   r9     +80   r10    +88   r11
//   +96   r12    +104  r13    +112  r14    +120  r15
//   +128  rip    +136  rflags
//   +144  cs     +152  ss     +160  ds     +168  es
//   +176  fs     +184  gs
//   +192  cr3
// ============================================================
typedef struct {
    uint64_t rax, rbx, rcx, rdx;
    uint64_t rsi, rdi, rbp, rsp;
    uint64_t r8,  r9,  r10, r11, r12, r13, r14, r15;
    uint64_t rip;
    uint64_t rflags;
    uint64_t cs, ss, ds, es, fs, gs;
    uint64_t cr3;
} cpu_context_t;

// ============================================================
// Task Control Block (TCB)
//
// Her task icin tam durum bilgisi burada tutulur.
// Bellek duzeni: cpu_context_t en buyuk alan; derleyici
// padding eklemez cunku tum uyeleri 8-byte hizali.
// ============================================================
// syscall.c'nin SYS_GETPPID'i derleme zamani kararla kullanmasi icin
#define TASK_HAS_PARENT_PID

typedef struct task {
    // ── Kimlik ───────────────────────────────────────────────
    uint32_t pid;               // Process ID (0 = idle)
    uint32_t parent_pid;        // Ebeveyn PID (SYS_GETPPID) [TASK_HAS_PARENT_PID]
    uint32_t pgid;              // Process Group ID (SYS_SETPGID / SYS_GETPGID)
                                //   fork() sonrası parent'ın pgid'ini miras alır.
                                //   Bash pipeline grupları ve iş kontrolü için kritik.
    uint32_t sid;               // Session ID (SYS_SETSID)
                                //   setsid() çağrısında pid = sid = pgid yapılır.
                                //   Login shell'in yeni session başlatması için gerekli.
    char     name[32];          // Task adı (debug/ps için)

    // ── Durum ────────────────────────────────────────────────
    uint32_t state;             // TASK_STATE_*
    uint32_t privilege_level;   // 0=kernel (Ring-0), 3=user (Ring-3)
    int32_t  exit_code;         // SYS_EXIT'in ilettigi cikis kodu

    // ── Zamanlama ────────────────────────────────────────────
    uint32_t priority;          // 0-255; buyuk = oncelikli
    uint64_t time_slice;        // Bu task'a verilen tick sayisi (zaman dilimi)
    uint64_t time_used;         // Mevcut time_slice icinde harcanan tick
    uint64_t last_run_time;     // Son calistirildigi tick (get_system_ticks())
    uint64_t wake_tick;         // TASK_STATE_SLEEPING: bu tick'te uyandır (SYS_SLEEP)

    // ── CPU Durumu ────────────────────────────────────────────
    cpu_context_t context;      // Kaydedilmis register durumu (context switch)

    // ── Kernel Stack ─────────────────────────────────────────
    // Ring-0'da calisir; SYSCALL/interrupt handler buraya gecer.
    // Her context switch'te tss_set_kernel_stack(kernel_stack_top) cagirilir.
    uint64_t kernel_stack_base; // kmalloc() donus adresi (kfree icin)
    uint64_t kernel_stack_top;  // base + size -> TSS RSP0 degeri
    uint64_t kernel_stack_size;

    // ── User Stack ────────────────────────────────────────────
    // Sadece Ring-3 task'larda dolu; kernel task'ta tum alanlar 0.
    // SYSRET sonrasi CPU RSP = user_stack_top olur.
    uint64_t user_stack_base;   // kmalloc() donus adresi (kfree icin)
    uint64_t user_stack_top;    // base + size -> ilk RSP
    uint64_t user_stack_size;

    // ── Dosya Tanimlayici Tablosu ─────────────────────────────
    // stdin=0 / stdout=1 / stderr=2 otomatik olarak serial'a baglanir.
    // SYS_OPEN yeni slot ayirir, SYS_CLOSE kapatir.
    fd_entry_t fd_table[MAX_FDS];

    // ── Sinyal Altyapisi (v10) ────────────────────────────────
    signal_table_t  signal_table;        // Sinyal handler'lari + mask
    syscall_frame_t signal_saved_frame;  // Signal delivery sirasinda kayit
    uint64_t        signal_trampoline;   // rt_sigreturn trampolin adresi

    // ── Zamanlayici Baglantisi ────────────────────────────────
    struct task* next;          // ready_queue linked list
    struct task* prev;

    // ── İstatistik ───────────────────────────────────────────
    uint64_t context_switches;  // Bu task kac kez secildi
    uint64_t total_ticks;       // Toplam harcanan tick

    // ── TLS (Thread Local Storage) ────────────────────────────
    // musl libc arch_prctl(ARCH_SET_FS) ile kurar.
    // task_switch assembly'si FS selector'ı 0 restore ederken
    // MSR_FS_BASE (0xC0000100) sıfırlanır; syscall_dispatch girişinde
    // bu değer wrmsr ile yenilenerek TLS pointer korunur.
    uint64_t fs_base;           // MSR_FS_BASE değeri — context switch'te korunur
    uint64_t gs_base;           // MSR_GS_BASE değeri — context switch'te korunur
    // ELF yükleme aralığı — mmap pool reset'te çakışma tespiti için
    uint64_t elf_load_min;      // elf64_load'ın yazdığı en düşük VA
    uint64_t elf_load_max;      // elf64_load'ın yazdığı en yüksek VA
} task_t;

typedef struct {
    task_t*  head;
    task_t*  tail;
    uint32_t count;
} task_queue_t;

// ============================================================
// Foreground Task Yönetimi
//
// foreground_pid != 0 iken shell input almaz; tüm tuş vuruşları
// ring buffer'a (kb_ring) yönlendirilir.
// task_exit() / SYS_EXIT foreground_pid'i sıfırlar ve shell'i
// geri getirir.
// ============================================================
extern volatile uint32_t foreground_pid;

// Shell'i geri getir (ekrana prompt yaz). keyboard_unified.c'de tanımlı.
extern void shell_restore_prompt(void);

// ============================================================
// TSS Yönetimi
// ============================================================

// TSS'yi sifirla, GDT'deki TSS descriptor'ini kernel_tss adresiyle
// doldur, ltr ile TR register'ini yukle.
// Cagri sirasi: gdt_install_user_segments() -> tss_init()
void tss_init(void);

// Her context switch'te yeni task'in kernel_stack_top'ini RSP0'a yaz.
// SYSCALL/interrupt geldiginde CPU bu adresi kernel RSP olarak kullanir.
static inline void tss_set_kernel_stack(uint64_t rsp0) {
    kernel_tss.rsp0 = rsp0;
}

// ============================================================
// GDT Yönetimi
// ============================================================

// GDT'ye Ring-3 descriptor'lari (0x18 User Data, 0x20 User Code)
// ve TSS descriptor'ini (0x28-0x30) yazar, ardindan lgdt'yi yeniler.
// task_init()'den ONCE cagrilmali.
void gdt_install_user_segments(void);

// ============================================================
// Task Yönetimi
// ============================================================

// Task sistemini baslat – idle task olusturulur.
// Cagri sirasi: gdt_install_user_segments() -> tss_init()
//               -> syscall_init() -> task_init()
void task_init(void);

// Ring-0 kernel task olustur.
// fd_table_init() otomatik cagirilir (stdin/stdout/stderr serial'a baglanir).
task_t* task_create(const char* name,
                    void (*entry_point)(void),
                    uint32_t priority);

// Ring-3 user task olustur.
// – Kernel stack  : SYSCALL/interrupt handler icin ayrilir.
// – User stack    : SYSRET sonrasi RSP olarak kullanilir.
// – context.cs    : 0x23 (User Code, DPL=3)
// – context.ss    : 0x1B (User Data, DPL=3)
// – fd_table_init() otomatik cagirilir.
task_t* task_create_user(const char* name,
                         void (*entry_point)(void),
                         uint32_t priority);

// ELF imajindan Ring-3 user task olustur.
// elf64_load() ile doldurulmus ElfImage yapisini alir;
// iretq frame'i ELF entry noktasina, RSP SysV ABI'ye uygun
// sekilde hizalanmis user stack tepesine ayarlanir.
//
// argc / argv: SysV AMD64 ABI'ye uygun olarak user stack'a yazilir.
//   argv[0] = program adi (genellikle dosya adi)
//   argv[1..] = kullanicinin girdigi argümanlar
//   argc = 0 ise stack'e sadece argc=0, NULL, NULL yazilir.
//
// task_start() ile kuyruğa alinmalidir.
task_t* task_create_from_elf(const char* name,
                              const ElfImage* img,
                              uint32_t priority,
                              int argc,
                              const char** argv);

// Task'i zamanlayici kuyruklarina ekle ve READY durumuna getir.
// task_create()'den sonra cagirilmali.
int  task_start(task_t* task);

// Belirtilen task'i sonlandir (dis cagri). Bellek serbest birakilmaz;
// gc / task_reap() ile yapilmali (henuz implement edilmedi).
void task_terminate(task_t* task);

// Mevcut task'i kendi kendine sonlandir (SYS_EXIT tarafindan cagirilir).
// Bu fonksiyon DONMEZ – scheduler bir sonraki task'a gecer.
void task_exit(void);

// Task durumunu degistir.
void task_set_state(task_t* task, uint32_t new_state);

// Zombie task'ı zombie listesinden çıkar ve belleğini serbest bırak.
// sys_waitpid() child'ı topladıktan sonra bu fonksiyonu çağırır.
void task_reap_zombie(task_t* task);

// ============================================================
// Sleep Yönetimi
// Scheduler tick handler'i (scheduler_tick()) bu fonksiyonu cagirir.
// TASK_STATE_SLEEPING task'larda wake_tick <= get_system_ticks()
// olanlari TASK_STATE_READY'e geri alir.
// ============================================================

// task'i ticks kadar uyut; wake_tick ayarlanir, durum SLEEPING yapilir.
// SYS_SLEEP icerisinden cagirilir.
void task_sleep(task_t* task, uint64_t ticks);

// Uyku suresini dolduran task'lari uyandır (scheduler tick handler'da cagir).
void task_wakeup_check(void);

// ============================================================
// Queue İşlemleri
// ============================================================
void    task_queue_init(task_queue_t* queue);
void    task_queue_push(task_queue_t* queue, task_t* task);
task_t* task_queue_pop(task_queue_t* queue);
void    task_queue_remove(task_queue_t* queue, task_t* task);
int     task_queue_is_empty(task_queue_t* queue);

// ============================================================
// Mevcut Task Yönetimi
// ============================================================
task_t*  task_get_current(void);
void     task_set_current(task_t* task);
task_t*  task_get_next(void);
uint32_t task_get_count(void);
task_t*  task_find_by_pid(uint32_t pid);

// signal64.c'nin extern beklediği wrapper (int pid parametreli)
static inline task_t* task_get_by_pid(int pid) {
    return (pid >= 0) ? task_find_by_pid((uint32_t)pid) : (task_t*)0;
}

// signal64.c accessor'ları
static inline signal_table_t* task_get_signal_table(task_t* t) {
    return t ? &t->signal_table : (signal_table_t*)0;
}
static inline syscall_frame_t* task_get_saved_frame(task_t* t) {
    return t ? &t->signal_saved_frame : (syscall_frame_t*)0;
}
static inline uint64_t task_get_trampoline(task_t* t) {
    return t ? t->signal_trampoline : 0;
}

// SIGSTOP/SIGCONT: task'i durdur veya devam ettir
void task_set_stopped(task_t* t, int stopped);

// ============================================================
// Process Group & Session Yönetimi  (v12 – iş kontrolü)
// ============================================================

// Belirtilen task'ın PGID'ini döndür (task == NULL → -1)
static inline int task_get_pgid(task_t* t) {
    return t ? (int)t->pgid : -1;
}

// Belirtilen task'ın SID'ini döndür (task == NULL → -1)
static inline int task_get_sid(task_t* t) {
    return t ? (int)t->sid : -1;
}

// task'ın PGID'ini değiştir. Aynı session içindeki bir gruba
// katılmak için kullanılır.
// Dönüş: 0=ok, -1=hata (EPERM / geçersiz pgid)
int task_set_pgid(task_t* t, uint32_t new_pgid);

// Yeni session oluştur: sid = pgid = pid; terminal bağını kopar.
// Çağıran zaten session lideriyse -1 döner (EPERM).
// Dönüş: yeni sid | -1
int task_do_setsid(task_t* t);

// ============================================================
// Context Switch
// ============================================================
void task_save_context(cpu_context_t* context);
void task_load_context(cpu_context_t* context);
void task_switch(task_t* from, task_t* to);
void task_save_fs_base(void); // isr_timer öncesi mevcut task FS.base kaydet
void task_restore_fs_base(void); // isr_timer iretq öncesi FS.base restore et

// Assembly implementasyonlari (interrupts64.asm)
extern void task_switch_context(cpu_context_t* old_ctx, cpu_context_t* new_ctx);
extern void task_save_current_context(cpu_context_t* ctx);
extern void task_load_and_jump_context(cpu_context_t* ctx);

// ============================================================
// Utility / Debug
// ============================================================
void task_print_info(task_t* task);   // Tek task bilgisi
void task_list_all(void);             // Tum task'lari listele
void task_print_stats(void);          // Istatistik ozeti

// Durum kodunu insan okunabilir string'e cevirir
const char* task_state_name(uint32_t state);

// ============================================================
// Idle & Test Task'lar
// ============================================================
void    idle_task_entry(void);
task_t* task_create_idle(void);

void test_task_a(void);
void test_task_b(void);
void test_task_c(void);
void offihito_task(void);

// Ring-3 test: SYSRET ile calistirilir, syscall'lari test eder
void user_mode_test_task(void);

#endif // TASK_H