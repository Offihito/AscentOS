// syscall.c - SYSCALL/SYSRET Infrastructure for AscentOS 64-bit
// MSR tabanlı syscall altyapısı + dispatcher + implementasyonlar
//
// Yeni eklenenler (v2):
//   SYS_READ        – serial port okuma (polling)
//   SYS_SLEEP       – timer tabanlı busy-wait sleep
//   SYS_OPEN        – per-task fd tablosu + temel VFS stub
//   SYS_CLOSE       – fd serbest birakma
//   SYS_GETPPID     – parent PID (task_t::parent)
//   SYS_SBRK        – heap genisletme stub (kmalloc_sbrk)
//   SYS_GETPRIORITY / SYS_SETPRIORITY – task onceligi okuma/yazma
//   SYS_GETTICKS    – SYS_UPTIME alias

#include "syscall.h"
#include "task.h"
#include "scheduler.h"
#include "timer.h"

// ============================================================
// External declarations
// ============================================================
extern void serial_print(const char* str);
extern void serial_putchar(char c);
extern void int_to_str(int num, char* str);

// Assembly entry point
extern void syscall_entry(void);

// ============================================================
// SERIAL INPUT – inline implementasyon
// kernel64.c'de sadece TX taraf (serial_putchar) tanimli.
// RX tarafi burada COM1 register'lari ile dogrudan implement edildi.
//
// COM1 port haritasi:
//   0x3F8 + 0  : Data Register (RBR okuma / THR yazma)
//   0x3F8 + 5  : Line Status Register (LSR)
//     LSR bit0 = Data Ready (DR) : 1 ise okunacak byte var
//     LSR bit5 = THR Empty       : 1 ise yazilabilir (TX)
// ============================================================
#define SERIAL_COM1_BASE    0x3F8
#define SERIAL_LSR_OFFSET   5
#define SERIAL_DR_BIT       (1 << 0)   // Data Ready

static inline uint8_t serial_inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// serial_data_ready: RX tamponunda okunacak byte var mi?
// 1 = evet, 0 = hayir (non-blocking kontrol)
int serial_data_ready(void) {
    return (serial_inb(SERIAL_COM1_BASE + SERIAL_LSR_OFFSET) & SERIAL_DR_BIT) ? 1 : 0;
}

// serial_getchar: Data hazirsa 1 byte oku, yoksa -1 doner (non-blocking).
// Bloklayan okuma icin: while(!serial_data_ready()); return serial_getchar();
char serial_getchar(void) {
    if (!serial_data_ready()) return (char)-1;
    return (char)serial_inb(SERIAL_COM1_BASE);
}

// SBRK – memory_unified.c'de tanimli, burada sadece extern bildirim
extern uint64_t kmalloc_get_brk(void);
extern uint64_t kmalloc_set_brk(uint64_t new_brk);

// ============================================================
// USER-MODE POINTER DOGRULAMA
// Sayfa tablosu henuz yok (flat memory). Temel kontroller:
//   1. NULL kontrolu
//   2. Canonical user address: bit[63:47] sifir olmali
//   3. Uzunluk tasma kontrolu
// Sayfa tablosu eklenince mmu_validate_user_range() ile guncelle.
// ============================================================
#define USER_SPACE_MAX  0x00007FFFFFFFFFFFull

static int is_valid_user_ptr(const void* ptr, uint64_t len) {
    if (!ptr) return 0;
    uint64_t addr = (uint64_t)ptr;
    if ((addr >> 47) != 0) return 0;              // kernel veya non-canonical
    if (len > 0 && (addr + len) > USER_SPACE_MAX) return 0;
    return 1;
}

// is_valid_user_string: NULL-terminated string icin maksimum maxlen byte
// kontrol eder, strinin gercekten sonlandigini test etmez (sayfa tablosu yok).
static int is_valid_user_string(const char* s, uint64_t maxlen) {
    return is_valid_user_ptr(s, maxlen);
}

// ============================================================
// Dahili yardimcilar
// ============================================================
static void print_hex64(uint64_t v) {
    const char* h = "0123456789ABCDEF";
    char buf[17];
    for (int i = 0; i < 16; i++)
        buf[i] = h[(v >> (60 - i * 4)) & 0xF];
    buf[16] = '\0';
    serial_print(buf);
}

static void print_uint64(uint64_t v) {
    if (v == 0) { serial_print("0"); return; }
    char buf[21];
    int  i = 0;
    uint64_t t = v;
    while (t > 0) { buf[i++] = '0' + (t % 10); t /= 10; }
    buf[i] = '\0';
    // tersine cevir
    for (int a = 0, b = i - 1; a < b; a++, b--) {
        char c = buf[a]; buf[a] = buf[b]; buf[b] = c;
    }
    serial_print(buf);
}

// ============================================================
// String kopyalama (libc yok)
// ============================================================
static void my_strncpy(char* dst, const char* src, int n) {
    int i = 0;
    while (i < n - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

// ============================================================
// Internal state
// ============================================================
static int syscall_enabled = 0;

// ============================================================
// FD TABLOSU IMPLEMENTASYONU
// Her task icin task_t::fd_table[MAX_FDS] ayrilir.
// Bu modul sadece yardimci fonksiyonlar saglar;
// tablo task_create() / task_create_user() tarafindan
// fd_table_init() ile hazirlanir.
// ============================================================

// 0=stdin, 1=stdout, 2=stderr'i seri porta bagla; gerisini kapat.
void fd_table_init(fd_entry_t* table) {
    for (int i = 0; i < MAX_FDS; i++) {
        table[i].type    = FD_TYPE_NONE;
        table[i].flags   = 0;
        table[i].is_open = 0;
        table[i].offset  = 0;
        table[i].path[0] = '\0';
    }
    // stdin  (fd 0) – okuma
    table[0].type    = FD_TYPE_SERIAL;
    table[0].flags   = O_RDONLY;
    table[0].is_open = 1;
    my_strncpy(table[0].path, "/dev/serial0", 60);

    // stdout (fd 1) – yazma
    table[1].type    = FD_TYPE_SERIAL;
    table[1].flags   = O_WRONLY;
    table[1].is_open = 1;
    my_strncpy(table[1].path, "/dev/serial0", 60);

    // stderr (fd 2) – yazma
    table[2].type    = FD_TYPE_SERIAL;
    table[2].flags   = O_WRONLY;
    table[2].is_open = 1;
    my_strncpy(table[2].path, "/dev/serial0", 60);
}

// Bos fd slotu ayir (3..MAX_FDS-1 arasinda).
// Basari: fd degeri doner. Hata: -1.
int fd_alloc(fd_entry_t* table, uint8_t type, uint8_t flags, const char* path) {
    for (int i = 3; i < MAX_FDS; i++) {
        if (!table[i].is_open) {
            table[i].type    = type;
            table[i].flags   = flags;
            table[i].is_open = 1;
            table[i].offset  = 0;
            if (path)
                my_strncpy(table[i].path, path, 60);
            else
                table[i].path[0] = '\0';
            return i;
        }
    }
    return -1;  // EMFILE
}

// fd'yi serbest birak. Basari: 0. Hata: -1.
int fd_free(fd_entry_t* table, int fd) {
    if (fd < 0 || fd >= MAX_FDS) return -1;
    if (!table[fd].is_open)      return -1;
    table[fd].is_open = 0;
    table[fd].type    = FD_TYPE_NONE;
    table[fd].path[0] = '\0';
    return 0;
}

// Gecerli fd_entry_t pointer'i doner; hata durumunda NULL.
fd_entry_t* fd_get(fd_entry_t* table, int fd) {
    if (fd < 0 || fd >= MAX_FDS) return 0;
    if (!table[fd].is_open)      return 0;
    return &table[fd];
}

// ============================================================
// SYSCALL_INIT
// ============================================================
void syscall_init(void) {
    serial_print("[SYSCALL] Initializing SYSCALL/SYSRET infrastructure...\n");

    // ── 1. CPUID: SYSCALL destegi kontrolu ──────────────────────
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile ("cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(0x80000001), "c"(0));
    if (!(edx & (1 << 11))) {
        serial_print("[SYSCALL] ERROR: CPU does not support SYSCALL!\n");
        return;
    }
    serial_print("[SYSCALL] CPU supports SYSCALL/SYSRET\n");

    // ── 2. IA32_EFER: SCE bitini set et ─────────────────────────
    uint64_t efer = rdmsr(MSR_EFER);
    efer |= EFER_SCE;
    wrmsr(MSR_EFER, efer);
    if (!(rdmsr(MSR_EFER) & EFER_SCE)) {
        serial_print("[SYSCALL] ERROR: EFER.SCE bit set failed!\n");
        return;
    }
    serial_print("[SYSCALL] EFER.SCE enabled\n");

    // ── 3. IA32_STAR: Segment selectors ─────────────────────────
    wrmsr(MSR_STAR, STAR_VALUE);
    serial_print("[SYSCALL] MSR_STAR = 0x");
    print_hex64(rdmsr(MSR_STAR));
    serial_print("\n");
    serial_print("[SYSCALL] SYSRET -> CS=0x23 (User Code), SS=0x1B (User Data)\n");

    // ── 4. IA32_LSTAR: syscall_entry adresi ─────────────────────
    uint64_t entry_addr = (uint64_t)syscall_entry;
    wrmsr(MSR_LSTAR, entry_addr);
    serial_print("[SYSCALL] LSTAR = 0x");
    print_hex64(entry_addr);
    serial_print("\n");

    // ── 5. IA32_CSTAR: 32-bit compat (kullanilmiyor) ────────────
    wrmsr(MSR_CSTAR, 0);

    // ── 6. IA32_FMASK: IF + DF entry'de sifirlanir ──────────────
    wrmsr(MSR_FMASK, SYSCALL_RFLAGS_MASK);
    serial_print("[SYSCALL] FMASK set (IF+DF masked on entry)\n");

    syscall_enabled = 1;
    serial_print("[SYSCALL] SYSCALL/SYSRET ready!\n");
    serial_print("[SYSCALL] Supported syscalls: WRITE READ EXIT GETPID YIELD "
                 "SLEEP UPTIME DEBUG OPEN CLOSE GETPPID SBRK "
                 "GETPRIORITY SETPRIORITY GETTICKS\n");
}

int syscall_is_enabled(void) { return syscall_enabled; }

// ============================================================
// SYSCALL IMPLEMENTASYONLARI (dispatcher'dan cagrilir)
// Her fonksiyon frame uzerinden arguman alir ve
// frame->rax'e donus degerini yazar.
// ============================================================

// ── SYS_WRITE (1) ──────────────────────────────────────────────
// write(fd, buf, len) -> bytes_written | SYSCALL_ERR_*
//
// fd=0 (stdin) yazma girisimi reddedilir.
// fd=1/2 (stdout/stderr): serial porta yonlendirilir.
// fd>=3: fd_table'dan bakilir (task_t fd_table iceriyorsa).
//
// NOT: task_t henuz fd_table iceremeyebilir. Fallback mantigi:
//   fd 0-2 icin fd_table'a BAKILMAZ, dogrudan serial kulllanilir.
//   fd >= 3 icin fd_table zorunlu.
static void sys_write(syscall_frame_t* frame) {
    int         fd  = (int)frame->rdi;
    const char* buf = (const char*)frame->rsi;
    uint64_t    len = frame->rdx;

    // Temel dogrulama
    if (fd < 0 || fd >= MAX_FDS)     { frame->rax = SYSCALL_ERR_BADF;  return; }
    if (!is_valid_user_ptr(buf, len)) { frame->rax = SYSCALL_ERR_INVAL; return; }
    if (len == 0)                     { frame->rax = 0; return; }
    if (len > 4096)                   { frame->rax = SYSCALL_ERR_INVAL; return; }

    // fd=0 stdin'e yazma: izin yok
    if (fd == 0) { frame->rax = SYSCALL_ERR_BADF; return; }

    // fd=1 stdout, fd=2 stderr: dogrudan serial (fd_table gerekmez)
    if (fd == 1 || fd == 2) {
        for (uint64_t i = 0; i < len; i++)
            serial_putchar(buf[i]);
        frame->rax = len;
        return;
    }

    // fd >= 3: fd_table gerekli
    task_t* cur = task_get_current();
    if (!cur) { frame->rax = SYSCALL_ERR_PERM; return; }

    fd_entry_t* ent = fd_get(cur->fd_table, fd);
    if (!ent)  { frame->rax = SYSCALL_ERR_BADF; return; }
    if (ent->flags == O_RDONLY) { frame->rax = SYSCALL_ERR_BADF; return; }

    for (uint64_t i = 0; i < len; i++)
        serial_putchar(buf[i]);

    if (ent->type == FD_TYPE_FILE)
        ent->offset += len;

    frame->rax = len;
}

// ── SYS_READ (2) ───────────────────────────────────────────────
// read(fd, buf, len) -> bytes_read | SYSCALL_ERR_*
//
// fd=0 (stdin): serial porttan non-blocking polling okuma.
// fd=1/2 yazma amaclı, okuma izni yok.
// fd>=3: fd_table gerekli.
//
// NOT: fd 0 icin fd_table GEREKMEZ – dogrudan serial RX kullanilir.
static void sys_read(syscall_frame_t* frame) {
    int      fd  = (int)frame->rdi;
    char*    buf = (char*)frame->rsi;
    uint64_t len = frame->rdx;

    if (fd < 0 || fd >= MAX_FDS)      { frame->rax = SYSCALL_ERR_BADF;  return; }
    if (!is_valid_user_ptr(buf, len))  { frame->rax = SYSCALL_ERR_INVAL; return; }
    if (len == 0)                      { frame->rax = 0; return; }
    if (len > 4096)                    { frame->rax = SYSCALL_ERR_INVAL; return; }

    // fd=1,2 stdout/stderr: sadece yazma amaclı
    if (fd == 1 || fd == 2) { frame->rax = SYSCALL_ERR_BADF; return; }

    // fd=0 stdin: dogrudan serial RX (fd_table gerekmez)
    if (fd == 0) {
        uint64_t count = 0;
        while (count < len) {
            if (!serial_data_ready()) break;   // veri yok, non-blocking: dur
            char c = serial_getchar();
            buf[count++] = c;
            if (c == '\n') break;              // satir sonu: okuma tamam
        }
        frame->rax = count;   // 0 = veri yok (non-blocking davranis)
        return;
    }

    // fd >= 3: fd_table gerekli
    task_t* cur = task_get_current();
    if (!cur) { frame->rax = SYSCALL_ERR_PERM; return; }

    fd_entry_t* ent = fd_get(cur->fd_table, fd);
    if (!ent)  { frame->rax = SYSCALL_ERR_BADF; return; }
    if (ent->flags == O_WRONLY) { frame->rax = SYSCALL_ERR_BADF; return; }

    if (ent->type == FD_TYPE_SERIAL) {
        uint64_t count = 0;
        while (count < len) {
            if (!serial_data_ready()) break;
            char c = serial_getchar();
            buf[count++] = c;
            if (c == '\n') break;
        }
        frame->rax = count;
        return;
    }

    // FD_TYPE_FILE: VFS henuz yok
    frame->rax = SYSCALL_ERR_NOSYS;
}

// ── SYS_EXIT (3) ────────────────────────────────────────────────
// exit(code) -> noreturn
//
// Mevcut task'i sonlandirir; idle task (pid=0) sonlandiramaz.
// task_exit() zaten donmez; bu fonksiyon da donmez.
static void sys_exit(syscall_frame_t* frame) {
    int exit_code = (int)frame->rdi;
    task_t* cur   = task_get_current();

    if (cur && cur->pid != 0) {
        serial_print("[SYSCALL] SYS_EXIT: pid=");
        print_uint64(cur->pid);
        serial_print(" code=");
        { char b[16]; int_to_str(exit_code, b); serial_print(b); }
        serial_print("\n");
        task_exit();   // donmez
    }
    // idle task icin hicbir sey yapma
    frame->rax = SYSCALL_OK;
}

// ── SYS_GETPID (4) ──────────────────────────────────────────────
// getpid() -> pid
static void sys_getpid(syscall_frame_t* frame) {
    task_t* cur = task_get_current();
    frame->rax  = cur ? (uint64_t)cur->pid : 0;
}

// ── SYS_YIELD (5) ───────────────────────────────────────────────
// yield() -> 0
// Calisiyor olan task'i preempt eder, bir sonraki task'a gecer.
static void sys_yield(syscall_frame_t* frame) {
    scheduler_yield();
    frame->rax = SYSCALL_OK;
}

// ── SYS_SLEEP (6) ───────────────────────────────────────────────
// sleep(ticks) -> 0
//
// Guvenli implementasyon: interrupt-safe busy-wait.
// scheduler_yield() yerine "sti; hlt" dongusu kullanilir;
// bu sayede timer interrupt'i CPU'yu uyandirabilir.
// Klavyeyi KILITMEZ.
//
// GELECEK: task'i SLEEPING yap, wake_tick ayarla, scheduler handle etsin.
static void sys_sleep(syscall_frame_t* frame) {
    uint64_t ticks = frame->rdi;
    if (ticks == 0)      { frame->rax = SYSCALL_OK;          return; }
    if (ticks > 60000)   { frame->rax = SYSCALL_ERR_INVAL;   return; }

    uint64_t end = get_system_ticks() + ticks;

    // Interrupt-safe: hlt interrupt'i bekler, timer tick artirir, kontrol eder.
    // IF zaten set ise calisir; degilse sadece spin yapar (en kotu durum).
    while (get_system_ticks() < end) {
        __asm__ volatile ("sti; hlt" ::: "memory");
    }

    frame->rax = SYSCALL_OK;
}

// ── SYS_UPTIME (7) ──────────────────────────────────────────────
// uptime() -> system_ticks
static void sys_uptime(syscall_frame_t* frame) {
    frame->rax = get_system_ticks();
}

// ── SYS_DEBUG (8) ───────────────────────────────────────────────
// debug(msg) -> 0
// Kernel debug cikisi: serial porta [DEBUG] prefiksi ile yazar.
static void sys_debug(syscall_frame_t* frame) {
    const char* msg = (const char*)frame->rdi;
    if (!is_valid_user_string(msg, 256)) { frame->rax = SYSCALL_ERR_INVAL; return; }
    serial_print("[DEBUG] ");
    serial_print(msg);
    serial_print("\n");
    frame->rax = SYSCALL_OK;
}

// ── SYS_OPEN (9) ────────────────────────────────────────────────
// open(path, flags) -> fd | SYSCALL_ERR_*
//
// Desteklenen ozel yollar:
//   /dev/serial0  -> FD_TYPE_SERIAL (stdin/stdout benzeri ek port)
//   diger         -> FD_TYPE_FILE stub (VFS hazir oldugunda guncelle)
//
// open() basarisiz olursa negatif hata kodu doner.
static void sys_open(syscall_frame_t* frame) {
    const char* path  = (const char*)frame->rdi;
    uint64_t    flags = frame->rsi;

    if (!is_valid_user_string(path, 128)) { frame->rax = SYSCALL_ERR_INVAL; return; }

    // Sadece taninan flag kombinasyonlarına izin ver
    uint64_t valid_flags = O_RDONLY | O_WRONLY | O_RDWR | O_CREAT | O_TRUNC | O_APPEND;
    if (flags & ~valid_flags)             { frame->rax = SYSCALL_ERR_INVAL; return; }

    task_t* cur = task_get_current();
    if (!cur)                             { frame->rax = SYSCALL_ERR_PERM;  return; }

    uint8_t fd_type;
    // /dev/serial0 – seri port
    // Basit path kiyaslamasi (strncmp yerine)
    const char* p1 = path;
    const char* p2 = "/dev/serial0";
    int match = 1;
    while (*p1 && *p2) { if (*p1 != *p2) { match = 0; break; } p1++; p2++; }
    if (match && *p1 == '\0' && *p2 == '\0') {
        fd_type = FD_TYPE_SERIAL;
    } else {
        // Gercek VFS yok: sadece O_CREAT olmadan FILE stub kabul et
        if (flags & O_CREAT) { frame->rax = SYSCALL_ERR_NOENT; return; }
        fd_type = FD_TYPE_FILE;
    }

    int new_fd = fd_alloc(cur->fd_table, fd_type, (uint8_t)(flags & 0xFF), path);
    if (new_fd < 0) { frame->rax = SYSCALL_ERR_MFILE; return; }

    serial_print("[SYSCALL] open(\"");
    serial_print(path);
    serial_print("\") -> fd=");
    print_uint64((uint64_t)new_fd);
    serial_print("\n");

    frame->rax = (uint64_t)new_fd;
}

// ── SYS_CLOSE (10) ──────────────────────────────────────────────
// close(fd) -> 0 | SYSCALL_ERR_*
//
// stdin/stdout/stderr (fd 0-2) katilikla kapatilmaz;
// kullanici fd'lerini (3..MAX_FDS-1) serbest birakir.
static void sys_close(syscall_frame_t* frame) {
    int fd = (int)frame->rdi;

    // Standart stream'leri kapatmaya izin verme
    if (fd < 3)                          { frame->rax = SYSCALL_ERR_BADF; return; }
    if (fd >= MAX_FDS)                   { frame->rax = SYSCALL_ERR_BADF; return; }

    task_t* cur = task_get_current();
    if (!cur)                            { frame->rax = SYSCALL_ERR_PERM; return; }

    int ret = fd_free(cur->fd_table, fd);
    frame->rax = (ret == 0) ? SYSCALL_OK : SYSCALL_ERR_BADF;
}

// ── SYS_GETPPID (11) ────────────────────────────────────────────
// getppid() -> parent_pid
//
// task_t'ye parent_pid alani eklenmediyse 0 doner.
// (task.h'da "uint32_t parent_pid;" eklenmesi onerilir.)
static void sys_getppid(syscall_frame_t* frame) {
    task_t* cur = task_get_current();
    if (!cur) { frame->rax = 0; return; }

    // task_t'de parent_pid varsa kullan; yoksa 0 (init süreci anlaminda)
#ifdef TASK_HAS_PARENT_PID
    frame->rax = (uint64_t)cur->parent_pid;
#else
    // Henuz implement edilmedi – donus degeri 0 (kernel/init parent)
    frame->rax = 0;
#endif
}

// ── SYS_SBRK (12) ───────────────────────────────────────────────
// sbrk(increment) -> old_break | SYSCALL_ERR_*
//
// Kernel heap'ini (kmalloc bölgesi) increment kadar büyütür.
// increment=0: mevcut break adresini doner (brk sorgulama).
// Gercek user-space heap yonetimi icin per-task brk gerekli;
// su an kernel heap uzerinde calisir (flat memory varsayimi).
static void sys_sbrk(syscall_frame_t* frame) {
    int64_t increment = (int64_t)frame->rdi;

    uint64_t old_brk = kmalloc_get_brk();

    if (increment == 0) {
        // Sadece sorgula
        frame->rax = old_brk;
        return;
    }

    if (increment < 0) {
        // Heap kucultme: basit implementasyonda desteklenmez
        frame->rax = SYSCALL_ERR_INVAL;
        return;
    }

    // Maksimum tek seferde 1MB artis
    if ((uint64_t)increment > (1024 * 1024)) {
        frame->rax = SYSCALL_ERR_INVAL;
        return;
    }

    uint64_t new_brk = kmalloc_set_brk(old_brk + (uint64_t)increment);
    if (new_brk == (uint64_t)-1) {
        frame->rax = SYSCALL_ERR_NOMEM;
        return;
    }

    frame->rax = old_brk;   // POSIX sbrk: eski break adresini doner
}

// ── SYS_GETPRIORITY (13) ────────────────────────────────────────
// getpriority() -> priority (0-255)
static void sys_getpriority(syscall_frame_t* frame) {
    task_t* cur = task_get_current();
    frame->rax  = cur ? (uint64_t)cur->priority : 0;
}

// ── SYS_SETPRIORITY (14) ────────────────────────────────────────
// setpriority(prio) -> 0 | SYSCALL_ERR_INVAL
//
// Gecerli aralik: 0-255.
// Mevcut task'in (idle dahil) onceligini degistirir.
// Sadece NULL task kontrolu var; PID=0 kisitlamasi kaldirildi
// cunku shell idle context'inde calisabilir.
static void sys_setpriority(syscall_frame_t* frame) {
    uint64_t prio = frame->rdi;
    if (prio > 255) { frame->rax = SYSCALL_ERR_INVAL; return; }

    task_t* cur = task_get_current();
    if (!cur) { frame->rax = SYSCALL_ERR_PERM; return; }

    cur->priority  = (uint32_t)prio;
    frame->rax     = SYSCALL_OK;
}

// ── SYS_GETTICKS (15) ───────────────────────────────────────────
// getticks() -> system_ticks  (SYS_UPTIME alias; daha net isim)
static void sys_getticks(syscall_frame_t* frame) {
    frame->rax = get_system_ticks();
}

// ============================================================
// SYSCALL_DISPATCH
// Assembly stub (syscall_entry) tarafindan cagirilir.
// Her syscall kendi static fonksiyonuna delege edilir;
// bu sayede dispatcher sade ve okunabilir kalir.
// ============================================================
void syscall_dispatch(syscall_frame_t* frame) {
    uint64_t num = frame->rax;

    switch (num) {
    case SYS_WRITE:        sys_write(frame);       break;
    case SYS_READ:         sys_read(frame);        break;
    case SYS_EXIT:         sys_exit(frame);        break;
    case SYS_GETPID:       sys_getpid(frame);      break;
    case SYS_YIELD:        sys_yield(frame);       break;
    case SYS_SLEEP:        sys_sleep(frame);       break;
    case SYS_UPTIME:       sys_uptime(frame);      break;
    case SYS_DEBUG:        sys_debug(frame);       break;
    case SYS_OPEN:         sys_open(frame);        break;
    case SYS_CLOSE:        sys_close(frame);       break;
    case SYS_GETPPID:      sys_getppid(frame);     break;
    case SYS_SBRK:         sys_sbrk(frame);        break;
    case SYS_GETPRIORITY:  sys_getpriority(frame); break;
    case SYS_SETPRIORITY:  sys_setpriority(frame); break;
    case SYS_GETTICKS:     sys_getticks(frame);    break;
    default:
        serial_print("[SYSCALL] Unknown syscall: ");
        { char b[16]; int_to_str((int)(num & 0xFFFF), b); serial_print(b); }
        serial_print("\n");
        frame->rax = SYSCALL_ERR_NOSYS;
        break;
    }
}

// ============================================================
// SYSCALL_TEST
// Tum syscall'lari kernel modunda test eder.
// Ring-3 task'tan da ayni sekilde kullanilabilir.
// ============================================================
void syscall_test(void) {
    if (!syscall_enabled) {
        serial_print("[SYSCALL TEST] SYSCALL not enabled!\n");
        return;
    }
    serial_print("\n========================================\n");
    serial_print("[SYSCALL TEST] Starting comprehensive syscall tests...\n");
    serial_print("========================================\n");

    uint64_t ret = 0;

#define DO_SYSCALL0(num) \
    __asm__ volatile ("syscall" : "=a"(ret) : "a"((uint64_t)(num)) : "rcx","r11")

#define DO_SYSCALL1(num, a1) \
    __asm__ volatile ("syscall" : "=a"(ret) \
        : "a"((uint64_t)(num)), "D"((uint64_t)(a1)) : "rcx","r11","memory")

#define DO_SYSCALL2(num, a1, a2) \
    __asm__ volatile ("syscall" : "=a"(ret) \
        : "a"((uint64_t)(num)), "D"((uint64_t)(a1)), "S"((uint64_t)(a2)) \
        : "rcx","r11","memory")

#define DO_SYSCALL3(num, a1, a2, a3) \
    __asm__ volatile ("syscall" : "=a"(ret) \
        : "a"((uint64_t)(num)), "D"((uint64_t)(a1)), "S"((uint64_t)(a2)), \
          "d"((uint64_t)(a3)) : "rcx","r11","memory")

    // ── Test 1: SYS_WRITE ──────────────────────────────────────
    serial_print("\n[TEST 1] SYS_WRITE (fd=1, stdout):\n");
    const char* wmsg = "  << Hello from SYS_WRITE! >>\n";
    DO_SYSCALL3(SYS_WRITE, 1, wmsg, 31);
    serial_print("  ret="); { char b[16]; int_to_str((int)ret, b); serial_print(b); } serial_print("\n");

    // ── Test 2: SYS_WRITE – gecersiz fd ────────────────────────
    serial_print("\n[TEST 2] SYS_WRITE (fd=99, expect EBADF):\n");
    DO_SYSCALL3(SYS_WRITE, 99, wmsg, 31);
    serial_print("  ret="); { char b[16]; int_to_str((int)ret, b); serial_print(b); } serial_print(" (expect -5)\n");

    // ── Test 3: SYS_READ ───────────────────────────────────────
    serial_print("\n[TEST 3] SYS_READ (fd=0, non-blocking, veri yoksa 0 beklenir):\n");
    char rbuf[32] = {0};
    DO_SYSCALL3(SYS_READ, 0, rbuf, 16);
    serial_print("  bytes_read="); print_uint64(ret); serial_print("\n");

    // ── Test 4: SYS_GETPID ─────────────────────────────────────
    serial_print("\n[TEST 4] SYS_GETPID:\n");
    DO_SYSCALL0(SYS_GETPID);
    serial_print("  pid="); print_uint64(ret); serial_print("\n");

    // ── Test 5: SYS_GETPPID ────────────────────────────────────
    serial_print("\n[TEST 5] SYS_GETPPID:\n");
    DO_SYSCALL0(SYS_GETPPID);
    serial_print("  ppid="); print_uint64(ret); serial_print("\n");

    // ── Test 6: SYS_GETPRIORITY ────────────────────────────────
    serial_print("\n[TEST 6] SYS_GETPRIORITY:\n");
    DO_SYSCALL0(SYS_GETPRIORITY);
    serial_print("  priority="); print_uint64(ret); serial_print("\n");

    // ── Test 7: SYS_SETPRIORITY ────────────────────────────────
    serial_print("\n[TEST 7] SYS_SETPRIORITY(128):\n");
    DO_SYSCALL1(SYS_SETPRIORITY, 128);
    serial_print("  ret="); { char b[16]; int_to_str((int)ret, b); serial_print(b); }
    serial_print(" (expect 0)\n");

    DO_SYSCALL0(SYS_GETPRIORITY);
    serial_print("  new_priority="); print_uint64(ret); serial_print(" (expect 128)\n");

    // ── Test 8: SYS_UPTIME / SYS_GETTICKS ─────────────────────
    serial_print("\n[TEST 8] SYS_UPTIME & SYS_GETTICKS:\n");
    DO_SYSCALL0(SYS_UPTIME);
    serial_print("  uptime_ticks="); print_uint64(ret); serial_print("\n");
    DO_SYSCALL0(SYS_GETTICKS);
    serial_print("  getticks="); print_uint64(ret); serial_print("\n");

    // ── Test 9: SYS_SLEEP ──────────────────────────────────────
    serial_print("\n[TEST 9] SYS_SLEEP(10 ticks):\n");
    uint64_t before = get_system_ticks();
    DO_SYSCALL1(SYS_SLEEP, 10);
    uint64_t after  = get_system_ticks();
    serial_print("  ret="); { char b[16]; int_to_str((int)ret, b); serial_print(b); }
    serial_print("  elapsed="); print_uint64(after - before); serial_print(" ticks\n");

    // ── Test 10: SYS_YIELD ─────────────────────────────────────
    serial_print("\n[TEST 10] SYS_YIELD:\n");
    DO_SYSCALL0(SYS_YIELD);
    serial_print("  ret="); { char b[16]; int_to_str((int)ret, b); serial_print(b); }
    serial_print(" (expect 0)\n");

    // ── Test 11: SYS_DEBUG ─────────────────────────────────────
    serial_print("\n[TEST 11] SYS_DEBUG:\n");
    DO_SYSCALL1(SYS_DEBUG, "Hello from SYS_DEBUG test!");
    serial_print("  ret="); { char b[16]; int_to_str((int)ret, b); serial_print(b); }
    serial_print(" (expect 0)\n");

    // ── Test 12: SYS_OPEN / SYS_CLOSE ─────────────────────────
    serial_print("\n[TEST 12] SYS_OPEN(\"/dev/serial0\", O_RDWR):\n");
    DO_SYSCALL2(SYS_OPEN, "/dev/serial0", O_RDWR);
    serial_print("  fd="); { char b[16]; int_to_str((int)ret, b); serial_print(b); }
    serial_print(" (expect >=3)\n");
    int open_fd = (int)ret;

    if ((int64_t)ret >= 3) {
        serial_print("\n[TEST 12b] SYS_CLOSE(fd):\n");
        DO_SYSCALL1(SYS_CLOSE, open_fd);
        serial_print("  ret="); { char b[16]; int_to_str((int)ret, b); serial_print(b); }
        serial_print(" (expect 0)\n");
    }

    // ── Test 13: SYS_CLOSE – gecersiz fd ───────────────────────
    serial_print("\n[TEST 13] SYS_CLOSE(stdin fd=0, expect EBADF):\n");
    DO_SYSCALL1(SYS_CLOSE, 0);
    serial_print("  ret="); { char b[16]; int_to_str((int)ret, b); serial_print(b); }
    serial_print(" (expect -5)\n");

    // ── Test 14: SYS_SBRK ──────────────────────────────────────
    serial_print("\n[TEST 14] SYS_SBRK(0) – brk sorgula:\n");
    DO_SYSCALL1(SYS_SBRK, 0);
    serial_print("  current_brk=0x"); print_hex64(ret); serial_print("\n");

    serial_print("\n[TEST 14b] SYS_SBRK(4096) – heap'i 4KB buyut:\n");
    uint64_t old_brk = ret;
    DO_SYSCALL1(SYS_SBRK, 4096);
    serial_print("  old_brk=0x"); print_hex64(ret);
    if (ret == old_brk) serial_print(" (ok, returned old_brk)\n");
    else                serial_print(" (unexpected!)\n");

    // ── Test 15: Bilinmeyen syscall ────────────────────────────
    serial_print("\n[TEST 15] Unknown syscall (999, expect ENOSYS=-2):\n");
    DO_SYSCALL0(999);
    serial_print("  ret="); { char b[16]; int_to_str((int)ret, b); serial_print(b); }
    serial_print(" (expect -2)\n");

    serial_print("\n========================================\n");
    serial_print("[SYSCALL TEST] All tests completed.\n");
    serial_print("========================================\n\n");

#undef DO_SYSCALL0
#undef DO_SYSCALL1
#undef DO_SYSCALL2
#undef DO_SYSCALL3
}