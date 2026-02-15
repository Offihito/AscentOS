// syscall.c - System Call Implementation for AscentOS
// PHASE 3: Expanded Syscall Interface
//   - File I/O  : open, read, write, close, stat, fstat, lseek
//   - Pipe I/O  : pipe, dup, dup2
//   - Process   : exit, getpid, fork (stub), execve (stub), waitpid, kill
//   - Memory    : brk, mmap (stub), munmap (stub)
//   - IPC       : shmget, shmmap, shmunmap, msgpost, msgrecv
//   - Ascent    : debug, info, yield, sleep, gettime

#include "syscall.h"
#include "task.h"
#include "scheduler.h"
#include <stddef.h>

// ── External kernel helpers ──────────────────────────────────────────────────
extern void    serial_print(const char* str);
extern void    serial_putchar(char c);
extern void    int_to_str(int num, char* str);
extern uint64_t get_system_ticks(void);

// ── FAT32 helpers (disk64.h interface) ──────────────────────────────────────
extern int      fat32_create_file(const char* name);
extern int      fat32_write_file(const char* name, const uint8_t* data, uint32_t size);
extern int      fat32_read_file(const char* name, uint8_t* buf, uint32_t max);
extern uint32_t fat32_file_size(const char* name);
extern int      fat32_delete_file(const char* name);

// ── Memory helpers ───────────────────────────────────────────────────────────
extern void* kmalloc(uint64_t size);
extern void  kfree(void* ptr);

// =============================================================================
// INTERNAL HELPERS
// =============================================================================

static void u64_to_str(uint64_t num, char* str) {
    if (num == 0) { str[0] = '0'; str[1] = '\0'; return; }
    char tmp[24]; int i = 0;
    while (num > 0) { tmp[i++] = '0' + (num % 10); num /= 10; }
    for (int j = 0; j < i; j++) str[j] = tmp[i - j - 1];
    str[i] = '\0';
}

static int k_strlen(const char* s) {
    int n = 0; while (s[n]) n++; return n;
}
static void k_memset(void* dst, uint8_t v, uint64_t n) {
    uint8_t* d = (uint8_t*)dst; while (n--) *d++ = v;
}
static void k_memcpy(void* dst, const void* src, uint64_t n) {
    uint8_t* d = (uint8_t*)dst; const uint8_t* s = (const uint8_t*)src;
    while (n--) *d++ = *s++;
}
static int k_strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *(const uint8_t*)a - *(const uint8_t*)b;
}
static void k_strncpy(char* dst, const char* src, int n) {
    int i = 0;
    while (i < n - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

// =============================================================================
// GLOBAL STATE
// =============================================================================

static int            syscall_enabled = 0;
static syscall_stats_t stats;

// ── Global FD table (one table per system; task-local tables future work) ────
// Index 0-2 are pre-filled as stdin/stdout/stderr (serial).
static fd_entry_t global_fd_table[MAX_OPEN_FILES];

// ── Pipe buffers (kernel-owned) ───────────────────────────────────────────────
static pipe_buf_t pipe_pool[8];
static int        pipe_pool_init_done = 0;

// ── Shared memory segments ───────────────────────────────────────────────────
static shm_segment_t shm_segments[SHM_MAX_SEGS];
static int           shm_init_done = 0;

// ── Message queues ────────────────────────────────────────────────────────────
static msg_queue_t msg_queues[MSG_MAX_QUEUES];
static int         msg_init_done = 0;

// ── Heap brk pointer (per-process in future; global stub for now) ─────────────
static uint8_t* process_heap_start = NULL;
static uint8_t* process_heap_end   = NULL;
#define HEAP_INITIAL_SIZE (64 * 1024)   // 64 KB initial heap

// =============================================================================
// FD TABLE IMPLEMENTATION
// =============================================================================

void fd_table_init(fd_entry_t* table) {
    k_memset(table, 0, sizeof(fd_entry_t) * MAX_OPEN_FILES);

    // FD 0: stdin  → serial (read-capable; currently returns EAGAIN)
    table[0].type      = FD_TYPE_SERIAL;
    table[0].flags     = O_RDONLY;
    table[0].ref_count = 1;
    k_strncpy(table[0].path, "stdin", 6);

    // FD 1: stdout → serial
    table[1].type      = FD_TYPE_SERIAL;
    table[1].flags     = O_WRONLY;
    table[1].ref_count = 1;
    k_strncpy(table[1].path, "stdout", 7);

    // FD 2: stderr → serial
    table[2].type      = FD_TYPE_SERIAL;
    table[2].flags     = O_WRONLY;
    table[2].ref_count = 1;
    k_strncpy(table[2].path, "stderr", 7);
}

int fd_alloc(fd_entry_t* table, int type, int flags, const char* path) {
    for (int i = 3; i < MAX_OPEN_FILES; i++) {   // 0-2 are reserved
        if (table[i].type == FD_TYPE_NONE) {
            table[i].type      = type;
            table[i].flags     = flags;
            table[i].offset    = 0;
            table[i].ref_count = 1;
            table[i].private_data = NULL;
            if (path) k_strncpy(table[i].path, path, 32);
            else      table[i].path[0] = '\0';
            return i;
        }
    }
    return -1;  // EMFILE
}

void fd_free(fd_entry_t* table, int fd) {
    if (fd < 0 || fd >= MAX_OPEN_FILES) return;
    if (table[fd].ref_count > 1) { table[fd].ref_count--; return; }
    k_memset(&table[fd], 0, sizeof(fd_entry_t));
}

fd_entry_t* fd_get(fd_entry_t* table, int fd) {
    if (fd < 0 || fd >= MAX_OPEN_FILES) return NULL;
    if (table[fd].type == FD_TYPE_NONE) return NULL;
    return &table[fd];
}

// Return the global fd table (task-local tables are future work)
fd_entry_t* syscall_get_fd_table(void) {
    return global_fd_table;
}

// =============================================================================
// PIPE POOL HELPERS
// =============================================================================

static void pipe_pool_init(void) {
    if (pipe_pool_init_done) return;
    k_memset(pipe_pool, 0, sizeof(pipe_pool));
    pipe_pool_init_done = 1;
}

static pipe_buf_t* pipe_alloc(void) {
    pipe_pool_init();
    for (int i = 0; i < 8; i++) {
        if (!pipe_pool[i].read_end_open && !pipe_pool[i].write_end_open) {
            k_memset(&pipe_pool[i], 0, sizeof(pipe_buf_t));
            pipe_pool[i].read_end_open  = 1;
            pipe_pool[i].write_end_open = 1;
            return &pipe_pool[i];
        }
    }
    return NULL;
}

// =============================================================================
// SYSCALL INITIALIZATION
// =============================================================================

void syscall_init(void) {
    if (syscall_enabled) { serial_print("[SYSCALL] Already initialized\n"); return; }

    serial_print("[SYSCALL] Initializing Phase 3 syscall system...\n");

    // Reset stats
    k_memset(&stats, 0, sizeof(syscall_stats_t));

    // Initialize global FD table
    fd_table_init(global_fd_table);
    serial_print("[SYSCALL] FD table initialized (stdin/stdout/stderr ready)\n");

    // Initialize IPC subsystems
    pipe_pool_init();
    k_memset(shm_segments, 0, sizeof(shm_segments));
    shm_init_done = 1;
    k_memset(msg_queues, 0, sizeof(msg_queues));
    msg_init_done = 1;
    serial_print("[SYSCALL] IPC subsystems initialized (pipe/shm/msgq)\n");

    // Setup MSRs
    extern void syscall_setup_msrs(void);
    syscall_setup_msrs();

    syscall_enabled = 1;
    serial_print("[SYSCALL] Phase 3 syscall system ready\n");
}

int syscall_is_enabled(void) { return syscall_enabled; }

// =============================================================================
// SYSCALL DISPATCHER
// =============================================================================

int64_t syscall_handler(uint64_t syscall_num,
                        uint64_t arg1, uint64_t arg2, uint64_t arg3,
                        uint64_t arg4, uint64_t arg5) {
    int64_t result = SYSCALL_ERROR;
    stats.total_syscalls++;

    if (syscall_num >= SYSCALL_MAX) {
        stats.invalid_syscalls++;
        return ENOSYS;
    }
    stats.syscall_counts[syscall_num]++;

    switch (syscall_num) {
        // ── File I/O ──────────────────────────────────────────────────────────
        case SYS_READ:    result = sys_read ((int)arg1, (void*)arg2, arg3); break;
        case SYS_WRITE:   result = sys_write((int)arg1, (const void*)arg2, arg3); break;
        case SYS_OPEN:    result = sys_open ((const char*)arg1, (int)arg2, (int)arg3); break;
        case SYS_CLOSE:   result = sys_close((int)arg1); break;
        case SYS_STAT:    result = sys_stat ((const char*)arg1, (ascent_stat_t*)arg2); break;
        case SYS_FSTAT:   result = sys_fstat((int)arg1, (ascent_stat_t*)arg2); break;
        case SYS_LSEEK:   result = sys_lseek((int)arg1, (int64_t)arg2, (int)arg3); break;
        case SYS_PIPE:    result = sys_pipe ((int*)arg1); break;
        case SYS_DUP:     result = sys_dup  ((int)arg1); break;
        case SYS_DUP2:    result = sys_dup2 ((int)arg1, (int)arg2); break;
        // ── Process ──────────────────────────────────────────────────────────
        case SYS_EXIT:    result = sys_exit   ((int)arg1); break;
        case SYS_GETPID:  result = sys_getpid(); break;
        case SYS_FORK:    result = sys_fork  (); break;
        case SYS_EXECVE:  result = sys_execve((const char*)arg1,(char*const*)arg2,(char*const*)arg3); break;
        case SYS_WAITPID: result = sys_waitpid((int)arg1,(int*)arg2,(int)arg3); break;
        case SYS_WAIT4:   result = sys_wait4  ((int)arg1,(int*)arg2,(int)arg3,(void*)arg4); break;
        case SYS_KILL:    result = sys_kill   ((int)arg1,(int)arg2); break;
        case SYS_GETUID:  result = sys_getuid(); break;
        case SYS_GETGID:  result = sys_getgid(); break;
        // ── Memory ───────────────────────────────────────────────────────────
        case SYS_BRK:     result = sys_brk   ((void*)arg1); break;
        case SYS_MMAP:    result = sys_mmap  ((void*)arg1,arg2,(int)arg3,(int)arg4,(int)arg5,0); break;
        case SYS_MUNMAP:  result = sys_munmap((void*)arg1,arg2); break;
        // ── AscentOS-specific ─────────────────────────────────────────────────
        case SYS_ASCENT_DEBUG:    result = sys_ascent_debug   ((const char*)arg1); break;
        case SYS_ASCENT_INFO:     result = sys_ascent_info    ((void*)arg1,arg2); break;
        case SYS_ASCENT_YIELD:    result = sys_ascent_yield   (); break;
        case SYS_ASCENT_SLEEP:    result = sys_ascent_sleep   (arg1); break;
        case SYS_ASCENT_GETTIME:  result = sys_ascent_gettime (); break;
        case SYS_ASCENT_SHMGET:   result = sys_ascent_shmget  ((int)arg1, arg2); break;
        case SYS_ASCENT_SHMMAP:   result = sys_ascent_shmmap  ((int)arg1); break;
        case SYS_ASCENT_SHMUNMAP: result = sys_ascent_shmunmap((int)arg1); break;
        case SYS_ASCENT_MSGPOST:  result = sys_ascent_msgpost ((int)arg1,(const void*)arg2,arg3); break;
        case SYS_ASCENT_MSGRECV:  result = sys_ascent_msgrecv ((int)arg1,(void*)arg2,arg3); break;

        default:
            // syscall_num is within SYSCALL_MAX range but has no handler.
            // Count as failed, NOT as invalid (invalid = out of range, caught above).
            result = ENOSYS;
            break;
    }

    // Only count as "failed" if the return is the generic SYSCALL_ERROR (-1).
    // Named error codes (ENOSYS, EINVAL, ECHILD, EAGAIN, etc.) are expected
    // outcomes and should NOT inflate the failed counter.
    if (result == SYSCALL_ERROR) stats.failed_syscalls++;
    return result;
}

// =============================================================================
// FILE I/O SYSCALLS
// =============================================================================

int64_t sys_read(int fd, void* buf, uint64_t count) {
    if (!buf || count == 0) return EINVAL;

    fd_entry_t* table = syscall_get_fd_table();
    fd_entry_t* entry = fd_get(table, fd);
    if (!entry) return EBADF;
    if (entry->flags == O_WRONLY) return EBADF;

    if (entry->type == FD_TYPE_SERIAL) {
        // stdin: keyboard input is not buffered yet → return EAGAIN
        return EAGAIN;
    }

    if (entry->type == FD_TYPE_FAT32) {
        // Read a chunk from the FAT32 file at the current offset
        uint32_t file_size = fat32_file_size(entry->path);
        if (file_size == 0) return ENOENT;

        if (entry->offset >= file_size) return 0; // EOF

        // Read the whole file into a temporary buffer
        uint8_t* tmp = (uint8_t*)kmalloc(file_size + 1);
        if (!tmp) return ENOMEM;

        int n = fat32_read_file(entry->path, tmp, file_size);
        if (n <= 0) { kfree(tmp); return ENOENT; }

        uint64_t available = (uint64_t)n - entry->offset;
        uint64_t to_copy   = (count < available) ? count : available;
        k_memcpy(buf, tmp + entry->offset, to_copy);
        entry->offset += to_copy;
        kfree(tmp);
        return (int64_t)to_copy;
    }

    if (entry->type == FD_TYPE_PIPE_R) {
        pipe_buf_t* pb = (pipe_buf_t*)entry->private_data;
        if (!pb) return EBADF;
        if (pb->count == 0) {
            if (!pb->write_end_open) return 0;  // EOF
            return EAGAIN;
        }
        uint64_t copied = 0;
        uint8_t* out = (uint8_t*)buf;
        while (copied < count && pb->count > 0) {
            out[copied++] = pb->buf[pb->read_pos];
            pb->read_pos  = (pb->read_pos + 1) % PIPE_BUF_SIZE;
            pb->count--;
        }
        return (int64_t)copied;
    }

    return ENOSYS;
}

int64_t sys_write(int fd, const void* buf, uint64_t count) {
    if (!buf || count == 0) return EINVAL;

    fd_entry_t* table = syscall_get_fd_table();
    fd_entry_t* entry = fd_get(table, fd);
    if (!entry) return EBADF;
    if (entry->flags == O_RDONLY) return EBADF;

    if (entry->type == FD_TYPE_SERIAL) {
        const char* str = (const char*)buf;
        for (uint64_t i = 0; i < count; i++) serial_putchar(str[i]);
        return (int64_t)count;
    }

    if (entry->type == FD_TYPE_FAT32) {
        // Append-or-truncate write: read existing content if append, overwrite if trunc
        uint32_t existing_size = fat32_file_size(entry->path);
        uint8_t* write_buf = NULL;
        uint32_t write_size = 0;

        if (entry->flags & O_APPEND) {
            // Append: read existing + concat new data
            write_size = existing_size + (uint32_t)count;
            write_buf  = (uint8_t*)kmalloc(write_size);
            if (!write_buf) return ENOMEM;
            if (existing_size > 0)
                fat32_read_file(entry->path, write_buf, existing_size);
            k_memcpy(write_buf + existing_size, buf, count);
            entry->offset = write_size;
        } else {
            // Overwrite from current offset
            uint32_t new_size = (uint32_t)(entry->offset + count);
            if (new_size < existing_size) new_size = existing_size;
            write_buf  = (uint8_t*)kmalloc(new_size);
            if (!write_buf) return ENOMEM;
            if (existing_size > 0)
                fat32_read_file(entry->path, write_buf, existing_size);
            k_memcpy(write_buf + entry->offset, buf, count);
            entry->offset += count;
            write_size = new_size;
        }

        int ok = fat32_write_file(entry->path, write_buf, write_size);
        kfree(write_buf);
        return ok ? (int64_t)count : ENOSPC;
    }

    if (entry->type == FD_TYPE_PIPE_W) {
        pipe_buf_t* pb = (pipe_buf_t*)entry->private_data;
        if (!pb) return EBADF;
        if (!pb->read_end_open) return EPIPE;
        const uint8_t* in = (const uint8_t*)buf;
        uint64_t written = 0;
        while (written < count) {
            if (pb->count >= PIPE_BUF_SIZE) break;  // pipe full
            pb->buf[pb->write_pos] = in[written++];
            pb->write_pos = (pb->write_pos + 1) % PIPE_BUF_SIZE;
            pb->count++;
        }
        return (int64_t)written;
    }

    return ENOSYS;
}

int64_t sys_open(const char* path, int flags, int mode) {
    (void)mode;
    if (!path) return EFAULT;

    fd_entry_t* table = syscall_get_fd_table();

    // Check if file exists on FAT32
    uint32_t sz = fat32_file_size(path);
    int exists = (sz > 0 || fat32_file_size(path) == 0);
    // fat32_file_size returns 0 both for "empty file" and "not found"
    // We try a read to distinguish
    {
        uint8_t probe[1];
        int r = fat32_read_file(path, probe, 1);
        exists = (r >= 0 || sz > 0);
        if (r < 0 && sz == 0) exists = 0;
    }

    if (!exists) {
        if (!(flags & O_CREAT)) return ENOENT;
        // Create the file
        if (!fat32_create_file(path)) return ENOSPC;
    } else if (flags & O_TRUNC) {
        // Truncate: overwrite with empty content
        fat32_write_file(path, (const uint8_t*)"", 0);
    }

    int fd = fd_alloc(table, FD_TYPE_FAT32, flags, path);
    if (fd < 0) return EMFILE;

    if (flags & O_APPEND) {
        // Position at end
        table[fd].offset = fat32_file_size(path);
    }

    serial_print("[SYS_OPEN] Opened '");
    serial_print(path);
    serial_print("' -> fd=");
    char tmp[8]; int_to_str(fd, tmp); serial_print(tmp);
    serial_print("\n");

    return fd;
}

int64_t sys_close(int fd) {
    if (fd < 0 || fd > 2) {
        // Don't allow closing stdin/stdout/stderr
    }
    fd_entry_t* table = syscall_get_fd_table();
    fd_entry_t* entry = fd_get(table, fd);
    if (!entry) return EBADF;

    // Handle pipe close
    if (entry->type == FD_TYPE_PIPE_W) {
        pipe_buf_t* pb = (pipe_buf_t*)entry->private_data;
        if (pb) pb->write_end_open = 0;
    } else if (entry->type == FD_TYPE_PIPE_R) {
        pipe_buf_t* pb = (pipe_buf_t*)entry->private_data;
        if (pb) pb->read_end_open = 0;
    }

    fd_free(table, fd);
    return SYSCALL_SUCCESS;
}

int64_t sys_stat(const char* path, ascent_stat_t* st) {
    if (!path || !st) return EFAULT;
    uint32_t sz = fat32_file_size(path);
    // Try to distinguish "no file" vs "empty file" with a read probe
    uint8_t probe[1];
    int r = fat32_read_file(path, probe, 1);
    if (r < 0 && sz == 0) return ENOENT;

    st->st_mode   = S_IFREG | S_IRUSR | S_IWUSR;
    st->st_size   = sz;
    st->st_blocks = (sz + 511) / 512;
    return SYSCALL_SUCCESS;
}

int64_t sys_fstat(int fd, ascent_stat_t* st) {
    if (!st) return EFAULT;
    fd_entry_t* table = syscall_get_fd_table();
    fd_entry_t* entry = fd_get(table, fd);
    if (!entry) return EBADF;

    if (entry->type == FD_TYPE_SERIAL) {
        st->st_mode   = S_IFCHR | S_IRUSR | S_IWUSR;
        st->st_size   = 0;
        st->st_blocks = 0;
        return SYSCALL_SUCCESS;
    }
    if (entry->type == FD_TYPE_FAT32) {
        return sys_stat(entry->path, st);
    }
    if (entry->type == FD_TYPE_PIPE_R || entry->type == FD_TYPE_PIPE_W) {
        st->st_mode   = S_IFIFO | S_IRUSR | S_IWUSR;
        st->st_size   = 0;
        st->st_blocks = 0;
        return SYSCALL_SUCCESS;
    }
    return ENOSYS;
}

int64_t sys_lseek(int fd, int64_t offset, int whence) {
    fd_entry_t* table = syscall_get_fd_table();
    fd_entry_t* entry = fd_get(table, fd);
    if (!entry) return EBADF;
    if (entry->type != FD_TYPE_FAT32) return ENOSYS;

    uint32_t file_size = fat32_file_size(entry->path);
    int64_t  new_offset;

    switch (whence) {
        case SEEK_SET: new_offset = offset; break;
        case SEEK_CUR: new_offset = (int64_t)entry->offset + offset; break;
        case SEEK_END: new_offset = (int64_t)file_size + offset; break;
        default:       return EINVAL;
    }
    if (new_offset < 0) return EINVAL;
    entry->offset = (uint64_t)new_offset;
    return new_offset;
}

int64_t sys_pipe(int pipefd[2]) {
    if (!pipefd) return EFAULT;

    pipe_buf_t* pb = pipe_alloc();
    if (!pb) return ENOMEM;

    fd_entry_t* table = syscall_get_fd_table();

    // Allocate read end
    int rfd = fd_alloc(table, FD_TYPE_PIPE_R, O_RDONLY, "pipe:r");
    if (rfd < 0) { pb->read_end_open = pb->write_end_open = 0; return EMFILE; }

    // Allocate write end
    int wfd = fd_alloc(table, FD_TYPE_PIPE_W, O_WRONLY, "pipe:w");
    if (wfd < 0) {
        fd_free(table, rfd);
        pb->read_end_open = pb->write_end_open = 0;
        return EMFILE;
    }

    table[rfd].private_data = pb;
    table[wfd].private_data = pb;

    pipefd[0] = rfd;
    pipefd[1] = wfd;

    serial_print("[SYS_PIPE] Created pipe rfd=");
    char tmp[8]; int_to_str(rfd, tmp); serial_print(tmp);
    serial_print(" wfd="); int_to_str(wfd, tmp); serial_print(tmp);
    serial_print("\n");

    return SYSCALL_SUCCESS;
}

int64_t sys_dup(int oldfd) {
    fd_entry_t* table = syscall_get_fd_table();
    fd_entry_t* entry = fd_get(table, oldfd);
    if (!entry) return EBADF;

    // Find a free slot
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (table[i].type == FD_TYPE_NONE) {
            table[i] = *entry;   // copy
            table[i].ref_count = 1;
            entry->ref_count++;  // bump original ref count too
            return i;
        }
    }
    return EMFILE;
}

int64_t sys_dup2(int oldfd, int newfd) {
    if (newfd < 0 || newfd >= MAX_OPEN_FILES) return EBADF;
    fd_entry_t* table = syscall_get_fd_table();
    fd_entry_t* entry = fd_get(table, oldfd);
    if (!entry) return EBADF;

    // Close newfd if already open
    if (table[newfd].type != FD_TYPE_NONE) fd_free(table, newfd);

    table[newfd] = *entry;
    table[newfd].ref_count = 1;
    entry->ref_count++;
    return newfd;
}

// =============================================================================
// PROCESS MANAGEMENT SYSCALLS
// =============================================================================

int64_t sys_exit(int status) {
    serial_print("[SYS_EXIT] Task exiting with status: ");
    char buf[16]; int_to_str(status, buf); serial_print(buf); serial_print("\n");
    task_exit();
    return 0;  // Never reached
}

int64_t sys_getpid(void) {
    task_t* cur = task_get_current();
    return cur ? (int64_t)cur->pid : -1;
}

int64_t sys_fork(void) {
    // Full fork requires page-table COW and FD duplication — stub for now
    serial_print("[SYS_FORK] fork() not implemented (Phase 4)\n");
    return ENOSYS;
}

int64_t sys_execve(const char* path, char* const argv[], char* const envp[]) {
    (void)argv; (void)envp;
    // Full execve requires ELF loader integration — stub for now
    serial_print("[SYS_EXECVE] execve() stub: ");
    if (path) serial_print(path);
    serial_print("\n");
    return ENOSYS;
}

int64_t sys_waitpid(int pid, int* status, int options) {
    (void)options;
    // Find the task; if terminated, collect status
    task_t* t = task_find_by_pid((uint32_t)pid);
    if (!t) {
        if (status) *status = 0;
        return ECHILD;
    }
    if (t->state == TASK_STATE_TERMINATED) {
        if (status) *status = 0;
        return pid;
    }
    // Task still running → would block; for now return EAGAIN
    return EAGAIN;
}

int64_t sys_wait4(int pid, int* status, int options, void* rusage) {
    (void)rusage;
    return sys_waitpid(pid, status, options);
}

int64_t sys_kill(int pid, int sig) {
    if (pid <= 0) return EINVAL;
    task_t* t = task_find_by_pid((uint32_t)pid);
    if (!t) return EINVAL;

    // Only handle SIGKILL (9) and SIGTERM (15) for now
    if (sig == 9 || sig == 15) {
        serial_print("[SYS_KILL] Terminating PID ");
        char tmp[16]; int_to_str(pid, tmp); serial_print(tmp); serial_print("\n");
        task_terminate(t);
        return SYSCALL_SUCCESS;
    }
    return ENOSYS;  // Other signals not implemented
}

int64_t sys_getuid(void) { return 0; }  // Root only for now
int64_t sys_getgid(void) { return 0; }

// =============================================================================
// MEMORY MANAGEMENT SYSCALLS
// =============================================================================

int64_t sys_brk(void* addr) {
    if (!process_heap_start) {
        // First call: allocate initial heap
        process_heap_start = (uint8_t*)kmalloc(HEAP_INITIAL_SIZE);
        if (!process_heap_start) return ENOMEM;
        process_heap_end = process_heap_start + HEAP_INITIAL_SIZE;
        serial_print("[SYS_BRK] Heap initialized\n");
    }

    if (!addr) {
        // brk(NULL) → return current break
        return (int64_t)(uint64_t)process_heap_end;
    }

    uint8_t* new_end = (uint8_t*)addr;
    if (new_end < process_heap_start) return EINVAL;
    if (new_end > process_heap_start + HEAP_INITIAL_SIZE) return ENOMEM;

    process_heap_end = new_end;
    return (int64_t)(uint64_t)process_heap_end;
}

int64_t sys_mmap(void* addr, uint64_t length, int prot, int flags, int fd, uint64_t offset) {
    (void)addr; (void)prot; (void)flags; (void)fd; (void)offset;
    // Anonymous mmap: just kmalloc for now
    if (length == 0) return EINVAL;
    void* mem = kmalloc(length);
    if (!mem) return ENOMEM;
    k_memset(mem, 0, length);
    serial_print("[SYS_MMAP] Allocated ");
    char tmp[16]; u64_to_str(length, tmp); serial_print(tmp);
    serial_print(" bytes\n");
    return (int64_t)(uint64_t)mem;
}

int64_t sys_munmap(void* addr, uint64_t length) {
    (void)length;
    if (!addr) return EINVAL;
    kfree(addr);
    return SYSCALL_SUCCESS;
}

// =============================================================================
// ASCENTOS-SPECIFIC SYSCALLS
// =============================================================================

int64_t sys_ascent_debug(const char* message) {
    if (!message) return EFAULT;
    serial_print("[USER DEBUG] ");
    serial_print(message);
    serial_print("\n");
    return SYSCALL_SUCCESS;
}

int64_t sys_ascent_info(void* info_buffer, uint64_t buffer_size) {
    if (!info_buffer || buffer_size < 64) return EINVAL;

    // Fill with a brief system info string
    char* buf = (char*)info_buffer;
    const char* msg = "AscentOS Phase3 - SysInfo: ticks=";
    int i = 0;
    while (msg[i] && i < (int)buffer_size - 20) { buf[i] = msg[i]; i++; }

    char tstr[20];
    u64_to_str(get_system_ticks(), tstr);
    int j = 0;
    while (tstr[j] && i < (int)buffer_size - 1) buf[i++] = tstr[j++];
    buf[i] = '\0';
    return SYSCALL_SUCCESS;
}

int64_t sys_ascent_yield(void) {
    scheduler_yield();
    return SYSCALL_SUCCESS;
}

int64_t sys_ascent_sleep(uint64_t milliseconds) {
    if (milliseconds == 0) return SYSCALL_SUCCESS;
    // Convert ms → ticks and spin-wait (proper blocking is Phase 4)
    uint64_t target = get_system_ticks() + milliseconds;
    while (get_system_ticks() < target) {
        scheduler_yield();
    }
    return SYSCALL_SUCCESS;
}

int64_t sys_ascent_gettime(void) {
    return (int64_t)get_system_ticks();
}

// ── Shared Memory ──────────────────────────────────────────────────────────

int64_t sys_ascent_shmget(int id, uint64_t size) {
    (void)size;  // We always use SHM_SEG_SIZE
    if (id < 0 || id >= SHM_MAX_SEGS) return EINVAL;

    if (!shm_segments[id].in_use) {
        k_memset(&shm_segments[id], 0, sizeof(shm_segment_t));
        shm_segments[id].in_use    = 1;
        shm_segments[id].id        = id;

        task_t* cur = task_get_current();
        shm_segments[id].owner_pid = cur ? cur->pid : 0;

        serial_print("[SYS_SHMGET] Created segment id=");
        char tmp[8]; int_to_str(id, tmp); serial_print(tmp); serial_print("\n");
    }
    return id;
}

int64_t sys_ascent_shmmap(int id) {
    if (id < 0 || id >= SHM_MAX_SEGS || !shm_segments[id].in_use)
        return EINVAL;
    // Return the kernel address of the segment data
    return (int64_t)(uint64_t)shm_segments[id].data;
}

int64_t sys_ascent_shmunmap(int id) {
    if (id < 0 || id >= SHM_MAX_SEGS) return EINVAL;
    shm_segments[id].in_use = 0;
    return SYSCALL_SUCCESS;
}

// ── Message Queues ──────────────────────────────────────────────────────────

int64_t sys_ascent_msgpost(int queue_id, const void* data, uint64_t size) {
    if (queue_id < 0 || queue_id >= MSG_MAX_QUEUES) return EINVAL;
    if (!data || size == 0 || size > MSG_MAX_SIZE) return EINVAL;

    msg_queue_t* q = &msg_queues[queue_id];
    if (!q->in_use) {
        k_memset(q, 0, sizeof(msg_queue_t));
        q->in_use = 1;
        q->id     = queue_id;
    }
    if (q->count >= MSG_MAX_MSGS) return ENOSPC;

    ipc_message_t* msg = &q->msgs[q->tail];
    task_t* cur = task_get_current();
    msg->sender_pid = cur ? cur->pid : 0;
    msg->size       = (uint32_t)size;
    k_memcpy(msg->data, data, size);

    q->tail  = (q->tail + 1) % MSG_MAX_MSGS;
    q->count++;

    serial_print("[SYS_MSGPOST] Queue ");
    char tmp[8]; int_to_str(queue_id, tmp); serial_print(tmp);
    serial_print(" now has "); int_to_str(q->count, tmp); serial_print(tmp);
    serial_print(" message(s)\n");

    return SYSCALL_SUCCESS;
}

int64_t sys_ascent_msgrecv(int queue_id, void* data, uint64_t max_size) {
    if (queue_id < 0 || queue_id >= MSG_MAX_QUEUES) return EINVAL;
    if (!data || max_size == 0) return EINVAL;

    msg_queue_t* q = &msg_queues[queue_id];
    if (!q->in_use || q->count == 0) return EAGAIN;

    ipc_message_t* msg = &q->msgs[q->head];
    uint64_t copy_size = (msg->size < max_size) ? msg->size : max_size;
    k_memcpy(data, msg->data, copy_size);

    q->head  = (q->head + 1) % MSG_MAX_MSGS;
    q->count--;

    return (int64_t)copy_size;
}

// =============================================================================
// STATISTICS & DEBUGGING
// =============================================================================

void syscall_get_stats(syscall_stats_t* out) {
    if (!out) return;
    out->total_syscalls   = stats.total_syscalls;
    out->invalid_syscalls = stats.invalid_syscalls;
    out->failed_syscalls  = stats.failed_syscalls;
    for (int i = 0; i < SYSCALL_MAX; i++)
        out->syscall_counts[i] = stats.syscall_counts[i];
}

// Human-readable name for the most common syscall numbers
static const char* syscall_name(int n) {
    switch (n) {
        case SYS_READ:    return "read";
        case SYS_WRITE:   return "write";
        case SYS_OPEN:    return "open";
        case SYS_CLOSE:   return "close";
        case SYS_STAT:    return "stat";
        case SYS_FSTAT:   return "fstat";
        case SYS_LSEEK:   return "lseek";
        case SYS_MMAP:    return "mmap";
        case SYS_MUNMAP:  return "munmap";
        case SYS_BRK:     return "brk";
        case SYS_PIPE:    return "pipe";
        case SYS_DUP:     return "dup";
        case SYS_DUP2:    return "dup2";
        case SYS_EXIT:    return "exit";
        case SYS_GETPID:  return "getpid";
        case SYS_WAITPID: return "waitpid";
        case SYS_FORK:    return "fork";
        case SYS_EXECVE:  return "execve";
        case SYS_WAIT4:   return "wait4";
        case SYS_KILL:    return "kill";
        case SYS_GETUID:  return "getuid";
        case SYS_GETGID:  return "getgid";
        case SYS_ASCENT_DEBUG:    return "ascent_debug";
        case SYS_ASCENT_INFO:     return "ascent_info";
        case SYS_ASCENT_YIELD:    return "ascent_yield";
        case SYS_ASCENT_SLEEP:    return "ascent_sleep";
        case SYS_ASCENT_GETTIME:  return "ascent_gettime";
        case SYS_ASCENT_SHMGET:   return "ascent_shmget";
        case SYS_ASCENT_SHMMAP:   return "ascent_shmmap";
        case SYS_ASCENT_SHMUNMAP: return "ascent_shmunmap";
        case SYS_ASCENT_MSGPOST:  return "ascent_msgpost";
        case SYS_ASCENT_MSGRECV:  return "ascent_msgrecv";
        default: return NULL;
    }
}

void syscall_print_stats(void) {
    serial_print("\n=== Syscall Statistics (Phase 3) ===\n");
    char num[32];
    u64_to_str(stats.total_syscalls,   num); serial_print("Total   : "); serial_print(num); serial_print("\n");
    u64_to_str(stats.invalid_syscalls, num); serial_print("Invalid : "); serial_print(num); serial_print("\n");
    u64_to_str(stats.failed_syscalls,  num); serial_print("Failed  : "); serial_print(num); serial_print("\n");

    serial_print("\nPer-syscall usage (non-zero only):\n");
    for (int i = 0; i < SYSCALL_MAX; i++) {
        if (stats.syscall_counts[i] == 0) continue;
        const char* name = syscall_name(i);
        serial_print("  ");
        if (name) { serial_print(name); }
        else      { int_to_str(i, num); serial_print("syscall#"); serial_print(num); }
        serial_print(": ");
        u64_to_str(stats.syscall_counts[i], num);
        serial_print(num);
        serial_print("\n");
    }
    serial_print("\n");
}

void syscall_reset_stats(void) {
    k_memset(&stats, 0, sizeof(syscall_stats_t));
    serial_print("[SYSCALL] Statistics reset\n");
}