// syscall.c - SYSCALL/SYSRET Infrastructure for AscentOS 64-bit
// MSR tabanlı syscall altyapısı + dispatcher + implementasyonlar
//
// v3 Yeni Eklemeler:
//   SYS_MMAP    (16) – anonim bellek haritalama (kmalloc tabanlı)
//   SYS_MUNMAP  (17) – haritalanmış bölgeyi serbest bırak
//   SYS_BRK     (18) – program break'i doğrudan set et
//   SYS_FORK    (19) – mevcut task'ı kopyala (tam kopya, CoW yok)
//   SYS_EXECVE  (20) – yeni program yükle (stub; gelecek ELF loader için)
//   SYS_WAITPID (21) – çocuk işlem bitmesini bekle (WNOHANG destekli)
//   SYS_PIPE    (22) – anonim pipe oluştur (fd[0]=okuma, fd[1]=yazma)
//   SYS_DUP2    (23) – fd kopyala (oldfd → newfd, atomik kapat+kopyala)

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
// VGA print — user process stdout (fd=1) için
extern void print_str64(const char* str, uint8_t color);
#define VGA_WHITE 0x0F

// Assembly entry point
extern void syscall_entry(void);

// ============================================================
// SERIAL INPUT
// ============================================================
#define SERIAL_COM1_BASE    0x3F8
#define SERIAL_LSR_OFFSET   5
#define SERIAL_DR_BIT       (1 << 0)

static inline uint8_t serial_inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

int serial_data_ready(void) {
    return (serial_inb(SERIAL_COM1_BASE + SERIAL_LSR_OFFSET) & SERIAL_DR_BIT) ? 1 : 0;
}

char serial_getchar(void) {
    if (!serial_data_ready()) return (char)-1;
    return (char)serial_inb(SERIAL_COM1_BASE);
}

// SBRK / BRK – memory_unified.c'de tanımlı
extern uint64_t kmalloc_get_brk(void);
extern uint64_t kmalloc_set_brk(uint64_t new_brk);

// Genel amaçlı bellek ayırma – memory_unified.c / kernel64.c
// kmalloc / kfree: memory_unified.c'de tanımlı (zaten kullanılıyor)
extern void* kmalloc(uint64_t size);
extern void  kfree(void* ptr);
// memset64 / memcpy64: kernel64.c'de tanımlı
extern void* memset64(void* dest, int val, uint64_t n);
extern void* memcpy64(void* dest, const void* src, uint64_t n);

// Kısa takma adlar – sadece bu dosyada, extern değil
#define kmemset(dst, val, n)  memset64((dst),(val),(uint64_t)(n))
#define kmemcpy(dst, src, n)  memcpy64((dst),(src),(uint64_t)(n))

// task_exit: task.c'de tanımlı – fork stub için gerekli
extern void task_exit(void);

// GDT selectors (task.h'dan yeniden tanımlamak yerine burada sabit)
// task.h include edilirse çakışmaması için ifndef
#ifndef GDT_KERNEL_CODE
#define GDT_KERNEL_CODE  0x08
#define GDT_KERNEL_DATA  0x10
#endif

// ============================================================
// USER-MODE POINTER DOĞRULAMA
// ============================================================
#define USER_SPACE_MAX  0x00007FFFFFFFFFFFull

static int is_valid_user_ptr(const void* ptr, uint64_t len) {
    if (!ptr) return 0;
    uint64_t addr = (uint64_t)ptr;
    if ((addr >> 47) != 0) return 0;
    if (len > 0 && (addr + len) > USER_SPACE_MAX) return 0;
    return 1;
}

static int is_valid_user_string(const char* s, uint64_t maxlen) {
    return is_valid_user_ptr(s, maxlen);
}

// ============================================================
// Dahili yardımcılar
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
    for (int a = 0, b = i - 1; a < b; a++, b--) {
        char c = buf[a]; buf[a] = buf[b]; buf[b] = c;
    }
    serial_print(buf);
}

// ============================================================
// String yardımcıları (libc yok)
// ============================================================
static void my_strncpy(char* dst, const char* src, int n) {
    int i = 0;
    while (i < n - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static int my_strcmp(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

// ============================================================
// Internal state
// ============================================================
static int syscall_enabled = 0;

// ============================================================
// FD TABLOSU IMPLEMENTASYONU
// ============================================================

void fd_table_init(fd_entry_t* table) {
    for (int i = 0; i < MAX_FDS; i++) {
        table[i].type    = FD_TYPE_NONE;
        table[i].flags   = 0;
        table[i].is_open = 0;
        table[i].offset  = 0;
        table[i].path[0] = '\0';
        table[i].pipe    = 0;
    }
    // stdin  (fd 0) – okuma
    table[0].type    = FD_TYPE_SERIAL;
    table[0].flags   = O_RDONLY;
    table[0].is_open = 1;
    my_strncpy(table[0].path, "/dev/serial0", 52);

    // stdout (fd 1) – yazma
    table[1].type    = FD_TYPE_SERIAL;
    table[1].flags   = O_WRONLY;
    table[1].is_open = 1;
    my_strncpy(table[1].path, "/dev/serial0", 52);

    // stderr (fd 2) – yazma
    table[2].type    = FD_TYPE_SERIAL;
    table[2].flags   = O_WRONLY;
    table[2].is_open = 1;
    my_strncpy(table[2].path, "/dev/serial0", 52);
}

int fd_alloc(fd_entry_t* table, uint8_t type, uint8_t flags, const char* path) {
    for (int i = 3; i < MAX_FDS; i++) {
        if (!table[i].is_open) {
            table[i].type    = type;
            table[i].flags   = flags;
            table[i].is_open = 1;
            table[i].offset  = 0;
            table[i].pipe    = 0;
            if (path)
                my_strncpy(table[i].path, path, 52);
            else
                table[i].path[0] = '\0';
            return i;
        }
    }
    return -1;
}

// Pipe için özel ayırıcı: pipe_buf işaretçisi de atanır.
int fd_alloc_pipe(fd_entry_t* table, uint8_t rw_flags, pipe_buf_t* pbuf) {
    for (int i = 3; i < MAX_FDS; i++) {
        if (!table[i].is_open) {
            table[i].type    = FD_TYPE_PIPE;
            table[i].flags   = rw_flags;
            table[i].is_open = 1;
            table[i].offset  = 0;
            table[i].pipe    = pbuf;
            my_strncpy(table[i].path, "[pipe]", 52);
            return i;
        }
    }
    return -1;
}

int fd_free(fd_entry_t* table, int fd) {
    if (fd < 0 || fd >= MAX_FDS) return -1;
    if (!table[fd].is_open)      return -1;

    // Pipe tamponu referans sayacını düşür
    if (table[fd].type == FD_TYPE_PIPE && table[fd].pipe) {
        pipe_buf_release(table[fd].pipe);
        table[fd].pipe = 0;
    }

    table[fd].is_open = 0;
    table[fd].type    = FD_TYPE_NONE;
    table[fd].path[0] = '\0';
    return 0;
}

fd_entry_t* fd_get(fd_entry_t* table, int fd) {
    if (fd < 0 || fd >= MAX_FDS) return 0;
    if (!table[fd].is_open)      return 0;
    return &table[fd];
}

// ============================================================
// PIPE TAMPON HAVUZU
//
// Basit statik havuz: MAX_PIPES adet pipe_buf_t.
// Gerçek OS'ta kmalloc kullanılır; burada sabit diziye bakıyoruz.
// ============================================================
#define MAX_PIPES   8

static pipe_buf_t pipe_pool[MAX_PIPES];
static uint8_t    pipe_pool_used[MAX_PIPES];

pipe_buf_t* pipe_buf_alloc(void) {
    for (int i = 0; i < MAX_PIPES; i++) {
        if (!pipe_pool_used[i]) {
            pipe_pool_used[i] = 1;
            pipe_buf_t* pb = &pipe_pool[i];
            kmemset(pb, 0, sizeof(pipe_buf_t));
            pb->ref_count = 0;
            return pb;
        }
    }
    return 0;   // havuz dolu
}

void pipe_buf_release(pipe_buf_t* pb) {
    if (!pb) return;
    if (pb->ref_count > 0) pb->ref_count--;
    if (pb->ref_count == 0) {
        // Hangi havuz slotu olduğunu bul ve serbest bırak
        for (int i = 0; i < MAX_PIPES; i++) {
            if (&pipe_pool[i] == pb) {
                pipe_pool_used[i] = 0;
                break;
            }
        }
    }
}

// ============================================================
// SYSCALL_INIT
// ============================================================
void syscall_init(void) {
    serial_print("[SYSCALL] Initializing SYSCALL/SYSRET infrastructure...\n");

    // 1. CPUID: SYSCALL desteği kontrolü
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile ("cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(0x80000001), "c"(0));
    if (!(edx & (1 << 11))) {
        serial_print("[SYSCALL] ERROR: CPU does not support SYSCALL!\n");
        return;
    }
    serial_print("[SYSCALL] CPU supports SYSCALL/SYSRET\n");

    // 2. EFER.SCE
    uint64_t efer = rdmsr(MSR_EFER);
    efer |= EFER_SCE;
    wrmsr(MSR_EFER, efer);
    if (!(rdmsr(MSR_EFER) & EFER_SCE)) {
        serial_print("[SYSCALL] ERROR: EFER.SCE bit set failed!\n");
        return;
    }
    serial_print("[SYSCALL] EFER.SCE enabled\n");

    // 3. MSR_STAR
    wrmsr(MSR_STAR, STAR_VALUE);
    serial_print("[SYSCALL] MSR_STAR = 0x");
    print_hex64(rdmsr(MSR_STAR));
    serial_print("\n");

    // 4. MSR_LSTAR
    uint64_t entry_addr = (uint64_t)syscall_entry;
    wrmsr(MSR_LSTAR, entry_addr);
    serial_print("[SYSCALL] LSTAR = 0x");
    print_hex64(entry_addr);
    serial_print("\n");

    // 5. MSR_CSTAR (32-bit compat, kullanılmıyor)
    wrmsr(MSR_CSTAR, 0);

    // 6. MSR_FMASK
    wrmsr(MSR_FMASK, SYSCALL_RFLAGS_MASK);
    serial_print("[SYSCALL] FMASK set (IF+DF masked on entry)\n");

    syscall_enabled = 1;
    serial_print("[SYSCALL] SYSCALL/SYSRET ready! (v3: +mmap/brk/fork/execve/waitpid/pipe/dup2)\n");
}

int syscall_is_enabled(void) { return syscall_enabled; }

// ============================================================
// SYSCALL IMPLEMENTASYONLARI
// ============================================================

// ── SYS_WRITE (1) ──────────────────────────────────────────────
static void sys_write(syscall_frame_t* frame) {
    int         fd  = (int)frame->rdi;
    const char* buf = (const char*)frame->rsi;
    uint64_t    len = frame->rdx;

    if (fd < 0 || fd >= MAX_FDS)      { frame->rax = SYSCALL_ERR_BADF;  return; }
    if (!is_valid_user_ptr(buf, len)) { frame->rax = SYSCALL_ERR_FAULT; return; }
    if (len == 0)                     { frame->rax = 0; return; }
    if (len > 65536)                  { frame->rax = SYSCALL_ERR_INVAL; return; }
    if (fd == 0)                      { frame->rax = SYSCALL_ERR_BADF;  return; }

    // stdout (fd=1): VGA + serial (kullanıcı programları VGA'da görünsün)
    if (fd == 1) {
        // Null-terminate edilmiş geçici buffer ile VGA'ya yaz
        char vga_buf[256];
        uint64_t i = 0;
        while (i < len) {
            uint64_t chunk = len - i;
            if (chunk > 255) chunk = 255;
            uint64_t j;
            for (j = 0; j < chunk; j++) vga_buf[j] = buf[i + j];
            vga_buf[j] = '\0';
            print_str64(vga_buf, VGA_WHITE);
            for (j = 0; j < chunk; j++) serial_putchar(buf[i + j]);
            i += chunk;
        }
        frame->rax = len;
        return;
    }
    // stderr (fd=2): sadece serial
    if (fd == 2) {
        for (uint64_t i = 0; i < len; i++) serial_putchar(buf[i]);
        frame->rax = len;
        return;
    }

    task_t* cur = task_get_current();
    if (!cur) { frame->rax = SYSCALL_ERR_PERM; return; }

    fd_entry_t* ent = fd_get(cur->fd_table, fd);
    if (!ent)  { frame->rax = SYSCALL_ERR_BADF; return; }
    if (ent->flags == O_RDONLY) { frame->rax = SYSCALL_ERR_BADF; return; }

    // Pipe yazma
    if (ent->type == FD_TYPE_PIPE) {
        pipe_buf_t* pb = ent->pipe;
        if (!pb || pb->read_closed) { frame->rax = SYSCALL_ERR_BADF; return; }

        uint64_t written = 0;
        while (written < len) {
            if (pb->bytes_avail >= PIPE_BUF_SIZE) {
                // Tampon dolu; bloklamak yerine yazılanı döndür
                break;
            }
            pb->data[pb->write_pos] = buf[written];
            pb->write_pos = (pb->write_pos + 1) % PIPE_BUF_SIZE;
            pb->bytes_avail++;
            written++;
        }
        frame->rax = written ? written : SYSCALL_ERR_AGAIN;
        return;
    }

    // Serial veya file fd
    for (uint64_t i = 0; i < len; i++) serial_putchar(buf[i]);
    if (ent->type == FD_TYPE_FILE) ent->offset += len;
    frame->rax = len;
}

// ── SYS_READ (2) ───────────────────────────────────────────────
static void sys_read(syscall_frame_t* frame) {
    int      fd  = (int)frame->rdi;
    char*    buf = (char*)frame->rsi;
    uint64_t len = frame->rdx;

    if (fd < 0 || fd >= MAX_FDS)     { frame->rax = SYSCALL_ERR_BADF;  return; }
    if (!is_valid_user_ptr(buf, len)){ frame->rax = SYSCALL_ERR_FAULT; return; }
    if (len == 0)                    { frame->rax = 0; return; }
    if (len > 65536)                 { frame->rax = SYSCALL_ERR_INVAL; return; }
    if (fd == 1 || fd == 2)         { frame->rax = SYSCALL_ERR_BADF;  return; }

    // stdin: doğrudan serial RX
    if (fd == 0) {
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

    task_t* cur = task_get_current();
    if (!cur) { frame->rax = SYSCALL_ERR_PERM; return; }

    fd_entry_t* ent = fd_get(cur->fd_table, fd);
    if (!ent)  { frame->rax = SYSCALL_ERR_BADF; return; }
    if (ent->flags == O_WRONLY) { frame->rax = SYSCALL_ERR_BADF; return; }

    // Pipe okuma
    if (ent->type == FD_TYPE_PIPE) {
        pipe_buf_t* pb = ent->pipe;
        if (!pb) { frame->rax = SYSCALL_ERR_BADF; return; }

        if (pb->bytes_avail == 0) {
            // EOF: yazma ucu kapandıysa 0 döndür, değilse EAGAIN
            frame->rax = pb->write_closed ? 0 : SYSCALL_ERR_AGAIN;
            return;
        }

        uint64_t count = 0;
        while (count < len && pb->bytes_avail > 0) {
            buf[count] = (char)pb->data[pb->read_pos];
            pb->read_pos = (pb->read_pos + 1) % PIPE_BUF_SIZE;
            pb->bytes_avail--;
            count++;
        }
        frame->rax = count;
        return;
    }

    // Serial
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

    frame->rax = SYSCALL_ERR_NOSYS;   // FD_TYPE_FILE: VFS hazır değil
}

// ── SYS_EXIT (3) ────────────────────────────────────────────────
static void sys_exit(syscall_frame_t* frame) {
    int exit_code = (int)frame->rdi;
    task_t* cur   = task_get_current();

    if (cur && cur->pid != 0) {
        cur->exit_code = exit_code;
        serial_print("[SYSCALL] SYS_EXIT: pid=");
        print_uint64(cur->pid);
        serial_print(" code=");
        { char b[16]; int_to_str(exit_code, b); serial_print(b); }
        serial_print("\n");
        task_exit();
    }
    frame->rax = SYSCALL_OK;
}

// ── SYS_GETPID (4) ──────────────────────────────────────────────
static void sys_getpid(syscall_frame_t* frame) {
    task_t* cur = task_get_current();
    frame->rax  = cur ? (uint64_t)cur->pid : 0;
}

// ── SYS_YIELD (5) ───────────────────────────────────────────────
static void sys_yield(syscall_frame_t* frame) {
    scheduler_yield();
    frame->rax = SYSCALL_OK;
}

// ── SYS_SLEEP (6) ───────────────────────────────────────────────
static void sys_sleep(syscall_frame_t* frame) {
    uint64_t ticks = frame->rdi;
    if (ticks == 0)     { frame->rax = SYSCALL_OK;        return; }
    if (ticks > 60000)  { frame->rax = SYSCALL_ERR_INVAL; return; }

    uint64_t end = get_system_ticks() + ticks;
    while (get_system_ticks() < end)
        __asm__ volatile ("sti; hlt" ::: "memory");

    frame->rax = SYSCALL_OK;
}

// ── SYS_UPTIME (7) ──────────────────────────────────────────────
static void sys_uptime(syscall_frame_t* frame) {
    frame->rax = get_system_ticks();
}

// ── SYS_DEBUG (8) ───────────────────────────────────────────────
static void sys_debug(syscall_frame_t* frame) {
    const char* msg = (const char*)frame->rdi;
    if (!is_valid_user_string(msg, 256)) { frame->rax = SYSCALL_ERR_INVAL; return; }
    serial_print("[DEBUG] ");
    serial_print(msg);
    serial_print("\n");
    frame->rax = SYSCALL_OK;
}

// ── SYS_OPEN (9) ────────────────────────────────────────────────
static void sys_open(syscall_frame_t* frame) {
    const char* path  = (const char*)frame->rdi;
    uint64_t    flags = frame->rsi;

    if (!is_valid_user_string(path, 128)) { frame->rax = SYSCALL_ERR_INVAL; return; }

    uint64_t valid_flags = O_RDONLY | O_WRONLY | O_RDWR | O_CREAT | O_TRUNC | O_APPEND;
    if (flags & ~valid_flags) { frame->rax = SYSCALL_ERR_INVAL; return; }

    task_t* cur = task_get_current();
    if (!cur) { frame->rax = SYSCALL_ERR_PERM; return; }

    uint8_t fd_type = (my_strcmp(path, "/dev/serial0") == 0)
                      ? FD_TYPE_SERIAL : FD_TYPE_FILE;

    if (fd_type == FD_TYPE_FILE && (flags & O_CREAT)) {
        frame->rax = SYSCALL_ERR_NOENT;
        return;
    }

    int new_fd = fd_alloc(cur->fd_table, fd_type, (uint8_t)(flags & 0xFF), path);
    if (new_fd < 0) { frame->rax = SYSCALL_ERR_MFILE; return; }

    serial_print("[SYSCALL] open -> fd=");
    print_uint64((uint64_t)new_fd);
    serial_print("\n");

    frame->rax = (uint64_t)new_fd;
}

// ── SYS_CLOSE (10) ──────────────────────────────────────────────
static void sys_close(syscall_frame_t* frame) {
    int fd = (int)frame->rdi;

    // 0-2 arası standart stream'leri kapatmayı engelle
    if (fd < 3 || fd >= MAX_FDS) { frame->rax = SYSCALL_ERR_BADF; return; }

    task_t* cur = task_get_current();
    if (!cur) { frame->rax = SYSCALL_ERR_PERM; return; }

    int ret = fd_free(cur->fd_table, fd);
    frame->rax = (ret == 0) ? SYSCALL_OK : SYSCALL_ERR_BADF;
}

// ── SYS_GETPPID (11) ────────────────────────────────────────────
static void sys_getppid(syscall_frame_t* frame) {
    task_t* cur = task_get_current();
    if (!cur) { frame->rax = 0; return; }
#ifdef TASK_HAS_PARENT_PID
    frame->rax = (uint64_t)cur->parent_pid;
#else
    frame->rax = 0;
#endif
}

// ── SYS_SBRK (12) ───────────────────────────────────────────────
static void sys_sbrk(syscall_frame_t* frame) {
    int64_t increment = (int64_t)frame->rdi;
    uint64_t old_brk  = kmalloc_get_brk();

    if (increment == 0) { frame->rax = old_brk; return; }
    if (increment < 0)  { frame->rax = SYSCALL_ERR_INVAL; return; }
    if ((uint64_t)increment > (1024 * 1024)) { frame->rax = SYSCALL_ERR_INVAL; return; }

    uint64_t new_brk = kmalloc_set_brk(old_brk + (uint64_t)increment);
    if (new_brk == (uint64_t)-1) { frame->rax = SYSCALL_ERR_NOMEM; return; }

    frame->rax = old_brk;
}

// ── SYS_GETPRIORITY (13) ────────────────────────────────────────
static void sys_getpriority(syscall_frame_t* frame) {
    task_t* cur = task_get_current();
    frame->rax  = cur ? (uint64_t)cur->priority : 0;
}

// ── SYS_SETPRIORITY (14) ────────────────────────────────────────
static void sys_setpriority(syscall_frame_t* frame) {
    uint64_t prio = frame->rdi;
    if (prio > 255) { frame->rax = SYSCALL_ERR_INVAL; return; }

    task_t* cur = task_get_current();
    if (!cur) { frame->rax = SYSCALL_ERR_PERM; return; }

    cur->priority = (uint32_t)prio;
    frame->rax    = SYSCALL_OK;
}

// ── SYS_GETTICKS (15) ───────────────────────────────────────────
static void sys_getticks(syscall_frame_t* frame) {
    frame->rax = get_system_ticks();
}

// ============================================================
// v3 YENİ SYSCALL IMPLEMENTASYONLARI
// ============================================================

// ── SYS_MMAP (16) ───────────────────────────────────────────────
// mmap(addr, len, prot, flags, fd, offset) -> mapped_addr | MAP_FAILED
//
// Şu an yalnızca MAP_ANONYMOUS desteklenir; sayfa tablosu yok (flat memory).
// Bellek kmalloc() ile ayrılır; addr ve prot parametreleri kaydedilir.
//
// Arayüz (Linux x86-64 uyumlu):
//   RDI = addr   (istenen adres; 0 = kernel seçsin)
//   RSI = len
//   RDX = prot   (PROT_*)
//   R10 = flags  (MAP_*)
//   R8  = fd     (-1 for anonymous)
//   R9  = offset (anonim için 0 olmalı)
//
// Dönüş: haritalanan adres (uint64_t) veya (uint64_t)MAP_FAILED
// ---------------------------------------------------------------
static void sys_mmap(syscall_frame_t* frame) {
    // uint64_t addr   = frame->rdi;   // istenen adres (şimdilik yok sayılır)
    uint64_t len    = frame->rsi;
    // uint64_t prot   = frame->rdx;
    uint64_t flags  = frame->r10;
    int      fd_arg = (int)(int64_t)frame->r8;
    uint64_t offset = frame->r9;

    // Uzunluk kontrolü
    if (len == 0 || len > (256 * 1024 * 1024ULL)) {
        frame->rax = (uint64_t)MAP_FAILED;
        return;
    }

    // Sadece MAP_ANONYMOUS desteklenir
    if (!(flags & MAP_ANONYMOUS)) {
        serial_print("[SYSCALL] mmap: only MAP_ANONYMOUS supported\n");
        frame->rax = (uint64_t)MAP_FAILED;
        return;
    }

    // Anonim mmap'te fd=-1 ve offset=0 olmalı
    if (fd_arg != -1 || offset != 0) {
        frame->rax = (uint64_t)MAP_FAILED;
        return;
    }

    // Sayfa hizalaması (4KB)
    uint64_t aligned_len = (len + 0xFFF) & ~0xFFFULL;

    void* mem = kmalloc(aligned_len);
    if (!mem) {
        frame->rax = (uint64_t)MAP_FAILED;
        return;
    }

    // Belleği sıfırla (MAP_ANONYMOUS garantisi)
    kmemset(mem, 0, aligned_len);

    serial_print("[SYSCALL] mmap -> addr=0x");
    print_hex64((uint64_t)mem);
    serial_print(" len=");
    print_uint64(aligned_len);
    serial_print("\n");

    frame->rax = (uint64_t)mem;
}

// ── SYS_MUNMAP (17) ─────────────────────────────────────────────
// munmap(addr, len) -> 0 | SYSCALL_ERR_*
//
// kmalloc tabanlı basit implementasyon: len yok sayılır,
// adres kfree() ile serbest bırakılır.
// UYARI: Geçersiz adres çift serbest bırakmaya (double-free) yol açar.
// Gerçek VM alt yapısı eklendiğinde bölge takibi gereklidir.
// ---------------------------------------------------------------
static void sys_munmap(syscall_frame_t* frame) {
    void*    addr = (void*)frame->rdi;
    uint64_t len  = frame->rsi;

    if (!addr)    { frame->rax = SYSCALL_ERR_INVAL; return; }
    if (len == 0) { frame->rax = SYSCALL_ERR_INVAL; return; }

    // Canonical adres kontrolü
    if ((uint64_t)addr > USER_SPACE_MAX) {
        frame->rax = SYSCALL_ERR_INVAL;
        return;
    }

    kfree(addr);

    serial_print("[SYSCALL] munmap addr=0x");
    print_hex64((uint64_t)addr);
    serial_print("\n");

    frame->rax = SYSCALL_OK;
}

// ── SYS_BRK (18) ────────────────────────────────────────────────
// brk(addr) -> new_brk | SYSCALL_ERR_*
//
// POSIX brk(): addr=0 ise mevcut brk döner; addr > current_brk ise genişletir.
// sbrk()'den farkı: artış miktarı değil, hedef adres alınır.
// ---------------------------------------------------------------
static void sys_brk(syscall_frame_t* frame) {
    uint64_t new_addr = frame->rdi;
    uint64_t cur_brk  = kmalloc_get_brk();

    // addr=0: sadece sorgula
    if (new_addr == 0) {
        frame->rax = cur_brk;
        return;
    }

    // Küçültme: mevcut brk'yi döndür (desteklenmiyor)
    if (new_addr < cur_brk) {
        frame->rax = cur_brk;
        return;
    }

    // Maksimum 64 MB artış
    if (new_addr - cur_brk > (64 * 1024 * 1024ULL)) {
        frame->rax = SYSCALL_ERR_NOMEM;
        return;
    }

    uint64_t result = kmalloc_set_brk(new_addr);
    if (result == (uint64_t)-1) {
        frame->rax = SYSCALL_ERR_NOMEM;
        return;
    }

    serial_print("[SYSCALL] brk -> 0x");
    print_hex64(result);
    serial_print("\n");

    frame->rax = result;
}

// ── SYS_FORK (19) ───────────────────────────────────────────────
// fork() -> child_pid (ebeveynde) | 0 (çocukta) | SYSCALL_ERR_*
//
// Implementasyon notları:
//   • Copy-on-write yok; stacks kmalloc+memcpy ile kopyalanır.
//   • Çocuk, ebeveynin fd_table'ının derin kopyasını alır (pipe ref++).
//   • Ebeveyne child_pid döner; çocukta RAX=0 olarak SYSRET ile döner.
//   • Çocuk SYSRET'ten döndükten sonra kullanıcı kodu fork()==0 dalını
//     çalıştırmalı ve en sonunda SYS_EXIT yapmalıdır.
//
// KERNEL CONTEXT'TE FORK (syscalltest gibi):
//   Kernel context'te SYSRET yapılamaz. Çocuk task için kernel_stack
//   üzerine task_exit() çağıran küçük bir wrapper frame kurulur.
//   Bu sayede çocuk scheduler'a geçtiğinde task_exit() çağırır ve
//   "ghost task" olarak askıda kalmaz.
// ---------------------------------------------------------------

// Kernel context fork çocuklarının girdi noktası.
// task_switch ile bu task'a geçildiğinde burası çalışır ve
// hemen task_exit() çağırarak temiz çıkış yapar.
static void fork_child_kernel_stub(void) {
    // Çocuk bağlamındayız; fork() == 0 döndürmüş gibi davranalım.
    // Gerçek kullanıcı kodu burada kendi mantığını çalıştırır.
    // Kernel fork testi için sadece temiz çıkış yapıyoruz.
    serial_print("[FORK-CHILD] Kernel stub running, exiting cleanly\n");
    task_exit();
    // task_exit() dönmez; yine de derleyiciyi memnun et
    while(1) __asm__ volatile("hlt");
}

static void sys_fork(syscall_frame_t* frame) {
    task_t* parent = task_get_current();
    if (!parent) { frame->rax = SYSCALL_ERR_PERM; return; }

    // ── 1. Yeni TCB ayır ──────────────────────────────────────
    task_t* child = (task_t*)kmalloc(sizeof(task_t));
    if (!child) { frame->rax = SYSCALL_ERR_NOMEM; return; }
    kmemset(child, 0, sizeof(task_t));

    // ── 2. Kimlik ──────────────────────────────────────────────
    // next_pid static değişkenine erişemiyoruz; çakışmayan pid bul.
    // NOT: Kalıcı çözüm → task.h'a task_alloc_pid() export et.
    {
        uint32_t candidate = 100;
        while (task_find_by_pid(candidate) != 0) candidate++;
        child->pid = candidate;
    }
    child->parent_pid = parent->pid;
    my_strncpy(child->name, parent->name, 32);
    // İsme "-fork" ekle (ps'de ayırt edebilmek için)
    int nlen = 0;
    while (child->name[nlen] && nlen < 26) nlen++;
    child->name[nlen]   = '-'; child->name[nlen+1] = 'f';
    child->name[nlen+2] = 'k'; child->name[nlen+3] = '\0';

    // ── 3. Ayrıcalık seviyesi ─────────────────────────────────
    child->privilege_level = parent->privilege_level;

    // ── 4. Kernel stack ayır ve kopyala ──────────────────────
    child->kernel_stack_size = parent->kernel_stack_size;
    if (child->kernel_stack_size == 0)
        child->kernel_stack_size = KERNEL_STACK_SIZE;

    child->kernel_stack_base = (uint64_t)kmalloc(child->kernel_stack_size);
    if (!child->kernel_stack_base) {
        kfree(child);
        frame->rax = SYSCALL_ERR_NOMEM;
        return;
    }
    kmemcpy((void*)child->kernel_stack_base,
            (void*)parent->kernel_stack_base,
            child->kernel_stack_size);
    child->kernel_stack_top = child->kernel_stack_base + child->kernel_stack_size;

    // ── 5. CPU bağlamı seç ────────────────────────────────────
    // Ayrıcalık seviyesine göre iki farklı yol:
    //
    // A) Ring-3 (user task): Ebeveynin register kopyasını al,
    //    RSP'yi yeni stack'e taşı, RAX=0 yap → SYSRET ile döner.
    //
    // B) Ring-0 (kernel context, örn. syscalltest):
    //    fork_child_kernel_stub() entry'li temiz bir stack frame kur.
    //    Çocuk scheduler'a geçince stub çalışır ve task_exit() yapar.
    // ---------------------------------------------------------
    if (parent->privilege_level == TASK_PRIVILEGE_USER && parent->user_stack_size > 0) {
        // ── Ring-3 yolu ───────────────────────────────────────
        child->user_stack_size = parent->user_stack_size;
        child->user_stack_base = (uint64_t)kmalloc(child->user_stack_size);
        if (!child->user_stack_base) {
            kfree((void*)child->kernel_stack_base);
            kfree(child);
            frame->rax = SYSCALL_ERR_NOMEM;
            return;
        }
        kmemcpy((void*)child->user_stack_base,
                (void*)parent->user_stack_base,
                child->user_stack_size);

        // Ebeveynin register bağlamını kopyala
        kmemcpy(&child->context, &parent->context, sizeof(cpu_context_t));

        // RSP'yi çocuğun user stack'ine taşı (delta hesabı)
        uint64_t stack_delta = child->user_stack_base - parent->user_stack_base;
        child->context.rsp  += stack_delta;
        child->user_stack_top = child->user_stack_base + child->user_stack_size;

        // Çocukta fork() == 0
        child->context.rax = 0;
        // SYSRET dönüş adresi: ebeveynin syscall sonrası RIP'i
        child->context.rip = frame->rcx;
        child->context.cs  = parent->context.cs;
        child->context.ss  = parent->context.ss;

    } else {
        // ── Ring-0 / kernel context yolu ─────────────────────
        // fork_child_kernel_stub() çağıracak bir iretq stack frame kur.
        uint64_t* stk = (uint64_t*)child->kernel_stack_top;
        *(--stk) = GDT_KERNEL_DATA;                       // SS
        *(--stk) = child->kernel_stack_top;               // RSP (referans)
        *(--stk) = 0x202ULL;                              // RFLAGS (IF=1)
        *(--stk) = GDT_KERNEL_CODE;                       // CS
        *(--stk) = (uint64_t)fork_child_kernel_stub;      // RIP

        // 15 genel amaçlı register (timer ISR pop sırası: r15..rax)
        for (int i = 0; i < 15; i++) *(--stk) = 0;

        child->context.rsp    = (uint64_t)stk;
        child->context.rip    = (uint64_t)fork_child_kernel_stub;
        child->context.rflags = 0x202;
        child->context.cs     = GDT_KERNEL_CODE;
        child->context.ss     = GDT_KERNEL_DATA;
        child->context.rax    = 0;    // çocukta fork() == 0
        // user stack yok
        child->user_stack_base = 0;
        child->user_stack_top  = 0;
        child->user_stack_size = 0;
    }

    // ── 6. fd_table derin kopya (pipe ref_count++) ────────────
    kmemcpy(child->fd_table, parent->fd_table, sizeof(fd_entry_t) * MAX_FDS);
    for (int i = 0; i < MAX_FDS; i++) {
        if (child->fd_table[i].is_open &&
            child->fd_table[i].type == FD_TYPE_PIPE &&
            child->fd_table[i].pipe) {
            child->fd_table[i].pipe->ref_count++;
        }
    }

    // ── 7. Zamanlama alanları ─────────────────────────────────
    child->priority   = parent->priority;
    child->time_slice = parent->time_slice;
    if (child->time_slice == 0) child->time_slice = 10;
    child->state      = TASK_STATE_READY;

    // ── 8. Kuyruğa ekle ──────────────────────────────────────
    if (task_start(child) != 0) {
        kfree((void*)child->user_stack_base);
        kfree((void*)child->kernel_stack_base);
        kfree(child);
        frame->rax = SYSCALL_ERR_AGAIN;
        return;
    }

    serial_print("[FORK] parent=");
    { char b[8]; int_to_str((int)parent->pid, b); serial_print(b); }
    serial_print(" -> child=");
    { char b[8]; int_to_str((int)child->pid, b); serial_print(b); }
    serial_print("\n");

    // Ebeveyne child_pid döndür
    frame->rax = (uint64_t)child->pid;
}

// ── SYS_EXECVE (20) ─────────────────────────────────────────────
// execve(path, argv, envp) -> hata kodu (başarıda dönmez)
//
// Gerçek implementasyon için ELF loader ve VFS gereklidir.
// Bu stub; çağrı arayüzünü doğrular ve ENOSYS döndürür.
// Gelecekte: mevcut task imgesini temizle → ELF yükle → entry jump.
// ---------------------------------------------------------------
static void sys_execve(syscall_frame_t* frame) {
    const char* path = (const char*)frame->rdi;
    // argv ve envp: ileride işlenecek
    // const char** argv = (const char**)frame->rsi;
    // const char** envp = (const char**)frame->rdx;

    if (!is_valid_user_string(path, 256)) {
        frame->rax = SYSCALL_ERR_FAULT;
        return;
    }

    serial_print("[SYSCALL] execve(\"");
    serial_print(path);
    serial_print("\") -> ENOSYS (ELF loader not yet implemented)\n");

    // TODO:
    //   1. VFS'ten ELF dosyasını aç ve doğrula (EI_MAG0..3 kontrolü)
    //   2. PT_LOAD segmentlerini uygun adreslere haritalandır
    //   3. Mevcut fd_table'ı temizle (O_CLOEXEC olanlar)
    //   4. Yeni stack kur, argv/envp kopyala
    //   5. task_t::context.rip = ELF entry point olarak ayarla
    //   6. task_load_and_jump_context() ile atla (bu syscall dönemez)

    frame->rax = SYSCALL_ERR_NOSYS;
}

// ── SYS_WAITPID (21) ────────────────────────────────────────────
// waitpid(pid, *status, options) -> waited_pid | 0 (WNOHANG) | SYSCALL_ERR_*
//
// pid > 0 : tam olarak bu PID'i bekle
// pid = -1 : herhangi bir çocuğu bekle (POSIX wait() eşdeğeri)
// pid = 0  : aynı process grubundaki herhangi bir çocuğu bekle (stub; -1 gibi davranır)
//
// options:
//   WNOHANG  – tamamlanmış çocuk yoksa hemen 0 döndür
// ---------------------------------------------------------------
static void sys_waitpid(syscall_frame_t* frame) {
    int64_t  pid_arg = (int64_t)frame->rdi;
    int*     status  = (int*)frame->rsi;
    uint64_t options = frame->rdx;

    // status pointer doğrulama (NULL kabul edilir)
    if (status && !is_valid_user_ptr(status, sizeof(int))) {
        frame->rax = SYSCALL_ERR_FAULT;
        return;
    }

    task_t* cur = task_get_current();
    if (!cur) { frame->rax = SYSCALL_ERR_PERM; return; }

    // ── Çocuk arama döngüsü ───────────────────────────────────
    // Maksimum bekleme: WNOHANG yoksa ~5 saniye (5000 tick)
    uint64_t deadline = get_system_ticks() + ((options & WNOHANG) ? 0 : 5000);

    do {
        // Tüm task listesini tara; zombie çocuk ara
        uint32_t total = task_get_count();
        task_t*  found = 0;

        for (uint32_t scan = 0; scan < total; scan++) {
            task_t* candidate = task_find_by_pid((uint32_t)scan);
            if (!candidate) continue;

            // Ebeveyn eşleşmesi
            if (candidate->parent_pid != cur->pid) continue;

            // PID filtresi
            if (pid_arg > 0 && (int64_t)candidate->pid != pid_arg) continue;

            // ZOMBIE: çocuk tamamlandı
            if (candidate->state == TASK_STATE_ZOMBIE ||
                candidate->state == TASK_STATE_TERMINATED) {
                found = candidate;
                break;
            }
        }

        if (found) {
            uint32_t waited_pid = found->pid;

            // Status kodunu yaz: WEXITSTATUS << 8
            if (status) {
                *status = (found->exit_code & 0xFF) << 8;
            }

            serial_print("[SYSCALL] waitpid: reaped pid=");
            print_uint64(waited_pid);
            serial_print("\n");

            // TCB kaynaklarını serbest bırak (basit; tam GC ileride)
            // task_reap(found);  -- implement edildiğinde aktif et

            frame->rax = (uint64_t)waited_pid;
            return;
        }

        // WNOHANG: çocuk bitmemişse hemen 0 döndür
        if (options & WNOHANG) {
            frame->rax = 0;
            return;
        }

        // Çocuk yok mu hiç? (pid_arg özelindeyse ECHILD)
        if (pid_arg > 0) {
            task_t* specific = task_find_by_pid((uint32_t)pid_arg);
            if (!specific || specific->parent_pid != cur->pid) {
                frame->rax = SYSCALL_ERR_CHILD;
                return;
            }
        }

        // Biraz bekle ve tekrar dene
        __asm__ volatile ("sti; hlt" ::: "memory");

    } while (get_system_ticks() < deadline);

    // Zaman aşımı
    frame->rax = SYSCALL_ERR_AGAIN;
}

// ── SYS_PIPE (22) ───────────────────────────────────────────────
// pipe(fd[2]) -> 0 | SYSCALL_ERR_*
//
//   fd[0] = okuma  ucu (O_RDONLY)
//   fd[1] = yazma  ucu (O_WRONLY)
//
// Paylaşılan pipe_buf_t, pipe_pool statik havuzundan alınır.
// Her iki fd de aynı tampona işaret eder; ref_count = 2.
// ---------------------------------------------------------------
static void sys_pipe(syscall_frame_t* frame) {
    int* fd_arr = (int*)frame->rdi;

    if (!is_valid_user_ptr(fd_arr, 2 * sizeof(int))) {
        frame->rax = SYSCALL_ERR_FAULT;
        return;
    }

    task_t* cur = task_get_current();
    if (!cur) { frame->rax = SYSCALL_ERR_PERM; return; }

    // Tampon ayır
    pipe_buf_t* pb = pipe_buf_alloc();
    if (!pb) { frame->rax = SYSCALL_ERR_NOMEM; return; }
    pb->ref_count = 2;   // okuma + yazma ucu

    // Okuma fd (fd[0])
    int rfd = fd_alloc_pipe(cur->fd_table, O_RDONLY, pb);
    if (rfd < 0) {
        pipe_buf_release(pb);
        pipe_buf_release(pb);  // ref_count'u 0'a düşür
        frame->rax = SYSCALL_ERR_MFILE;
        return;
    }

    // Yazma fd (fd[1])
    int wfd = fd_alloc_pipe(cur->fd_table, O_WRONLY, pb);
    if (wfd < 0) {
        fd_free(cur->fd_table, rfd);   // pipe_buf_release içinde ref--
        frame->rax = SYSCALL_ERR_MFILE;
        return;
    }

    fd_arr[0] = rfd;
    fd_arr[1] = wfd;

    serial_print("[SYSCALL] pipe -> rfd=");
    print_uint64((uint64_t)rfd);
    serial_print(" wfd=");
    print_uint64((uint64_t)wfd);
    serial_print("\n");

    frame->rax = SYSCALL_OK;
}

// ── SYS_DUP2 (23) ───────────────────────────────────────────────
// dup2(oldfd, newfd) -> newfd | SYSCALL_ERR_*
//
// POSIX dup2() semantiği:
//   • oldfd == newfd ise doğrudan newfd döner (işlem yok).
//   • newfd açıksa önce atomik olarak kapatılır.
//   • Yeni fd, oldfd'nin tam kopyasıdır (pipe tamponu ref_count++).
//   • 0 ≤ newfd < MAX_FDS; ancak stdin/stdout/stderr (0-2)
//     üzerine yazma izni verilir (shell yönlendirme için gerekli).
// ---------------------------------------------------------------
static void sys_dup2(syscall_frame_t* frame) {
    int oldfd = (int)frame->rdi;
    int newfd = (int)frame->rsi;

    if (oldfd < 0 || oldfd >= MAX_FDS) { frame->rax = SYSCALL_ERR_BADF; return; }
    if (newfd < 0 || newfd >= MAX_FDS) { frame->rax = SYSCALL_ERR_BADF; return; }

    task_t* cur = task_get_current();
    if (!cur) { frame->rax = SYSCALL_ERR_PERM; return; }

    fd_entry_t* src = fd_get(cur->fd_table, oldfd);
    if (!src) { frame->rax = SYSCALL_ERR_BADF; return; }

    // oldfd == newfd: hiçbir şey yapma
    if (oldfd == newfd) { frame->rax = (uint64_t)newfd; return; }

    // newfd açıksa kapat (pipe referansını düşürür)
    if (cur->fd_table[newfd].is_open)
        fd_free(cur->fd_table, newfd);

    // Tam kopya
    cur->fd_table[newfd] = *src;

    // Pipe tamponu referansını artır
    if (src->type == FD_TYPE_PIPE && src->pipe)
        src->pipe->ref_count++;

    serial_print("[SYSCALL] dup2 oldfd=");
    print_uint64((uint64_t)oldfd);
    serial_print(" -> newfd=");
    print_uint64((uint64_t)newfd);
    serial_print("\n");

    frame->rax = (uint64_t)newfd;
}

// ============================================================
// SYSCALL_DISPATCH
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
    // v3
    case SYS_MMAP:         sys_mmap(frame);        break;
    case SYS_MUNMAP:       sys_munmap(frame);      break;
    case SYS_BRK:          sys_brk(frame);         break;
    case SYS_FORK:         sys_fork(frame);        break;
    case SYS_EXECVE:       sys_execve(frame);      break;
    case SYS_WAITPID:      sys_waitpid(frame);     break;
    case SYS_PIPE:         sys_pipe(frame);        break;
    case SYS_DUP2:         sys_dup2(frame);        break;
    default:
        serial_print("[SYSCALL] Unknown syscall: ");
        { char b[16]; int_to_str((int)(num & 0xFFFF), b); serial_print(b); }
        serial_print("\n");
        frame->rax = SYSCALL_ERR_NOSYS;
        break;
    }
}

// ============================================================
// SYSCALL_TEST – v3 testleri dahil
// ============================================================
void syscall_test(void) {
    if (!syscall_enabled) {
        serial_print("[SYSCALL TEST] SYSCALL not enabled!\n");
        return;
    }
    serial_print("\n========================================\n");
    serial_print("[SYSCALL TEST] v3 comprehensive tests\n");
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

    // ── Önceki testler (v1/v2) – kısa özet ─────────────────────
    serial_print("\n[T01] SYS_WRITE stdout:\n");
    const char* wmsg = "  Hello from SYS_WRITE!\n";
    DO_SYSCALL3(SYS_WRITE, 1, wmsg, 24);
    serial_print("  ret="); { char b[8]; int_to_str((int)ret,b); serial_print(b); } serial_print("\n");

    serial_print("\n[T02] SYS_GETPID:\n");
    DO_SYSCALL0(SYS_GETPID);
    serial_print("  pid="); print_uint64(ret); serial_print("\n");

    serial_print("\n[T03] SYS_SBRK(0) query:\n");
    DO_SYSCALL1(SYS_SBRK, 0);
    serial_print("  brk=0x"); print_hex64(ret); serial_print("\n");

    // ── SYS_BRK (18) ────────────────────────────────────────────
    serial_print("\n[T04] SYS_BRK(0) query:\n");
    DO_SYSCALL1(SYS_BRK, 0);
    uint64_t initial_brk = ret;
    serial_print("  brk=0x"); print_hex64(ret); serial_print("\n");

    serial_print("\n[T05] SYS_BRK(brk+8192) extend:\n");
    DO_SYSCALL1(SYS_BRK, initial_brk + 8192);
    serial_print("  new_brk=0x"); print_hex64(ret); serial_print("\n");

    // ── SYS_MMAP (16) ───────────────────────────────────────────
    serial_print("\n[T06] SYS_MMAP anonymous 4096 bytes:\n");
    // mmap(0, 4096, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0)
    // R10=flags, R8=fd, R9=offset → inline asm ile 6-arg syscall
    register uint64_t r10_flags asm("r10") = MAP_ANONYMOUS | MAP_PRIVATE;
    register uint64_t r8_fd     asm("r8")  = (uint64_t)(int64_t)-1;
    register uint64_t r9_off    asm("r9")  = 0;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "a"((uint64_t)SYS_MMAP),
          "D"((uint64_t)0),
          "S"((uint64_t)4096),
          "d"((uint64_t)(PROT_READ|PROT_WRITE)),
          "r"(r10_flags), "r"(r8_fd), "r"(r9_off)
        : "rcx", "r11", "memory");
    serial_print("  mmap_addr=0x"); print_hex64(ret); serial_print("\n");
    uint64_t mmap_addr = ret;

    // Haritalanan belleğe yaz/oku
    if (ret != (uint64_t)MAP_FAILED) {
        volatile char* p = (volatile char*)ret;
        p[0] = 'A'; p[1] = 'B'; p[2] = '\0';
        serial_print("  mmap[0]="); serial_putchar(p[0]);
        serial_print(" mmap[1]="); serial_putchar(p[1]);
        serial_print("\n");
    }

    // ── SYS_MUNMAP (17) ─────────────────────────────────────────
    if (mmap_addr != (uint64_t)MAP_FAILED) {
        serial_print("\n[T07] SYS_MUNMAP:\n");
        DO_SYSCALL2(SYS_MUNMAP, mmap_addr, 4096);
        serial_print("  ret="); { char b[8]; int_to_str((int)ret,b); serial_print(b); }
        serial_print(" (expect 0)\n");
    }

    // ── SYS_PIPE (22) ───────────────────────────────────────────
    serial_print("\n[T08] SYS_PIPE:\n");
    int pipe_fds[2] = {-1, -1};
    DO_SYSCALL1(SYS_PIPE, pipe_fds);
    serial_print("  ret="); { char b[8]; int_to_str((int)ret,b); serial_print(b); }
    serial_print(" rfd="); { char b[8]; int_to_str(pipe_fds[0],b); serial_print(b); }
    serial_print(" wfd="); { char b[8]; int_to_str(pipe_fds[1],b); serial_print(b); }
    serial_print("\n");

    // Pipe'a yaz ve oku
    if (ret == (uint64_t)SYSCALL_OK && pipe_fds[0] > 0 && pipe_fds[1] > 0) {
        serial_print("\n[T09] PIPE write/read:\n");
        const char* pmsg = "pipe_test_data";
        DO_SYSCALL3(SYS_WRITE, pipe_fds[1], pmsg, 14);
        serial_print("  write_ret="); { char b[8]; int_to_str((int)ret,b); serial_print(b); }
        serial_print("\n");

        char rbuf[32] = {0};
        DO_SYSCALL3(SYS_READ, pipe_fds[0], rbuf, 14);
        serial_print("  read_ret="); { char b[8]; int_to_str((int)ret,b); serial_print(b); }
        serial_print(" data=\""); serial_print(rbuf); serial_print("\"\n");

        // ── SYS_DUP2 (23) ─────────────────────────────────────
        serial_print("\n[T10] SYS_DUP2 (wfd -> fd 8):\n");
        DO_SYSCALL2(SYS_DUP2, pipe_fds[1], 8);
        serial_print("  ret="); { char b[8]; int_to_str((int)ret,b); serial_print(b); }
        serial_print(" (expect 8)\n");

        // dup2 ile kopyalanan fd'ye yaz
        DO_SYSCALL3(SYS_WRITE, 8, "dup2_ok\n", 8);

        // Temizle
        DO_SYSCALL1(SYS_CLOSE, pipe_fds[0]);
        DO_SYSCALL1(SYS_CLOSE, pipe_fds[1]);
        DO_SYSCALL1(SYS_CLOSE, 8);
    }

    // ── SYS_EXECVE (20) – stub ──────────────────────────────────
    serial_print("\n[T11] SYS_EXECVE stub (expect ENOSYS):\n");
    DO_SYSCALL3(SYS_EXECVE, "/bin/sh", 0, 0);
    serial_print("  ret="); { char b[8]; int_to_str((int)ret,b); serial_print(b); }
    serial_print(" (expect -2)\n");

    // ── SYS_FORK (19) ───────────────────────────────────────────
    // UYARI: kernel context'te fork tehlikeli olabilir.
    // Bu test yalnızca fork'un çökmediğini doğrular.
    serial_print("\n[T12] SYS_FORK (kernel context smoke test):\n");
    DO_SYSCALL0(SYS_FORK);
    serial_print("  fork_ret="); { char b[16]; int_to_str((int)ret,b); serial_print(b); }
    serial_print("\n");
    // ret=0 ise çocuk bağlamındayız; ret>0 ise ebeveyn (child_pid)
    if ((int64_t)ret > 0) {
        serial_print("  [parent] child_pid="); print_uint64(ret); serial_print("\n");

        // ── SYS_WAITPID (21) – WNOHANG ──────────────────────────
        serial_print("\n[T13] SYS_WAITPID(child, WNOHANG):\n");
        int ws = 0;
        uint64_t child_pid = ret;
        // waitpid(pid, &status, WNOHANG)
        __asm__ volatile ("syscall"
            : "=a"(ret)
            : "a"((uint64_t)SYS_WAITPID),
              "D"(child_pid),
              "S"((uint64_t)&ws),
              "d"((uint64_t)WNOHANG)
            : "rcx","r11","memory");
        serial_print("  waitpid_ret="); { char b[16]; int_to_str((int)ret,b); serial_print(b); }
        serial_print(" status="); { char b[16]; int_to_str(ws,b); serial_print(b); }
        serial_print("\n");
    }

    // ── Bilinmeyen syscall ───────────────────────────────────────
    serial_print("\n[T14] Unknown syscall 999 (expect ENOSYS=-2):\n");
    DO_SYSCALL0(999);
    serial_print("  ret="); { char b[8]; int_to_str((int)ret,b); serial_print(b); }
    serial_print(" (expect -2)\n");

    serial_print("\n========================================\n");
    serial_print("[SYSCALL TEST] All v3 tests completed.\n");
    serial_print("========================================\n\n");

#undef DO_SYSCALL0
#undef DO_SYSCALL1
#undef DO_SYSCALL2
#undef DO_SYSCALL3
}