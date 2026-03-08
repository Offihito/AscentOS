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
#define PAGE_SIZE_SC        4096ULL

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
// VFS kırpma – files64.c'de tanımlı (v18)
extern int fs_truncate64(const char* path, uint64_t length);

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

int fd_alloc(fd_entry_t* table, uint8_t type, uint16_t flags, const char* path) {
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
    if (len > 524288)                 { frame->rax = SYSCALL_ERR_INVAL; return; }  // 512KB limit (kilo büyük frame'ler gönderiyor)
    if (fd == 0)                      { frame->rax = SYSCALL_ERR_BADF;  return; }

    // stdout ve stderr: VGA + serial
    if (fd == 1 || fd == 2) {
        // vesa_write_buf: tüm buffer'ı tek seferde ANSI parser'dan geçirir.
        // putchar64 ile karakter karakter yazmak kilo gibi uygulamalarda
        // yavaş ve artefaktlı olur. vesa_write_buf batch işlem yapar.
        extern void vesa_write_buf(const char* buf, int len);
        vesa_write_buf(buf, (int)len);
        for (uint64_t i = 0; i < len; i++)
            serial_putchar(buf[i]);
        frame->rax = len;
        return;
    }

    task_t* cur = task_get_current();
    if (!cur) { frame->rax = SYSCALL_ERR_PERM; return; }

    fd_entry_t* ent = fd_get(cur->fd_table, fd);
    if (!ent)  { frame->rax = SYSCALL_ERR_BADF; return; }
    if (!(ent->flags & (O_WRONLY | O_RDWR))) { frame->rax = SYSCALL_ERR_BADF; return; }

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

    if (ent->type == FD_TYPE_FILE) {
        // VFS write buffer: fd_table path'deki dosyaya yaz
        // fd_entry içinde write_buf biriktirilir, close'da flush edilir
        // Basit yaklaşım: tüm write'ları fd_entry'nin path'inden VFS'e ilet
        // Geçici buffer: ent->write_buf alanı yoksa yeni alan simüle et
        // files64.c: fs_write_file64(name, content) → FAT32'ye kaydeder

        // Accumulate in a static per-fd write buffer (offset tabanlı)
        // write_buf_data: fd_table'da yok, bu yüzden global yazma tamponu kullanırız
        extern int fs_vfs_write(const char* path, uint64_t offset,
                                const char* data, uint32_t len);
        int n = fs_vfs_write(ent->path, ent->offset, buf, (uint32_t)len);
        if (n < 0) n = 0;
        ent->offset += (uint64_t)n;
        frame->rax = (uint64_t)n;
        serial_print("[SYSCALL] file write: ");
        { char b[12]; int_to_str(n, b); serial_print(b); }
        serial_print(" bytes -> ");
        serial_print(ent->path);
        serial_print("\n");
        return;
    }
    // Pipe veya diğer: sadece serial
    for (uint64_t i = 0; i < len; i++) serial_putchar(buf[i]);
    frame->rax = len;
}

// ── SYS_READ (2) ───────────────────────────────────────────────
// kernel_termios: sys_ioctl bölümünde tanımlı (satır ~2197); forward decl.
extern termios_t kernel_termios;

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
            // ── termios ICANON kontrolü ──────────────────────────────────
            // ICANON=1 (canonical): satır bazlı okuma, '\n' bekle (shell, vb.)
            // ICANON=0 (raw):       VMIN kadar byte al, '\n' bekleme (Lua REPL, kilo, vb.)
            int is_canonical = (kernel_termios.c_lflag & ICANON) ? 1 : 0;
            // Raw modda VMIN: kaç byte beklenecek (0 = non-blocking, 1+ = o kadar bekle)
            uint8_t vmin = is_canonical ? 1 : kernel_termios.c_cc[VMIN];
            if (vmin == 0) vmin = 1; // en az 1 byte al (yoksa sonsuz döngü olur)

            // Blocking read: VMIN kadar karakter gelene kadar bekle
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

                if (is_canonical) {
                    // Canonical mod: '\n' veya buffer dolu olana kadar devam et
                    while (count < len) {
                        ch = kb_ring_pop();
                        if (ch < 0) break;
                        buf[count++] = (char)ch;
                        if ((char)ch == '\n') break;
                    }
                } else {
                    // Raw mod: VMIN-1 karakter daha al (ilk zaten alındı),
                    // '\n' bekleme — Lua/kilo gibi karakter bazlı uygulamalar için
                    while (count < (uint64_t)vmin && count < len) {
                        ch = kb_ring_pop();
                        if (ch < 0) break; // kalan karakterler sonraki read'de
                        buf[count++] = (char)ch;
                    }
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
    if ((ent->flags & (O_WRONLY | O_RDWR)) == O_WRONLY) { frame->rax = SYSCALL_ERR_BADF; return; }

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

    if (ent->type == FD_TYPE_FILE) {
        // VFS + FAT32'den oku
        const EmbeddedFile64* vf = fs_get_file64(ent->path);
        if (vf && vf->content) {
            uint64_t fsize = (uint64_t)vf->size;
            if (ent->offset >= fsize) { frame->rax = 0; return; }
            uint64_t avail = fsize - ent->offset;
            uint64_t copy  = (avail < len) ? avail : len;
            kmemcpy(buf, vf->content + ent->offset, (uint32_t)copy);
            ent->offset += copy;
            frame->rax = copy;
            return;
        }
        // FAT32'den oku
        uint32_t fsize = fat32_file_size(ent->path);
        if (fsize == 0) { frame->rax = 0; return; }
        uint8_t* tmp = (uint8_t*)kmalloc(fsize + 1);
        if (!tmp) { frame->rax = SYSCALL_ERR_NOMEM; return; }
        int rd = fat32_read_file(ent->path, tmp, fsize);
        if (rd > 0 && ent->offset < (uint64_t)rd) {
            uint32_t avail = (uint32_t)rd - (uint32_t)ent->offset;
            uint32_t copy  = (avail < (uint32_t)len) ? avail : (uint32_t)len;
            kmemcpy(buf, tmp + ent->offset, copy);
            ent->offset += copy;
            kfree(tmp);
            frame->rax = copy;
            return;
        }
        kfree(tmp);
        frame->rax = 0;
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

    flags &= (uint64_t)(O_RDONLY | O_WRONLY | O_RDWR | O_CREAT | O_TRUNC | O_APPEND | O_NONBLOCK);

    task_t* cur = task_get_current();
    if (!cur) { frame->rax = SYSCALL_ERR_PERM; return; }

    uint8_t fd_type = (my_strcmp(path, "/dev/serial0") == 0)
                      ? FD_TYPE_SERIAL : FD_TYPE_FILE;

    // basename al
    const char* fname = path;
    for (const char* p = path; *p; p++)
        if (*p == '/') fname = p + 1;

    if (fd_type == FD_TYPE_FILE && (flags & O_CREAT)) {
        extern int fs_touch_file64(const char* filename);
        extern int fs_truncate64(const char* path, uint64_t length);
        if (!fs_path_is_file(fname)) {
            fs_touch_file64(fname);
        }
        if (flags & O_TRUNC) {
            fs_truncate64(fname, 0);
        }
    }

    if (fd_type == FD_TYPE_FILE && !(flags & O_CREAT)) {
        // Önce full path ile dene, bulamazsan basename ile tekrar dene
        if (!fs_path_is_file(path) && !fs_path_is_file(fname)) {
            serial_print("[SYSCALL] open: ENOENT ");
            serial_print(path);
            serial_print("\n");
            frame->rax = SYSCALL_ERR_NOENT;
            return;
        }
    }

    int new_fd = fd_alloc(cur->fd_table, fd_type, (uint16_t)(flags & 0xFFFF), path);
    if (new_fd < 0) { frame->rax = SYSCALL_ERR_MFILE; return; }

    serial_print("[SYSCALL] open -> fd=");
    print_uint64((uint64_t)new_fd);
    serial_print(" path=");
    serial_print(path);
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

// ── mmap allocation table + freelist cache ───────────────────
// musl mallocng, mmap() ile aldığı adresi doğrudan munmap()'e verir.
// Kilo gibi uygulamalar her tuşta mmap+munmap döngüsü yapar (4KB bloklar).
// Çözüm: munmap'te kfree yerine freelist'e al; aynı boyut tekrar istenirse
// yeni kmalloc yapmadan direkt ver → mmap/munmap çifti sıfır maliyetli olur.
#define MMAP_TABLE_SIZE 256

typedef struct {
    uint64_t user_addr;   // mmap'in döndürdüğü adres (0 = boş slot)
    void*    raw_ptr;     // kfree için gerçek kmalloc pointer'ı
    uint64_t alloc_size;  // kmalloc'a verilen toplam boyut (hizalama dahil)
    uint64_t aligned_len; // sayfaya hizalanmış kullanıcı boyu
} mmap_entry_t;

static mmap_entry_t mmap_table[MMAP_TABLE_SIZE];
static int mmap_table_init = 0;

// Serbest bırakılmış ama henüz kfree edilmemiş blok havuzu.
// musl 4KB'lık iki adresi ping-pong gibi değiştiriyor; onları burada tutuyoruz.
#define MMAP_CACHE_SIZE 16
typedef struct {
    void*    raw_ptr;
    uint64_t aligned_len; // hangi boyut için geçerli
    uint64_t user_addr;
    uint64_t alloc_size;
} mmap_cache_entry_t;

static mmap_cache_entry_t mmap_cache[MMAP_CACHE_SIZE];
static int mmap_cache_count = 0;

static void mmap_table_clear(void) {
    for (int i = 0; i < MMAP_TABLE_SIZE; i++) {
        mmap_table[i].user_addr   = 0;
        mmap_table[i].raw_ptr     = 0;
        mmap_table[i].alloc_size  = 0;
        mmap_table[i].aligned_len = 0;
    }
    for (int i = 0; i < MMAP_CACHE_SIZE; i++) {
        mmap_cache[i].raw_ptr     = 0;
        mmap_cache[i].aligned_len = 0;
        mmap_cache[i].user_addr   = 0;
        mmap_cache[i].alloc_size  = 0;
    }
    mmap_cache_count = 0;
    mmap_table_init = 1;
}

static void mmap_table_insert(uint64_t user_addr, void* raw_ptr,
                               uint64_t alloc_size, uint64_t aligned_len) {
    if (!mmap_table_init) mmap_table_clear();
    for (int i = 0; i < MMAP_TABLE_SIZE; i++) {
        if (mmap_table[i].user_addr == 0) {
            mmap_table[i].user_addr   = user_addr;
            mmap_table[i].raw_ptr     = raw_ptr;
            mmap_table[i].alloc_size  = alloc_size;
            mmap_table[i].aligned_len = aligned_len;
            return;
        }
    }
    // Tablo doluysa en eskiyi ezeriz (bellek sızıntısı, pratik olmaz)
    mmap_table[0].user_addr   = user_addr;
    mmap_table[0].raw_ptr     = raw_ptr;
    mmap_table[0].alloc_size  = alloc_size;
    mmap_table[0].aligned_len = aligned_len;
}

// munmap: raw_ptr'ı kfree etmek yerine cache'e al.
// Dönüş: 1 = cache'e alındı, 0 = cache dolu (caller kfree etmeli)
static int mmap_cache_push(void* raw_ptr, uint64_t user_addr,
                            uint64_t alloc_size, uint64_t aligned_len) {
    if (mmap_cache_count >= MMAP_CACHE_SIZE) return 0;
    mmap_cache[mmap_cache_count].raw_ptr     = raw_ptr;
    mmap_cache[mmap_cache_count].user_addr   = user_addr;
    mmap_cache[mmap_cache_count].alloc_size  = alloc_size;
    mmap_cache[mmap_cache_count].aligned_len = aligned_len;
    mmap_cache_count++;
    return 1;
}

// mmap: önce cache'de aynı boyuta uygun blok ara.
// Bulunursa sıfırla ve döndür (kmalloc yok!), bulunamazsa NULL döner.
static void* mmap_cache_pop(uint64_t needed_aligned_len, uint64_t* out_raw,
                             uint64_t* out_user, uint64_t* out_alloc) {
    for (int i = 0; i < mmap_cache_count; i++) {
        if (mmap_cache[i].aligned_len >= needed_aligned_len) {
            void*    raw        = mmap_cache[i].raw_ptr;
            uint64_t user_addr  = mmap_cache[i].user_addr;
            uint64_t alloc_size = mmap_cache[i].alloc_size;
            // Slot'u kapat (son girişle değiştir)
            mmap_cache[i] = mmap_cache[mmap_cache_count - 1];
            mmap_cache_count--;
            *out_raw   = (uint64_t)raw;
            *out_user  = user_addr;
            *out_alloc = alloc_size;
            return raw;
        }
    }
    return (void*)0;
}

static void* mmap_table_remove(uint64_t user_addr) {
    if (!mmap_table_init) return (void*)0;
    for (int i = 0; i < MMAP_TABLE_SIZE; i++) {
        if (mmap_table[i].user_addr == user_addr) {
            void* raw             = mmap_table[i].raw_ptr;
            mmap_table[i].user_addr   = 0;
            mmap_table[i].raw_ptr     = 0;
            mmap_table[i].alloc_size  = 0;
            mmap_table[i].aligned_len = 0;
            return raw;
        }
    }
    return (void*)0;  // bulunamadı
}

// Tablo'dan entry'nin aligned_len bilgisini de al (cache için gerekli)
static void mmap_table_remove_full(uint64_t user_addr, void** out_raw,
                                    uint64_t* out_alloc, uint64_t* out_alen) {
    if (!mmap_table_init) { *out_raw = 0; return; }
    for (int i = 0; i < MMAP_TABLE_SIZE; i++) {
        if (mmap_table[i].user_addr == user_addr) {
            *out_raw   = mmap_table[i].raw_ptr;
            *out_alloc = mmap_table[i].alloc_size;
            *out_alen  = mmap_table[i].aligned_len;
            mmap_table[i].user_addr   = 0;
            mmap_table[i].raw_ptr     = 0;
            mmap_table[i].alloc_size  = 0;
            mmap_table[i].aligned_len = 0;
            return;
        }
    }
    *out_raw = 0;
}

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

        uint64_t alloc_size = aligned_len + PAGE_SIZE_SC;
        uint64_t aligned_addr;
        uint8_t* raw;

        // Önce freelist cache'e bak: aynı boyutta daha önce munmap edilmiş
        // blok varsa kmalloc+memset yapmadan anında geri ver.
        uint64_t cached_raw = 0, cached_user = 0, cached_alloc = 0;
        void* cached = mmap_cache_pop(aligned_len, &cached_raw, &cached_user, &cached_alloc);
        if (cached) {
            raw          = (uint8_t*)cached_raw;
            aligned_addr = cached_user;
            alloc_size   = cached_alloc;
        } else {
            // Cache boş: gerçek kmalloc
            raw = (uint8_t*)kmalloc(alloc_size);
            if (!raw) { frame->rax = (uint64_t)MAP_FAILED; return; }
            aligned_addr = ((uint64_t)raw + PAGE_SIZE_SC - 1) & ~(PAGE_SIZE_SC - 1);
            kmemset((void*)aligned_addr, 0, aligned_len);
        }

        // raw → aligned_addr eşlemesini tabloya kaydet
        mmap_table_insert(aligned_addr, (void*)raw, alloc_size, aligned_len);


        frame->rax = aligned_addr;
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

    // Bellek ayır ve sıfırla (page-aligned, raw ptr tabloda saklanır)
    uint64_t alloc_size2 = aligned_len + PAGE_SIZE_SC;
    uint8_t* raw2 = (uint8_t*)kmalloc(alloc_size2);
    if (!raw2) { frame->rax = (uint64_t)MAP_FAILED; return; }
    uint64_t aligned2_addr = ((uint64_t)raw2 + PAGE_SIZE_SC - 1) & ~(PAGE_SIZE_SC - 1);
    mmap_table_insert(aligned2_addr, (void*)raw2, alloc_size2, aligned_len);
    void* mem = (void*)aligned2_addr;
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
    uint64_t addr = frame->rdi;
    uint64_t len  = frame->rsi;

    if (!addr)    { frame->rax = SYSCALL_ERR_INVAL; return; }
    if (len == 0) { frame->rax = SYSCALL_ERR_INVAL; return; }

    // mmap_table'dan raw pointer + boyut bilgisini al.
    void*    raw        = 0;
    uint64_t alloc_size = 0;
    uint64_t aligned_len_cached = 0;
    mmap_table_remove_full(addr, &raw, &alloc_size, &aligned_len_cached);


    if (raw) {
        // kfree yerine freelist cache'e al.
        // Aynı adresler ping-pong şeklinde mmap/munmap ediliyorsa (musl malloc
        // döngüsü) bir sonraki mmap isteğine anında cevap verir — kmalloc/kfree
        // çağrısı tamamen önlenir.
        if (!mmap_cache_push(raw, addr, alloc_size, aligned_len_cached)) {
            // Cache dolu: gerçek kfree
            kfree(raw);
        }
    } else {
    }

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

    // pmm_alloc_pages kullan (task_create_user ile tutarlı) →
    // task_reap_zombie'de pmm_free_pages ile güvenle free edilebilir.
    extern void* pmm_alloc_pages(uint64_t count);
    uint64_t ks_pages = child->kernel_stack_size / 4096;
    void* ks = pmm_alloc_pages(ks_pages);
    if (!ks) ks = (void*)kmalloc(child->kernel_stack_size);  // fallback
    if (!ks) {
        kfree(child);
        frame->rax = SYSCALL_ERR_NOMEM;
        return;
    }
    child->kernel_stack_base = (uint64_t)ks;
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

        // KRITIK: user stack Ring-3'ten erişilebilir olmalı.
        // kmalloc kernel-only sayfa verir (USER flag yok) → Ring-3 #PF.
        // pmm_alloc_pages_flags(..., 0x7) = PRESENT|WRITE|USER flag'li sayfa kullan.
        extern void* pmm_alloc_pages_flags(uint64_t count, uint64_t map_flags);
        uint64_t page_count = child->user_stack_size / 4096;
        void* us = pmm_alloc_pages_flags(page_count, 0x7);
        if (!us) {
            // fallback: kmalloc (flat-memory kernel'de erişilebilir olabilir)
            us = (void*)kmalloc(child->user_stack_size);
        }
        if (!us) {
            kfree((void*)child->kernel_stack_base);
            kfree(child);
            frame->rax = SYSCALL_ERR_NOMEM;
            return;
        }
        child->user_stack_base = (uint64_t)us;
        kmemcpy((void*)child->user_stack_base,
                (void*)parent->user_stack_base,
                child->user_stack_size);

        // ── User stack: parent'ı kopyala + RSP delta ─────────────
        child->user_stack_top = child->user_stack_base + child->user_stack_size;
        uint64_t stack_delta  = child->user_stack_base - parent->user_stack_base;

        // Gerçek user RSP'yi bul.
        //
        // syscall_entry user-path stack düzeni (frame = rsp dispatch anında):
        //   frame+0..+72  : syscall_frame_t (rax..user_rsp)
        //   frame+80      : r14  (callee-saved)
        //   frame+88      : r13
        //   frame+96      : r12
        //   frame+104     : rbp
        //   frame+112     : rbx
        //   frame+120     : r15  ← user RSP (mov r15,rsp ile kaydedildi)
        //
        // frame->user_rsp: eski assembly'de 0, yeni versiyonda gerçek user RSP.
        // Geçersizse frame+120'den r15'i oku.
        uint64_t parent_user_rsp = frame->user_rsp;

        if (parent_user_rsp == 0 ||
            parent_user_rsp >= 0x8000000000000000ULL ||
            parent_user_rsp < parent->user_stack_base ||
            parent_user_rsp > parent->user_stack_top) {

            // frame+120 = index 15 = r15 = SYSCALL anındaki user RSP
            uint64_t* frame_base = (uint64_t*)frame;
            parent_user_rsp = frame_base[15];

            if (parent_user_rsp < parent->user_stack_base ||
                parent_user_rsp > parent->user_stack_top) {
                parent_user_rsp = parent->user_stack_top - 128;
            }
        }

        uint64_t child_user_rsp = parent_user_rsp + stack_delta;

        // ── Çocuğun kernel stack'ine iretq frame kur ─────────────
        // Scheduler task_switch_context sonrası iretq ile Ring-3'e döner.
        // Frame düzeni (yüksekten alçağa):
        //   SS, RSP(user), RFLAGS, CS, RIP
        // Ardından 15 GPR (r15..rax) — task_switch_context bunları pop eder.
        uint64_t* stk = (uint64_t*)child->kernel_stack_top;

        // iretq frame
        *(--stk) = GDT_USER_DATA_RPL3;               // SS  = 0x1B
        *(--stk) = child_user_rsp;                   // RSP = user stack
        *(--stk) = 0x202ULL;                         // RFLAGS = IF=1
        *(--stk) = GDT_USER_CODE_RPL3;               // CS  = 0x23
        *(--stk) = frame->rcx;                       // RIP = fork() sonrası user RIP

        // 15 GPR: task_create_user ile ayni sekilde sifirla
        // Sira: iretq'dan once pop edilecek r15..rax (interrupts64.asm sirasina gore)
        // fork()==0 musl/libc tarafindan RAX'tan okunur — iretq sonrasi RAX=0 olmali.
        // task_create_user da hepsini 0 yapiyor, ayni yaklasim guvenli.
        for (int _gi = 0; _gi < 14; _gi++) *(--stk) = 0; // r15..rbx = 0
        *(--stk) = 0;                                // RAX = 0 (fork() == 0 cocukta)

        // context.rsp = kernel stack'teki GPR dizisinin basi
        child->context.rsp    = (uint64_t)stk;
        child->context.rip    = frame->rcx;          // user RIP
        child->context.rflags = 0x202;
        child->context.cs     = GDT_USER_CODE_RPL3;  // 0x23
        child->context.ss     = GDT_USER_DATA_RPL3;  // 0x1B
        child->context.rax    = 0;                   // fork() == 0

        serial_print("[FORK] Ring-3 child iretq frame kuruldu, RIP=0x");
        print_uint64(frame->rcx);
        serial_print(" user_rsp=0x");
        print_uint64(child_user_rsp);
        serial_print("\n");

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

    // Klavyeyi userland moduna al: keyboard_unified.c'deki kb_ring_push
    // akışı artık bu task'a yönlenecek; sys_read blocking okuma yapabilir.
    extern void kb_set_userland_mode(int on);
    kb_set_userland_mode(1);

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
//   WNOHANG    – tamamlanmış/değişen çocuk yoksa hemen 0 döndür
//   WUNTRACED  – SIGTSTP/SIGSTOP ile durdurulan çocukları da raporla
//   WCONTINUED – SIGCONT ile devam eden durdurulmuş çocukları raporla
//
// Status encoding:
//   Normal exit   : (exit_code & 0xFF) << 8          → WIFEXITED
//   Signal kill    : signo & 0x7F                     → WIFSIGNALED
//   Stopped        : 0x7F | (stopsig << 8)            → WIFSTOPPED
//   Continued      : 0xFFFF                           → WIFCONTINUED
// ---------------------------------------------------------------
static void sys_waitpid(syscall_frame_t* frame) {
    int64_t  pid_arg = (int64_t)frame->rdi;
    int*     status  = (int*)frame->rsi;
    uint64_t options = frame->rdx;

    if (status && !is_valid_user_ptr(status, sizeof(int))) {
        frame->rax = SYSCALL_ERR_FAULT; return;
    }
    task_t* cur = task_get_current();
    if (!cur) { frame->rax = SYSCALL_ERR_PERM; return; }

    uint64_t deadline = get_system_ticks() + ((options & WNOHANG) ? 0 : 5000);

    do {
        task_t* found = 0; int found_type = 0;

        if (pid_arg > 0) {
            task_t* c = task_find_by_pid((uint32_t)pid_arg);
            if (c && c->parent_pid == cur->pid) {
                if (c->state == TASK_STATE_ZOMBIE || c->state == TASK_STATE_TERMINATED)
                    { found = c; found_type = 1; }
                else if ((options & WUNTRACED) && c->state == TASK_STATE_STOPPED)
                    { found = c; found_type = 2; }
            }
        } else {
            // Tüm child'ları tara — PID 1..4095 + zombie listesi
            for (uint32_t sp = 1; sp < 4096 && !found; sp++) {
                task_t* t = task_find_by_pid(sp);
                if (!t || t->parent_pid != cur->pid) continue;
                if (t->state == TASK_STATE_ZOMBIE || t->state == TASK_STATE_TERMINATED)
                    { found = t; found_type = 1; }
                else if ((options & WUNTRACED) && t->state == TASK_STATE_STOPPED)
                    { found = t; found_type = 2; }
            }
        }

        if (found) {
            uint32_t wpid = found->pid;
            if (status) {
                if      (found_type == 1) *status = (found->exit_code & 0xFF) << 8;
                else if (found_type == 2) *status = 0x7F | (SIGTSTP << 8);
                else                      *status = 0xFFFF;
            }
            serial_print("[SYSCALL] waitpid: pid="); print_uint64(wpid);
            serial_print(" exitcode=");
            { char b[8]; int_to_str(found->exit_code, b); serial_print(b); }
            serial_print("\n");
            if (found_type == 1) {
                extern void task_reap_zombie(task_t*);
                task_reap_zombie(found);
            }
            frame->rax = (uint64_t)wpid;
            return;
        }

        if (options & WNOHANG) { frame->rax = 0; return; }

        if (pid_arg > 0) {
            task_t* s = task_find_by_pid((uint32_t)pid_arg);
            if (!s || s->parent_pid != cur->pid) { frame->rax = SYSCALL_ERR_CHILD; return; }
        }

        __asm__ volatile ("sti; hlt" ::: "memory");
    } while (get_system_ticks() < deadline);

    frame->rax = SYSCALL_ERR_AGAIN;
}

// ── SYS_GETGROUPS (106) ─────────────────────────────────────────
// getgroups(int size, gid_t list[]) -> ngroups | SYSCALL_ERR_*
//
// Bu çekirdek tek-kullanıcılı; supplementary grup yoktur.
// Root (gid=0) için list[0]=0 döner, diğerleri için list boş.
// size=0 ise sadece grup sayısını döner (list'e yazmaz).
// ---------------------------------------------------------------
static void sys_getgroups(syscall_frame_t* frame) {
    int      size = (int)(int64_t)frame->rdi;
    uint32_t* list = (uint32_t*)frame->rsi;

    if (size < 0) { frame->rax = SYSCALL_ERR_INVAL; return; }

    // Bu kernel'de supplementary grup yok → ngroups = 0
    int ngroups = 0;

    if (size == 0) {
        // Sadece sayıyı döndür
        frame->rax = (uint64_t)ngroups;
        return;
    }

    // size > 0 ama list pointer'ı doğrula
    if (!is_valid_user_ptr(list, (uint64_t)size * sizeof(uint32_t))) {
        frame->rax = SYSCALL_ERR_FAULT;
        return;
    }

    // EINVAL: buffer çok küçük (ngroups > size)
    if (ngroups > size) {
        frame->rax = SYSCALL_ERR_INVAL;
        return;
    }

    // ngroups == 0: listeye yazılacak bir şey yok
    serial_print("[SYSCALL] getgroups: ngroups=0\n");
    frame->rax = (uint64_t)ngroups;
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
termios_t kernel_termios = {
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
    case TIOCGWINSZ: {
        // VESA boyutunu SADECE kernel_winsize henüz varsayılansa (80x25) al.
        // TIOCSWINSZ ile set edilmiş bir değer varsa onu koru — ezmeme!
        if (kernel_winsize.ws_col == 80 && kernel_winsize.ws_row == 25) {
            extern void get_screen_size64(size_t* width, size_t* height);
            size_t vesa_cols = 0, vesa_rows = 0;
            get_screen_size64(&vesa_cols, &vesa_rows);
            if (vesa_cols > 0 && vesa_rows > 0) {
                kernel_winsize.ws_col = (uint16_t)vesa_cols;
                kernel_winsize.ws_row = (uint16_t)vesa_rows;
            }
        }
        if (!is_valid_user_ptr(arg, sizeof(winsize_t))) {
            frame->rax = SYSCALL_ERR_FAULT;
            return;
        }
        kmemcpy(arg, &kernel_winsize, sizeof(winsize_t));
        serial_print("[SYSCALL] ioctl TIOCGWINSZ: ");
        { char b[8]; int_to_str(kernel_winsize.ws_col, b); serial_print(b); }
        serial_print("x");
        { char b[8]; int_to_str(kernel_winsize.ws_row, b); serial_print(b); }
        serial_print("\n");
        frame->rax = SYSCALL_OK;
        break;
    }

    // ── TIOCSWINSZ: pencere boyutunu ayarla ──────────────────────
    // Boyut değiştikten sonra ön plan process grubuna SIGWINCH gönder;
    // bash/readline bu sinyali alınca ekranı yeniden çizer.
    case TIOCSWINSZ: {
        if (!is_valid_user_ptr(arg, sizeof(winsize_t))) {
            frame->rax = SYSCALL_ERR_FAULT;
            return;
        }
        winsize_t old = kernel_winsize;
        kmemcpy(&kernel_winsize, arg, sizeof(winsize_t));
        serial_print("[SYSCALL] ioctl TIOCSWINSZ ok (");
        { char b[8]; int_to_str(kernel_winsize.ws_col, b); serial_print(b); }
        serial_print("x");
        { char b[8]; int_to_str(kernel_winsize.ws_row, b); serial_print(b); }
        serial_print(")\n");

        // Boyut gerçekten değiştiyse ön plan pgrp'ye SIGWINCH gönder
        if (old.ws_row != kernel_winsize.ws_row ||
            old.ws_col != kernel_winsize.ws_col) {
            // kernel_tty_pgrp: ön plan süreç grubu (tcsetpgrp tarafından güncellenir)
            // signal_send negatif pid alırsa tüm gruba gönderir (Linux semantiği)
            signal_send(-(int)kernel_tty_pgrp, SIGWINCH);
            serial_print("[SYSCALL] SIGWINCH -> pgrp ");
            { char b[8]; int_to_str((int)kernel_tty_pgrp, b); serial_print(b); }
            serial_print("\n");
        }
        frame->rax = SYSCALL_OK;
        break;
    }

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

    // pid > 0: hedef task'a signal_send ile gönder.
    // Task bulunamazsa EINVAL dön (test [53] bunu bekliyor).
    task_t* target = task_find_by_pid((uint32_t)pid);
    if (!target) {
        frame->rax = SYSCALL_ERR_INVAL;  // -EINVAL: geçersiz/bulunamayan pid
        return;
    }
    // Sleeping task'a da pending_sigs set et (signal_send sadece ready_queue'a bakar)
    signal_table_t* st = task_get_signal_table(target);
    if (st) {
        st->pending_sigs |= SIG_BIT(sig);
        if (sig == SIGKILL) {
            // SIGKILL: anında sonlandır — sleeping olsa bile
            target->state = TASK_STATE_TERMINATED;
            serial_print("[SYSCALL] kill SIGKILL: task terminated pid=");
            { char b[12]; int_to_str(pid, b); serial_print(b); }
            serial_print("\n");
        }
    }
    frame->rax = SYSCALL_OK;
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

    if (!is_valid_user_ptr(path, 1)) {   // NULL dahil → EFAULT
        serial_print("[SYSCALL] chdir: geçersiz pointer (EFAULT)\n");
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

    if (!is_valid_user_ptr(path, 1)) {   // NULL dahil tüm geçersiz pointer'lar → EFAULT
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
        uint32_t fsz = fs_path_filesize(path);
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

    if (!is_valid_user_ptr(path, 1)) {   // NULL dahil → EFAULT
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

    if (!is_valid_user_ptr(path, 1)) {   // NULL dahil → EFAULT
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
//
// ÖNEMLİ: sti+hlt kullanılmaz — kernel context'te HLT interrupt
// gelmezse sonsuza kilitlenir. Bunun yerine:
//   - tv_sec == 0 && tv_nsec < 10ms  → hemen döner (0 tick)
//   - Gerçek uyku gerekiyorsa tick sayar, her iterasyonda pause kullanır
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
    uint64_t ticks   = (uint64_t)(total_ns / 10000000LL);

    // 10ms altı istekler (1ms, 0.5ms vb.) sıfır tick → hemen dön.
    // HLT kullanmıyoruz: timer interrupt'ı güvensiz olduğu context'lerde
    // sonsuz beklemeye girer. tick > 0 için sadece get_system_ticks() poll.
    if (ticks > 0) {
        uint64_t end = get_system_ticks() + ticks;
        uint64_t deadline = end + 10000;  // güvenlik: max 10000 iterasyon
        uint64_t iter = 0;
        while (get_system_ticks() < end && iter < deadline) {
            __asm__ volatile ("pause" ::: "memory");
            iter++;
        }
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

// ── SYS_CLOCK_GETTIME (90) ───────────────────────────────────────
// clock_gettime(clockid_t clockid, timespec_t* tp) -> 0 | err
//
// bash, readline ve newlib libc bu syscall'ı:
//   • CLOCK_REALTIME  : time(), gettimeofday() alternatifleri
//   • CLOCK_MONOTONIC : $SECONDS, elapsed-time hesabı, select/poll timeout
//   • CPUTIME_ID      : profiling (stub olarak sıfır döner)
// amaçlarıyla kullanır.
//
// Zaman kaynağı: get_system_ticks() — 1 tick ≈ 10 ms (100 Hz).
// tv_sec  = ticks / 100
// tv_nsec = (ticks % 100) * 10_000_000
// ---------------------------------------------------------------
static void sys_clock_gettime(syscall_frame_t* frame) {
    uint32_t    clockid = (uint32_t)frame->rdi;
    timespec_t* tp      = (timespec_t*)frame->rsi;

    if (!is_valid_user_ptr(tp, sizeof(timespec_t))) {
        frame->rax = SYSCALL_ERR_FAULT;
        return;
    }

    switch (clockid) {
        case CLOCK_REALTIME:
        case CLOCK_MONOTONIC: {
            uint64_t ticks_ms = get_system_ticks();   // milisaniye
            tp->tv_sec  = (int64_t)(ticks_ms / 1000ULL);
            tp->tv_nsec = (int64_t)((ticks_ms % 1000ULL) * 1000000ULL);
            break;
        }
        case CLOCK_PROCESS_CPUTIME_ID:
        case CLOCK_THREAD_CPUTIME_ID:
            // Stub: CPU süresi takibi yok, sıfır döndür
            tp->tv_sec  = 0;
            tp->tv_nsec = 0;
            break;
        default:
            frame->rax = SYSCALL_ERR_INVAL;
            return;
    }

    frame->rax = SYSCALL_OK;
}

// ── SYS_CLOCK_GETRES (91) ────────────────────────────────────────
// clock_getres(clockid_t clockid, timespec_t* res) -> 0 | err
//
// Saat çözünürlüğünü döndürür. Tick tabanlı sistemde:
//   CLOCK_REALTIME / CLOCK_MONOTONIC : 10 ms (10_000_000 ns)
//   CPUTIME_ID                        : 1 ms (stub)
// res NULL ise sadece clockid geçerliliği kontrol edilir.
// ---------------------------------------------------------------
static void sys_clock_getres(syscall_frame_t* frame) {
    uint32_t    clockid = (uint32_t)frame->rdi;
    timespec_t* res     = (timespec_t*)frame->rsi;  // NULL olabilir

    // clockid geçerli mi?
    if (clockid != CLOCK_REALTIME &&
        clockid != CLOCK_MONOTONIC &&
        clockid != CLOCK_PROCESS_CPUTIME_ID &&
        clockid != CLOCK_THREAD_CPUTIME_ID) {
        frame->rax = SYSCALL_ERR_INVAL;
        return;
    }

    if (res) {
        if (!is_valid_user_ptr(res, sizeof(timespec_t))) {
            frame->rax = SYSCALL_ERR_FAULT;
            return;
        }
        res->tv_sec  = 0;
        res->tv_nsec = (clockid == CLOCK_REALTIME || clockid == CLOCK_MONOTONIC)
                       ? 10000000LL   // 10 ms
                       : 1000000LL;   // 1 ms (stub)
    }

    frame->rax = SYSCALL_OK;
}

// ── SYS_ALARM (92) ───────────────────────────────────────────────
// alarm(unsigned int seconds) -> kalan_saniye
//
// bash kullanımı:
//   • read -t N       : N saniye timeout ile girdi bekle
//   • wait timeout    : alt süreç bekleme timeout'u
//   • SIGALRM handler : script içi zamanlayıcılar
//
// Davranış (POSIX):
//   alarm(0)  → bekleyen alarm'ı iptal et, kalan süreyi döndür
//   alarm(N)  → N saniye sonra SIGALRM gönder; önceki alarm varsa
//               iptal edip kalanı döndür
//
// Kernel tick tabanlı implementasyon:
//   1 saniye ≈ 100 tick (100 Hz varsayımı, timer.h ile tutarlı)
// ---------------------------------------------------------------

// Per-task alarm state (şimdilik global — tek task varsayımı)
static uint64_t g_alarm_deadline = 0;   // 0 = alarm yok
static uint8_t  g_alarm_active   = 0;

static void sys_alarm(syscall_frame_t* frame) {
    uint32_t seconds = (uint32_t)frame->rdi;

    uint64_t now   = get_system_ticks();
    uint64_t remaining = 0;

    // Bekleyen alarm varsa kalan süreyi hesapla
    if (g_alarm_active && g_alarm_deadline > now) {
        uint64_t ticks_left = g_alarm_deadline - now;
        remaining = (ticks_left + 99) / 100;   // tick → saniye (yukarı yuvarla)
    }

    if (seconds == 0) {
        // alarm(0): iptal
        g_alarm_active   = 0;
        g_alarm_deadline = 0;
    } else {
        // Yeni alarm kur: 1 sn = 100 tick
        g_alarm_deadline = now + (uint64_t)seconds * 100ULL;
        g_alarm_active   = 1;

        // Basit busy-wait yerine scheduler tick'te kontrol edilmeli;
        // şimdilik signal64.c / scheduler entegrasyonu için kancayı kur.
        // Gerçek SIGALRM gönderimi: scheduler her tick'te g_alarm_deadline
        // kontrolü yapıp task'a SIGALRM inject etmeli.
        // Bu stub: alarm set edildi, sinyal scheduler tarafından gönderilir.
    }

    serial_print("[SYSCALL] alarm: remaining=");
    {
        char buf[16];
        int i = 0;
        uint64_t v = remaining;
        if (v == 0) { buf[i++] = '0'; }
        else { while (v > 0) { buf[i++] = '0' + (int)(v % 10); v /= 10; } }
        buf[i] = '\0';
        // reverse
        for (int a = 0, b = i-1; a < b; a++, b--) {
            char c = buf[a]; buf[a] = buf[b]; buf[b] = c;
        }
        serial_print(buf);
    }
    serial_print("\n");

    frame->rax = (uint64_t)remaining;
}

// alarm state'e dışarıdan erişim (scheduler entegrasyonu için)
uint8_t  alarm_is_active(void)   { return g_alarm_active; }
uint64_t alarm_get_deadline(void){ return g_alarm_deadline; }
void     alarm_clear(void)       { g_alarm_active = 0; g_alarm_deadline = 0; }

// ── SYS_FTRUNCATE (93) ───────────────────────────────────────────
// ftruncate(fd, length) -> 0 | err
//
// bash kullanımı:
//   • history dosyasını (fd açık) HISTSIZE'a kırpma
//   • heredoc temp dosyaları
//   • mktemp + ftruncate ile atomic geçici dosya oluşturma
//
// VFS flat-array implementasyonu: dosya içeriği yeniden yazılır.
// length > mevcut boyut → sıfır ile doldurulur (zero-extend).
// length < mevcut boyut → kırpılır.
// ---------------------------------------------------------------
static void sys_ftruncate(syscall_frame_t* frame) {
    int      fd     = (int)frame->rdi;
    uint64_t length = frame->rsi;

    // fd geçerli mi?
    task_t* cur = task_get_current();
    if (!cur) { frame->rax = SYSCALL_ERR_BADF; return; }

    fd_entry_t* entry = fd_get(cur->fd_table, fd);
    if (!entry || !entry->is_open) { frame->rax = SYSCALL_ERR_BADF; return; }
    if (entry->type != FD_TYPE_FILE) { frame->rax = SYSCALL_ERR_INVAL; return; }
    if (!(entry->flags & O_WRONLY) && !(entry->flags & O_RDWR)) {
        frame->rax = SYSCALL_ERR_PERM;
        return;
    }

    // VFS üzerinden truncate
    int r = fs_truncate64(entry->path, length);
    if (r < 0) { frame->rax = SYSCALL_ERR_INVAL; return; }

    // fd offset'i length'i aşıyorsa sona çek
    if (entry->offset > length)
        entry->offset = length;

    serial_print("[SYSCALL] ftruncate ok\n");
    frame->rax = SYSCALL_OK;
}

// ── SYS_TRUNCATE (94) ────────────────────────────────────────────
// truncate(path, length) -> 0 | err
//
// ftruncate ile aynı işlev, fd yerine path alır.
// ---------------------------------------------------------------
static void sys_truncate(syscall_frame_t* frame) {
    const char* path   = (const char*)frame->rdi;
    uint64_t    length = frame->rsi;

    if (!is_valid_user_ptr(path, 1)) { frame->rax = SYSCALL_ERR_FAULT;  return; }
    if (path[0] == '\0')             { frame->rax = SYSCALL_ERR_NOENT;  return; }
    if (!fs_path_is_file(path))      { frame->rax = SYSCALL_ERR_NOENT;  return; }

    int r = fs_truncate64(path, length);
    if (r < 0) { frame->rax = SYSCALL_ERR_PERM; return; }

    serial_print("[SYSCALL] truncate ok\n");
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

// ── SYS_GETRLIMIT (95) / SYS_SETRLIMIT (96) ──────────────────────────────
// getrlimit(resource, *rlimit) -> 0 | err
// setrlimit(resource, *rlimit) -> 0 | err
//
// Bash başlarken RLIMIT_NOFILE'ı sorgular; üst fd limitini bilerek
// fd tablosunu başlatır. Kernel state olarak RLIMIT_NLIMITS adetlik
// bir tablo tutulur. Başlangıçta sane varsayılanlar atanır.
// ---------------------------------------------------------------

// Çekirdek limit tablosu (per-sistem; ileride per-task yapılabilir)
static rlimit_t kernel_rlimits[RLIMIT_NLIMITS] = {
    [RLIMIT_CPU]        = { RLIM_INFINITY, RLIM_INFINITY },
    [RLIMIT_FSIZE]      = { RLIM_INFINITY, RLIM_INFINITY },
    [RLIMIT_DATA]       = { RLIM_INFINITY, RLIM_INFINITY },
    [RLIMIT_STACK]      = { 8u*1024u*1024u, RLIM_INFINITY }, // 8 MiB soft
    [RLIMIT_CORE]       = { 0,              RLIM_INFINITY },
    [RLIMIT_RSS]        = { RLIM_INFINITY, RLIM_INFINITY },
    [RLIMIT_NPROC]      = { 64,            64            },
    [RLIMIT_NOFILE]     = { MAX_FDS,       MAX_FDS       }, // 32 — MAX_FDS ile eşleşir
    [RLIMIT_MEMLOCK]    = { RLIM_INFINITY, RLIM_INFINITY },
    [RLIMIT_AS]         = { RLIM_INFINITY, RLIM_INFINITY },
    [RLIMIT_LOCKS]      = { RLIM_INFINITY, RLIM_INFINITY },
    [RLIMIT_SIGPENDING] = { 256,           256           },
    [RLIMIT_MSGQUEUE]   = { 0,             0             },
    [RLIMIT_NICE]       = { 0,             0             },
    [RLIMIT_RTPRIO]     = { 0,             0             },
};

static void sys_getrlimit(syscall_frame_t* frame) {
    int       resource = (int)frame->rdi;
    rlimit_t* rl       = (rlimit_t*)frame->rsi;

    if (resource < 0 || resource >= RLIMIT_NLIMITS) {
        frame->rax = SYSCALL_ERR_INVAL;
        return;
    }
    if (!is_valid_user_ptr(rl, sizeof(rlimit_t))) {
        frame->rax = SYSCALL_ERR_FAULT;
        return;
    }

    kmemcpy(rl, &kernel_rlimits[resource], sizeof(rlimit_t));

    serial_print("[SYSCALL] getrlimit(");
    { char b[8]; int_to_str(resource, b); serial_print(b); }
    serial_print(") cur=");
    if (kernel_rlimits[resource].rlim_cur == RLIM_INFINITY)
        serial_print("INF");
    else { char b[24]; int_to_str((int)kernel_rlimits[resource].rlim_cur, b); serial_print(b); }
    serial_print("\n");

    frame->rax = SYSCALL_OK;
}

static void sys_setrlimit(syscall_frame_t* frame) {
    int             resource = (int)frame->rdi;
    const rlimit_t* rl       = (const rlimit_t*)frame->rsi;

    if (resource < 0 || resource >= RLIMIT_NLIMITS) {
        frame->rax = SYSCALL_ERR_INVAL;
        return;
    }
    if (!is_valid_user_ptr(rl, sizeof(rlimit_t))) {
        frame->rax = SYSCALL_ERR_FAULT;
        return;
    }
    // Soft limit, hard limit'i geçemez
    if (rl->rlim_cur != RLIM_INFINITY &&
        rl->rlim_max != RLIM_INFINITY &&
        rl->rlim_cur > rl->rlim_max) {
        frame->rax = SYSCALL_ERR_INVAL;
        return;
    }
    // Hard limit'i yükseltmek EPERM (root yokken)
    if (rl->rlim_max != RLIM_INFINITY &&
        kernel_rlimits[resource].rlim_max != RLIM_INFINITY &&
        rl->rlim_max > kernel_rlimits[resource].rlim_max) {
        frame->rax = SYSCALL_ERR_PERM;
        return;
    }

    kmemcpy(&kernel_rlimits[resource], rl, sizeof(rlimit_t));

    serial_print("[SYSCALL] setrlimit(");
    { char b[8]; int_to_str(resource, b); serial_print(b); }
    serial_print(") ok\n");

    frame->rax = SYSCALL_OK;
}

// ── SYS_LSTAT (97) ───────────────────────────────────────────────────────
// lstat(path, *stat_buf) -> 0 | err
//
// Sembolik link'in kendisini stat'lar (link'in gösterdiği dosyayı değil).
// AscentOS'ta henüz sembolik link yok; bu yüzden lstat = stat davranışı
// gösterir — symlink olmayan sistemde doğru ve POSIX uyumlu.
// bash [[ -L path ]] testi S_ISLNK bit'ini kontrol eder; 0 gelirse
// false döner — link olmadığı için bu da doğru davranış.
// -----------------------------------------------------------------------
static void sys_lstat(syscall_frame_t* frame) {
    // lstat imzası stat ile aynı; doğrudan yönlendir
    sys_stat(frame);
    // Not: Eğer ileride symlink eklenirse burada ayrı işleme gerekir.
    // Şimdilik st_mode'da S_IFLNK bit'i hiçbir zaman set edilmez — doğru.
}

// ── SYS_LINK (98) ────────────────────────────────────────────────────────
// link(oldpath, newpath) -> 0 | err
//
// Hard link oluşturur. AscentOS VFS'i hard link desteklemediğinden
// EPERM döner — bu POSIX'te geçerli bir cevaptır ("operation not
// permitted on this filesystem"). newlib _link() stub'ı bu kodu
// görünce errno=EPERM set eder, çağıran uygulamaya hata yayar.
// Bash doğrudan link() çağırmaz; newlib derleme stub'ı için yeterli.
// -----------------------------------------------------------------------
static void sys_link(syscall_frame_t* frame) {
    const char* oldpath = (const char*)frame->rdi;
    const char* newpath = (const char*)frame->rsi;

    if (!is_valid_user_ptr(oldpath, 1) || !is_valid_user_ptr(newpath, 1)) {
        frame->rax = SYSCALL_ERR_FAULT;
        return;
    }
    if (oldpath[0] == '\0' || newpath[0] == '\0') {
        frame->rax = SYSCALL_ERR_NOENT;
        return;
    }
    if (!fs_path_is_file(oldpath) && !fs_path_is_dir(oldpath)) {
        frame->rax = SYSCALL_ERR_NOENT;
        return;
    }

    // Hard link VFS tarafından desteklenmiyor
    serial_print("[SYSCALL] link: ");
    serial_print(oldpath);
    serial_print(" -> ");
    serial_print(newpath);
    serial_print(" (EPERM: hard links not supported)\n");

    frame->rax = SYSCALL_ERR_PERM;
}

// ── SYS_TIMES (99) ───────────────────────────────────────────────────────
// times(*tms) -> elapsed_ticks | err
//
// POSIX times(): process CPU kullanımını tms_t struct'ına yazar ve
// keyfi bir başlangıç noktasından bu yana geçen clock tick sayısını
// döndürür. newlib _times() ve bash 'time' builtin bu syscall'ı kullanır.
//
// Kernel preemption ve per-task CPU muhasebesi olmadığından utime/stime
// stub olarak 0 doldurulur; elapsed tick olarak system uptime döner.
// Bu POSIX'e uygundur: "CPU times may not be available" belgesi izin verir,
// utime=0 dönen gerçek implementasyonlar mevcuttur (QEMU minimal kernel vb).
// -----------------------------------------------------------------------
static void sys_times(syscall_frame_t* frame) {
    tms_t* buf = (tms_t*)frame->rdi;

    if (buf != 0) {   // NULL geçmek geçerli (sadece elapsed tick isteniyor)
        if (!is_valid_user_ptr(buf, sizeof(tms_t))) {
            frame->rax = SYSCALL_ERR_FAULT;
            return;
        }
        buf->tms_utime  = 0;   // Kullanıcı CPU süresi (stub)
        buf->tms_stime  = 0;   // Kernel CPU süresi (stub)
        buf->tms_cutime = 0;   // Çocuk kullanıcı CPU süresi
        buf->tms_cstime = 0;   // Çocuk kernel CPU süresi
    }

    // Dönüş değeri: başlangıçtan bu yana geçen clock tick sayısı
    // get_system_ticks() scheduler.h'dan; zaten SYS_GETTICKS'te kullanılıyor
    extern uint64_t get_system_ticks(void);
    uint64_t elapsed = get_system_ticks();

    serial_print("[SYSCALL] times: elapsed=");
    { char b[24]; int_to_str((int)elapsed, b); serial_print(b); }
    serial_print("\n");

    frame->rax = elapsed;
}

// ============================================================
// v21 – SYS_UMASK / SYS_SYMLINK / SYS_READLINK
// ============================================================

// ── SYS_UMASK (100) ──────────────────────────────────────────────────────
// umask(mode_t mask) -> old_mask
//
// bash init sırasında umask() çağırır; builtin `umask` komutu da bunu
// kullanır. Yeni dosya/dizin oluştururken permission bit'lerini maskelemek
// için gereklidir.
//
// Davranış (POSIX):
//   • Her zaman başarılı, errno set etmez.
//   • Eski mask değerini döndürür.
//   • Sadece en düşük 9 bit anlamlı (rwxrwxrwx).
// -----------------------------------------------------------------------
static uint32_t g_umask = 0022;   // Unix varsayılanı: owner tam, group/other no-write

static void sys_umask(syscall_frame_t* frame) {
    uint32_t new_mask = (uint32_t)(frame->rdi & 0777);
    uint32_t old_mask = g_umask;
    g_umask = new_mask;

    serial_print("[SYSCALL] umask: old=0");
    { char b[8]; int_to_str((int)old_mask, b); serial_print(b); }
    serial_print(" new=0");
    { char b[8]; int_to_str((int)new_mask, b); serial_print(b); }
    serial_print("\n");

    frame->rax = (uint64_t)old_mask;
}

// ── SYS_SYMLINK (101) ────────────────────────────────────────────────────
// symlink(const char* target, const char* linkpath) -> 0 | err
//
// bash kullanımı:
//   • `ln -s target link`  (bash'in kendi ln builtini yoktur ama newlib
//     üzerinden glibc/stub symlink() çağırır)
//   • Komut tamamlama (readline) sembolik linkleri takip eder
//   • [[ -L path ]] : lstat + S_IFLNK kontrolü
//
// AscentOS VFS'i gerçek symlink'i desteklemiyor; ancak:
//   1) newlib _symlink() stub'ının derlenmesi için syscall numarası gerekli
//   2) `ln -s` benzeri araçların hata yerine başarı dönmesi bash scriptlerini
//      kırmaz (link oluşturulamaz ama crash olmaz)
//   3) İleride basit bir symlink tablosu eklenebilir
//
// Şimdilik: linkpath'i target'ın içeriğini gösteren özel bir dosya olarak
// VFS'e kaydet (flat copy, gerçek symlink değil).
// -----------------------------------------------------------------------

// Basit symlink tablosu — ileride files64.c'ye taşınabilir
#define MAX_SYMLINKS  16
typedef struct {
    char linkpath[64];
    char target[64];
    uint8_t used;
} symlink_entry_t;

static symlink_entry_t g_symlinks[MAX_SYMLINKS];
static int g_symlinks_inited = 0;

static void symlink_table_init(void) {
    if (g_symlinks_inited) return;
    for (int i = 0; i < MAX_SYMLINKS; i++) g_symlinks[i].used = 0;
    g_symlinks_inited = 1;
}

static void sys_symlink(syscall_frame_t* frame) {
    const char* target   = (const char*)frame->rdi;
    const char* linkpath = (const char*)frame->rsi;

    if (!is_valid_user_ptr(target,   1)) { frame->rax = SYSCALL_ERR_FAULT; return; }
    if (!is_valid_user_ptr(linkpath, 1)) { frame->rax = SYSCALL_ERR_FAULT; return; }
    if (target[0]   == '\0') { frame->rax = SYSCALL_ERR_NOENT; return; }
    if (linkpath[0] == '\0') { frame->rax = SYSCALL_ERR_NOENT; return; }

    symlink_table_init();

    // linkpath zaten var mı?
    for (int i = 0; i < MAX_SYMLINKS; i++) {
        if (g_symlinks[i].used) {
            int eq = 1;
            for (int j = 0; j < 63; j++) {
                if (g_symlinks[i].linkpath[j] != linkpath[j]) { eq = 0; break; }
                if (linkpath[j] == '\0') break;
            }
            if (eq) {
                serial_print("[SYSCALL] symlink: linkpath exists (EBUSY)\n");
                frame->rax = SYSCALL_ERR_BUSY;
                return;
            }
        }
    }

    // Boş slot bul
    for (int i = 0; i < MAX_SYMLINKS; i++) {
        if (!g_symlinks[i].used) {
            my_strncpy(g_symlinks[i].target,   target,   64);
            my_strncpy(g_symlinks[i].linkpath, linkpath, 64);
            g_symlinks[i].used = 1;

            serial_print("[SYSCALL] symlink: ");
            serial_print(linkpath);
            serial_print(" -> ");
            serial_print(target);
            serial_print("\n");

            frame->rax = SYSCALL_OK;
            return;
        }
    }

    // Tablo dolu
    serial_print("[SYSCALL] symlink: table full (ENOMEM)\n");
    frame->rax = SYSCALL_ERR_NOMEM;
}

// ── SYS_READLINK (102) ───────────────────────────────────────────────────
// readlink(const char* path, char* buf, size_t bufsiz) -> nbytes | err
//
// bash kullanımı:
//   • /proc/self/exe benzeri path'lerin resolve edilmesi
//   • argv[0] → gerçek binary yolunun bulunması (bash --version, $0)
//   • Komut tamamlamada symlink zinciri takibi
//   • `readlink` harici komutu (newlib stub → bu syscall)
//
// Özel path'ler:
//   /proc/self/exe  → mevcut task'ın execve edildiği binary yolu
//   /proc/self/fd/N → fd N'nin açık olduğu dosyanın yolu
//
// Normal path'ler:
//   Önce g_symlinks tablosunda ara; bulunursa target kopyala.
//   Bulunamazsa EINVAL (POSIX: path bir symlink değil).
// -----------------------------------------------------------------------
static void sys_readlink(syscall_frame_t* frame) {
    const char* path   = (const char*)frame->rdi;
    char*       buf    = (char*)frame->rsi;
    uint64_t    bufsiz = frame->rdx;

    if (!is_valid_user_ptr(path, 1))        { frame->rax = SYSCALL_ERR_FAULT; return; }
    if (!is_valid_user_ptr(buf, bufsiz))    { frame->rax = SYSCALL_ERR_FAULT; return; }
    if (bufsiz == 0)                        { frame->rax = SYSCALL_ERR_INVAL; return; }
    if (path[0] == '\0')                    { frame->rax = SYSCALL_ERR_NOENT; return; }

    serial_print("[SYSCALL] readlink(\"");
    serial_print(path);
    serial_print("\")\n");

    // ── /proc/self/exe: mevcut binary yolu ───────────────────────
    // path == "/proc/self/exe"
    {
        const char* pse = "/proc/self/exe";
        int match = 1;
        for (int i = 0; pse[i] || path[i]; i++) {
            if (pse[i] != path[i]) { match = 0; break; }
        }
        if (match) {
            task_t* cur = task_get_current();
            const char* exe = (cur && cur->name[0]) ? cur->name : "/bin/bash";
            uint64_t len = 0;
            while (exe[len]) len++;
            if (len > bufsiz) len = bufsiz;   // POSIX: kopyala bufsiz byte, null ekleme
            for (uint64_t i = 0; i < len; i++) buf[i] = exe[i];
            // readlink POSIX: null terminator EKLEMEZ
            frame->rax = len;
            return;
        }
    }

    // ── /proc/self/fd/N: fd'nin path'i ───────────────────────────
    {
        const char* prefix = "/proc/self/fd/";
        int plen = 0;
        while (prefix[plen]) plen++;
        int match = 1;
        for (int i = 0; i < plen; i++) {
            if (path[i] != prefix[i]) { match = 0; break; }
        }
        if (match) {
            int fd_num = 0;
            for (int i = plen; path[i] >= '0' && path[i] <= '9'; i++)
                fd_num = fd_num * 10 + (path[i] - '0');
            task_t* cur = task_get_current();
            if (cur && fd_num >= 0 && fd_num < MAX_FDS) {
                fd_entry_t* ent = fd_get(cur->fd_table, fd_num);
                if (ent && ent->path[0]) {
                    uint64_t len = 0;
                    while (ent->path[len]) len++;
                    if (len > bufsiz) len = bufsiz;  // POSIX: null ekleme
                    for (uint64_t i = 0; i < len; i++) buf[i] = ent->path[i];
                    frame->rax = len;
                    return;
                }
            }
            frame->rax = SYSCALL_ERR_BADF;
            return;
        }
    }

    // ── Normal symlink tablosu araması ───────────────────────────
    symlink_table_init();
    for (int i = 0; i < MAX_SYMLINKS; i++) {
        if (!g_symlinks[i].used) continue;
        int eq = 1;
        for (int j = 0; j < 63; j++) {
            if (g_symlinks[i].linkpath[j] != path[j]) { eq = 0; break; }
            if (path[j] == '\0') break;
        }
        if (eq) {
            uint64_t len = 0;
            while (g_symlinks[i].target[len]) len++;
            if (len > bufsiz) len = bufsiz;   // POSIX: null ekleme
            for (uint64_t k = 0; k < len; k++) buf[k] = g_symlinks[i].target[k];
            serial_print("[SYSCALL] readlink -> ");
            serial_print(g_symlinks[i].target);
            serial_print("\n");
            frame->rax = len;
            return;
        }
    }

    // Symlink değil
    serial_print("[SYSCALL] readlink: EINVAL (not a symlink)\n");
    frame->rax = SYSCALL_ERR_INVAL;
}

// ============================================================
// v22 – SYS_CHMOD / SYS_MPROTECT / SYS_PIPE2
// ============================================================

// ── SYS_CHMOD (103) ──────────────────────────────────────────────────────
// chmod(const char* path, mode_t mode) -> 0 | err
//
// Bash kullanımı:
//   - Dosya/dizin oluşturduktan sonra `chmod 755 file` shell built-in'i
//   - newlib _chmod() stub çağrısı
//   - install(1) gibi araçlar
//
// AscentOS VFS: dosya izinleri st_mode'da tutulur; chmod bu alanı günceller.
// Desteklenmeyen özellik: setuid/setgid bitler sessizce yoksayılır (POSIX
// önerir ama EPERM yerine yoksaymak da geçerlidir).
//
// Özel durumlar:
//   NULL path → EFAULT
//   Boş path  → ENOENT
//   Yok path  → ENOENT
//   Sadece alt 12 bit anlamlı (rwx rwx rwx + setuid/setgid/sticky)
static void sys_chmod(syscall_frame_t* frame) {
    const char* path = (const char*)frame->rdi;
    uint64_t    mode = frame->rsi & 07777;   // 12 bit: rwxrwxrwx + suid/sgid/sticky

    // ── pointer doğrulama ─────────────────────────────────────
    if (!is_valid_user_ptr(path, 1)) {
        serial_print("[SYSCALL] chmod: EFAULT (bad path ptr)\n");
        frame->rax = SYSCALL_ERR_FAULT;
        return;
    }
    if (path[0] == '\0') {
        serial_print("[SYSCALL] chmod: ENOENT (empty path)\n");
        frame->rax = SYSCALL_ERR_NOENT;
        return;
    }

    serial_print("[SYSCALL] chmod(\"");
    serial_print(path);
    serial_print("\", 0");
    { char b[8]; int_to_str((int)mode, b); serial_print(b); }
    serial_print(")\n");

    // ── VFS varlık kontrolü ───────────────────────────────────
    // fs_path_is_dir / fs_path_is_file mevcut API'lerdir.
    // AscentOS VFS FAT32 tabanlı; chmod kalıcı permission desteği yok.
    // Varlık kontrolü başarılıysa 0 döndür (bash tolerant: chmod sonucu
    // kontrol etmez, sadece 0 bekler).
    int is_dir  = fs_path_is_dir(path);
    int is_file = (!is_dir) && fs_path_is_file(path);

    if (!is_dir && !is_file) {
        serial_print("[SYSCALL] chmod: ENOENT (path not found)\n");
        frame->rax = SYSCALL_ERR_NOENT;
        return;
    }

    // FAT32'de izin bitleri kalıcı değil; bellekteki stat cache'ini güncelle
    // (mevcut sys_stat zaten sabit 0644/0755 döndürüyor — bu yeterli).
    // İleride VFS permission cache eklenirse burada güncellenebilir.
    serial_print("[SYSCALL] chmod: ok (vfs accepts, fat32 stub)\n");
    frame->rax = 0;
}

// ── SYS_MPROTECT (104) ───────────────────────────────────────────────────
// mprotect(void* addr, size_t len, int prot) -> 0 | err
//
// Bash / newlib kullanımı:
//   - ELF loader: segment'leri yüklendikten sonra PT_LOAD izinlerini uygular
//   - dlopen/dlclose: paylaşımlı kütüphane yükleme
//   - newlib _sbrk/brk sonrası sayfa koruma
//   - JIT derleme: RWX → RX geçişi
//
// AscentOS bellek modeli: mmap tabanlı basit allocator; sayfa granülaritesi.
// Gerçek MMU sayfa tablolarını değiştirmek komplekstir; bu stub:
//   1. addr ve len doğrulama yapar (EINVAL / ENOMEM)
//   2. prot bitlerini kayıt altına almaz (uygulamamak da POSIX izinli)
//   3. Her zaman 0 döndürür — ELF loader çalışmaya devam eder
//
// Gelecekte: gerçek MMU page table yürüyüşü ile koruma bitleri güncellenebilir.
//
// POSIX hata durumları (uygulananlar):
//   EINVAL: addr sayfa sınırına hizalanmamış, len=0, geçersiz prot
//   ENOMEM: [addr, addr+len) aralığı map edilmemiş
//   EFAULT: addr NULL veya canonical değil
#define PROT_NONE_SC   0
#define PROT_READ_SC   1
#define PROT_WRITE_SC  2
#define PROT_EXEC_SC   4

static void sys_mprotect(syscall_frame_t* frame) {
    uint64_t addr = frame->rdi;
    uint64_t len  = frame->rsi;
    int      prot = (int)(int64_t)frame->rdx;

    // ── EFAULT: NULL adres ───────────────────────────────────
    // Not: mmap kernel adresi dondurebilir; yalnizca NULL reddedilir.
    if (addr == 0) {
        serial_print("[SYSCALL] mprotect: EFAULT (NULL addr)\n");
        frame->rax = SYSCALL_ERR_FAULT;
        return;
    }

    // ── EINVAL: addr sayfa sınırına hizalı değil ─────────────
    if (addr & (PAGE_SIZE_SC - 1)) {
        serial_print("[SYSCALL] mprotect: EINVAL (addr not page-aligned)\n");
        frame->rax = SYSCALL_ERR_INVAL;
        return;
    }

    // ── EINVAL: len = 0 ──────────────────────────────────────
    if (len == 0) {
        serial_print("[SYSCALL] mprotect: EINVAL (len=0)\n");
        frame->rax = SYSCALL_ERR_INVAL;
        return;
    }

    // ── EINVAL: geçersiz prot bitleri ────────────────────────
    // prot sadece PROT_NONE|PROT_READ|PROT_WRITE|PROT_EXEC içerebilir
    if ((uint32_t)prot & ~(uint32_t)(PROT_READ_SC|PROT_WRITE_SC|PROT_EXEC_SC)) {
        serial_print("[SYSCALL] mprotect: EINVAL (bad prot bits)\n");
        frame->rax = SYSCALL_ERR_INVAL;
        return;
    }

    serial_print("[SYSCALL] mprotect(addr=0x");
    { char b[20]; int_to_str((int)(addr & 0xFFFFFFFF), b); serial_print(b); }
    serial_print(", len=");
    { char b[20]; int_to_str((int)(len & 0xFFFFFFFF), b); serial_print(b); }
    serial_print(", prot=");
    { char b[8]; int_to_str(prot, b); serial_print(b); }
    serial_print(") -> 0 (stub)\n");

    // AscentOS stub: MMU koruma bitleri uygulanmaz; 0 döndür.
    // ELF loader mprotect başarılı olursa yüklemeye devam eder.
    frame->rax = 0;
}

// ── SYS_PIPE2 (105) ──────────────────────────────────────────────────────
// pipe2(int pipefd[2], int flags) -> 0 | err
//
// Bash kullanımı:
//   - Pipe oluştururken O_CLOEXEC (0x80000) atomik olarak set etmek için
//   - bash 4.4+ her pipe oluşturmada pipe2(fds, O_CLOEXEC) çağırır
//   - Ardından ayrıca fcntl(fd, F_SETFD, FD_CLOEXEC) yapmaya gerek kalmaz
//
// Desteklenen flags:
//   O_CLOEXEC (0x80000) → her iki fd için FD_CLOEXEC set et
//   O_NONBLOCK (0x800)  → her iki fd için O_NONBLOCK set et (ileride)
//   0                   → SYS_PIPE ile aynı davranış
//
// Desteklenmeyen flags → EINVAL
//
// Implementasyon: sys_pipe() mantığını tekrar kullanır; ardından flags uygular.
#define O_CLOEXEC_SC   0x80000
#define O_NONBLOCK_SC  0x800

static void sys_pipe2(syscall_frame_t* frame) {
    int*    pipefd = (int*)frame->rdi;
    int     flags  = (int)(int64_t)frame->rsi;

    // ── EFAULT: NULL pointer ──────────────────────────────────
    if (!is_valid_user_ptr(pipefd, sizeof(int) * 2)) {
        serial_print("[SYSCALL] pipe2: EFAULT (bad pipefd ptr)\n");
        frame->rax = SYSCALL_ERR_FAULT;
        return;
    }

    // ── EINVAL: bilinmeyen flag bitleri ──────────────────────
    if ((uint32_t)flags & ~(uint32_t)(O_CLOEXEC_SC | O_NONBLOCK_SC)) {
        serial_print("[SYSCALL] pipe2: EINVAL (unknown flags)\n");
        frame->rax = SYSCALL_ERR_INVAL;
        return;
    }

    serial_print("[SYSCALL] pipe2(flags=0x");
    { char b[12]; int_to_str(flags, b); serial_print(b); }
    serial_print(")\n");

    task_t* cur = task_get_current();
    if (!cur) {
        frame->rax = SYSCALL_ERR_INVAL;
        return;
    }

    // ── sys_pipe ile aynı pattern ─────────────────────────────
    pipe_buf_t* pb = pipe_buf_alloc();
    if (!pb) {
        serial_print("[SYSCALL] pipe2: ENOMEM (no pipe buffer)\n");
        frame->rax = SYSCALL_ERR_NOMEM;
        return;
    }
    pb->ref_count = 2;   // okuma + yazma ucu

    // Okuma fd (fd[0]) — fd_alloc_pipe doğru API
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
        fd_free(cur->fd_table, rfd);
        frame->rax = SYSCALL_ERR_MFILE;
        return;
    }

    // ── O_CLOEXEC uygula → fd_flags alanını kullan ───────────
    if (flags & O_CLOEXEC_SC) {
        cur->fd_table[rfd].fd_flags |= FD_CLOEXEC;
        cur->fd_table[wfd].fd_flags |= FD_CLOEXEC;
        serial_print("[SYSCALL] pipe2: O_CLOEXEC set on both fds\n");
    }

    // ── O_NONBLOCK: flags alanına yaz ────────────────────────
    if (flags & O_NONBLOCK_SC) {
        cur->fd_table[rfd].flags |= (uint8_t)(O_NONBLOCK_SC & 0xFF);
        cur->fd_table[wfd].flags |= (uint8_t)(O_NONBLOCK_SC & 0xFF);
        serial_print("[SYSCALL] pipe2: O_NONBLOCK set on both fds\n");
    }

    pipefd[0] = rfd;
    pipefd[1] = wfd;

    serial_print("[SYSCALL] pipe2: rfd=");
    { char b[8]; int_to_str(rfd, b); serial_print(b); }
    serial_print(" wfd=");
    { char b[8]; int_to_str(wfd, b); serial_print(b); }
    serial_print("\n");

    frame->rax = 0;
}

// ============================================================
// sys_futex — Linux x86-64 uyumlu futex (SYS_FUTEX = 202)
//
// AscentOS şu an single-threaded çalıştığından gerçek bekleme/
// uyandırma mantığı gerekmez; POSIX semantiğini karşılaştırma
// tabanlı stub ile uyguluyoruz:
//
//   FUTEX_WAIT / FUTEX_WAIT_PRIVATE:
//     *uaddr != val  → EAGAIN  (zaten değer farklı, bekleme yok)
//     *uaddr == val  → uniprocessor: anında EINTR dön
//                      (gerçek scheduler entegrasyonuna kadar)
//   FUTEX_WAKE / FUTEX_WAKE_PRIVATE:
//     0 dön (kimse uyanmadı — waiter yok)
//   NULL uaddr      → EFAULT
//   Geçersiz op     → EINVAL
//
// Argümanlar (Linux uyumlu):
//   RDI = uaddr   RSI = op   RDX = val   R10 = timeout   R8 = uaddr2   R9 = val3
// ============================================================
static void sys_futex(syscall_frame_t* frame) {
    uint32_t* uaddr   = (uint32_t*)(uintptr_t)frame->rdi;
    int       op      = (int)(int64_t)frame->rsi;
    uint32_t  val     = (uint32_t)frame->rdx;
    // timeout = frame->r10, uaddr2 = frame->r8, val3 = frame->r9 (şimdilik kullanılmıyor)

    // PRIVATE flag: sadece aynı mm-içi semantik — tek process'te fark yok
    int base_op = op & ~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME);

    serial_print("[SYSCALL] futex op=");
    { char b[8]; int_to_str(op, b); serial_print(b); }
    serial_print("\n");

    // ── NULL uaddr → EFAULT ──────────────────────────────────────
    if (!uaddr || !is_valid_user_ptr(uaddr, sizeof(uint32_t))) {
        serial_print("[SYSCALL] futex: EFAULT\n");
        frame->rax = SYSCALL_ERR_FAULT;
        return;
    }

    switch (base_op) {
    // ── FUTEX_WAIT ───────────────────────────────────────────────
    case FUTEX_WAIT:
        // Değer eşleşmiyor → EAGAIN (thread uyutulmayacak)
        if (*uaddr != val) {
            serial_print("[SYSCALL] futex WAIT: val mismatch -> EAGAIN\n");
            frame->rax = SYSCALL_ERR_AGAIN;
        } else {
            // Değer eşleşiyor ama gerçek bekleme yok (single-threaded stub)
            // Linux davranışı: sinyalle kesilirse EINTR döner
            serial_print("[SYSCALL] futex WAIT: no real waitqueue -> EINTR\n");
            frame->rax = SYSCALL_ERR_INTR;
        }
        break;

    // ── FUTEX_WAKE ───────────────────────────────────────────────
    case FUTEX_WAKE:
        // Uyandırılan thread sayısını döndür; hiç waiter yok → 0
        serial_print("[SYSCALL] futex WAKE: 0 woken\n");
        frame->rax = 0;
        break;

    // ── FUTEX_REQUEUE ────────────────────────────────────────────
    case FUTEX_REQUEUE:
    case FUTEX_CMP_REQUEUE:
        // Stub: waiter yok, 0 taşındı
        frame->rax = 0;
        break;

    // ── Bilinmeyen op ────────────────────────────────────────────
    default:
        serial_print("[SYSCALL] futex: unknown op -> EINVAL\n");
        frame->rax = SYSCALL_ERR_INVAL;
        break;
    }
}

// ============================================================
// sys_getrandom — Linux x86-64 uyumlu (SYS_GETRANDOM = 318)
//
// Kernel PRNG: xorshift64 tabanlı, timer tick ile seed'lenir.
// Gerçek entropi kaynağı yerine deterministic PRNG kullanılır
// (bare-metal ortamda /dev/urandom yoktur).
//
// flags:
//   GRND_NONBLOCK (0x01): entropi bekleme yok (zaten yok, görmezden gel)
//   GRND_RANDOM   (0x02): "yüksek kalite" istendi — aynı PRNG'i kullan
//   Bilinmeyen bit → EINVAL
//
// Argümanlar:
//   RDI = buf   RSI = buflen   RDX = flags
// Dönüş: okunan byte sayısı | hata
// ============================================================

// Xorshift64 PRNG — her çağrıda farklı random üretir
static uint64_t grnd_state = 0xDEADBEEFCAFEBABEull;

static uint64_t grnd_next(void) {
    uint64_t x = grnd_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    grnd_state = x;
    return x;
}

// Dışarıdan seed ayarlamak için (timer.c'den çağrılabilir)
void getrandom_seed(uint64_t seed) {
    if (seed) grnd_state ^= seed;
}

static void sys_getrandom(syscall_frame_t* frame) {
    uint8_t*  buf    = (uint8_t*)(uintptr_t)frame->rdi;
    uint64_t  buflen = frame->rsi;
    uint32_t  flags  = (uint32_t)frame->rdx;

    serial_print("[SYSCALL] getrandom len=");
    { char b[12]; int_to_str((int)buflen, b); serial_print(b); }
    serial_print("\n");

    // ── buflen == 0 → hemen 0 döndür ────────────────────────────
    if (buflen == 0) {
        frame->rax = 0;
        return;
    }

    // ── NULL / geçersiz buf → EFAULT ────────────────────────────
    if (!is_valid_user_ptr(buf, buflen)) {
        serial_print("[SYSCALL] getrandom: EFAULT\n");
        frame->rax = SYSCALL_ERR_FAULT;
        return;
    }

    // ── Bilinmeyen flags → EINVAL ───────────────────────────────
    if (flags & ~(uint32_t)(GRND_NONBLOCK | GRND_RANDOM)) {
        serial_print("[SYSCALL] getrandom: bad flags -> EINVAL\n");
        frame->rax = SYSCALL_ERR_INVAL;
        return;
    }

    // ── PRNG ile doldur ─────────────────────────────────────────
    uint64_t filled = 0;
    while (filled < buflen) {
        uint64_t rnd = grnd_next();
        uint64_t chunk = buflen - filled;
        if (chunk > 8) chunk = 8;
        kmemcpy(buf + filled, &rnd, chunk);
        filled += chunk;
    }

    serial_print("[SYSCALL] getrandom: filled=");
    { char b[12]; int_to_str((int)filled, b); serial_print(b); }
    serial_print("\n");

    frame->rax = filled;
}

// ============================================================
// sys_set_tid_address — musl __init_tp() için  (SYS_SET_TID_ADDRESS = 218)
//
// Argüman: RDI = tidptr (kullanıcı alanı int* — thread çıkışında 0 yazılır)
// Dönüş:   mevcut task'ın TID'i (AscentOS'ta PID == TID)
//
// tidptr task_t içinde saklanır; sys_exit() bunu kullanarak
// futex_wake uygulayabilir (gelecek implementasyon).
// ============================================================
static void sys_set_tid_address(syscall_frame_t* frame) {
    int* tidptr = (int*)(uintptr_t)frame->rdi;

    serial_print("[SYSCALL] set_tid_address tidptr=0x");
    print_hex64((uint64_t)(uintptr_t)tidptr);
    serial_print("\n");

    // cur->pid sıfır olabilir (idle task); pg_current_pid() fallback ile
    // her zaman geçerli bir pid döndürür — sys_getpid ile aynı pattern.
    uint32_t tid = pg_current_pid();

    // tidptr kaydı — gelecek: cur->clear_child_tid = (uint64_t)tidptr
    (void)tidptr;

    frame->rax = (uint64_t)tid;

    serial_print("[SYSCALL] set_tid_address: tid=");
    { char b[12]; int_to_str((int)tid, b); serial_print(b); }
    serial_print("\n");
}

// ============================================================
// sys_set_robust_list — pthreads mutex recovery  (SYS_SET_ROBUST_LIST = 273)
//
// Argümanlar: RDI = head (robust_list_head*),  RSI = len
// Dönüş: 0 (başarı) | EINVAL (yanlış len)
//
// musl her thread init'te bu syscall'ı yapar.
// Gerçek robust futex recovery gelecekte implement edilecek;
// şimdilik len doğrulama + 0 dönüş yeterli (ENOSYS yerine 0 tercih edilir
// çünkü ENOSYS musl'un başlatmasını kesebilir).
// ============================================================
static void sys_set_robust_list(syscall_frame_t* frame) {
    uint64_t head = frame->rdi;
    uint64_t len  = frame->rsi;

    serial_print("[SYSCALL] set_robust_list head=0x");
    print_hex64(head);
    serial_print(" len=");
    { char b[12]; int_to_str((int)len, b); serial_print(b); }
    serial_print("\n");

    // Linux: len != sizeof(struct robust_list_head) → EINVAL
    if (len != ROBUST_LIST_HEAD_SIZE) {
        serial_print("[SYSCALL] set_robust_list: bad len -> EINVAL\n");
        frame->rax = SYSCALL_ERR_INVAL;
        return;
    }

    // head = NULL kabul edilir (robust list temizleme)
    // Gerçek implementasyon: cur->robust_list = head;
    (void)head;

    serial_print("[SYSCALL] set_robust_list: registered (stub)\n");
    frame->rax = 0;
}

// ============================================================
// sys_arch_prctl — x86-64'e özgü işlem/thread kontrol  (SYS_ARCH_PRCTL = 158)
//
// Desteklenen kodlar:
//   ARCH_SET_FS (0x1002): FS.base = addr  — TLS pointer kurulumu
//   ARCH_GET_FS (0x1003): *addr = FS.base — TLS pointer okuma
//   ARCH_SET_GS (0x1001): GS.base = addr
//   ARCH_GET_GS (0x1004): *addr = GS.base
//   ARCH_GET_CPUID / ARCH_SET_CPUID: stub, 0 döner
//   Bilinmeyen kod → EINVAL
//
// x86-64'te FS.base ve GS.base MSR_FS_BASE / MSR_GS_BASE üzerinden yazılır.
// FS.base = TLS pointer — pthreads ve newlib'in __thread değişkenleri için.
// ============================================================
#define MSR_FS_BASE  0xC0000100
#define MSR_GS_BASE  0xC0000101

// Per-task FS/GS base saklamak için (task_t henüz bu alanı taşımıyorsa burada tutarız)
static uint64_t current_fs_base = 0;
static uint64_t current_gs_base = 0;

static void sys_arch_prctl(syscall_frame_t* frame) {
    int      code = (int)(int64_t)frame->rdi;
    uint64_t addr = frame->rsi;

    serial_print("[SYSCALL] arch_prctl code=0x");
    { char b[12]; int_to_str(code, b); serial_print(b); }
    serial_print("\n");

    switch (code) {
    // ── ARCH_SET_FS: TLS pointer yaz ─────────────────────────────
    case ARCH_SET_FS:
        current_fs_base = addr;
        wrmsr(MSR_FS_BASE, addr);
        serial_print("[SYSCALL] arch_prctl: FS.base set\n");
        frame->rax = 0;
        break;

    // ── ARCH_GET_FS: TLS pointer oku ─────────────────────────────
    case ARCH_GET_FS: {
        uint64_t* out = (uint64_t*)(uintptr_t)addr;
        if (!is_valid_user_ptr(out, sizeof(uint64_t))) {
            frame->rax = SYSCALL_ERR_FAULT;
            return;
        }
        *out = current_fs_base;
        serial_print("[SYSCALL] arch_prctl: FS.base get\n");
        frame->rax = 0;
        break;
    }

    // ── ARCH_SET_GS ──────────────────────────────────────────────
    case ARCH_SET_GS:
        current_gs_base = addr;
        wrmsr(MSR_GS_BASE, addr);
        serial_print("[SYSCALL] arch_prctl: GS.base set\n");
        frame->rax = 0;
        break;

    // ── ARCH_GET_GS ──────────────────────────────────────────────
    case ARCH_GET_GS: {
        uint64_t* out = (uint64_t*)(uintptr_t)addr;
        if (!is_valid_user_ptr(out, sizeof(uint64_t))) {
            frame->rax = SYSCALL_ERR_FAULT;
            return;
        }
        *out = current_gs_base;
        serial_print("[SYSCALL] arch_prctl: GS.base get\n");
        frame->rax = 0;
        break;
    }

    // ── ARCH_GET_CPUID / ARCH_SET_CPUID: stub ────────────────────
    case ARCH_GET_CPUID:
        frame->rax = 1;  // CPUID izinli
        break;
    case ARCH_SET_CPUID:
        frame->rax = 0;  // sessizce kabul et
        break;

    // ── Bilinmeyen kod ───────────────────────────────────────────
    default:
        serial_print("[SYSCALL] arch_prctl: unknown code -> EINVAL\n");
        frame->rax = SYSCALL_ERR_INVAL;
        break;
    }
}

// ============================================================
// sys_clone — Linux x86-64 uyumlu thread/process oluşturma  (SYS_CLONE = 56)
//
// AscentOS implementasyon stratejisi:
//
//   ┌──────────────────────────────────────────────────────────────┐
//   │ flags & CLONE_THREAD == 0  →  fork() gibi davran            │
//   │   (CLONE_VFORK dahil; child_stack varsa child RSP'ye set)    │
//   │                                                              │
//   │ flags & CLONE_THREAD != 0  →  aynı adres alanında yeni task │
//   │   CLONE_SETTLS: FS.base = tls (R8)                          │
//   │   CLONE_PARENT_SETTID: *ptid = child_pid                    │
//   │   CLONE_CHILD_SETTID / CHILD_CLEARTID: kaydedilir           │
//   └──────────────────────────────────────────────────────────────┘
//
// Argümanlar (Linux x86-64):
//   RDI = flags   RSI = child_stack   RDX = ptid   R10 = ctid   R8 = tls
//
// Dönüş: parent → child_pid,  child → 0,  hata → negatif
// ============================================================
static void sys_clone(syscall_frame_t* frame) {
    uint64_t flags       = frame->rdi;
    uint64_t child_stack = frame->rsi;
    int*     ptid        = (int*)(uintptr_t)frame->rdx;
    int*     ctid        = (int*)(uintptr_t)frame->r10;
    uint64_t tls         = frame->r8;

    serial_print("[SYSCALL] clone flags=0x");
    print_hex64(flags);
    serial_print("\n");

    // ── CLONE_THREAD yoksa → fork() semantiği ────────────────────
    if (!(flags & CLONE_THREAD)) {
        // Mevcut fork implementasyonunu çağır
        sys_fork(frame);
        int64_t fork_ret = (int64_t)frame->rax;

        if (fork_ret > 0) {
            // Parent: child_stack verilmişse child'ın RSP'sini ayarla
            // (fork'tan dönen task burada değil; scheduler devralır)

            // CLONE_PARENT_SETTID: *ptid = child_pid
            if ((flags & CLONE_PARENT_SETTID) && ptid &&
                is_valid_user_ptr(ptid, sizeof(int))) {
                *ptid = (int)fork_ret;
            }
            serial_print("[SYSCALL] clone(no-thread): fork-like, child_pid=");
            { char b[12]; int_to_str((int)fork_ret, b); serial_print(b); }
            serial_print("\n");
        }
        // frame->rax zaten sys_fork tarafından set edildi
        return;
    }

    // ── CLONE_THREAD: yeni thread oluştur ────────────────────────
    // child_stack sağlanmalı
    if (child_stack == 0) {
        serial_print("[SYSCALL] clone(THREAD): no child_stack -> EINVAL\n");
        frame->rax = SYSCALL_ERR_INVAL;
        return;
    }

    // CLONE_SIGHAND + CLONE_VM zorunlu (Linux kuralı)
    if (!(flags & CLONE_VM) || !(flags & CLONE_SIGHAND)) {
        serial_print("[SYSCALL] clone(THREAD): missing VM|SIGHAND -> EINVAL\n");
        frame->rax = SYSCALL_ERR_INVAL;
        return;
    }

    // Yeni task oluştur (fork ile aynı altyapı)
    sys_fork(frame);
    int64_t child_pid = (int64_t)frame->rax;

    if (child_pid <= 0) {
        // fork başarısız veya child context
        return;
    }

    // Parent: child'a thread ayarlarını uygula
    // CLONE_SETTLS → child'ın FS.base = tls
    if (flags & CLONE_SETTLS) {
        wrmsr(MSR_FS_BASE, tls);
        current_fs_base = tls;
        serial_print("[SYSCALL] clone: TLS (FS.base) set\n");
    }

    // CLONE_PARENT_SETTID: *ptid = child_pid
    if ((flags & CLONE_PARENT_SETTID) && ptid &&
        is_valid_user_ptr(ptid, sizeof(int))) {
        *ptid = (int)child_pid;
    }

    // CLONE_CHILD_SETTID / CHILD_CLEARTID: task_t'ye kaydedilmeli
    // (task.h'da tid_addr alanı yoksa şimdilik logluyoruz)
    if (flags & (CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID)) {
        serial_print("[SYSCALL] clone: CHILD_SETTID/CLEARTID ctid registered\n");
        (void)ctid;  // gelecek: task->clear_child_tid = ctid
    }

    serial_print("[SYSCALL] clone(THREAD): child_pid=");
    { char b[12]; int_to_str((int)child_pid, b); serial_print(b); }
    serial_print("\n");

    // frame->rax = child_pid (sys_fork zaten set etti)
}

// ============================================================
// sys_readv — scatter/gather read  (SYS_READV = 19)
//
// musl stdio fread/scanf buffer'larını bu syscall ile okur.
// Her iovec için sys_read mantığını tekrar kullanır.
//
// Argümanlar: RDI=fd  RSI=iov[]  RDX=iovcnt
// Dönüş: toplam okunan byte | err
// ============================================================
static void sys_readv(syscall_frame_t* frame) {
    int       fd     = (int)(int64_t)frame->rdi;
    iovec_t*  iov    = (iovec_t*)(uintptr_t)frame->rsi;
    int       iovcnt = (int)(int64_t)frame->rdx;

    if (!is_valid_user_ptr(iov, sizeof(iovec_t) * (uint64_t)iovcnt)) {
        frame->rax = SYSCALL_ERR_FAULT;
        return;
    }
    if (iovcnt < 0 || iovcnt > 1024) {
        frame->rax = SYSCALL_ERR_INVAL;
        return;
    }

    int64_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        uint64_t len = iov[i].iov_len;
        if (len == 0) continue;
        if (!is_valid_user_ptr(iov[i].iov_base, len)) {
            if (total == 0) { frame->rax = SYSCALL_ERR_FAULT; return; }
            break;
        }

        // sys_read çağrısını simüle et: frame'i geçici olarak ayarla
        syscall_frame_t tmp = *frame;
        tmp.rdi = (uint64_t)fd;
        tmp.rsi = (uint64_t)(uintptr_t)iov[i].iov_base;
        tmp.rdx = len;
        sys_read(&tmp);

        int64_t got = (int64_t)tmp.rax;
        if (got < 0) {
            if (total == 0) { frame->rax = tmp.rax; return; }
            break;
        }
        total += got;
        if ((uint64_t)got < len) break;  // kısa okuma — daha fazla yok
    }

    frame->rax = (uint64_t)total;
}

// sys_writev — scatter/gather write  (SYS_WRITEV = 20)
//
// musl stdio fwrite/printf buffer'larını bu syscall ile flush eder.
// Her iovec için sys_write mantığını tekrar kullanır.
//
// Argümanlar: RDI=fd  RSI=iov[]  RDX=iovcnt
// Dönüş: toplam yazılan byte | err
// ============================================================
static void sys_writev(syscall_frame_t* frame) {
    int       fd     = (int)(int64_t)frame->rdi;
    iovec_t*  iov    = (iovec_t*)(uintptr_t)frame->rsi;
    int       iovcnt = (int)(int64_t)frame->rdx;

    serial_print("[SYSCALL] writev fd=");
    { char b[8]; int_to_str(fd, b); serial_print(b); }
    serial_print(" iovcnt=");
    { char b[8]; int_to_str(iovcnt, b); serial_print(b); }
    serial_print("\n");

    if (!is_valid_user_ptr(iov, sizeof(iovec_t) * (uint64_t)iovcnt)) {
        frame->rax = SYSCALL_ERR_FAULT;
        return;
    }
    if (iovcnt < 0 || iovcnt > 1024) {
        frame->rax = SYSCALL_ERR_INVAL;
        return;
    }

    task_t* cur = task_get_current();
    fd_entry_t* fde = cur ? fd_get(cur->fd_table, fd) : NULL;
    if (!fde) {
        frame->rax = SYSCALL_ERR_BADF;
        return;
    }

    int64_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        if (iov[i].iov_len == 0) continue;
        if (!is_valid_user_ptr(iov[i].iov_base, iov[i].iov_len)) {
            if (total == 0) frame->rax = SYSCALL_ERR_FAULT;
            else            frame->rax = (uint64_t)total;
            return;
        }

        const char* buf = (const char*)iov[i].iov_base;
        uint64_t    len = iov[i].iov_len;

        if (fde->type == FD_TYPE_SERIAL) {
            // stdout/stderr → vesa_write_buf (batch) + serial
            extern void vesa_write_buf(const char* buf, int len);
            vesa_write_buf(buf, (int)len);
            for (uint64_t j = 0; j < len; j++)
                serial_putchar(buf[j]);
            total += (int64_t)len;
        } else if (fde->type == FD_TYPE_PIPE && fde->pipe) {
            // Pipe write — sys_write ile aynı mantık
            pipe_buf_t* pb = fde->pipe;
            if (pb->write_closed) { frame->rax = SYSCALL_ERR_PIPE; return; }
            uint64_t written = 0;
            while (written < len && pb->bytes_avail < PIPE_BUF_SIZE) {
                pb->data[pb->write_pos] = buf[written++];
                pb->write_pos = (pb->write_pos + 1) % PIPE_BUF_SIZE;
                pb->bytes_avail++;
            }
            total += (int64_t)written;
        } else {
            // Dosya yazma — offset ilerlet
            fde->offset += len;
            total += (int64_t)len;
        }
    }

    frame->rax = (uint64_t)total;
}

// ============================================================
// sys_madvise — bellek kullanım tavsiyesi  (SYS_MADVISE = 28)
//
// musl malloc/free tarafından MADV_DONTNEED/FREE ile çağrılır.
// AscentOS'ta gerçek sayfa yönetimi gelecekte; şimdilik:
//   MADV_DONTNEED / MADV_FREE → 0 (kabul et, işlem yapma)
//   Geçersiz addr/len → EINVAL
//   Bilinmeyen advice → 0 (sessizce kabul et)
// ============================================================
static void sys_madvise(syscall_frame_t* frame) {
    uint64_t addr   = frame->rdi;
    uint64_t len    = frame->rsi;
    int      advice = (int)(int64_t)frame->rdx;

    serial_print("[SYSCALL] madvise advice=");
    { char b[8]; int_to_str(advice, b); serial_print(b); }
    serial_print("\n");

    // addr sayfa hizalı olmalı
    if (addr & (PAGE_SIZE_SC - 1)) {
        frame->rax = SYSCALL_ERR_INVAL;
        return;
    }
    if (len == 0) {
        frame->rax = 0;
        return;
    }

    // Tüm tavsiyeler için stub: 0 döndür
    (void)advice;
    frame->rax = 0;
}

// ============================================================
// sys_exit_group — tüm thread'leri sonlandır  (SYS_EXIT_GROUP = 231)
//
// musl exit() her zaman exit_group çağırır, SYS_EXIT değil.
// AscentOS şu an single-threaded; sys_exit ile aynı davranış.
// ============================================================
static void sys_exit_group(syscall_frame_t* frame) {
    serial_print("[SYSCALL] exit_group\n");
    // sys_exit ile aynı implementasyon
    sys_exit(frame);
}

// ============================================================
// sys_openat — dizin-göreceli dosya açma  (SYS_OPENAT = 257)
//
// musl open() wrapper'ı her zaman openat(AT_FDCWD, path, flags, mode)
// şeklinde çağırır. AT_FDCWD (-100) = geçerli çalışma dizini.
//
// Argümanlar: RDI=dirfd  RSI=path  RDX=flags  R10=mode
// Dönüş: fd | err
// ============================================================
static void sys_openat(syscall_frame_t* frame) {
    int         dirfd = (int)(int64_t)frame->rdi;
    const char* path  = (const char*)(uintptr_t)frame->rsi;
    int         flags = (int)(int64_t)frame->rdx;

    serial_print("[SYSCALL] openat dirfd=");
    { char b[8]; int_to_str(dirfd, b); serial_print(b); }
    serial_print(" path=");
    if (path && is_valid_user_ptr(path, 1)) serial_print(path);
    serial_print("\n");

    // NULL path → EFAULT (sys_open EINVAL döndürür, POSIX EFAULT ister)
    if (!path) {
        frame->rax = SYSCALL_ERR_FAULT;
        return;
    }
    if (!is_valid_user_ptr(path, 1)) {
        frame->rax = SYSCALL_ERR_FAULT;
        return;
    }

    // AT_FDCWD veya absolute path → sys_open'a yönlendir
    if (dirfd == AT_FDCWD || path[0] == '/') {
        frame->rdi = (uint64_t)(uintptr_t)path;
        frame->rsi = (uint64_t)(uint32_t)flags;
        sys_open(frame);
        return;
    }

    // Relative path + gerçek dirfd
    task_t* cur = task_get_current();
    fd_entry_t* fde = cur ? fd_get(cur->fd_table, dirfd) : NULL;
    if (!fde) {
        frame->rax = SYSCALL_ERR_BADF;
        return;
    }
    frame->rax = SYSCALL_ERR_INVAL;
}

// ============================================================
// sys_newfstatat — dizin-göreceli stat  (SYS_NEWFSTATAT = 262)
//
// musl stat() wrapper'ı newfstatat(AT_FDCWD, path, &st, 0) çağırır.
// lstat() için AT_SYMLINK_NOFOLLOW flag'i kullanılır.
//
// Argümanlar: RDI=dirfd  RSI=path  RDX=*stat  R10=flags
// Dönüş: 0 | err
// ============================================================
static void sys_newfstatat(syscall_frame_t* frame) {
    int         dirfd = (int)(int64_t)frame->rdi;
    const char* path  = (const char*)(uintptr_t)frame->rsi;
    // stat buf ve flags aynı registerlar
    uint64_t    stat_buf = frame->rdx;
    int         atflags  = (int)(int64_t)frame->r10;

    serial_print("[SYSCALL] newfstatat dirfd=");
    { char b[8]; int_to_str(dirfd, b); serial_print(b); }
    if (path && is_valid_user_ptr(path, 1)) {
        serial_print(" path="); serial_print(path);
    }
    serial_print("\n");

    // AT_EMPTY_PATH: dirfd'nin kendisini stat et (fstat gibi)
    if ((atflags & AT_EMPTY_PATH) && (path == NULL || path[0] == '\0')) {
        frame->rdi = (uint64_t)(uint32_t)dirfd;
        frame->rsi = stat_buf;
        sys_fstat(frame);
        return;
    }

    // AT_FDCWD + absolute path → sys_stat veya sys_lstat'a yönlendir
    if (dirfd == AT_FDCWD || (path && path[0] == '/')) {
        frame->rdi = (uint64_t)(uintptr_t)path;
        frame->rsi = stat_buf;
        if (atflags & AT_SYMLINK_NOFOLLOW) {
            sys_lstat(frame);
        } else {
            sys_stat(frame);
        }
        return;
    }

    // Relative path + real dirfd → gelecek implementasyon
    frame->rax = SYSCALL_ERR_INVAL;
}

// ============================================================
// sys_prlimit64 — işlem kaynak limiti al/set  (SYS_PRLIMIT64 = 302)
//
// musl getrlimit/setrlimit yerine bunu kullanır.
// pid=0 → mevcut process.
//
// Argümanlar: RDI=pid  RSI=resource  RDX=*new_limit  R10=*old_limit
// Dönüş: 0 | err
// ============================================================
static void sys_prlimit64(syscall_frame_t* frame) {
    int          pid_arg   = (int)(int64_t)frame->rdi;
    int          resource  = (int)(int64_t)frame->rsi;
    rlimit64_t*  new_lim   = (rlimit64_t*)(uintptr_t)frame->rdx;
    rlimit64_t*  old_lim   = (rlimit64_t*)(uintptr_t)frame->r10;

    serial_print("[SYSCALL] prlimit64 resource=");
    { char b[8]; int_to_str(resource, b); serial_print(b); }
    serial_print("\n");

    // pid != 0 ve kendi pid'imiz değilse EPERM
    if (pid_arg != 0) {
        task_t* cur = task_get_current();
        uint32_t my_pid = cur ? cur->pid : 0;
        if ((uint32_t)pid_arg != my_pid) {
            frame->rax = SYSCALL_ERR_PERM;
            return;
        }
    }

    // resource sınır kontrolü
    if (resource < 0 || resource >= RLIMIT_NLIMITS) {
        frame->rax = SYSCALL_ERR_INVAL;
        return;
    }

    // old_limit istendi → doğrudan kernel_rlimits'ten kopyala
    if (old_lim) {
        if (!is_valid_user_ptr(old_lim, sizeof(rlimit64_t))) {
            frame->rax = SYSCALL_ERR_FAULT;
            return;
        }
        old_lim->rlim_cur = kernel_rlimits[resource].rlim_cur;
        old_lim->rlim_max = kernel_rlimits[resource].rlim_max;
        serial_print("[SYSCALL] prlimit64: old_lim cur=");
        { char b[12]; int_to_str((int)old_lim->rlim_cur, b); serial_print(b); }
        serial_print("\n");
    }

    // new_limit verildi → kernel_rlimits güncelle
    if (new_lim) {
        if (!is_valid_user_ptr(new_lim, sizeof(rlimit64_t))) {
            frame->rax = SYSCALL_ERR_FAULT;
            return;
        }
        // Hard limit artırımı root gerektiriyor — stub: cur <= max kontrolü
        if (new_lim->rlim_cur > kernel_rlimits[resource].rlim_max &&
            kernel_rlimits[resource].rlim_max != RLIM_INFINITY) {
            frame->rax = SYSCALL_ERR_PERM;
            return;
        }
        if (new_lim->rlim_cur > new_lim->rlim_max) {
            frame->rax = SYSCALL_ERR_INVAL;
            return;
        }
        kernel_rlimits[resource].rlim_cur = new_lim->rlim_cur;
        kernel_rlimits[resource].rlim_max = new_lim->rlim_max;
    }

    frame->rax = 0;
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
    // v17 – POSIX saat arayüzü
    case SYS_CLOCK_GETTIME: sys_clock_gettime(frame); break;
    case SYS_CLOCK_GETRES:  sys_clock_getres(frame);  break;
    // v18 – zamanlayıcı & dosya kırpma
    case SYS_ALARM:         sys_alarm(frame);          break;
    case SYS_FTRUNCATE:     sys_ftruncate(frame);      break;
    case SYS_TRUNCATE:      sys_truncate(frame);       break;
    // v19 – kaynak limitleri
    case SYS_GETRLIMIT:     sys_getrlimit(frame);      break;
    case SYS_SETRLIMIT:     sys_setrlimit(frame);      break;
    // v20 – newlib uyumu & bash eksikleri
    case SYS_LSTAT:         sys_lstat(frame);          break;
    case SYS_LINK:          sys_link(frame);           break;
    case SYS_TIMES:         sys_times(frame);          break;
    // v21 – bash port eksikleri (umask / symlink / readlink)
    case SYS_UMASK:         sys_umask(frame);          break;
    case SYS_SYMLINK:       sys_symlink(frame);        break;
    case SYS_READLINK:      sys_readlink(frame);       break;
    // v22 – dosya/bellek izinleri + atomik pipe
    case SYS_CHMOD:         sys_chmod(frame);          break;
    case SYS_MPROTECT:      sys_mprotect(frame);       break;
    case SYS_PIPE2:         sys_pipe2(frame);          break;
    // v23 – grup listesi (bash $GROUPS, id builtin)
    case SYS_GETGROUPS:     sys_getgroups(frame);      break;
    // v24 – futex & getrandom
    case SYS_FUTEX:         sys_futex(frame);          break;
    case SYS_GETRANDOM:     sys_getrandom(frame);      break;
    // v25 – arch_prctl & clone
    case SYS_ARCH_PRCTL:      sys_arch_prctl(frame);      break;
    case SYS_CLONE:           sys_clone(frame);            break;
    // v26 – musl libc başlatma (set_tid_address, set_robust_list)
    case SYS_SET_TID_ADDRESS:  sys_set_tid_address(frame);  break;
    case SYS_SET_ROBUST_LIST:  sys_set_robust_list(frame);  break;
    // v27 – musl libc çalışması (writev, madvise, exit_group, openat, newfstatat, prlimit64)
    case SYS_WRITEV:           sys_writev(frame);           break;
    case 19:                   sys_readv(frame);            break;  /* SYS_READV */
    case SYS_MADVISE:          sys_madvise(frame);          break;
    case SYS_EXIT_GROUP:       sys_exit_group(frame);       break;
    case SYS_OPENAT:           sys_openat(frame);           break;
    case SYS_NEWFSTATAT:       sys_newfstatat(frame);       break;
    case SYS_PRLIMIT64:        sys_prlimit64(frame);        break;
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