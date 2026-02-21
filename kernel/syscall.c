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
//
// v4 Yeni Eklemeler:
//   SYS_LSEEK   (24) – dosya ofseti konumlandırma (SEEK_SET/CUR/END)
//   SYS_FSTAT   (25) – fd üzerinden dosya meta verisi (stat_t)
//   SYS_IOCTL   (26) – terminal mod / genel aygıt kontrolü (TCGETS/TCSETS/…)

#include "syscall.h"
#include "../fs/files64.h"
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

// vesa64.c'de tanımlı: yazılımsal cursor'u framebuffer'a çizer.
extern void update_cursor64(void);
// putchar64: her karakteri anında framebuffer'a çizer (erase+draw+cursor).
// print_str64'ten farklı olarak kısmi/scroll durumunda da güvenilir.
extern void putchar64(char c, uint8_t color);

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

// VFS / FAT32 – files64.c + disk64.c'de tanımlı
// sys_mmap (MAP_FILE), sys_lseek (SEEK_END), sys_fstat için gerekli
extern const EmbeddedFile64* fs_get_file64(const char* filename);
extern uint32_t fat32_file_size(const char* name83);
extern int      fat32_read_file(const char* name83, uint8_t* buf, uint32_t max_len);

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
    serial_print("[SYSCALL] SYSCALL/SYSRET ready! (v5: +mmap_file/select/poll)\n");
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

    // stdout ve stderr: VGA + serial
    if (fd == 1 || fd == 2) {
        // putchar64: her karakteri anında erase_cursor+draw+update_cursor
        // yaparak framebuffer'a yazar. print_str64'ten farklı olarak
        // scroll/partial-redraw sorunlarından etkilenmez.
        for (uint64_t i = 0; i < len; i++) {
            putchar64(buf[i], VGA_WHITE);
            serial_putchar(buf[i]);
        }
        frame->rax = len;
        return;
    }

    task_t* cur = task_get_current();
    if (!cur) { frame->rax = SYSCALL_ERR_PERM; return; }

    fd_entry_t* ent = fd_get(cur->fd_table, fd);
    if (!ent)  { frame->rax = SYSCALL_ERR_BADF; return; }
    if (ent->flags == O_RDONLY) { frame->rax = SYSCALL_ERR_BADF; return; }

    if (ent->type == FD_TYPE_PIPE) {
        pipe_buf_t* pb = ent->pipe;
        if (!pb || pb->read_closed) { frame->rax = SYSCALL_ERR_BADF; return; }
        uint64_t written = 0;
        while (written < len) {
            if (pb->bytes_avail >= PIPE_BUF_SIZE) break;
            pb->data[pb->write_pos] = buf[written];
            pb->write_pos = (pb->write_pos + 1) % PIPE_BUF_SIZE;
            pb->bytes_avail++;
            written++;
        }
        frame->rax = written ? written : SYSCALL_ERR_AGAIN;
        return;
    }

    for (uint64_t i = 0; i < len; i++) serial_putchar(buf[i]);
    if (ent->type == FD_TYPE_FILE) ent->offset += len;
    frame->rax = len;
}

// ── SYS_READ (2) ───────────────────────────────────────────────
static void sys_read(syscall_frame_t* frame) {
    int      fd  = (int)frame->rdi;
    char*    buf = (char*)frame->rsi;
    uint64_t len = frame->rdx;

    if (fd < 0 || fd >= MAX_FDS)      { frame->rax = SYSCALL_ERR_BADF;  return; }
    if (!is_valid_user_ptr(buf, len)) { frame->rax = SYSCALL_ERR_FAULT; return; }
    if (len == 0)                     { frame->rax = 0; return; }
    if (len > 65536)                  { frame->rax = SYSCALL_ERR_INVAL; return; }
    if (fd == 1 || fd == 2)          { frame->rax = SYSCALL_ERR_BADF;  return; }

    if (fd == 0) {
        extern int kb_ring_pop(void);
        extern int kb_userland_active(void);

        uint64_t count = 0;

        if (kb_userland_active()) {
            // Blocking read: en az 1 karakter gelene kadar bekle
            int ch;
            do {
                ch = kb_ring_pop();
                if (ch < 0) {
                    // Karakter yok — interrupt bekle, task'ı meşgul etme
                    __asm__ volatile ("sti; hlt" ::: "memory");

                    // Task sonlandırılıyorsa çık (sonsuz döngü engeli)
                    task_t* cur = task_get_current();
                    if (cur && cur->state == TASK_STATE_TERMINATED)
                        break;
                }
            } while (ch < 0);

            if (ch >= 0) {
                buf[count++] = (char)ch;

                // '\n' görene kadar veya buffer dolana kadar devam et
                while (count < len) {
                    ch = kb_ring_pop();
                    if (ch < 0) break;          // non-blocking: kalan karakterler
                    buf[count++] = (char)ch;
                    if ((char)ch == '\n') break;
                }
            }
        } else {
            // Kernel shell modu: serial'dan blocking oku
            // (userland aktif değilse buraya düşmemeli ama yine de güvenli)
            while (count < len) {
                while (!serial_data_ready())
                    __asm__ volatile ("pause");
                char c = serial_getchar();
                buf[count++] = c;
                if (c == '\n') break;
            }
        }

        frame->rax = count;
        return;
    }

    // fd >= 3
    task_t* cur = task_get_current();
    if (!cur) { frame->rax = SYSCALL_ERR_PERM; return; }

    fd_entry_t* ent = fd_get(cur->fd_table, fd);
    if (!ent)  { frame->rax = SYSCALL_ERR_BADF; return; }
    if (ent->flags == O_WRONLY) { frame->rax = SYSCALL_ERR_BADF; return; }

    if (ent->type == FD_TYPE_PIPE) {
        pipe_buf_t* pb = ent->pipe;
        if (!pb) { frame->rax = SYSCALL_ERR_BADF; return; }
        if (pb->bytes_avail == 0) {
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

    frame->rax = SYSCALL_ERR_NOSYS;
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
        // Userland bitti → klavyeyi kernel shell'e geri ver
        extern void kb_set_userland_mode(int on);
        kb_set_userland_mode(0);
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
// v5: MAP_FILE desteği eklendi.
//
// MAP_ANONYMOUS: kmalloc tabanlı, fd=-1, offset=0
// MAP_FILE:      fd'nin gösterdiği dosyayı offset'ten itibaren oku,
//                heap'e kopyala. Sayfa tablosu yok (flat memory model).
//                MAP_SHARED bile olsa yazma dosyaya geri dönmez (COW stub).
//
// Arayüz (Linux x86-64 uyumlu):
//   RDI = addr   (istenen adres; 0 = kernel seçsin, MAP_FIXED değilse yok sayılır)
//   RSI = len
//   RDX = prot   (PROT_*)
//   R10 = flags  (MAP_*)
//   R8  = fd     (MAP_ANONYMOUS: -1; MAP_FILE: geçerli fd)
//   R9  = offset (dosya ofseti; 512 byte hizalamalı olmalı)
//
// Dönüş: haritalanan adres (uint64_t) veya (uint64_t)MAP_FAILED
// ---------------------------------------------------------------
static void sys_mmap(syscall_frame_t* frame) {
    uint64_t len    = frame->rsi;
    uint64_t prot   = frame->rdx;   (void)prot;  // gelecekte NX için
    uint64_t flags  = frame->r10;
    int      fd_arg = (int)(int64_t)frame->r8;
    uint64_t offset = frame->r9;

    // Uzunluk kontrolü
    if (len == 0 || len > (256 * 1024 * 1024ULL)) {
        frame->rax = (uint64_t)MAP_FAILED;
        return;
    }

    // Sayfa hizalaması (4KB)
    uint64_t aligned_len = (len + 0xFFF) & ~0xFFFULL;

    // ── MAP_ANONYMOUS ─────────────────────────────────────────────
    if (flags & MAP_ANONYMOUS) {
        if (fd_arg != -1 || offset != 0) {
            frame->rax = (uint64_t)MAP_FAILED;
            return;
        }

        void* mem = kmalloc(aligned_len);
        if (!mem) { frame->rax = (uint64_t)MAP_FAILED; return; }
        kmemset(mem, 0, aligned_len);

        serial_print("[SYSCALL] mmap anon -> 0x");
        print_hex64((uint64_t)mem);
        serial_print(" len="); print_uint64(aligned_len);
        serial_print("\n");

        frame->rax = (uint64_t)mem;
        return;
    }

    // ── MAP_FILE ──────────────────────────────────────────────────
    // fd geçerli ve okuma izinli olmalı
    if (fd_arg < 0 || fd_arg >= MAX_FDS) {
        frame->rax = (uint64_t)MAP_FAILED;
        return;
    }

    // fd 0-2: stdin/stdout/stderr → haritalama yapılamaz
    if (fd_arg <= 2) {
        frame->rax = (uint64_t)MAP_FAILED;
        return;
    }

    task_t* cur = task_get_current();
    if (!cur) { frame->rax = (uint64_t)MAP_FAILED; return; }

    fd_entry_t* ent = fd_get(cur->fd_table, fd_arg);
    if (!ent || ent->type != FD_TYPE_FILE) {
        frame->rax = (uint64_t)MAP_FAILED;
        return;
    }

    // Dosyanın boyutunu sorgula (VFS → FAT32)
    uint64_t file_size = 0;
    {
        const EmbeddedFile64* vf = fs_get_file64(ent->path);
        if (vf) {
            file_size = (uint64_t)vf->size;
        } else {
            file_size = (uint64_t)fat32_file_size(ent->path);
        }
    }

    // offset + len dosya sınırlarını aşmamalı
    if (offset > file_size) {
        frame->rax = (uint64_t)MAP_FAILED;
        return;
    }

    // Haritalanacak gerçek byte sayısı
    uint64_t map_bytes = len;
    if (offset + map_bytes > file_size)
        map_bytes = file_size - offset;   // dosya sonuna kadar

    // Bellek ayır ve sıfırla
    void* mem = kmalloc(aligned_len);
    if (!mem) { frame->rax = (uint64_t)MAP_FAILED; return; }
    kmemset(mem, 0, aligned_len);

    // Dosyadan oku: VFS in-memory mi yoksa FAT32 mi?
    {
        const EmbeddedFile64* vf = fs_get_file64(ent->path);
        if (vf && vf->content && map_bytes > 0) {
            // In-memory VFS: doğrudan kopyala
            kmemcpy(mem, (const void*)(vf->content + offset), map_bytes);
        } else if (map_bytes > 0) {
            // FAT32: fat32_read_file ile offset'ten oku
            // fat32_read_file tüm dosyayı okur; offset'i sonradan uygula
            uint8_t* tmp_buf = (uint8_t*)kmalloc(file_size + 1);
            if (tmp_buf) {
                int rd = fat32_read_file(ent->path, tmp_buf, (uint32_t)file_size);
                if (rd > 0 && (uint64_t)rd > offset) {
                    uint64_t copy_bytes = (uint64_t)rd - offset;
                    if (copy_bytes > map_bytes) copy_bytes = map_bytes;
                    kmemcpy(mem, tmp_buf + offset, copy_bytes);
                }
                kfree(tmp_buf);
            } else {
                // Yetersiz bellek: boş haritalama ile devam et (0-dolu)
                serial_print("[SYSCALL] mmap file: tmp_buf alloc failed, zeroed\n");
            }
        }
    }

    // fd offset'ini güncelle (haritalamadan sonra)
    ent->offset = offset + map_bytes;

    serial_print("[SYSCALL] mmap file fd=");
    { char b[8]; int_to_str(fd_arg, b); serial_print(b); }
    serial_print(" off="); print_uint64(offset);
    serial_print(" len="); print_uint64(map_bytes);
    serial_print(" -> 0x"); print_hex64((uint64_t)mem);
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
// v4 YENİ SYSCALL IMPLEMENTASYONLARI
// ============================================================

// VFS yardımcı fonksiyonları – files64.c / disk64.c'de tanımlı
// (sys_lseek, sys_fstat, sys_mmap MAP_FILE için kullanılır)

// ── SYS_LSEEK (24) ──────────────────────────────────────────────
// lseek(fd, offset, whence) -> yeni ofset | SYSCALL_ERR_*
//
// whence:
//   SEEK_SET (0) – dosyanın başından mutlak konum
//   SEEK_CUR (1) – mevcut konumdan göreli
//   SEEK_END (2) – dosyanın sonundan göreli (dosya boyutu bilinmeli)
//
// Notlar:
//   • Serial ve pipe fd'leri seek'i desteklemez (ESPIPE benzeri).
//   • Dosya boyutu VFS'ten sorgulanır; VFS hazır değilse sadece
//     SEEK_SET ve SEEK_CUR desteklenir.
// ---------------------------------------------------------------
static void sys_lseek(syscall_frame_t* frame) {
    int      fd     = (int)frame->rdi;
    int64_t  offset = (int64_t)frame->rsi;
    int      whence = (int)frame->rdx;

    if (fd < 0 || fd >= MAX_FDS) { frame->rax = SYSCALL_ERR_BADF; return; }
    if (whence < SEEK_SET || whence > SEEK_END) { frame->rax = SYSCALL_ERR_INVAL; return; }

    // stdin/stdout/stderr: seek desteklenmez
    if (fd <= 2) { frame->rax = SYSCALL_ERR_INVAL; return; }

    task_t* cur = task_get_current();
    if (!cur) { frame->rax = SYSCALL_ERR_PERM; return; }

    fd_entry_t* ent = fd_get(cur->fd_table, fd);
    if (!ent) { frame->rax = SYSCALL_ERR_BADF; return; }

    // Serial ve pipe seek'i desteklemez
    if (ent->type == FD_TYPE_SERIAL || ent->type == FD_TYPE_PIPE) {
        frame->rax = SYSCALL_ERR_INVAL;
        return;
    }

    uint64_t cur_offset = ent->offset;
    uint64_t file_size  = 0;

    // SEEK_END için dosya boyutunu VFS'ten al
    if (whence == SEEK_END) {
        const EmbeddedFile64* f = fs_get_file64(ent->path);
        if (f) {
            file_size = (uint64_t)f->size;
        } else {
            // FAT32 sorgula (8.3 isim gerektiriyor; ham path ile dene)
            file_size = (uint64_t)fat32_file_size(ent->path);
        }
    }

    int64_t new_offset;
    switch (whence) {
    case SEEK_SET:
        if (offset < 0) { frame->rax = SYSCALL_ERR_INVAL; return; }
        new_offset = offset;
        break;
    case SEEK_CUR:
        new_offset = (int64_t)cur_offset + offset;
        break;
    case SEEK_END:
        new_offset = (int64_t)file_size + offset;
        break;
    default:
        frame->rax = SYSCALL_ERR_INVAL;
        return;
    }

    if (new_offset < 0) { frame->rax = SYSCALL_ERR_INVAL; return; }

    ent->offset = (uint64_t)new_offset;

    serial_print("[SYSCALL] lseek fd=");
    { char b[8]; int_to_str(fd, b); serial_print(b); }
    serial_print(" new_offset=");
    print_uint64((uint64_t)new_offset);
    serial_print("\n");

    frame->rax = (uint64_t)new_offset;
}

// ── SYS_FSTAT (25) ──────────────────────────────────────────────
// fstat(fd, *stat) -> 0 | SYSCALL_ERR_*
//
// Açık fd'nin meta verilerini stat_t yapısına yazar.
// Dosya bilgileri önce VFS (in-memory), sonra FAT32 katmanından sorgulanır.
// Serial ve pipe fd'leri için st_mode'a uygun bit maskeleri atanır.
// ---------------------------------------------------------------
static void sys_fstat(syscall_frame_t* frame) {
    int      fd      = (int)frame->rdi;
    stat_t*  stat_buf = (stat_t*)frame->rsi;

    if (fd < 0 || fd >= MAX_FDS) { frame->rax = SYSCALL_ERR_BADF; return; }
    if (!is_valid_user_ptr(stat_buf, sizeof(stat_t))) {
        frame->rax = SYSCALL_ERR_FAULT;
        return;
    }

    // stat yapısını sıfırla
    kmemset(stat_buf, 0, sizeof(stat_t));

    // stdin/stdout/stderr → karakter aygıtı
    if (fd <= 2) {
        stat_buf->st_mode    = S_IFCHR | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP;
        stat_buf->st_nlink   = 1;
        stat_buf->st_blksize = 512;
        frame->rax = SYSCALL_OK;
        return;
    }

    task_t* cur = task_get_current();
    if (!cur) { frame->rax = SYSCALL_ERR_PERM; return; }

    fd_entry_t* ent = fd_get(cur->fd_table, fd);
    if (!ent) { frame->rax = SYSCALL_ERR_BADF; return; }

    stat_buf->st_nlink   = 1;
    stat_buf->st_blksize = 512;

    if (ent->type == FD_TYPE_SERIAL) {
        stat_buf->st_mode = S_IFCHR | S_IRUSR | S_IWUSR;
        frame->rax = SYSCALL_OK;
        return;
    }

    if (ent->type == FD_TYPE_PIPE) {
        stat_buf->st_mode = S_IFIFO | S_IRUSR | S_IWUSR;
        // Pipe'taki mevcut byte sayısını boyut olarak ver
        if (ent->pipe) stat_buf->st_size = (uint64_t)ent->pipe->bytes_avail;
        frame->rax = SYSCALL_OK;
        return;
    }

    // Düzenli dosya
    stat_buf->st_mode = S_IFREG | S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;

    // VFS'ten dosya boyutunu sorgula
    uint64_t fsize = 0;
    const EmbeddedFile64* vf = fs_get_file64(ent->path);
    if (vf) {
        fsize = (uint64_t)vf->size;
    } else {
        fsize = (uint64_t)fat32_file_size(ent->path);
    }

    stat_buf->st_size   = fsize;
    stat_buf->st_blocks = (uint32_t)((fsize + 511) / 512);

    serial_print("[SYSCALL] fstat fd=");
    { char b[8]; int_to_str(fd, b); serial_print(b); }
    serial_print(" size=");
    print_uint64(fsize);
    serial_print("\n");

    frame->rax = SYSCALL_OK;
}

// ── SYS_IOCTL (26) ──────────────────────────────────────────────
// ioctl(fd, request, arg) -> 0 | SYSCALL_ERR_*
//
// Desteklenen istekler:
//   TCGETS  (0x5401) – termios_t al (arg = termios_t*)
//   TCSETS  (0x5402) – termios_t ayarla (arg = const termios_t*)
//   TCSETSW (0x5403) – TCSETS ile aynı (çıkış boşalması beklenmez)
//   TCSETSF (0x5404) – TCSETS ile aynı + giriş tamponunu temizle
//   TIOCGWINSZ (0x5413) – winsize_t al (arg = winsize_t*)
//   TIOCSWINSZ (0x5414) – winsize_t ayarla (arg = const winsize_t*)
//   FIONREAD   (0x541B) – okunabilir byte sayısını al (arg = int*)
//   TIOCGPGRP  (0x540F) – ön plan süreç grubunu al (arg = int*)
//   TIOCSPGRP  (0x5410) – ön plan süreç grubunu ayarla (arg = const int*)
//
// Notlar:
//   • Gerçek serial port register ayarı yapılmıyor; termios durumu
//     kernel-side statik yapıda saklanıyor (terminal emülatör yeterli).
//   • winsize varsayılan: 80 sütun × 25 satır.
// ---------------------------------------------------------------

// Kernel-tarafı terminal durumu (tüm seri fd'ler için paylaşımlı)
static termios_t kernel_termios = {
    .c_iflag = ICRNL | IXON,
    .c_oflag = OPOST | ONLCR,
    .c_cflag = CS8 | CREAD | CLOCAL,
    .c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | IEXTEN,
    .c_line  = 0,
    .c_cc    = {
        [VINTR]    = 0x03,  // ^C
        [VQUIT]    = 0x1C,  /* ^\ */
        [VERASE]   = 0x7F,  // DEL
        [VKILL]    = 0x15,  // ^U
        [VEOF]     = 0x04,  // ^D
        [VTIME]    = 0,
        [VMIN]     = 1,
        [VSTART]   = 0x11,  // ^Q
        [VSTOP]    = 0x13,  // ^S
        [VSUSP]    = 0x1A,  // ^Z
    },
    .c_ispeed = B115200,
    .c_ospeed = B115200,
};

// Terminal pencere boyutu (varsayılan VGA 80×25)
static winsize_t kernel_winsize = {
    .ws_row    = 25,
    .ws_col    = 80,
    .ws_xpixel = 0,
    .ws_ypixel = 0,
};

// Ön plan süreç grubu (şimdilik mevcut pid'e eşit)
static int kernel_tty_pgrp = 1;

static void sys_ioctl(syscall_frame_t* frame) {
    int      fd      = (int)frame->rdi;
    uint64_t request = frame->rsi;
    void*    arg     = (void*)frame->rdx;

    if (fd < 0 || fd >= MAX_FDS) { frame->rax = SYSCALL_ERR_BADF; return; }

    // fd 0-2: her zaman terminal
    // Diğer fd'ler için FD_TYPE_SERIAL kontrolü
    if (fd > 2) {
        task_t* cur = task_get_current();
        if (!cur) { frame->rax = SYSCALL_ERR_PERM; return; }
        fd_entry_t* ent = fd_get(cur->fd_table, fd);
        if (!ent) { frame->rax = SYSCALL_ERR_BADF; return; }
        // Sadece serial fd'lere terminal ioctl'ı
        if (ent->type != FD_TYPE_SERIAL && ent->type != FD_TYPE_PIPE) {
            // Dosya fd'lerine genel olarak ENOTTY
            frame->rax = SYSCALL_ERR_INVAL;
            return;
        }
    }

    switch (request) {

    // ── TCGETS: mevcut termios ayarlarını döndür ──────────────────
    case TCGETS:
        if (!is_valid_user_ptr(arg, sizeof(termios_t))) {
            frame->rax = SYSCALL_ERR_FAULT;
            return;
        }
        kmemcpy(arg, &kernel_termios, sizeof(termios_t));
        serial_print("[SYSCALL] ioctl TCGETS ok\n");
        frame->rax = SYSCALL_OK;
        break;

    // ── TCSETS / TCSETSW / TCSETSF: termios ayarlarını uygula ────
    case TCSETS:
    case TCSETSW:
    case TCSETSF:
        if (!is_valid_user_ptr(arg, sizeof(termios_t))) {
            frame->rax = SYSCALL_ERR_FAULT;
            return;
        }
        kmemcpy(&kernel_termios, arg, sizeof(termios_t));
        // TCSETSF: giriş tamponunu temizle (şimdilik stub)
        serial_print("[SYSCALL] ioctl TCSETS");
        if (request == TCSETSF) serial_print("F (flush)");
        else if (request == TCSETSW) serial_print("W (drain)");
        serial_print(" ok\n");
        frame->rax = SYSCALL_OK;
        break;

    // ── TIOCGWINSZ: pencere boyutunu döndür ──────────────────────
    case TIOCGWINSZ:
        if (!is_valid_user_ptr(arg, sizeof(winsize_t))) {
            frame->rax = SYSCALL_ERR_FAULT;
            return;
        }
        kmemcpy(arg, &kernel_winsize, sizeof(winsize_t));
        serial_print("[SYSCALL] ioctl TIOCGWINSZ ok\n");
        frame->rax = SYSCALL_OK;
        break;

    // ── TIOCSWINSZ: pencere boyutunu ayarla ──────────────────────
    case TIOCSWINSZ:
        if (!is_valid_user_ptr(arg, sizeof(winsize_t))) {
            frame->rax = SYSCALL_ERR_FAULT;
            return;
        }
        kmemcpy(&kernel_winsize, arg, sizeof(winsize_t));
        serial_print("[SYSCALL] ioctl TIOCSWINSZ ok\n");
        frame->rax = SYSCALL_OK;
        break;

    // ── FIONREAD: okunabilir byte sayısını döndür ─────────────────
    case FIONREAD: {
        if (!is_valid_user_ptr(arg, sizeof(int))) {
            frame->rax = SYSCALL_ERR_FAULT;
            return;
        }
        int available = 0;
        if (fd == 0) {
            // stdin: seri port LSR'dan
            available = serial_data_ready() ? 1 : 0;
        } else if (fd > 2) {
            task_t* cur = task_get_current();
            if (cur) {
                fd_entry_t* ent = fd_get(cur->fd_table, fd);
                if (ent && ent->type == FD_TYPE_PIPE && ent->pipe)
                    available = (int)ent->pipe->bytes_avail;
            }
        }
        *((int*)arg) = available;
        frame->rax = SYSCALL_OK;
        break;
    }

    // ── TIOCGPGRP: ön plan süreç grubunu döndür ──────────────────
    case TIOCGPGRP:
        if (!is_valid_user_ptr(arg, sizeof(int))) {
            frame->rax = SYSCALL_ERR_FAULT;
            return;
        }
        *((int*)arg) = kernel_tty_pgrp;
        frame->rax = SYSCALL_OK;
        break;

    // ── TIOCSPGRP: ön plan süreç grubunu ayarla ──────────────────
    case TIOCSPGRP:
        if (!is_valid_user_ptr(arg, sizeof(int))) {
            frame->rax = SYSCALL_ERR_FAULT;
            return;
        }
        kernel_tty_pgrp = *((const int*)arg);
        frame->rax = SYSCALL_OK;
        break;

    default:
        serial_print("[SYSCALL] ioctl unknown request=0x");
        print_hex64(request);
        serial_print("\n");
        frame->rax = SYSCALL_ERR_INVAL;
        break;
    }
}


// ============================================================
// v5 YENİ SYSCALL IMPLEMENTASYONLARI
// ============================================================

// ── SYS_SELECT (27) ─────────────────────────────────────────────
static void sys_select(syscall_frame_t* frame) {
    int        nfds      = (int)frame->rdi;
    fd_set_t*  readfds   = (fd_set_t*)frame->rsi;
    fd_set_t*  writefds  = (fd_set_t*)frame->rdx;
    fd_set_t*  exceptfds = (fd_set_t*)frame->r10;
    timeval_t* timeout   = (timeval_t*)frame->r8;

    if (nfds < 0 || nfds > MAX_FDS) { frame->rax = SYSCALL_ERR_INVAL; return; }
    if (readfds   && !is_valid_user_ptr(readfds,   sizeof(fd_set_t)))  { frame->rax = SYSCALL_ERR_FAULT; return; }
    if (writefds  && !is_valid_user_ptr(writefds,  sizeof(fd_set_t)))  { frame->rax = SYSCALL_ERR_FAULT; return; }
    if (exceptfds && !is_valid_user_ptr(exceptfds, sizeof(fd_set_t)))  { frame->rax = SYSCALL_ERR_FAULT; return; }
    if (timeout   && !is_valid_user_ptr(timeout,   sizeof(timeval_t))) { frame->rax = SYSCALL_ERR_FAULT; return; }

    task_t* cur = task_get_current();

    // Deadline hesabı
    uint64_t deadline = 0;
    int      has_deadline = 0;
    if (timeout) {
        uint64_t ms = (uint64_t)timeout->tv_sec * 1000ULL
                    + (uint64_t)timeout->tv_usec / 1000ULL;
        deadline = get_system_ticks() + ms;
        has_deadline = 1;
    }

    fd_set_t out_read, out_write, out_except;
    FD_ZERO(&out_read);
    FD_ZERO(&out_write);
    FD_ZERO(&out_except);
    int nready = 0;

    do {
        nready = 0;
        FD_ZERO(&out_read);
        FD_ZERO(&out_write);

        for (int fd = 0; fd < nfds; fd++) {
            // ── Okuma hazırlığı
            if (readfds && FD_ISSET(fd, readfds)) {
                int ready = 0;
                if (fd == 0) {
                    ready = serial_data_ready();
                } else if (fd == 1 || fd == 2) {
                    ready = 0;
                } else if (cur) {
                    fd_entry_t* ent = fd_get(cur->fd_table, fd);
                    if (ent) {
                        if (ent->type == FD_TYPE_PIPE && ent->pipe)
                            ready = (ent->pipe->bytes_avail > 0);
                        else if (ent->type == FD_TYPE_FILE)
                            ready = 1;
                        else if (ent->type == FD_TYPE_SERIAL)
                            ready = serial_data_ready();
                    }
                }
                if (ready) { FD_SET(fd, &out_read); nready++; }
            }
            // ── Yazma hazırlığı
            if (writefds && FD_ISSET(fd, writefds)) {
                int ready = 0;
                if (fd == 1 || fd == 2) {
                    ready = 1;
                } else if (fd > 2 && cur) {
                    fd_entry_t* ent = fd_get(cur->fd_table, fd);
                    if (ent) {
                        if (ent->type == FD_TYPE_PIPE && ent->pipe)
                            ready = (ent->pipe->bytes_avail < PIPE_BUF_SIZE);
                        else if (ent->type == FD_TYPE_FILE ||
                                 ent->type == FD_TYPE_SERIAL)
                            ready = 1;
                    }
                }
                if (ready) { FD_SET(fd, &out_write); nready++; }
            }
        }

        if (nready > 0) break;
        if (!has_deadline) break;
        if (timeout->tv_sec == 0 && timeout->tv_usec == 0) break;  // non-blocking
        if (get_system_ticks() >= deadline) break;
        scheduler_yield();
    } while (1);

    if (readfds)   *readfds   = out_read;
    if (writefds)  *writefds  = out_write;
    if (exceptfds) FD_ZERO(exceptfds);

    frame->rax = (uint64_t)(int64_t)nready;
}

// ── SYS_POLL (28) ───────────────────────────────────────────────
static void sys_poll(syscall_frame_t* frame) {
    pollfd_t* fds        = (pollfd_t*)frame->rdi;
    uint64_t  nfds       = frame->rsi;
    int       timeout_ms = (int)(int64_t)frame->rdx;

    if (!fds || nfds == 0 || nfds > (uint64_t)MAX_FDS) {
        frame->rax = (nfds == 0) ? 0 : SYSCALL_ERR_INVAL;
        return;
    }
    if (!is_valid_user_ptr(fds, nfds * sizeof(pollfd_t))) {
        frame->rax = SYSCALL_ERR_FAULT;
        return;
    }

    task_t* cur = task_get_current();

    uint64_t deadline    = get_system_ticks() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 0);
    int      has_timeout = (timeout_ms >= 0);

    int nready = 0;

    do {
        nready = 0;

        for (uint64_t i = 0; i < nfds; i++) {
            pollfd_t* pfd = &fds[i];
            pfd->revents = 0;

            if (pfd->fd < 0) continue;

            if (pfd->fd >= MAX_FDS) {
                pfd->revents = POLLNVAL;
                nready++;
                continue;
            }

            // stdin
            if (pfd->fd == 0) {
                if ((pfd->events & POLLIN) && serial_data_ready()) {
                    pfd->revents |= POLLIN;
                    nready++;
                }
                continue;
            }
            // stdout/stderr
            if (pfd->fd == 1 || pfd->fd == 2) {
                if (pfd->events & POLLOUT) {
                    pfd->revents |= POLLOUT;
                    nready++;
                }
                continue;
            }

            if (!cur) { pfd->revents = POLLNVAL; nready++; continue; }

            fd_entry_t* ent = fd_get(cur->fd_table, pfd->fd);
            if (!ent) { pfd->revents = POLLNVAL; nready++; continue; }

            int added = 0;

            if (pfd->events & POLLIN) {
                int ready = 0;
                if (ent->type == FD_TYPE_PIPE && ent->pipe) {
                    ready = (ent->pipe->bytes_avail > 0);
                    if (!ready && ent->pipe->write_closed) {
                        pfd->revents |= POLLHUP | POLLRDHUP;
                        added = 1;
                    }
                } else if (ent->type == FD_TYPE_FILE) {
                    ready = 1;
                } else if (ent->type == FD_TYPE_SERIAL) {
                    ready = serial_data_ready();
                }
                if (ready) { pfd->revents |= POLLIN; added = 1; }
            }

            if (pfd->events & POLLOUT) {
                int ready = 0;
                if (ent->type == FD_TYPE_PIPE && ent->pipe) {
                    ready = (ent->pipe->bytes_avail < PIPE_BUF_SIZE);
                    if (ent->pipe->read_closed) {
                        pfd->revents |= POLLERR; added = 1;
                    }
                } else if (ent->type == FD_TYPE_FILE ||
                           ent->type == FD_TYPE_SERIAL) {
                    ready = 1;
                }
                if (ready) { pfd->revents |= POLLOUT; added = 1; }
            }

            if (added) nready++;
        }

        if (nready > 0) break;
        if (!has_timeout || timeout_ms == 0) break;
        if (get_system_ticks() >= deadline) break;
        scheduler_yield();
    } while (1);

    frame->rax = (uint64_t)(int64_t)nready;
}


// ── SYS_KILL (29) ───────────────────────────────────────────────
// kill(pid, sig) -> 0 | SYSCALL_ERR_*
//
// Desteklenen sinyaller:
//   SIGKILL (9)  – görevi zorla sonlandır (task_kill)
//   SIGTERM (15) – nazikçe sonlandır (şimdilik SIGKILL gibi davranır)
//   Diğerleri   – yoksayılır, 0 döndürülür (stub)
//
// Notlar:
//   • Gerçek sinyal maskesi / handler mekanizması henüz yok.
//   • pid=0: mevcut işleme sinyal gönder.
//   • pid<0: grup sinyal gönderme; şimdilik ENOSYS.
// ---------------------------------------------------------------
static void sys_kill(syscall_frame_t* frame) {
    int pid = (int)(int64_t)frame->rdi;
    int sig = (int)(int64_t)frame->rsi;

    serial_print("[SYSCALL] kill(pid=");
    { char b[12]; int_to_str(pid, b); serial_print(b); }
    serial_print(", sig=");
    { char b[12]; int_to_str(sig, b); serial_print(b); }
    serial_print(")\n");

    // Grup sinyali desteklenmiyor
    if (pid < 0) {
        frame->rax = SYSCALL_ERR_NOSYS;
        return;
    }

    // pid=0: mevcut task'a sinyal gönder
    if (pid == 0) {
        task_t* cur = task_get_current();
        if (!cur) { frame->rax = SYSCALL_ERR_PERM; return; }
        pid = (int)cur->pid;
    }

    // Hedef task'ı bul
    task_t* target = task_find_by_pid((uint32_t)pid);
    if (!target) {
        frame->rax = SYSCALL_ERR_INVAL;  // ESRCH: böyle bir process yok
        return;
    }

    switch (sig) {
    case SIGKILL:
    case SIGTERM:
        // Task'ı sonlandır: state'ini ZOMBIE yap, scheduler devre dışı bırakır
        serial_print("[SYSCALL] kill: terminating pid=");
        { char b[12]; int_to_str(pid, b); serial_print(b); }
        serial_print("\n");
        target->state = TASK_STATE_ZOMBIE;
        target->exit_code = (sig == SIGKILL) ? 137 : 143;
        frame->rax = SYSCALL_OK;
        break;

    case 0:
        // Sinyal 0: sadece process var mı kontrol et (zaten bulduk)
        frame->rax = SYSCALL_OK;
        break;

    default:
        // Diğer sinyaller: şimdilik yoksay, başarı döndür
        // (newlib'in bazı sinyalleri göndermesi gerekebilir)
        serial_print("[SYSCALL] kill: ignoring signal ");
        { char b[12]; int_to_str(sig, b); serial_print(b); }
        serial_print("\n");
        frame->rax = SYSCALL_OK;
        break;
    }
}

// ── SYS_GETTIMEOFDAY (30) ───────────────────────────────────────
// gettimeofday(timeval_t* tv, void* tz) -> 0 | SYSCALL_ERR_*
//
// tv->tv_sec : sistem başlangıcından bu yana geçen saniye
//              (gerçek UTC yok; boot zamanını 0 kabul ediyoruz)
// tv->tv_usec: mikrosaniye (timer tick'ten hesaplanır)
//
// Notlar:
//   • RTC (Real Time Clock) entegrasyonu yapılana kadar boot-relative zaman verir.
//   • tz parametresi her zaman yoksayılır (POSIX da öyle önerir).
//   • newlib'in time(), clock() fonksiyonları bu syscall'a bağımlıdır.
// ---------------------------------------------------------------
static void sys_gettimeofday(syscall_frame_t* frame) {
    timeval_t* tv = (timeval_t*)frame->rdi;
    // tz (frame->rsi) yoksayılır

    if (tv) {
        if (!is_valid_user_ptr(tv, sizeof(timeval_t))) {
            frame->rax = SYSCALL_ERR_FAULT;
            return;
        }

        // get_system_ticks() milisaniye cinsinden tick sayısı döndürür
        // (timer.h: varsayılan 1000 Hz → 1 tick = 1 ms)
        uint64_t ticks_ms = get_system_ticks();

        tv->tv_sec  = (int64_t)(ticks_ms / 1000ULL);
        tv->tv_usec = (int64_t)((ticks_ms % 1000ULL) * 1000ULL);
    }

    frame->rax = SYSCALL_OK;
}

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
    // v4
    case SYS_LSEEK:        sys_lseek(frame);       break;
    case SYS_FSTAT:        sys_fstat(frame);       break;
    case SYS_IOCTL:        sys_ioctl(frame);       break;
    // v5
    case SYS_SELECT:       sys_select(frame);       break;
    case SYS_POLL:         sys_poll(frame);         break;
    // v6 – newlib uyumu
    case SYS_KILL:         sys_kill(frame);         break;
    case SYS_GETTIMEOFDAY: sys_gettimeofday(frame); break;
    default:
        serial_print("[SYSCALL] Unknown syscall: ");
        { char b[16]; int_to_str((int)(num & 0xFFFF), b); serial_print(b); }
        serial_print("\n");
        frame->rax = SYSCALL_ERR_NOSYS;
        break;
    }
}

// ============================================================
// SYSCALL_TEST – v4 testleri dahil
// ============================================================
void syscall_test(void) {
    if (!syscall_enabled) {
        serial_print("[SYSCALL TEST] SYSCALL not enabled!\n");
        return;
    }
    serial_print("\n========================================\n");
    serial_print("[SYSCALL TEST] v4 comprehensive tests\n");
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

        DO_SYSCALL3(SYS_WRITE, 8, "dup2_ok\n", 8);

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
    serial_print("\n[T12] SYS_FORK (kernel context smoke test):\n");
    DO_SYSCALL0(SYS_FORK);
    serial_print("  fork_ret="); { char b[16]; int_to_str((int)ret,b); serial_print(b); }
    serial_print("\n");
    if ((int64_t)ret > 0) {
        serial_print("  [parent] child_pid="); print_uint64(ret); serial_print("\n");

        serial_print("\n[T13] SYS_WAITPID(child, WNOHANG):\n");
        int ws = 0;
        uint64_t child_pid = ret;
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

    // ============================================================
    // v4 YENİ TESTLER
    // ============================================================
    serial_print("\n----------------------------------------\n");
    serial_print("[SYSCALL TEST] v4 tests begin\n");
    serial_print("----------------------------------------\n");

    // ── SYS_FSTAT – stdin (fd=0) ────────────────────────────────
    serial_print("\n[T15] SYS_FSTAT fd=0 (stdin, expect S_IFCHR):\n");
    stat_t st;
    DO_SYSCALL2(SYS_FSTAT, 0, &st);
    serial_print("  ret="); { char b[8]; int_to_str((int)ret,b); serial_print(b); }
    serial_print(" st_mode=0x"); print_hex64((uint64_t)st.st_mode);
    serial_print(" st_size="); print_uint64(st.st_size);
    serial_print("\n");
    if (st.st_mode & S_IFCHR)
        serial_print("  [OK] S_IFCHR set (character device)\n");
    else
        serial_print("  [FAIL] S_IFCHR not set!\n");

    // ── SYS_FSTAT – stdout (fd=1) ───────────────────────────────
    serial_print("\n[T16] SYS_FSTAT fd=1 (stdout, expect S_IFCHR):\n");
    stat_t st2;
    DO_SYSCALL2(SYS_FSTAT, 1, &st2);
    serial_print("  ret="); { char b[8]; int_to_str((int)ret,b); serial_print(b); }
    serial_print(" st_mode=0x"); print_hex64((uint64_t)st2.st_mode);
    serial_print("\n");

    // ── SYS_FSTAT – pipe fd ──────────────────────────────────────
    serial_print("\n[T17] SYS_FSTAT üzerinden pipe (S_IFIFO bekleniyor):\n");
    int pstat_fds[2] = {-1, -1};
    DO_SYSCALL1(SYS_PIPE, pstat_fds);
    if (ret == (uint64_t)SYSCALL_OK && pstat_fds[0] > 0) {
        // Pipe'a birkaç byte yaz (bytes_avail için)
        DO_SYSCALL3(SYS_WRITE, pstat_fds[1], "hello", 5);
        stat_t sp;
        DO_SYSCALL2(SYS_FSTAT, pstat_fds[0], &sp);
        serial_print("  ret="); { char b[8]; int_to_str((int)ret,b); serial_print(b); }
        serial_print(" st_mode=0x"); print_hex64((uint64_t)sp.st_mode);
        serial_print(" st_size(bytes_avail)="); print_uint64(sp.st_size);
        serial_print("\n");
        if (sp.st_mode & S_IFIFO)
            serial_print("  [OK] S_IFIFO set\n");
        else
            serial_print("  [FAIL] S_IFIFO not set!\n");
        if (sp.st_size == 5)
            serial_print("  [OK] bytes_avail=5\n");
        else
            serial_print("  [WARN] bytes_avail mismatch\n");
        DO_SYSCALL1(SYS_CLOSE, pstat_fds[0]);
        DO_SYSCALL1(SYS_CLOSE, pstat_fds[1]);
    } else {
        serial_print("  [SKIP] pipe allocation failed\n");
    }

    // ── SYS_FSTAT – geçersiz fd ──────────────────────────────────
    serial_print("\n[T18] SYS_FSTAT fd=999 (expect EBADF=-5):\n");
    stat_t st_bad;
    DO_SYSCALL2(SYS_FSTAT, 999, &st_bad);
    serial_print("  ret="); { char b[8]; int_to_str((int)(int64_t)ret,b); serial_print(b); }
    serial_print(" (expect -5)\n");

    // ── SYS_LSEEK – open bir dosyada SEEK_SET ────────────────────
    // Not: SYS_OPEN /dev/serial0'a seek yapılabilmesi için FD_TYPE_SERIAL
    // seek'i desteklemiyor; bu yüzden geçersiz test yapıyoruz.
    serial_print("\n[T19] SYS_LSEEK stdin (expect EINVAL=-1, serial):\n");
    DO_SYSCALL3(SYS_LSEEK, 0, 0, SEEK_SET);
    serial_print("  ret="); { char b[8]; int_to_str((int)(int64_t)ret,b); serial_print(b); }
    serial_print(" (expect -1, stdin not seekable)\n");

    // Gerçek dosya fd'si açılabilirse lseek testi
    serial_print("\n[T20] SYS_OPEN + SYS_LSEEK (file fd):\n");
    DO_SYSCALL2(SYS_OPEN, "/dev/serial0", O_RDWR);
    int test_fd = (int)(int64_t)ret;
    serial_print("  open ret="); { char b[8]; int_to_str(test_fd,b); serial_print(b); }
    serial_print("\n");
    if (test_fd >= 3) {
        // Serial fd seek desteklemez → EINVAL bekliyoruz
        DO_SYSCALL3(SYS_LSEEK, test_fd, 0, SEEK_SET);
        serial_print("  lseek SEEK_SET ret="); { char b[8]; int_to_str((int)(int64_t)ret,b); serial_print(b); }
        serial_print(" (expect -1 for serial)\n");
        // SEEK_CUR
        DO_SYSCALL3(SYS_LSEEK, test_fd, 10, SEEK_CUR);
        serial_print("  lseek SEEK_CUR+10 ret="); { char b[8]; int_to_str((int)(int64_t)ret,b); serial_print(b); }
        serial_print("\n");
        DO_SYSCALL1(SYS_CLOSE, test_fd);
    }

    // ── SYS_LSEEK – geçersiz whence ─────────────────────────────
    serial_print("\n[T21] SYS_LSEEK geçersiz whence=99 (expect EINVAL=-1):\n");
    DO_SYSCALL3(SYS_LSEEK, 1, 0, 99);
    serial_print("  ret="); { char b[8]; int_to_str((int)(int64_t)ret,b); serial_print(b); }
    serial_print(" (expect -1)\n");

    // ── SYS_IOCTL – TCGETS ──────────────────────────────────────
    serial_print("\n[T22] SYS_IOCTL TCGETS (fd=0):\n");
    termios_t tios;
    DO_SYSCALL3(SYS_IOCTL, 0, TCGETS, &tios);
    serial_print("  ret="); { char b[8]; int_to_str((int)ret,b); serial_print(b); }
    serial_print(" c_iflag=0x"); print_hex64((uint64_t)tios.c_iflag);
    serial_print(" c_lflag=0x"); print_hex64((uint64_t)tios.c_lflag);
    serial_print("\n");
    if (tios.c_lflag & ECHO)
        serial_print("  [OK] ECHO flag set\n");
    if (tios.c_lflag & ICANON)
        serial_print("  [OK] ICANON flag set\n");

    // ── SYS_IOCTL – TCSETS: raw mode geçişi ─────────────────────
    serial_print("\n[T23] SYS_IOCTL TCSETS (raw mode):\n");
    termios_t raw = tios;
    // Raw mod: ECHO + ICANON + ISIG + IEXTEN kapat
    raw.c_lflag &= ~(ECHO | ECHOE | ECHOK | ICANON | ISIG | IEXTEN);
    raw.c_iflag &= ~(ICRNL | IXON);
    raw.c_oflag &= ~OPOST;
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;
    DO_SYSCALL3(SYS_IOCTL, 0, TCSETS, &raw);
    serial_print("  ret="); { char b[8]; int_to_str((int)ret,b); serial_print(b); }
    serial_print(" (expect 0)\n");

    // Doğrula: geri oku
    termios_t verify;
    DO_SYSCALL3(SYS_IOCTL, 0, TCGETS, &verify);
    serial_print("  verify c_lflag=0x"); print_hex64((uint64_t)verify.c_lflag);
    if (!(verify.c_lflag & ECHO))
        serial_print(" [OK] ECHO off\n");
    else
        serial_print(" [FAIL] ECHO still on!\n");

    // Canonical moda geri dön
    serial_print("\n[T24] SYS_IOCTL TCSETSF (canonical'a geri dön):\n");
    DO_SYSCALL3(SYS_IOCTL, 0, TCSETSF, &tios);
    serial_print("  ret="); { char b[8]; int_to_str((int)ret,b); serial_print(b); }
    serial_print(" (expect 0)\n");
    termios_t verify2;
    DO_SYSCALL3(SYS_IOCTL, 0, TCGETS, &verify2);
    if (verify2.c_lflag & ECHO)
        serial_print("  [OK] ECHO restored\n");
    else
        serial_print("  [FAIL] ECHO not restored!\n");

    // ── SYS_IOCTL – TIOCGWINSZ ──────────────────────────────────
    serial_print("\n[T25] SYS_IOCTL TIOCGWINSZ:\n");
    winsize_t ws;
    DO_SYSCALL3(SYS_IOCTL, 1, TIOCGWINSZ, &ws);
    serial_print("  ret="); { char b[8]; int_to_str((int)ret,b); serial_print(b); }
    serial_print(" rows="); { char b[8]; int_to_str((int)ws.ws_row,b); serial_print(b); }
    serial_print(" cols="); { char b[8]; int_to_str((int)ws.ws_col,b); serial_print(b); }
    serial_print("\n");
    if (ws.ws_row == 25 && ws.ws_col == 80)
        serial_print("  [OK] Default 80x25\n");
    else
        serial_print("  [WARN] Unexpected window size\n");

    // ── SYS_IOCTL – TIOCSWINSZ ──────────────────────────────────
    serial_print("\n[T26] SYS_IOCTL TIOCSWINSZ (132x50):\n");
    winsize_t new_ws = { .ws_row = 50, .ws_col = 132, .ws_xpixel = 0, .ws_ypixel = 0 };
    DO_SYSCALL3(SYS_IOCTL, 1, TIOCSWINSZ, &new_ws);
    serial_print("  ret="); { char b[8]; int_to_str((int)ret,b); serial_print(b); }
    serial_print(" (expect 0)\n");
    // Geri oku
    winsize_t ws2;
    DO_SYSCALL3(SYS_IOCTL, 1, TIOCGWINSZ, &ws2);
    serial_print("  verify rows="); { char b[8]; int_to_str((int)ws2.ws_row,b); serial_print(b); }
    serial_print(" cols="); { char b[8]; int_to_str((int)ws2.ws_col,b); serial_print(b); }
    serial_print("\n");
    if (ws2.ws_row == 50 && ws2.ws_col == 132)
        serial_print("  [OK] 132x50 set\n");
    else
        serial_print("  [FAIL] winsize mismatch!\n");
    // Orijinale geri dön
    winsize_t restore_ws = { .ws_row = 25, .ws_col = 80 };
    DO_SYSCALL3(SYS_IOCTL, 1, TIOCSWINSZ, &restore_ws);

    // ── SYS_IOCTL – FIONREAD ────────────────────────────────────
    serial_print("\n[T27] SYS_IOCTL FIONREAD (stdin):\n");
    int avail = -1;
    DO_SYSCALL3(SYS_IOCTL, 0, FIONREAD, &avail);
    serial_print("  ret="); { char b[8]; int_to_str((int)ret,b); serial_print(b); }
    serial_print(" avail="); { char b[8]; int_to_str(avail,b); serial_print(b); }
    serial_print(" (0 if no input pending)\n");

    // ── SYS_IOCTL – TIOCGPGRP / TIOCSPGRP ──────────────────────
    serial_print("\n[T28] SYS_IOCTL TIOCGPGRP/TIOCSPGRP:\n");
    int pgrp = -1;
    DO_SYSCALL3(SYS_IOCTL, 0, TIOCGPGRP, &pgrp);
    serial_print("  TIOCGPGRP ret="); { char b[8]; int_to_str((int)ret,b); serial_print(b); }
    serial_print(" pgrp="); { char b[8]; int_to_str(pgrp,b); serial_print(b); }
    serial_print("\n");
    int new_pgrp = 42;
    DO_SYSCALL3(SYS_IOCTL, 0, TIOCSPGRP, &new_pgrp);
    serial_print("  TIOCSPGRP(42) ret="); { char b[8]; int_to_str((int)ret,b); serial_print(b); }
    serial_print("\n");
    int pgrp2 = 0;
    DO_SYSCALL3(SYS_IOCTL, 0, TIOCGPGRP, &pgrp2);
    if (pgrp2 == 42)
        serial_print("  [OK] pgrp=42\n");
    else
        serial_print("  [FAIL] pgrp mismatch!\n");
    // Geri dön
    DO_SYSCALL3(SYS_IOCTL, 0, TIOCSPGRP, &pgrp);

    // ── SYS_IOCTL – bilinmeyen request ──────────────────────────
    serial_print("\n[T29] SYS_IOCTL unknown request 0xDEAD (expect EINVAL=-1):\n");
    DO_SYSCALL3(SYS_IOCTL, 0, 0xDEAD, 0);
    serial_print("  ret="); { char b[8]; int_to_str((int)(int64_t)ret,b); serial_print(b); }
    serial_print(" (expect -1)\n");

    // ── SYS_IOCTL – NULL arg ile TCGETS (expect EFAULT) ─────────
    serial_print("\n[T30] SYS_IOCTL TCGETS NULL arg (expect EFAULT=-11):\n");
    DO_SYSCALL3(SYS_IOCTL, 0, TCGETS, 0);
    serial_print("  ret="); { char b[8]; int_to_str((int)(int64_t)ret,b); serial_print(b); }
    serial_print(" (expect -11)\n");

    serial_print("\n========================================\n");
    serial_print("[SYSCALL TEST] All v4 tests completed.\n");
    serial_print("========================================\n\n");

    // ============================================================
    // v6 Testleri: SYS_KILL + SYS_GETTIMEOFDAY
    // ============================================================
    serial_print("\n========================================\n");
    serial_print("[SYSCALL TEST] v6: kill + gettimeofday\n");
    serial_print("========================================\n");

    // ── SYS_GETTIMEOFDAY – normal kullanım ───────────────────────
    serial_print("\n[T31] SYS_GETTIMEOFDAY (normal):\n");
    timeval_t tv1;
    DO_SYSCALL2(SYS_GETTIMEOFDAY, &tv1, 0);
    serial_print("  ret="); { char b[8]; int_to_str((int)ret,b); serial_print(b); }
    serial_print(" tv_sec="); print_uint64((uint64_t)tv1.tv_sec);
    serial_print(" tv_usec="); print_uint64((uint64_t)tv1.tv_usec);
    serial_print("\n");
    if (ret == SYSCALL_OK)
        serial_print("  [OK] gettimeofday başarılı\n");
    else
        serial_print("  [FAIL] gettimeofday hata döndü!\n");

    // ── SYS_GETTIMEOFDAY – kısa sleep sonrası zaman ilerliyor mu? ─
    serial_print("\n[T32] SYS_GETTIMEOFDAY – sleep(50) sonrası zaman ilerlemeli:\n");
    timeval_t tv_before, tv_after;
    DO_SYSCALL2(SYS_GETTIMEOFDAY, &tv_before, 0);
    DO_SYSCALL1(SYS_SLEEP, 50);   // 50 tick bekle
    DO_SYSCALL2(SYS_GETTIMEOFDAY, &tv_after, 0);
    int64_t delta_sec  = tv_after.tv_sec  - tv_before.tv_sec;
    int64_t delta_usec = tv_after.tv_usec - tv_before.tv_usec;
    int64_t delta_ms   = delta_sec * 1000LL + delta_usec / 1000LL;
    serial_print("  before: sec="); print_uint64((uint64_t)tv_before.tv_sec);
    serial_print(" usec=");  print_uint64((uint64_t)tv_before.tv_usec);
    serial_print("\n  after:  sec="); print_uint64((uint64_t)tv_after.tv_sec);
    serial_print(" usec=");  print_uint64((uint64_t)tv_after.tv_usec);
    serial_print("\n  delta_ms="); { char b[16]; int_to_str((int)delta_ms,b); serial_print(b); }
    serial_print("\n");
    if (delta_ms > 0)
        serial_print("  [OK] zaman ilerledi\n");
    else
        serial_print("  [WARN] zaman ilerlemedi (tick hızı?\n");

    // ── SYS_GETTIMEOFDAY – NULL tv (tz parametresini yoksay) ────
    serial_print("\n[T33] SYS_GETTIMEOFDAY NULL tv (tz yoksayılmalı):\n");
    DO_SYSCALL2(SYS_GETTIMEOFDAY, 0, 0);
    serial_print("  ret="); { char b[8]; int_to_str((int)ret,b); serial_print(b); }
    serial_print(" (expect 0, NULL tv ok)\n");
    if (ret == SYSCALL_OK)
        serial_print("  [OK]\n");
    else
        serial_print("  [FAIL]\n");

    // ── SYS_GETTIMEOFDAY – geçersiz pointer ─────────────────────
    serial_print("\n[T34] SYS_GETTIMEOFDAY geçersiz ptr (expect EFAULT=-11):\n");
    DO_SYSCALL2(SYS_GETTIMEOFDAY, (void*)0xDEADBABEDEADBABEull, 0);
    serial_print("  ret="); { char b[8]; int_to_str((int)(int64_t)ret,b); serial_print(b); }
    serial_print(" (expect -11)\n");
    if ((int64_t)ret == -11)
        serial_print("  [OK]\n");
    else
        serial_print("  [FAIL]\n");

    // ── SYS_KILL – sinyal 0 (process var mı kontrolü) ───────────
    serial_print("\n[T35] SYS_KILL sig=0 kendi pid'imize (process kontrolü):\n");
    DO_SYSCALL0(SYS_GETPID);
    uint64_t my_pid = ret;
    DO_SYSCALL2(SYS_KILL, my_pid, 0);
    serial_print("  ret="); { char b[8]; int_to_str((int)ret,b); serial_print(b); }
    serial_print(" (expect 0)\n");
    if (ret == SYSCALL_OK)
        serial_print("  [OK] process bulundu\n");
    else
        serial_print("  [FAIL]\n");

    // ── SYS_KILL – geçersiz pid ──────────────────────────────────
    serial_print("\n[T36] SYS_KILL geçersiz pid=9999 (expect EINVAL=-1):\n");
    DO_SYSCALL2(SYS_KILL, 9999, SIGTERM);
    serial_print("  ret="); { char b[8]; int_to_str((int)(int64_t)ret,b); serial_print(b); }
    serial_print(" (expect -1)\n");
    if ((int64_t)ret == -1)
        serial_print("  [OK]\n");
    else
        serial_print("  [FAIL]\n");

    // ── SYS_KILL – pid=0 (mevcut task'a SIGUSR1, yoksanır) ──────
    serial_print("\n[T37] SYS_KILL pid=0 SIGUSR1 (yoksanmalı, ret=0):\n");
    DO_SYSCALL2(SYS_KILL, 0, SIGUSR1);
    serial_print("  ret="); { char b[8]; int_to_str((int)ret,b); serial_print(b); }
    serial_print(" (expect 0)\n");
    if (ret == SYSCALL_OK)
        serial_print("  [OK]\n");
    else
        serial_print("  [FAIL]\n");

    // ── SYS_KILL – grup sinyal (pid<0, expect ENOSYS) ────────────
    serial_print("\n[T38] SYS_KILL pid=-1 (grup sinyal, expect ENOSYS=-2):\n");
    DO_SYSCALL2(SYS_KILL, (uint64_t)(int64_t)-1, SIGTERM);
    serial_print("  ret="); { char b[8]; int_to_str((int)(int64_t)ret,b); serial_print(b); }
    serial_print(" (expect -2)\n");
    if ((int64_t)ret == -2)
        serial_print("  [OK]\n");
    else
        serial_print("  [FAIL]\n");

    // ── SYS_KILL – SIGKILL ile fork çocuğunu sonlandır ──────────
    serial_print("\n[T39] SYS_FORK + SYS_KILL(SIGKILL) çocuğu öldür:\n");
    DO_SYSCALL0(SYS_FORK);
    int64_t fork_ret = (int64_t)ret;
    serial_print("  fork ret="); { char b[12]; int_to_str((int)fork_ret,b); serial_print(b); }
    serial_print("\n");
    if (fork_ret > 0) {
        // Ebeveyndeyiz, çocuğu öldür
        DO_SYSCALL2(SYS_KILL, (uint64_t)fork_ret, SIGKILL);
        serial_print("  kill(child, SIGKILL) ret=");
        { char b[8]; int_to_str((int)ret,b); serial_print(b); }
        serial_print("\n");
        if (ret == SYSCALL_OK)
            serial_print("  [OK] çocuk sonlandırıldı\n");
        else
            serial_print("  [FAIL] kill başarısız\n");
        // Çocuğu topla
        DO_SYSCALL3(SYS_WAITPID, (uint64_t)fork_ret, 0, 0);
        serial_print("  waitpid ret="); { char b[12]; int_to_str((int)(int64_t)ret,b); serial_print(b); }
        serial_print("\n");
    } else if (fork_ret == 0) {
        // Çocuktayız: uyuyalım, ebeveyn bizi öldürecek
        DO_SYSCALL1(SYS_SLEEP, 1000);
        DO_SYSCALL1(SYS_EXIT, 0);
    } else {
        serial_print("  [SKIP] fork başarısız\n");
    }

    serial_print("\n========================================\n");
    serial_print("[SYSCALL TEST] v6 testleri tamamlandı.\n");
    serial_print("========================================\n\n");

#undef DO_SYSCALL0
#undef DO_SYSCALL1
#undef DO_SYSCALL2
#undef DO_SYSCALL3
}