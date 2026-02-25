#include "syscall.h"
#include "signal64.h"
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

// signal64.c — sinyal syscall handler'ları
extern void sys_sigaction   (syscall_frame_t* frame);
extern void sys_sigprocmask (syscall_frame_t* frame);
extern void sys_sigreturn   (syscall_frame_t* frame);
extern void sys_sigpending  (syscall_frame_t* frame);
extern void sys_sigsuspend  (syscall_frame_t* frame);

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

// VFS dizin syscall'ları için – files64.c'de tanımlı
extern const char* fs_getcwd64(void);
extern int         fs_chdir64(const char* path);

// VFS stat/access yardımcıları – files64.c'de tanımlı (v8)
extern int      fs_path_is_file(const char* path);
extern int      fs_path_is_dir(const char* path);
extern uint32_t fs_path_filesize(const char* path);

// VFS getdents yardımcısı – files64.c'de tanımlı (v9)
extern int fs_getdents64(const char* path, dirent64_t* buf, int buf_size);

// VFS yazma işlemleri – files64.c'de tanımlı (v14)
// İmzalar files64.h ile eşleşmeli
extern int fs_mkdir64(const char* path);
extern int fs_rmdir64(const char* path);
extern int fs_unlink64(const char* path);
extern int fs_rename64(const char* oldpath, const char* newpath);

// Kısa takma adlar – sadece bu dosyada, extern değil
#define kmemset(dst, val, n)  memset64((dst),(val),(uint64_t)(n))
#define kmemcpy(dst, src, n)  memcpy64((dst),(src),(uint64_t)(n))

// task_exit: task.c'de tanımlı – fork stub için gerekli
extern void task_exit(void);

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
    table[0].type     = FD_TYPE_SERIAL;
    table[0].flags    = O_RDONLY;
    table[0].fd_flags = 0;
    table[0].is_open  = 1;
    my_strncpy(table[0].path, "/dev/serial0", 52);

    // stdout (fd 1) – yazma
    table[1].type     = FD_TYPE_SERIAL;
    table[1].flags    = O_WRONLY;
    table[1].fd_flags = 0;
    table[1].is_open  = 1;
    my_strncpy(table[1].path, "/dev/serial0", 52);

    // stderr (fd 2) – yazma
    table[2].type     = FD_TYPE_SERIAL;
    table[2].flags    = O_WRONLY;
    table[2].fd_flags = 0;
    table[2].is_open  = 1;
    my_strncpy(table[2].path, "/dev/serial0", 52);
}

int fd_alloc(fd_entry_t* table, uint8_t type, uint8_t flags, const char* path) {
    for (int i = 3; i < MAX_FDS; i++) {
        if (!table[i].is_open) {
            table[i].type     = type;
            table[i].flags    = flags;
            table[i].fd_flags = 0;
            table[i].is_open  = 1;
            table[i].offset   = 0;
            table[i].pipe     = 0;
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
            table[i].type     = FD_TYPE_PIPE;
            table[i].flags    = rw_flags;
            table[i].fd_flags = 0;
            table[i].is_open  = 1;
            table[i].offset   = 0;
            table[i].pipe     = pbuf;
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
    wrmsr(0xC0000083, 0);

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
            // Kernel context: non-blocking serial okuma.
            // Veri yoksa hemen EAGAIN döndür; blocking bekleme yok.
            while (count < len) {
                if (!serial_data_ready()) break;
                char c = serial_getchar();
                buf[count++] = c;
                if (c == '\n') break;
            }
            if (count == 0) {
                frame->rax = SYSCALL_ERR_AGAIN;
                return;
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
// Forward: pg_update_current_pid, pg_resolve_pid ile ayni dosyada tanimlidir.
static void pg_update_current_pid(uint32_t pid);

static void sys_getpid(syscall_frame_t* frame) {
    task_t* cur = task_get_current();
    uint64_t pid = cur ? (uint64_t)cur->pid : 0;
    if (pid != 0) pg_update_current_pid((uint32_t)pid);
    frame->rax = pid;
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
    if (!is_valid_user_ptr(msg, 256)) { frame->rax = SYSCALL_ERR_INVAL; return; }
    serial_print("[DEBUG] ");
    serial_print(msg);
    serial_print("\n");
    frame->rax = SYSCALL_OK;
}

// ── SYS_OPEN (9) ────────────────────────────────────────────────
static void sys_open(syscall_frame_t* frame) {
    const char* path  = (const char*)frame->rdi;
    uint64_t    flags = frame->rsi;

    if (!is_valid_user_ptr(path, 128)) { frame->rax = SYSCALL_ERR_INVAL; return; }

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
    // POSIX: fork() sonrası child, parent'ın process group ve session'ını miras alır.
    child->pgid = parent->pgid;
    child->sid  = parent->sid;
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
// execve(path, argv, envp) -> err  (başarıda mevcut task'a DÖNMEZ)
//
// Uygulama özeti:
//   1. path doğrula  (EFAULT / ENOENT)
//   2. Dosyayı in-memory VFS'den veya FAT32'den oku
//   3. ELF64 doğrula + yükle  (elf64_validate / elf64_load)
//   4. Mevcut task'ın user stack'ini temizle, yeni stack ayır
//   5. argv[] / envp[] dizilerini yeni user stack'e kopyala
//   6. task::context'i entry'ye yönlendir  (SYSRET ile döner)
//
// Kısıtlamalar (flat-memory / no-VMM):
//   • Dinamik linker (ld.so) desteği yok → ET_DYN PIE statik.
//   • ELF segmentleri fiziksel adrese doğrudan yüklenir;
//     ayrıcalık seviyesi user ise sayfa tablosu güncellemesi
//     gerekir — bu çekirdek VMM katmanının sorumluluğundadır.
//   • Başarılı execve sonrası eskiden açık dosyalar O_CLOEXEC ise
//     kapatılır (POSIX uyumu).
//
// Yol çözümleme:
//   • Path "/bin/<name>" ise FAT32'den "BIN/<NAME>" 8.3 adıyla dene.
//   • Path VFS'de bulunursa in-memory içerik ELF olarak yorumlanır.
//   • Hiçbirinde yoksa ENOENT döner.
// ---------------------------------------------------------------

// ELF yükleme için elf64.h fonksiyonları
#include "elf64.h"
#include <stddef.h>
// execve argümanlarını yeni user stack'e yazan yardımcı.
// argv[] / envp[] NULL-terminated dizi; her string stack'e kopyalanır.
// stack_ptr (inout): mevcut RSP; aşağı doğru büyür.
// Döner: yeni RSP; argc ve argv başlangıç adresi *out_argc / *out_argv'a yazılır.
static uint64_t execve_build_stack(uint64_t stack_top,
                                   const char** argv, const char** envp,
                                   int* out_argc, uint64_t* out_argv_ptr,
                                   uint64_t* out_envp_ptr) {
    // Güvenli erişim için NULL kontrolü
    if (!argv) argv = (const char*[]){NULL};
    if (!envp) envp = (const char*[]){NULL};

    // argc hesapla
    int argc = 0;
    while (argv[argc]) argc++;

    int envc = 0;
    while (envp[envc]) envc++;

    // Stack'i 16-byte hizalı başlat
    uint64_t sp = stack_top & ~(uint64_t)0xF;

    // String içeriklerini stack'e kopyala (ters sıra — üstten aşağı)
    // Önce envp stringleri
    uint64_t env_strs[64];
    for (int i = envc - 1; i >= 0; i--) {
        int len = 0;
        while (envp[i][len]) len++;
        len++;  // null terminator
        sp -= (uint64_t)len;
        sp &= ~(uint64_t)0x0;   // byte hizası yeterli
        char* dst = (char*)sp;
        for (int j = 0; j < len; j++) dst[j] = envp[i][j];
        env_strs[i] = sp;
    }

    // Sonra argv stringleri
    uint64_t arg_strs[64];
    for (int i = argc - 1; i >= 0; i--) {
        int len = 0;
        while (argv[i][len]) len++;
        len++;
        sp -= (uint64_t)len;
        char* dst = (char*)sp;
        for (int j = 0; j < len; j++) dst[j] = argv[i][j];
        arg_strs[i] = sp;
    }

    // 16-byte hizala
    sp &= ~(uint64_t)0xF;

    // SysV ABI: NULL | envp ptrs (ters) | NULL | argv ptrs (ters) | argc
    // Küçük çekirdek ABI: sadece argc, argv[], 0, envp[], 0 dizileri

    // envp NULL sentinel
    sp -= 8; *(uint64_t*)sp = 0;
    // envp pointers (ters sıra kayıt, doğru okunur)
    for (int i = envc - 1; i >= 0; i--) {
        sp -= 8; *(uint64_t*)sp = env_strs[i];
    }
    uint64_t envp_base = sp;

    // argv NULL sentinel
    sp -= 8; *(uint64_t*)sp = 0;
    // argv pointers
    for (int i = argc - 1; i >= 0; i--) {
        sp -= 8; *(uint64_t*)sp = arg_strs[i];
    }
    uint64_t argv_base = sp;

    // argc
    sp -= 8; *(uint64_t*)sp = (uint64_t)argc;

    // 16-byte hizala (ABI gereksinimi)
    sp &= ~(uint64_t)0xF;

    *out_argc     = argc;
    *out_argv_ptr = argv_base;
    *out_envp_ptr = envp_base;
    return sp;
}

// execve için küçük bir okuma tamponu (statik; reentrant değil,
// ama çekirdek tek iş parçacıklı syscall handler'da güvenli)
#define EXECVE_BUF_SIZE (1u * 1024u * 1024u)   // 1 MB
static uint8_t execve_elf_buf[EXECVE_BUF_SIZE];

// Basit path → FAT 8.3 dönüşümü:
//   "/bin/hello" → "BIN/HELLO"  (nokta yok ise uzantısız 8 kar)
//   "/usr/bin/hello" → "HELLO"  (sadece son bileşen)
static void path_to_fat83(const char* path, char* out83) {
    // Son '/' bul
    int last_slash = -1;
    for (int i = 0; path[i]; i++)
        if (path[i] == '/') last_slash = i;

    const char* base = (last_slash >= 0) ? path + last_slash + 1 : path;

    // Büyük harfe çevir, en fazla 8 karakter al
    int j = 0;
    for (int i = 0; base[i] && j < 8; i++) {
        char c = base[i];
        if (c == '.') break;
        if (c >= 'a' && c <= 'z') c -= 32;
        out83[j++] = c;
    }
    out83[j] = '\0';
}

static void sys_execve(syscall_frame_t* frame) {
    const char*  path = (const char*)frame->rdi;
    const char** argv = (const char**)frame->rsi;
    const char** envp = (const char**)frame->rdx;

    // ── 1. Argüman doğrulama ──────────────────────────────────
    if (!path) { frame->rax = SYSCALL_ERR_FAULT; return; }

    // path kernel adresi olabilir (test senaryosu) veya user adresi;
    // sadece NULL-pointer ve erişilebilirlik kontrolü yapıyoruz.
    // Tam user-ptr doğrulamasını is_valid_user_ptr ile yap;
    // kernel test bağlamında path çekirdek adresinde olabilir.
    if ((uint64_t)path > 0x0000800000000000ULL) {
        // Açıkça geçersiz canonical adresi
        frame->rax = SYSCALL_ERR_FAULT;
        return;
    }

    serial_print("[EXECVE] path=\"");
    serial_print(path);
    serial_print("\"\n");

    // ── 2. Dosyayı oku ────────────────────────────────────────
    uint32_t file_size = 0;
    int      read_ok   = 0;

    // Önce in-memory VFS'yi dene
    const EmbeddedFile64* vfs_file = fs_get_file64(path);
    if (vfs_file && vfs_file->content && vfs_file->size > 0 &&
        vfs_file->size <= EXECVE_BUF_SIZE) {
        kmemcpy(execve_elf_buf, vfs_file->content, vfs_file->size);
        file_size = vfs_file->size;
        read_ok   = 1;
        serial_print("[EXECVE] loaded from VFS\n");
    }

    // VFS'de yoksa FAT32'yi dene
    if (!read_ok) {
        char fat83[16];
        path_to_fat83(path, fat83);

        uint32_t fsize = fat32_file_size(fat83);
        if (fsize > 0 && fsize <= EXECVE_BUF_SIZE) {
            int n = fat32_read_file(fat83, execve_elf_buf, fsize);
            if (n > 0) {
                file_size = (uint32_t)n;
                read_ok   = 1;
                serial_print("[EXECVE] loaded from FAT32: ");
                serial_print(fat83);
                serial_print("\n");
            }
        }
    }

    if (!read_ok || file_size == 0) {
        serial_print("[EXECVE] ENOENT: file not found\n");
        frame->rax = SYSCALL_ERR_NOENT;
        return;
    }

    // ── 3. ELF doğrula ───────────────────────────────────────
    int elf_rc = elf64_validate(execve_elf_buf, file_size);
    if (elf_rc != ELF_OK) {
        serial_print("[EXECVE] ELF validate error: ");
        serial_print(elf64_strerror(elf_rc));
        serial_print("\n");
        frame->rax = SYSCALL_ERR_INVAL;
        return;
    }

    // ── 4. Mevcut task'ı al ───────────────────────────────────
    task_t* cur = task_get_current();
    if (!cur) { frame->rax = SYSCALL_ERR_PERM; return; }

    // ── 5. ELF yükle ─────────────────────────────────────────
    // ET_DYN için load_base: 0x400000 (kullanıcı alanı varsayılan).
    // ET_EXEC için load_base 0 (elf64_load zaten ihmal eder).
    ElfImage img;
    uint64_t load_base = 0x400000ULL;
    elf_rc = elf64_load(execve_elf_buf, file_size, load_base, &img);
    if (elf_rc != ELF_OK) {
        serial_print("[EXECVE] ELF load error: ");
        serial_print(elf64_strerror(elf_rc));
        serial_print("\n");
        frame->rax = SYSCALL_ERR_NOMEM;
        return;
    }

    serial_print("[EXECVE] ELF loaded, entry=0x");
    print_hex64(img.entry);
    serial_print("\n");

    // ── 6. Yeni user stack ayır ───────────────────────────────
    // Eski user stack'i serbest bırak
    if (cur->user_stack_base) {
        kfree((void*)cur->user_stack_base);
        cur->user_stack_base = 0;
        cur->user_stack_top  = 0;
        cur->user_stack_size = 0;
    }

    uint64_t new_stack_size = USER_STACK_SIZE;
    void* new_stack = kmalloc(new_stack_size);
    if (!new_stack) {
        serial_print("[EXECVE] ENOMEM: stack alloc failed\n");
        frame->rax = SYSCALL_ERR_NOMEM;
        return;
    }
    kmemset(new_stack, 0, new_stack_size);

    cur->user_stack_base = (uint64_t)new_stack;
    cur->user_stack_size = new_stack_size;
    cur->user_stack_top  = cur->user_stack_base + new_stack_size;

    // ── 7. argv / envp → stack'e yerleştir ───────────────────
    // argv / envp user pointer olabilir veya kernel test'inde NULL.
    // NULL ise boş dizi kullan.
    const char* empty_argv0 = path;
    const char* argv_fallback[2] = { empty_argv0, (const char*)0 };
    const char* envp_fallback[1] = { (const char*)0 };

    const char** real_argv = argv;
    const char** real_envp = envp;

    // Geçersiz/kernel pointer → fallback
    if (!real_argv || !is_valid_user_ptr(real_argv, 8))
        real_argv = argv_fallback;
    if (!real_envp || !is_valid_user_ptr(real_envp, 8))
        real_envp = envp_fallback;

    int      new_argc;
    uint64_t new_argv_ptr;
    uint64_t new_envp_ptr;

    uint64_t new_sp = execve_build_stack(
        cur->user_stack_top,
        real_argv, real_envp,
        &new_argc, &new_argv_ptr, &new_envp_ptr);

    // ── 8. FD tablosu: O_CLOEXEC olan fd'leri kapat ──────────
    for (int i = 0; i < MAX_FDS; i++) {
        if (cur->fd_table[i].is_open &&
            (cur->fd_table[i].fd_flags & FD_CLOEXEC)) {
            fd_free(cur->fd_table, i);
            serial_print("[EXECVE] closed cloexec fd=");
            { char b[8]; int_to_str(i, b); serial_print(b); }
            serial_print("\n");
        }
    }

    // ── 9. Sinyal tablosunu sıfırla (POSIX) ──────────────────
    extern void signal_table_init(signal_table_t* st);
    signal_table_init(&cur->signal_table);

    // ── 10. Task ismini güncelle ───────────────────────────────
    // Son path bileşeni
    {
        const char* base = path;
        for (int i = 0; path[i]; i++)
            if (path[i] == '/') base = path + i + 1;
        my_strncpy(cur->name, base, 32);
    }

    // ── 11. CPU bağlamını yeni entry'e yönlendir ──────────────
    // Ring-3 SYSRET için:
    //   RIP      = ELF entry noktası          (frame->rcx)
    //   RSP      = yeni user stack            (frame->user_rsp)  ← DÜZELTİLDİ
    //   RDI      = argc                       (SysV ABI: ilk argüman)
    //   RSI      = argv*                      (ikinci argüman)
    //   RDX      = envp*                      (üçüncü argüman)
    //   CS/SS    = Ring-3 selectors           (SYSRET donanımsal set eder)
    //
    // DÜZELTME: Daha önce frame->user_rsp alanı yoktu; assembly stub
    // SYSRET öncesi RSP'yi orijinal user RSP'ye (execve'yi çağıran) geri
    // yüklüyordu. Yeni stack (new_sp) hiçbir zaman RSP olarak uygulanmıyor,
    // _start [rsp]'den argc okuyamıyordu → yığın bozulması → triple fault.
    //
    // Çözüm: frame->user_rsp != 0 ise assembly bu değeri RSP olarak kullanır.
    // Bkz. interrupts64.asm .user_syscall bölümü (.use_new_rsp etiketi).

    frame->rcx      = img.entry;          // RIP (SYSRET sonrası)
    frame->rax      = 0;                  // başarı
    frame->rdi      = (uint64_t)new_argc;
    frame->rsi      = new_argv_ptr;
    frame->rdx      = new_envp_ptr;
    frame->user_rsp = new_sp;             // ← DÜZELTME: yeni user stack RSP'si
                                           //   Assembly bunu görür ve
                                           //   orijinal user RSP yerine kullanır.

    // r11 (RFLAGS) güncelle: IF=1 (interrupt'lar açık)
    frame->r11 = 0x202;

    // context'i de güncelle (task switch sonrası geçerli olsun)
    cur->context.rip    = img.entry;
    cur->context.rsp    = new_sp;
    cur->context.rax    = 0;
    cur->context.rdi    = (uint64_t)new_argc;
    cur->context.rsi    = new_argv_ptr;
    cur->context.rdx    = new_envp_ptr;
    cur->context.cs     = GDT_USER_CODE_RPL3;
    cur->context.ss     = GDT_USER_DATA_RPL3;
    cur->context.rflags = 0x202;   // IF=1

    // Kernel stack'i sıfırla (eski bağlam geçersiz)
    // RSP0 (TSS) güncelle
    tss_set_kernel_stack(cur->kernel_stack_top);

    serial_print("[EXECVE] success: jumping to entry 0x");
    print_hex64(img.entry);
    serial_print(" new_rsp=0x");
    print_hex64(new_sp);
    serial_print("\n");

    // frame->rax = 0 → SYSRET ile entry'ye atlayacak.
    // execve başarıda "dönmez"; SYSRET yeni koda geçer.
    // frame->rcx = RIP, frame->user_rsp = RSP (assembly kullanır)
    frame->rax = SYSCALL_OK;
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
// v11 YENİ SYSCALL IMPLEMENTASYONLARI — fcntl + dup
// ============================================================

// ── SYS_DUP (67) ────────────────────────────────────────────────
static void sys_dup(syscall_frame_t* frame) {
    int oldfd = (int)frame->rdi;
    if (oldfd < 0 || oldfd >= MAX_FDS) { frame->rax = SYSCALL_ERR_BADF; return; }
    task_t* cur = task_get_current();
    if (!cur) { frame->rax = SYSCALL_ERR_PERM; return; }
    fd_entry_t* src = fd_get(cur->fd_table, oldfd);
    if (!src) { frame->rax = SYSCALL_ERR_BADF; return; }
    // En kucuk bos fd bul
    int newfd = -1;
    for (int i = 0; i < MAX_FDS; i++) {
        if (!cur->fd_table[i].is_open) { newfd = i; break; }
    }
    if (newfd < 0) { frame->rax = SYSCALL_ERR_MFILE; return; }
    cur->fd_table[newfd] = *src;
    cur->fd_table[newfd].fd_flags = 0;  // dup() FD_CLOEXEC temizler
    if (src->type == FD_TYPE_PIPE && src->pipe) src->pipe->ref_count++;
    serial_print("[SYSCALL] dup fd=");
    { char b[8]; int_to_str(oldfd, b); serial_print(b); }
    serial_print(" -> newfd=");
    { char b[8]; int_to_str(newfd, b); serial_print(b); }
    serial_print("\n");
    frame->rax = (uint64_t)newfd;
}

// ── SYS_FCNTL (66) ──────────────────────────────────────────────
static void sys_fcntl(syscall_frame_t* frame) {
    int      fd  = (int)frame->rdi;
    int      cmd = (int)frame->rsi;
    uint64_t arg = frame->rdx;
    if (fd < 0 || fd >= MAX_FDS) { frame->rax = SYSCALL_ERR_BADF; return; }
    task_t* cur = task_get_current();
    if (!cur) { frame->rax = SYSCALL_ERR_PERM; return; }
    fd_entry_t* ent = fd_get(cur->fd_table, fd);
    if (!ent) { frame->rax = SYSCALL_ERR_BADF; return; }

    switch (cmd) {
    case F_DUPFD:
    case F_DUPFD_CLOEXEC: {
        int start = (int)arg;
        if (start < 0) start = 0;
        if (start >= MAX_FDS) { frame->rax = SYSCALL_ERR_INVAL; return; }
        int newfd = -1;
        for (int i = start; i < MAX_FDS; i++) {
            if (!cur->fd_table[i].is_open) { newfd = i; break; }
        }
        if (newfd < 0) { frame->rax = SYSCALL_ERR_MFILE; return; }
        cur->fd_table[newfd] = *ent;
        cur->fd_table[newfd].fd_flags = (cmd == F_DUPFD_CLOEXEC) ? FD_CLOEXEC : 0;
        if (ent->type == FD_TYPE_PIPE && ent->pipe) ent->pipe->ref_count++;
        serial_print("[SYSCALL] fcntl F_DUPFD -> ");
        { char b[8]; int_to_str(newfd, b); serial_print(b); }
        serial_print("\n");
        frame->rax = (uint64_t)newfd;
        break;
    }
    case F_GETFD:
        frame->rax = (uint64_t)ent->fd_flags;
        break;
    case F_SETFD:
        ent->fd_flags = (uint8_t)(arg & 0xFF);
        frame->rax = SYSCALL_OK;
        break;
    case F_GETFL:
        frame->rax = (uint64_t)ent->flags;
        break;
    case F_SETFL: {
        uint8_t access = ent->flags & 0x03;
        ent->flags = access | ((uint8_t)(arg & 0xFF) & ~0x03);
        frame->rax = SYSCALL_OK;
        break;
    }
    default:
        serial_print("[SYSCALL] fcntl: unknown cmd\n");
        frame->rax = SYSCALL_ERR_INVAL;
        break;
    }
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

// ── pg_* forward declarations ────────────────────────────────────
// Tanımlar dosyanın ilerisinde (v12 bölümü); sys_ioctl için önceden bildirilir.
static uint32_t pg_current_pid(void);
static uint32_t pg_effective_pgid(uint32_t pid);
static uint32_t pg_effective_sid(uint32_t pid);

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
static uint32_t kernel_tty_pgrp = 1;

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
        if (!is_valid_user_ptr(arg, sizeof(uint32_t))) {
            frame->rax = SYSCALL_ERR_FAULT;
            return;
        }
        *((uint32_t*)arg) = kernel_tty_pgrp;
        frame->rax = SYSCALL_OK;
        break;

    // ── TIOCSPGRP: ön plan süreç grubunu ayarla ──────────────────
    case TIOCSPGRP:
        if (!is_valid_user_ptr(arg, sizeof(uint32_t))) {
            frame->rax = SYSCALL_ERR_FAULT;
            return;
        }
        kernel_tty_pgrp = *((const uint32_t*)arg);
        frame->rax = SYSCALL_OK;
        break;

    // ── TIOCSCTTY: controlling terminal olarak ata ────────────────
    // Bash setsid() sonrası bunu çağırır; setsid yapılmamışsa EPERM.
    // Başka process zaten bu terminal'i kontrol ediyorsa EBUSY.
    case TIOCSCTTY: {
        task_t* cur = task_get_current();
        uint32_t cur_pid = cur ? cur->pid : 1;

        // Session leader değilsek EPERM
        if (pg_effective_pgid(cur_pid) != cur_pid) {
            serial_print("[SYSCALL] ioctl TIOCSCTTY: not session leader -> EPERM\n");
            frame->rax = SYSCALL_ERR_PERM;
            return;
        }

        // Terminali bu session'a bağla (kernel_tty_pgrp güncelle)
        kernel_tty_pgrp = cur_pid;
        serial_print("[SYSCALL] ioctl TIOCSCTTY ok\n");
        frame->rax = SYSCALL_OK;
        break;
    }

    // ── TIOCNOTTY: controlling terminal'den ayrıl ─────────────────
    // Session leader olmayanlar için EINVAL döner.
    case TIOCNOTTY: {
        task_t* cur = task_get_current();
        uint32_t cur_pid = cur ? cur->pid : 1;

        // Controlling terminal'e sahip değilsek EINVAL
        if (kernel_tty_pgrp != pg_effective_pgid(cur_pid)) {
            serial_print("[SYSCALL] ioctl TIOCNOTTY: not controlling terminal -> EINVAL\n");
            frame->rax = SYSCALL_ERR_INVAL;
            return;
        }

        // Bağı kopar
        kernel_tty_pgrp = 0;
        serial_print("[SYSCALL] ioctl TIOCNOTTY ok\n");
        frame->rax = SYSCALL_OK;
        break;
    }

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
    int      nonblocking  = 0;   // timeout={0,0} → tek geçiş, hemen dön
    if (timeout) {
        if (timeout->tv_sec == 0 && timeout->tv_usec == 0) {
            nonblocking = 1;
        } else {
            uint64_t ms = (uint64_t)timeout->tv_sec * 1000ULL
                        + (uint64_t)timeout->tv_usec / 1000ULL;
            deadline = get_system_ticks() + ms;
            has_deadline = 1;
        }
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
        if (nonblocking) break;             // timeout={0,0}: tek geçiş
        if (!has_deadline) break;           // timeout=NULL: sonsuz bekle → tek geçiş (stub)
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

    // timeout_ms=0  → non-blocking: tek geçiş, hemen dön
    // timeout_ms>0  → deadline'a kadar bekle
    // timeout_ms<0  → sonsuz bekle (stub: tek geçiş)
    int      nonblocking  = (timeout_ms == 0);
    uint64_t deadline     = get_system_ticks() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 0);
    int      has_deadline = (timeout_ms > 0);

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
        if (nonblocking) break;             // timeout=0: tek geçiş
        if (!has_deadline) break;           // timeout<0: stub, tek geçiş
        if (get_system_ticks() >= deadline) break;
        scheduler_yield();
    } while (1);

    frame->rax = (uint64_t)(int64_t)nready;
}


// ── SYS_KILL (29) ───────────────────────────────────────────────
// kill(pid, sig) -> 0 | SYSCALL_ERR_*
//
// v10: signal64.h altyapısına bağlandı. Artık signal_send() üzerinden
// gerçek sinyal dağıtımı yapılıyor; pending bit set edilip
// signal_dispatch_pending() tetikleniyor.
//
//   pid  > 0 : o PID'e sinyal gönder
//   pid == 0 : mevcut task'ın grubuna (stub: mevcut task)
//   pid ==-1 : tüm task'lara (stub: sadece mevcut task)
//   pid  < -1: grup -pid'e (stub: şimdilik ENOSYS)
//   sig == 0 : process varlık kontrolü (sinyal gönderilmez)
// ---------------------------------------------------------------
static void sys_kill(syscall_frame_t* frame) {
    int pid = (int)(int64_t)frame->rdi;
    int sig = (int)(int64_t)frame->rsi;

    serial_print("[SYSCALL] kill(pid=");
    { char b[12]; int_to_str(pid, b); serial_print(b); }
    serial_print(", sig=");
    { char b[12]; int_to_str(sig, b); serial_print(b); }
    serial_print(")\n");

    // Geçersiz sinyal numarası
    if (sig < 0 || sig > NSIG) {
        frame->rax = SYSCALL_ERR_INVAL;
        return;
    }

    // sig == 0: process varlık kontrolü, sinyal gönderme
    if (sig == 0) {
        if (pid <= 0) {
            frame->rax = SYSCALL_OK;
            return;
        }
        task_t* t = task_find_by_pid((uint32_t)pid);
        frame->rax = t ? SYSCALL_OK : SYSCALL_ERR_INVAL;
        return;
    }

    // pid < -1: grup sinyali (stub)
    if (pid < -1) {
        serial_print("[SYSCALL] kill: grup sinyali desteklenmiyor\n");
        frame->rax = SYSCALL_ERR_NOSYS;
        return;
    }

    // pid == -1: broadcast desteklenmez -> ENOSYS
    if (pid == -1) {
        serial_print("[SYSCALL] kill: pid=-1 broadcast desteklenmiyor\n");
        frame->rax = SYSCALL_ERR_NOSYS;
        return;
    }

    // pid == 0: kendine sinyal gonder
    if (pid == 0) {
        task_t* cur = task_get_current();
        if (!cur) { frame->rax = SYSCALL_ERR_PERM; return; }
        signal_table_t* st = task_get_signal_table(cur);
        if (st) st->pending_sigs |= SIG_BIT(sig);
        frame->rax = SYSCALL_OK;
        return;
    }

    // pid > 0: hedef task'a signal_send ile gönder
    int ret = signal_send(pid, sig);
    frame->rax = (ret == 0) ? SYSCALL_OK : SYSCALL_ERR_INVAL;
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

// ── SYS_GETCWD (43) ─────────────────────────────────────────────
// getcwd(char* buf, uint64_t size) -> buf (kullanıcı ptr) | 0 (hata)
//
// Bash'in çalışma dizinini öğrenmek için kullandığı temel çağrı.
// Kernel tarafındaki current_dir string'ini kullanıcı buffer'ına kopyalar.
//
// POSIX davranışı:
//   • buf NULL ise ya da size yetmezse EINVAL/ERANGE döner.
//   • Başarıda RAX = buf adresi (glibc bunu char* olarak kullanır).
//   • Başarısızda RAX = 0 (NULL) ve errno ERANGE/EINVAL olur.
// ---------------------------------------------------------------
static void sys_getcwd(syscall_frame_t* frame) {
    char*    buf  = (char*)frame->rdi;
    uint64_t size = frame->rsi;

    // Argüman doğrulama
    if (!buf || size == 0) {
        serial_print("[SYSCALL] getcwd: NULL buf veya size=0 (EINVAL)\n");
        frame->rax = 0;
        return;
    }

    if (!is_valid_user_ptr(buf, size)) {
        serial_print("[SYSCALL] getcwd: geçersiz kullanıcı pointer (EFAULT)\n");
        frame->rax = 0;
        return;
    }

    // Kernel'deki cwd'yi al; VFS henüz başlamamışsa "/" varsayılan
    const char* cwd = fs_getcwd64();
    if (!cwd) cwd = "/";

    // Uzunluk kontrolü: null terminator dahil
    uint64_t cwd_len = 0;
    while (cwd[cwd_len]) cwd_len++;
    cwd_len++;  // '\0'

    if (cwd_len > size) {
        serial_print("[SYSCALL] getcwd: buffer çok küçük (ERANGE)\n");
        frame->rax = 0;
        return;
    }

    // Kullanıcı buffer'ına kopyala
    for (uint64_t i = 0; i < cwd_len; i++)
        buf[i] = cwd[i];

    serial_print("[SYSCALL] getcwd -> \"");
    serial_print(buf);
    serial_print("\"\n");

    // POSIX: başarıda buf adresini döndür (glibc bunu char* olarak cast eder)
    frame->rax = (uint64_t)buf;
}

// ── SYS_CHDIR (44) ──────────────────────────────────────────────
// chdir(const char* path) -> 0 | err
//
// Bash'in `cd` builtin'i bu syscall'ı doğrudan çağırır.
// Ayrıca execve öncesi çalışma dizini ayarlamak için kullanılır.
//
// POSIX davranışı:
//   • Dizin yoksa ENOENT döner.
//   • path NULL ise EINVAL döner.
//   • Başarıda 0 (SYSCALL_OK) döner.
// ---------------------------------------------------------------
static void sys_chdir(syscall_frame_t* frame) {
    const char* path = (const char*)frame->rdi;

    if (!path) {
        serial_print("[SYSCALL] chdir: NULL path (EINVAL)\n");
        frame->rax = SYSCALL_ERR_INVAL;
        return;
    }

    if (!is_valid_user_ptr(path, 1)) {
        serial_print("[SYSCALL] chdir: geçersiz kullanıcı pointer (EFAULT)\n");
        frame->rax = SYSCALL_ERR_FAULT;
        return;
    }

    serial_print("[SYSCALL] chdir(\"");
    serial_print(path);
    serial_print("\")\n");

    // fs_chdir64: 1 = başarı, 0 = dizin bulunamadı
    int ok = fs_chdir64(path);
    if (!ok) {
        serial_print("[SYSCALL] chdir: dizin bulunamadı (ENOENT)\n");
        frame->rax = SYSCALL_ERR_NOENT;
        return;
    }

    serial_print("[SYSCALL] chdir: ok, yeni cwd=\"");
    serial_print(fs_getcwd64());
    serial_print("\"\n");

    frame->rax = SYSCALL_OK;
}

// ── SYS_STAT (31) ────────────────────────────────────────────────
// stat(const char* path, stat_t* buf) -> 0 | err
//
// Bash her komut öncesi executable'ları, dosyaları ve dizinleri
// bu syscall ile sorgular. fstat'tan farkı: fd yerine path alır.
//
// Davranış:
//   • path bir dosyaysa  → S_IFREG, st_size = dosya boyutu
//   • path bir dizinse   → S_IFDIR, st_size = 0
//   • path yoksa         → ENOENT
//   • Tüm dosyalar root sahibi (uid=0, gid=0), rw-r--r-- (0644)
//   • Dizinler rwxr-xr-x (0755)
//   • Zaman alanları mevcut tick'ten hesaplanır (boot-relative)
// ---------------------------------------------------------------
static void sys_stat(syscall_frame_t* frame) {
    const char* path  = (const char*)frame->rdi;
    stat_t*     buf   = (stat_t*)frame->rsi;

    if (!path) {
        frame->rax = SYSCALL_ERR_INVAL;
        return;
    }
    if (!is_valid_user_ptr(path, 1)) {
        frame->rax = SYSCALL_ERR_FAULT;
        return;
    }
    if (!buf || !is_valid_user_ptr(buf, sizeof(stat_t))) {
        frame->rax = SYSCALL_ERR_FAULT;
        return;
    }

    serial_print("[SYSCALL] stat(\"");
    serial_print(path);
    serial_print("\")\n");

    // stat_t'yi sıfırla
    kmemset(buf, 0, sizeof(stat_t));

    uint32_t now = (uint32_t)get_system_ticks();

    int is_dir  = fs_path_is_dir(path);
    int is_file = (!is_dir) && fs_path_is_file(path);

    if (!is_dir && !is_file) {
        serial_print("[SYSCALL] stat: ENOENT\n");
        frame->rax = SYSCALL_ERR_NOENT;
        return;
    }

    buf->st_dev     = 1;
    buf->st_ino     = 1;          // inode stub
    buf->st_nlink   = 1;
    buf->st_uid     = 0;          // root
    buf->st_gid     = 0;
    buf->st_rdev    = 0;
    buf->st_blksize = 512;
    buf->st_atime   = now;
    buf->st_mtime   = now;
    buf->st_ctime   = now;

    if (is_dir) {
        buf->st_mode   = S_IFDIR | 0755;
        buf->st_size   = 0;
        buf->st_blocks = 0;
    } else {
        uint32_t fsz   = fs_path_filesize(path);
        buf->st_mode   = S_IFREG | 0644;
        buf->st_size   = fsz;
        buf->st_blocks = (fsz + 511) / 512;
    }

    serial_print("[SYSCALL] stat: ok, mode=0x");
    { char b[12]; int_to_str((int)buf->st_mode, b); serial_print(b); }
    serial_print("\n");

    frame->rax = SYSCALL_OK;
}

// ── SYS_ACCESS (42) ──────────────────────────────────────────────
// access(const char* path, int mode) -> 0 | err
//
// Bash PATH taramasında ve `command -v`, shebang kontrolünde kullanır.
// mode bitleri: F_OK(0)=var mı, R_OK(4)=okuma, W_OK(2)=yazma, X_OK(1)=çalıştırma
//
// Davranış:
//   • F_OK : dosya veya dizin varsa 0, yoksa ENOENT
//   • R_OK : tüm dosya/dizinler okunabilir → her zaman 0 (kernel root)
//   • W_OK : dinamik (yazılabilir) dosyalar için 0, statik için EACCES
//   • X_OK : /bin altındaki dosyalar ve dizinler çalıştırılabilir → 0
//            diğer dosyalar için EACCES
// ---------------------------------------------------------------
static void sys_access(syscall_frame_t* frame) {
    const char* path = (const char*)frame->rdi;
    int         mode = (int)(int64_t)frame->rsi;

    if (!path) {
        frame->rax = SYSCALL_ERR_INVAL;
        return;
    }
    if (!is_valid_user_ptr(path, 1)) {
        frame->rax = SYSCALL_ERR_FAULT;
        return;
    }

    serial_print("[SYSCALL] access(\"");
    serial_print(path);
    serial_print("\")\n");

    int is_dir  = fs_path_is_dir(path);
    int is_file = (!is_dir) && fs_path_is_file(path);

    // F_OK: sadece varlık kontrolü
    if (!(mode & (R_OK | W_OK | X_OK))) {
        frame->rax = (is_dir || is_file) ? SYSCALL_OK : SYSCALL_ERR_NOENT;
        return;
    }

    if (!is_dir && !is_file) {
        serial_print("[SYSCALL] access: ENOENT\n");
        frame->rax = SYSCALL_ERR_NOENT;
        return;
    }

    // R_OK: her zaman izin var (kernel root gibi davranıyoruz)
    if (mode & R_OK) {
        // izin verildi
    }

    // W_OK: dizinler ve dinamik dosyalar yazılabilir
    if (mode & W_OK) {
        if (is_file) {
            const EmbeddedFile64* f = fs_get_file64(path);
            if (f && !f->is_dynamic) {
                // Salt-okunur gömülü dosya
                serial_print("[SYSCALL] access: W_OK EACCES (read-only file)\n");
                frame->rax = SYSCALL_ERR_PERM;
                return;
            }
        }
        // Dizinler ve dinamik dosyalar yazılabilir
    }

    // X_OK: dizinler her zaman çalıştırılabilir
    //       dosyalar: /bin veya /usr/bin altındakilere izin ver
    if (mode & X_OK) {
        if (is_dir) {
            // dizine execute = traverse izni → her zaman ok
        } else {
            // /bin/ veya /usr/bin/ prefix'i var mı?
            int exec_ok = 0;
            // Basit prefix kontrolü
            const char* p = path;
            // "/bin/" ile başlıyor mu?
            if (p[0]=='/' && p[1]=='b' && p[2]=='i' && p[3]=='n' && p[4]=='/') exec_ok = 1;
            // "/usr/bin/" ile başlıyor mu?
            if (p[0]=='/' && p[1]=='u' && p[2]=='s' && p[3]=='r' &&
                p[4]=='/' && p[5]=='b' && p[6]=='i' && p[7]=='n' && p[8]=='/') exec_ok = 1;
            if (!exec_ok) {
                serial_print("[SYSCALL] access: X_OK EACCES\n");
                frame->rax = SYSCALL_ERR_PERM;
                return;
            }
        }
    }

    serial_print("[SYSCALL] access: ok\n");
    frame->rax = SYSCALL_OK;
}

// ================================================================
// v9 – SYS_OPENDIR / SYS_GETDENTS / SYS_CLOSEDIR
//
// Bash glob expansion (*.c, for f in *) ve ls için şart.
// Tasarım: opendir() bir "dir fd" döndürür. Bu fd, FD_TYPE_SPECIAL
// olarak açılır ve path bilgisini fd_entry_t.path'te saklar.
// getdents() o fd üzerinden fs_getdents64() çağırır, offset takip eder.
// closedir() sadece fd_free() yapar.
// ================================================================

// ── SYS_OPENDIR (59) ────────────────────────────────────────────
// opendir(const char* path) -> dirfd | err
//
// POSIX opendir(3) yerine kullanılan raw syscall versiyonu.
// Başarıda dizine bağlı bir fd döndürür; getdents ile okunur.
// ---------------------------------------------------------------
static void sys_opendir(syscall_frame_t* frame) {
    const char* path = (const char*)frame->rdi;

    if (!path) {
        frame->rax = SYSCALL_ERR_INVAL;
        return;
    }
    if (!is_valid_user_ptr(path, 1)) {
        frame->rax = SYSCALL_ERR_FAULT;
        return;
    }

    serial_print("[SYSCALL] opendir(\"");
    serial_print(path);
    serial_print("\")\n");

    if (!fs_path_is_dir(path)) {
        frame->rax = SYSCALL_ERR_NOENT;
        return;
    }

    task_t* cur = task_get_current();
    if (!cur) {
        frame->rax = SYSCALL_ERR_PERM;
        return;
    }

    int fd = fd_alloc(cur->fd_table, FD_TYPE_SPECIAL, O_RDONLY, path);
    if (fd < 0) {
        frame->rax = SYSCALL_ERR_MFILE;
        return;
    }

    // offset = 0: getdents henüz hiçbir şey okumadı
    cur->fd_table[fd].offset = 0;

    serial_print("[SYSCALL] opendir: dirfd=");
    { char b[8]; int_to_str(fd, b); serial_print(b); }
    serial_print("\n");

    frame->rax = (uint64_t)fd;
}

// ── SYS_GETDENTS (58) ────────────────────────────────────────────
// getdents(int dirfd, dirent64_t* buf, int count) -> nbytes | err
//
// dirfd: opendir() ile alınan fd
// buf  : kullanıcı buffer'ı
// count: buffer boyutu (byte)
//
// Döndürür: yazılan byte sayısı (0 = dizin sonu), negatif = hata
//
// Basit implementasyon: tüm entry'leri ilk çağrıda döker.
// İkinci çağrıda offset != 0 kontrolü ile 0 (EOF) döner.
// ---------------------------------------------------------------
static void sys_getdents(syscall_frame_t* frame) {
    int         dirfd = (int)(int64_t)frame->rdi;
    dirent64_t* buf   = (dirent64_t*)frame->rsi;
    int         count = (int)(int64_t)frame->rdx;

    if (!buf || count <= 0) {
        frame->rax = SYSCALL_ERR_INVAL;
        return;
    }
    if (!is_valid_user_ptr(buf, (uint64_t)count)) {
        frame->rax = SYSCALL_ERR_FAULT;
        return;
    }

    task_t* cur = task_get_current();
    if (!cur) {
        frame->rax = SYSCALL_ERR_PERM;
        return;
    }

    fd_entry_t* ent = fd_get(cur->fd_table, dirfd);
    if (!ent || ent->type != FD_TYPE_SPECIAL) {
        frame->rax = SYSCALL_ERR_BADF;
        return;
    }

    // EOF kontrolü: offset > 0 → daha önce okundu, dizin sonu
    if (ent->offset > 0) {
        frame->rax = 0;   // 0 = no more entries
        return;
    }

    serial_print("[SYSCALL] getdents(dirfd=");
    { char b[8]; int_to_str(dirfd, b); serial_print(b); }
    serial_print(", path=\"");
    serial_print(ent->path);
    serial_print("\")\n");

    int written = fs_getdents64(ent->path, buf, count);

    if (written < 0) {
        frame->rax = SYSCALL_ERR_NOENT;
        return;
    }

    // offset > 0 olarak işaretle → bir sonraki çağrı EOF döner
    ent->offset = (uint64_t)(written > 0 ? written : 1);

    serial_print("[SYSCALL] getdents: wrote ");
    { char b[12]; int_to_str(written, b); serial_print(b); }
    serial_print(" bytes\n");

    frame->rax = (uint64_t)written;
}

// ── SYS_CLOSEDIR (60) ────────────────────────────────────────────
// closedir(int dirfd) -> 0 | err
// ---------------------------------------------------------------
static void sys_closedir(syscall_frame_t* frame) {
    int dirfd = (int)(int64_t)frame->rdi;

    task_t* cur = task_get_current();
    if (!cur) {
        frame->rax = SYSCALL_ERR_PERM;
        return;
    }

    fd_entry_t* ent = fd_get(cur->fd_table, dirfd);
    if (!ent || ent->type != FD_TYPE_SPECIAL) {
        frame->rax = SYSCALL_ERR_BADF;
        return;
    }

    fd_free(cur->fd_table, dirfd);

    serial_print("[SYSCALL] closedir: ok\n");
    frame->rax = SYSCALL_OK;
}

// ============================================================
// v12 – Process Group & Session Syscall'ları
//
// Bu dört syscall bash'in iş kontrolü (job control) altyapısını
// tamamlar:
//   setpgid  – pipeline grubu oluştur / gruba katıl
//   getpgid  – herhangi bir process'in grup ID'sini sorgula
//   setsid   – yeni session başlat (login shell)
//   tcsetpgrp – terminale foreground process grubunu ayarla
//   tcgetpgrp – terminaldeki foreground process grubunu sorgula
// ============================================================

// ── SYS_SETPGID (68) ─────────────────────────────────────────────
// setpgid(pid_t pid, pid_t pgid) -> 0 | err
//
// pid  = 0 → çağıran task kullanılır
// pgid = 0 → pgid = pid yapılır (yeni grup lideri)
// Kısıtlamalar:
//   • Session liderinin (pid == sid) grubu değiştirilemez → EPERM
//   • Hedef task çağıranın session'ında olmalı           → EPERM
//   • pgid, aynı session'daki mevcut bir gruba ait olmalı
//     ya da pgid == pid (yeni grup) olmalı               → EPERM
// ---------------------------------------------------------------
// ============================================================
// v13 – Process Group & Session: PID-indexed bağımsız tablo
//
// task_t struct'ına DOKUNMUYORUZ. pgid ve sid değerleri burada
// tutulur; böylece task.h layout değişikliğinden etkilenmeyiz.
// Tüm erişim getpid() syscall sonucundan elde edilen PID üzerinden.
//
// MAX_TASKS: task.c'deki maksimum task sayısıyla uyumlu olmalı.
// Pratikte küçük bir kernel için 64 slot fazlasıyla yeterli.
// ============================================================
#define PG_MAX_TASKS  64
// Bos slot isareti: pid hicbir zaman bu degeri almaz (uint32 max)
#define PG_SLOT_EMPTY 0xFFFFFFFFu

typedef struct {
    uint32_t pid;    // PG_SLOT_EMPTY = bos slot; pid=0 gecerlidir
    uint32_t pgid;
    uint32_t sid;
} pg_entry_t;

static pg_entry_t g_pg_table[PG_MAX_TASKS];
static int        g_pg_inited = 0;

static void pg_ensure_init(void) {
    if (g_pg_inited) return;
    for (int i = 0; i < PG_MAX_TASKS; i++)
        g_pg_table[i].pid = PG_SLOT_EMPTY;
    g_pg_inited = 1;
}

// pid icin slot bul; yoksa yeni slot ac.
// pid=0 artik gecerlidir (test/idle task olabilir).
static pg_entry_t* pg_get_or_create(uint32_t pid) {
    pg_ensure_init();
    for (int i = 0; i < PG_MAX_TASKS; i++) {
        if (g_pg_table[i].pid == pid)
            return &g_pg_table[i];
    }
    for (int i = 0; i < PG_MAX_TASKS; i++) {
        if (g_pg_table[i].pid == PG_SLOT_EMPTY) {
            g_pg_table[i].pid  = pid;
            g_pg_table[i].pgid = pid;
            g_pg_table[i].sid  = pid;
            return &g_pg_table[i];
        }
    }
    return (void*)0;
}

// Sadece okuma; slot yoksa NULL doner
static pg_entry_t* pg_find(uint32_t pid) {
    pg_ensure_init();
    for (int i = 0; i < PG_MAX_TASKS; i++) {
        if (g_pg_table[i].pid == pid)
            return &g_pg_table[i];
    }
    return (void*)0;
}

// Virtual PID: task->pid=0 oldugunda kullanilir.
// Syscall context'te task_get_current() NULL donebilir;
// bu durumda g_virtual_pid onceki syscall'da set edilmis olmali.
static uint32_t g_virtual_pid = 0;

// sys_getpid ve pg_current_pid tarafindan cagirilir; her gecerli pid
// geldikten sonra g_virtual_pid'i tazeler.
static void pg_update_current_pid(uint32_t pid) {
    if (pid != 0) g_virtual_pid = pid;
}

static uint32_t pg_resolve_pid(uint32_t raw_pid) {
    if (raw_pid != 0) {
        // Gecerli pid bulundu — g_virtual_pid'i guncelle (gelecek NULL durumu icin)
        g_virtual_pid = raw_pid;
        return raw_pid;
    }
    // raw_pid=0: onceki syscall'dan kalan gecerli pid'i kullan
    if (g_virtual_pid != 0) return g_virtual_pid;
    // Son care: scheduler listesinden ilk gecerli pid
    for (uint32_t try_pid = 1; try_pid < 256; try_pid++) {
        task_t* t = task_find_by_pid(try_pid);
        if (t) { g_virtual_pid = try_pid; return try_pid; }
    }
    g_virtual_pid = 1;
    return 1;
}

static uint32_t pg_current_pid(void) {
    task_t* cur = task_get_current();
    uint32_t raw = cur ? cur->pid : 0u;
    if (raw != 0) pg_update_current_pid(raw);
    return pg_resolve_pid(raw);
}

// Verilen pid'in pgid'ini dondur (slot yoksa pid'den turet)
static uint32_t pg_effective_pgid(uint32_t pid) {
    pg_entry_t* e = pg_find(pid);
    return e ? e->pgid : pid;
}

// Verilen pid'in sid'ini dondur (slot yoksa pid'den turet)
static uint32_t pg_effective_sid(uint32_t pid) {
    pg_entry_t* e = pg_find(pid);
    return e ? e->sid : pid;
}

// ── SYS_SETPGID (68) ─────────────────────────────────────────────
// setpgid(pid_t pid, pid_t pgid) -> 0 | err
// ---------------------------------------------------------------
static void sys_setpgid(syscall_frame_t* frame) {
    int arg_pid  = (int)(int64_t)frame->rdi;
    int arg_pgid = (int)(int64_t)frame->rsi;

    uint32_t caller_pid = pg_current_pid();

    // pgid negatifse hemen EINVAL
    if (arg_pgid < 0) { frame->rax = SYSCALL_ERR_INVAL; return; }

    // pid == 0 → çağıranın kendisi
    uint32_t target_pid = (arg_pid == 0) ? caller_pid : (uint32_t)arg_pid;

    // Hedef task var mı?
    if (target_pid != caller_pid) {
        task_t* t = task_find_by_pid(target_pid);
        if (!t) { frame->rax = SYSCALL_ERR_INVAL; return; }
    }

    // pgid == 0 → pgid = target'ın pid'i (yeni grup lideri)
    uint32_t new_pgid = (arg_pgid == 0) ? target_pid : (uint32_t)arg_pgid;

    // Session kontrolü
    uint32_t caller_sid = pg_effective_sid(caller_pid);
    uint32_t target_sid = pg_effective_sid(target_pid);
    if (target_sid != caller_sid) {
        frame->rax = SYSCALL_ERR_PERM;
        return;
    }

    // new_pgid ya target_pid (yeni lider) ya da aynı session'daki mevcut grup
    if (new_pgid != target_pid) {
        // Aynı session'da bu pgid'e sahip bir task var mı?
        int found = 0;
        for (int i = 0; i < PG_MAX_TASKS; i++) {
            if (g_pg_table[i].pid != PG_SLOT_EMPTY &&
                g_pg_table[i].pgid == new_pgid &&
                g_pg_table[i].sid  == caller_sid) {
                found = 1; break;
            }
        }
        // Ayrıca task_find_by_pid ile kontrol (tabloya henüz eklenmediyse)
        if (!found) {
            task_t* pg = task_find_by_pid(new_pgid);
            if (pg && pg_effective_sid(pg->pid) == caller_sid) found = 1;
        }
        if (!found) { frame->rax = SYSCALL_ERR_PERM; return; }
    }

    pg_entry_t* ent = pg_get_or_create(target_pid);
    if (!ent) { frame->rax = SYSCALL_ERR_NOMEM; return; }
    ent->pgid = new_pgid;

    // task_t'ye de yaz (varsa ve alan mevcutsa, olmasa da zarar yok)
    task_t* t = task_find_by_pid(target_pid);
    if (t) t->pgid = new_pgid;

    serial_print("[SYSCALL] setpgid: pid=");
    { char b[12]; int_to_str((int)target_pid, b); serial_print(b); }
    serial_print(" -> pgid=");
    { char b[12]; int_to_str((int)new_pgid, b); serial_print(b); }
    serial_print("\n");

    frame->rax = SYSCALL_OK;
}

// ── SYS_GETPGID (69) ─────────────────────────────────────────────
// getpgid(pid_t pid) -> pgid | err
// ---------------------------------------------------------------
static void sys_getpgid(syscall_frame_t* frame) {
    int arg_pid = (int)(int64_t)frame->rdi;

    uint32_t caller_pid = pg_current_pid();

    uint32_t target_pid = (arg_pid == 0) ? caller_pid : (uint32_t)arg_pid;

    // Hedef var mı? (kendi pid'i ise task aramasına gerek yok)
    if (target_pid != caller_pid) {
        task_t* t = task_find_by_pid(target_pid);
        if (!t) { frame->rax = SYSCALL_ERR_INVAL; return; }
    }

    // Farklı session → EPERM
    if (pg_effective_sid(target_pid) != pg_effective_sid(caller_pid)) {
        frame->rax = SYSCALL_ERR_PERM;
        return;
    }

    frame->rax = (uint64_t)pg_effective_pgid(target_pid);
}

// ── SYS_SETSID (70) ──────────────────────────────────────────────
// setsid() -> new_sid | err
// ---------------------------------------------------------------
static void sys_setsid(syscall_frame_t* frame) {
    uint32_t cur_pid = pg_current_pid();

    // Zaten grup lideri ise EPERM
    if (pg_effective_pgid(cur_pid) == cur_pid) {
        frame->rax = SYSCALL_ERR_PERM;
        return;
    }

    pg_entry_t* ent = pg_get_or_create(cur_pid);
    if (!ent) { frame->rax = SYSCALL_ERR_NOMEM; return; }
    ent->pgid = cur_pid;
    ent->sid  = cur_pid;

    // task_t'ye de yaz
    task_t* t = task_find_by_pid(cur_pid);
    if (t) { t->pgid = cur_pid; t->sid = cur_pid; }

    serial_print("[SYSCALL] setsid: new sid=");
    { char b[12]; int_to_str((int)cur_pid, b); serial_print(b); }
    serial_print("\n");

    frame->rax = (uint64_t)cur_pid;
}

// ── SYS_TCSETPGRP (71) / SYS_TCGETPGRP (72) ─────────────────────
// Tek terminal için global foreground pgid.
// 0 = henüz ayarlanmamış.
static uint32_t g_terminal_foreground_pgid = 0;

static void sys_tcsetpgrp(syscall_frame_t* frame) {
    int fd   = (int)(int64_t)frame->rdi;
    int pgrp = (int)(int64_t)frame->rsi;

    // fd doğrulama — 0,1,2 her zaman terminal; diğerleri fd tablosundan kontrol
    if (fd < 0 || fd >= MAX_FDS) {
        frame->rax = SYSCALL_ERR_BADF;
        return;
    }
    if (fd > 2) {
        task_t* cur = task_get_current();
        fd_entry_t* ent = cur ? fd_get(cur->fd_table, fd) : (void*)0;
        if (!ent || !ent->is_open) {
            frame->rax = SYSCALL_ERR_BADF;
            return;
        }
    }

    // pgrp <= 0 → EINVAL (POSIX: pgrp pozitif olmalı)
    if (pgrp <= 0) { frame->rax = SYSCALL_ERR_INVAL; return; }

    uint32_t cur_pid = pg_current_pid();

    // Session kontrolü
    task_t* pg_task = task_find_by_pid((uint32_t)pgrp);
    if (pg_task && pg_effective_sid(pg_task->pid) != pg_effective_sid(cur_pid)) {
        frame->rax = SYSCALL_ERR_PERM;
        return;
    }

    g_terminal_foreground_pgid = (uint32_t)pgrp;

    serial_print("[SYSCALL] tcsetpgrp: pgrp=");
    { char b[12]; int_to_str(pgrp, b); serial_print(b); }
    serial_print("\n");

    frame->rax = SYSCALL_OK;
}

static void sys_tcgetpgrp(syscall_frame_t* frame) {
    int fd = (int)(int64_t)frame->rdi;

    if (fd < 0 || fd >= MAX_FDS) {
        frame->rax = SYSCALL_ERR_BADF;
        return;
    }
    if (fd > 2) {
        task_t* cur = task_get_current();
        fd_entry_t* ent = cur ? fd_get(cur->fd_table, fd) : (void*)0;
        if (!ent || !ent->is_open) {
            frame->rax = SYSCALL_ERR_BADF;
            return;
        }
    }

    uint32_t cur_pid = pg_current_pid();

    // Henüz set edilmemişse çağıranın pgid'ini döndür
    uint32_t result = (g_terminal_foreground_pgid != 0)
                      ? g_terminal_foreground_pgid
                      : pg_effective_pgid(cur_pid);

    frame->rax = (uint64_t)result;
}

// ============================================================
// v15 YENİ SYSCALL IMPLEMENTASYONLARI
// getuid/geteuid/getgid/getegid / nanosleep / sigaltstack
// ============================================================

// AscentOS sabit kimlik değerleri
// Gerçek çok-kullanıcılı yapı olana kadar root gibi davranıyoruz.
// bash $UID=0 görünce bazı güvenlik kısıtlamalarını atlar — istenen davranış.
#define ASCENT_UID   0   // root
#define ASCENT_GID   0   // root

// ── SYS_GETUID (84) ──────────────────────────────────────────────
static void sys_getuid(syscall_frame_t* frame) {
    frame->rax = ASCENT_UID;
}

// ── SYS_GETEUID (85) ─────────────────────────────────────────────
static void sys_geteuid(syscall_frame_t* frame) {
    frame->rax = ASCENT_UID;
}

// ── SYS_GETGID (86) ──────────────────────────────────────────────
static void sys_getgid(syscall_frame_t* frame) {
    frame->rax = ASCENT_GID;
}

// ── SYS_GETEGID (87) ─────────────────────────────────────────────
static void sys_getegid(syscall_frame_t* frame) {
    frame->rax = ASCENT_GID;
}

// ── SYS_NANOSLEEP (88) ───────────────────────────────────────────
// nanosleep(*req, *rem) -> 0 | err
//
// bash'in harici sleep komutu ve readline timeout'ları bunu kullanır.
// Gerçek nanosaniye hassasiyeti yok; timer tick'lerine çevrilir.
// 1 tick ≈ 10ms (100 Hz) varsayılır.
// ---------------------------------------------------------------
static void sys_nanosleep(syscall_frame_t* frame) {
    timespec_t* req = (timespec_t*)frame->rdi;
    timespec_t* rem = (timespec_t*)frame->rsi;  // NULL olabilir

    if (!is_valid_user_ptr(req, sizeof(timespec_t))) {
        frame->rax = SYSCALL_ERR_FAULT;
        return;
    }
    if (req->tv_sec < 0 || req->tv_nsec < 0 || req->tv_nsec >= 1000000000LL) {
        frame->rax = SYSCALL_ERR_INVAL;
        return;
    }

    // Toplam nanosaniyeyi tick'e çevir (1 tick = 10_000_000 ns = 10 ms)
    int64_t total_ns = req->tv_sec * 1000000000LL + req->tv_nsec;
    uint64_t ticks   = (uint64_t)(total_ns / 10000000LL);  // 10ms per tick
    if (ticks == 0 && total_ns > 0) ticks = 1;             // en az 1 tick

    if (ticks > 0) {
        uint64_t end = get_system_ticks() + ticks;
        while (get_system_ticks() < end)
            __asm__ volatile ("sti; hlt" ::: "memory");
    }

    // rem: kalan süre — sinyal kesemediyse 0
    if (rem && is_valid_user_ptr(rem, sizeof(timespec_t))) {
        rem->tv_sec  = 0;
        rem->tv_nsec = 0;
    }

    serial_print("[SYSCALL] nanosleep ok\n");
    frame->rax = SYSCALL_OK;
}

// ── SYS_SIGALTSTACK (89) ─────────────────────────────────────────
// sigaltstack(*ss, *old_ss) -> 0 | err
//
// bash SIGSEGV için alternate stack kurar (stack overflow'dan kurtarma).
// Kernel sanal — stack'i gerçekten kullanmıyor, sadece kaydediyor.
// Asıl handler dispatch'i signal64.c'de yapılıyor.
// ---------------------------------------------------------------

// Per-task alternate stack (şimdilik global — tek task varsayımı)
static stack_t g_altstack = { .ss_sp = (void*)0, .ss_flags = SS_DISABLE, .ss_size = 0 };

static void sys_sigaltstack(syscall_frame_t* frame) {
    const stack_t* ss     = (const stack_t*)frame->rdi;
    stack_t*       old_ss = (stack_t*)frame->rsi;

    // old_ss: mevcut durumu döndür
    if (old_ss) {
        if (!is_valid_user_ptr(old_ss, sizeof(stack_t))) {
            frame->rax = SYSCALL_ERR_FAULT;
            return;
        }
        *old_ss = g_altstack;
    }

    // ss: yeni stack kur
    if (ss) {
        if (!is_valid_user_ptr(ss, sizeof(stack_t))) {
            frame->rax = SYSCALL_ERR_FAULT;
            return;
        }
        // SS_DISABLE: alternate stack'i devre dışı bırak
        if (ss->ss_flags & SS_DISABLE) {
            g_altstack.ss_flags = SS_DISABLE;
            g_altstack.ss_sp    = (void*)0;
            g_altstack.ss_size  = 0;
            frame->rax = SYSCALL_OK;
            return;
        }
        // Boyut kontrolü
        if (ss->ss_size < MINSIGSTKSZ) {
            frame->rax = SYSCALL_ERR_INVAL;
            return;
        }
        if (!is_valid_user_ptr(ss->ss_sp, ss->ss_size)) {
            frame->rax = SYSCALL_ERR_FAULT;
            return;
        }
        g_altstack = *ss;
        g_altstack.ss_flags = 0;  // SS_ONSTACK değil — henüz kullanılmıyor
        serial_print("[SYSCALL] sigaltstack: sp=");
        print_hex64((uint64_t)ss->ss_sp);
        serial_print(" size=");
        print_uint64(ss->ss_size);
        serial_print("\n");
    }

    frame->rax = SYSCALL_OK;
}

// ── SYS_MKDIR (80) ───────────────────────────────────────────────
// mkdir(path, mode) -> 0 | err
// ---------------------------------------------------------------
static void sys_mkdir(syscall_frame_t* frame) {
    const char* path = (const char*)frame->rdi;
    uint32_t    mode = (uint32_t)frame->rsi;
    (void)mode;  // VFS kendi izin yönetimini yapıyor

    if (!is_valid_user_ptr(path, 1)) { frame->rax = SYSCALL_ERR_FAULT;  return; }
    if (path[0] == '\0')             { frame->rax = SYSCALL_ERR_NOENT;  return; }

    // Zaten var mı?
    if (fs_path_is_dir(path))  { frame->rax = SYSCALL_ERR_BUSY;   return; }
    if (fs_path_is_file(path)) { frame->rax = SYSCALL_ERR_BUSY;   return; }

    int r = fs_mkdir64(path);
    if (r < 0) { frame->rax = SYSCALL_ERR_INVAL; return; }

    serial_print("[SYSCALL] mkdir: ");
    serial_print(path);
    serial_print("\n");

    frame->rax = SYSCALL_OK;
}

// ── SYS_RMDIR (81) ───────────────────────────────────────────────
// rmdir(path) -> 0 | err
// Sadece boş dizinleri siler.
// ---------------------------------------------------------------
static void sys_rmdir(syscall_frame_t* frame) {
    const char* path = (const char*)frame->rdi;

    if (!is_valid_user_ptr(path, 1)) { frame->rax = SYSCALL_ERR_FAULT;  return; }
    if (path[0] == '\0')             { frame->rax = SYSCALL_ERR_NOENT;  return; }
    if (!fs_path_is_dir(path))       { frame->rax = SYSCALL_ERR_NOENT;  return; }

    int r = fs_rmdir64(path);
    if (r == -2) { frame->rax = SYSCALL_ERR_BUSY;  return; }  // dolu dizin
    if (r <   0) { frame->rax = SYSCALL_ERR_INVAL; return; }

    serial_print("[SYSCALL] rmdir: ");
    serial_print(path);
    serial_print("\n");

    frame->rax = SYSCALL_OK;
}

// ── SYS_UNLINK (82) ──────────────────────────────────────────────
// unlink(path) -> 0 | err
// Dosyayı siler; dizin için EISDIR döner.
// ---------------------------------------------------------------
static void sys_unlink(syscall_frame_t* frame) {
    const char* path = (const char*)frame->rdi;

    if (!is_valid_user_ptr(path, 1)) { frame->rax = SYSCALL_ERR_FAULT;  return; }
    if (path[0] == '\0')             { frame->rax = SYSCALL_ERR_NOENT;  return; }
    if (fs_path_is_dir(path))        { frame->rax = SYSCALL_ERR_INVAL;  return; }  // EISDIR
    if (!fs_path_is_file(path))      { frame->rax = SYSCALL_ERR_NOENT;  return; }

    int r = fs_unlink64(path);
    if (r < 0) { frame->rax = SYSCALL_ERR_INVAL; return; }

    serial_print("[SYSCALL] unlink: ");
    serial_print(path);
    serial_print("\n");

    frame->rax = SYSCALL_OK;
}

// ── SYS_RENAME (83) ──────────────────────────────────────────────
// rename(oldpath, newpath) -> 0 | err
// Dosya veya dizini yeniden adlandırır / taşır.
// ---------------------------------------------------------------
static void sys_rename(syscall_frame_t* frame) {
    const char* oldpath = (const char*)frame->rdi;
    const char* newpath = (const char*)frame->rsi;

    if (!is_valid_user_ptr(oldpath, 1)) { frame->rax = SYSCALL_ERR_FAULT; return; }
    if (!is_valid_user_ptr(newpath, 1)) { frame->rax = SYSCALL_ERR_FAULT; return; }
    if (oldpath[0] == '\0')             { frame->rax = SYSCALL_ERR_NOENT; return; }
    if (newpath[0] == '\0')             { frame->rax = SYSCALL_ERR_NOENT; return; }

    // Kaynak var mı?
    int src_is_dir  = fs_path_is_dir(oldpath);
    int src_is_file = !src_is_dir && fs_path_is_file(oldpath);
    if (!src_is_dir && !src_is_file) { frame->rax = SYSCALL_ERR_NOENT; return; }

    int r = fs_rename64(oldpath, newpath);
    if (r < 0) { frame->rax = SYSCALL_ERR_INVAL; return; }

    serial_print("[SYSCALL] rename: ");
    serial_print(oldpath);
    serial_print(" -> ");
    serial_print(newpath);
    serial_print("\n");

    frame->rax = SYSCALL_OK;
}

// ── SYS_UNAME (73) ───────────────────────────────────────────────
// uname(utsname_t* buf) -> 0 | err
//
// bash bu bilgiyi şu amaçlarla kullanır:
//   $MACHTYPE, $HOSTTYPE, $OSTYPE  → machine + sysname alanlarından
//   PS1 \s (shell adı) / \v (versiyon) escape'leri → sysname/release
// ---------------------------------------------------------------
static void sys_uname(syscall_frame_t* frame) {
    utsname_t* buf = (utsname_t*)frame->rdi;

    if (!is_valid_user_ptr(buf, sizeof(utsname_t))) {
        frame->rax = SYSCALL_ERR_FAULT;
        return;
    }

    // Alanları sıfırla
    kmemset(buf, 0, sizeof(utsname_t));

    // sysname: işletim sistemi adı
    my_strncpy(buf->sysname,  "AscentOS",  UTS_LEN);
    // nodename: hostname (şimdilik sabit; ileride hostname syscall ile değişir)
    my_strncpy(buf->nodename, "ascent",    UTS_LEN);
    // release: kernel sürümü
    my_strncpy(buf->release,  "1.0.0",     UTS_LEN);
    // version: build tarihi / derleme bilgisi
    my_strncpy(buf->version,  "#1 SMP AscentOS kernel", UTS_LEN);
    // machine: donanım mimarisi — bash $MACHTYPE için kritik
    my_strncpy(buf->machine,  "x86_64",    UTS_LEN);

    serial_print("[SYSCALL] uname: AscentOS x86_64\n");
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
    // v7 – bash dizin syscall'ları
    case SYS_GETCWD:       sys_getcwd(frame);       break;
    case SYS_CHDIR:        sys_chdir(frame);        break;
    // v8 – bash dosya sorgulama
    case SYS_STAT:         sys_stat(frame);         break;
    case SYS_ACCESS:       sys_access(frame);       break;
    // v9 – bash dizin okuma
    case SYS_GETDENTS:     sys_getdents(frame);     break;
    case SYS_OPENDIR:      sys_opendir(frame);      break;
    case SYS_CLOSEDIR:     sys_closedir(frame);     break;
    // v11 – fcntl + dup
    case SYS_FCNTL:        sys_fcntl(frame);       break;
    case SYS_DUP:          sys_dup(frame);         break;
    // v12 – process group & session (bash iş kontrolü)
    case SYS_SETPGID:      sys_setpgid(frame);     break;
    case SYS_GETPGID:      sys_getpgid(frame);     break;
    case SYS_SETSID:       sys_setsid(frame);      break;
    case SYS_TCSETPGRP:    sys_tcsetpgrp(frame);   break;
    case SYS_TCGETPGRP:    sys_tcgetpgrp(frame);   break;
    // v13 – sistem bilgisi
    case SYS_UNAME:        sys_uname(frame);        break;
    // v14 – dosya sistemi yazma (bash mkdir/rm/mv)
    case SYS_MKDIR:        sys_mkdir(frame);        break;
    case SYS_RMDIR:        sys_rmdir(frame);        break;
    case SYS_UNLINK:       sys_unlink(frame);       break;
    case SYS_RENAME:       sys_rename(frame);       break;
    // v15 – kullanıcı kimliği / nanosleep / sigaltstack
    case SYS_GETUID:       sys_getuid(frame);       break;
    case SYS_GETEUID:      sys_geteuid(frame);      break;
    case SYS_GETGID:       sys_getgid(frame);       break;
    case SYS_GETEGID:      sys_getegid(frame);      break;
    case SYS_NANOSLEEP:    sys_nanosleep(frame);    break;
    case SYS_SIGALTSTACK:  sys_sigaltstack(frame);  break;
    // v10 – sinyal altyapısı
    case SYS_SIGACTION:    sys_sigaction(frame);    break;
    case SYS_SIGPROCMASK:  sys_sigprocmask(frame);  break;
    case SYS_SIGRETURN:    sys_sigreturn(frame);    break;
    case SYS_SIGPENDING:   sys_sigpending(frame);   break;
    case SYS_SIGSUSPEND:   sys_sigsuspend(frame);   break;
    default:
        serial_print("[SYSCALL] Unknown syscall: ");
        { char b[16]; int_to_str((int)(num & 0xFFFF), b); serial_print(b); }
        serial_print("\n");
        frame->rax = SYSCALL_ERR_NOSYS;
        break;
    }

    // Her syscall dönüşünde bekleyen sinyalleri kontrol et ve işle.
    // Bu sayede userspace handler'ları syscall noktalarında teslim edilir.
    signal_dispatch_pending();
}