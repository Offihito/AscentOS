// syscall_test.c - Phase 3 Syscall Test Suite
// FIX: All tests call syscall_handler() so statistics counters are accurate.
//      Invalid-syscall test uses numbers truly >= SYSCALL_MAX.
#include "syscall.h"
#include "task.h"
#include <stddef.h>

extern void    serial_print(const char* str);
extern void    int_to_str(int num, char* str);
extern uint64_t get_system_ticks(void);

// =============================================================================
// KERNEL-SPACE DISPATCHER WRAPPERS
// Every call goes through syscall_handler() so the stats counters increment.
// =============================================================================

static inline int64_t SC0(uint64_t n)
    { return syscall_handler(n,0,0,0,0,0); }
static inline int64_t SC1(uint64_t n, uint64_t a1)
    { return syscall_handler(n,a1,0,0,0,0); }
static inline int64_t SC2(uint64_t n, uint64_t a1, uint64_t a2)
    { return syscall_handler(n,a1,a2,0,0,0); }
static inline int64_t SC3(uint64_t n, uint64_t a1, uint64_t a2, uint64_t a3)
    { return syscall_handler(n,a1,a2,a3,0,0); }

#define T_write(fd,buf,cnt)      SC3(SYS_WRITE,  (uint64_t)(fd),(uint64_t)(buf),(uint64_t)(cnt))
#define T_read(fd,buf,cnt)       SC3(SYS_READ,   (uint64_t)(fd),(uint64_t)(buf),(uint64_t)(cnt))
#define T_open(path,fl,md)       SC3(SYS_OPEN,   (uint64_t)(path),(uint64_t)(fl),(uint64_t)(md))
#define T_close(fd)              SC1(SYS_CLOSE,  (uint64_t)(fd))
#define T_stat(path,st)          SC2(SYS_STAT,   (uint64_t)(path),(uint64_t)(st))
#define T_fstat(fd,st)           SC2(SYS_FSTAT,  (uint64_t)(fd),(uint64_t)(st))
#define T_lseek(fd,off,wh)       SC3(SYS_LSEEK,  (uint64_t)(fd),(uint64_t)(off),(uint64_t)(wh))
#define T_pipe(pfd)              SC1(SYS_PIPE,   (uint64_t)(pfd))
#define T_dup(old)               SC1(SYS_DUP,    (uint64_t)(old))
#define T_dup2(old,nw)           SC2(SYS_DUP2,   (uint64_t)(old),(uint64_t)(nw))
#define T_getpid()               SC0(SYS_GETPID)
#define T_getuid()               SC0(SYS_GETUID)
#define T_getgid()               SC0(SYS_GETGID)
#define T_fork()                 SC0(SYS_FORK)
#define T_execve(p,av,ev)        SC3(SYS_EXECVE,  (uint64_t)(p),(uint64_t)(av),(uint64_t)(ev))
#define T_waitpid(pid,st,opt)    SC3(SYS_WAITPID, (uint64_t)(pid),(uint64_t)(st),(uint64_t)(opt))
#define T_kill(pid,sig)          SC2(SYS_KILL,    (uint64_t)(pid),(uint64_t)(sig))
#define T_brk(addr)              SC1(SYS_BRK,     (uint64_t)(addr))
#define T_mmap(a,l,p,f,fd,o)    SC3(SYS_MMAP,    (uint64_t)(a),(uint64_t)(l),(uint64_t)(p))
#define T_munmap(a,l)            SC2(SYS_MUNMAP,  (uint64_t)(a),(uint64_t)(l))
#define T_debug(msg)             SC1(SYS_ASCENT_DEBUG,   (uint64_t)(msg))
#define T_gettime()              SC0(SYS_ASCENT_GETTIME)
#define T_yield()                SC0(SYS_ASCENT_YIELD)
#define T_shmget(id,sz)          SC2(SYS_ASCENT_SHMGET,  (uint64_t)(id),(uint64_t)(sz))
#define T_shmmap(id)             SC1(SYS_ASCENT_SHMMAP,  (uint64_t)(id))
#define T_shmunmap(id)           SC1(SYS_ASCENT_SHMUNMAP,(uint64_t)(id))
#define T_msgpost(q,d,sz)        SC3(SYS_ASCENT_MSGPOST, (uint64_t)(q),(uint64_t)(d),(uint64_t)(sz))
#define T_msgrecv(q,d,sz)        SC3(SYS_ASCENT_MSGRECV, (uint64_t)(q),(uint64_t)(d),(uint64_t)(sz))

// =============================================================================
// TEST HARNESS
// =============================================================================

static int g_pass = 0;
static int g_fail = 0;

static void test_begin(const char* name) {
    serial_print("\n[TEST] "); serial_print(name); serial_print("\n");
}

static void check(int cond, const char* label) {
    if (cond) { g_pass++; serial_print("  [PASS] "); }
    else       { g_fail++; serial_print("  [FAIL] "); }
    serial_print(label); serial_print("\n");
}

static void check_eq64(int64_t got, int64_t expected, const char* label) {
    char buf[32];
    if (got == expected) {
        g_pass++;
        serial_print("  [PASS] "); serial_print(label); serial_print("\n");
    } else {
        g_fail++;
        serial_print("  [FAIL] "); serial_print(label);
        serial_print(" (got=");    int_to_str((int)got,      buf); serial_print(buf);
        serial_print(" exp=");     int_to_str((int)expected, buf); serial_print(buf);
        serial_print(")\n");
    }
}

// =============================================================================
// TEST GROUPS
// =============================================================================

static void test_syscall_init(void) {
    test_begin("1. SYSCALL INIT");
    check(syscall_is_enabled(), "syscall_is_enabled() returns true");
}

static void test_msr_config(void) {
    test_begin("2. MSR CONFIGURATION");
    extern void syscall_verify_setup(void);
    syscall_verify_setup();
    serial_print("  (see MSR values above)\n");
}

static void test_ascent_basic(void) {
    test_begin("3. ASCENT BASIC SYSCALLS");

    int64_t t0 = T_gettime();
    check(t0 >= 0, "sys_ascent_gettime() returns non-negative ticks");

    int64_t pid = T_getpid();
    check(pid >= 0, "sys_getpid() returns a valid PID");
    char buf[16]; int_to_str((int)pid, buf);
    serial_print("  Current PID: "); serial_print(buf); serial_print("\n");

    int64_t r = T_debug("Hello from syscall_test Phase 3!");
    check_eq64(r, SYSCALL_SUCCESS, "sys_ascent_debug() returns SUCCESS");

    check_eq64(T_getuid(), 0, "sys_getuid() == 0 (root)");
    check_eq64(T_getgid(), 0, "sys_getgid() == 0 (root)");

    T_yield();
    check(1, "sys_ascent_yield() does not crash");
}

static void test_fd_table(void) {
    test_begin("4. FD TABLE & STDIN/STDOUT/STDERR");

    fd_entry_t* tbl = syscall_get_fd_table();
    check(tbl != NULL, "fd table is not NULL");
    check(tbl[0].type == FD_TYPE_SERIAL, "fd 0 (stdin)  is FD_TYPE_SERIAL");
    check(tbl[1].type == FD_TYPE_SERIAL, "fd 1 (stdout) is FD_TYPE_SERIAL");
    check(tbl[2].type == FD_TYPE_SERIAL, "fd 2 (stderr) is FD_TYPE_SERIAL");
    check(tbl[0].flags == O_RDONLY, "stdin  flags == O_RDONLY");
    check(tbl[1].flags == O_WRONLY, "stdout flags == O_WRONLY");
    check(tbl[2].flags == O_WRONLY, "stderr flags == O_WRONLY");
}

static void test_write_stdout(void) {
    test_begin("5. SYS_WRITE TO STDOUT/STDERR");

    const char* msg  = "sys_write stdout test\n";
    int64_t n = T_write(STDOUT_FD, msg, 22);
    check_eq64(n, 22, "sys_write(stdout) returns byte count");

    const char* emsg = "sys_write stderr test\n";
    n = T_write(STDERR_FD, emsg, 22);
    check_eq64(n, 22, "sys_write(stderr) returns byte count");

    n = T_write(STDIN_FD, msg, 5);
    check(n < 0, "sys_write(stdin) returns error (read-only)");
}

static void test_file_io(void) {
    test_begin("6. FILE I/O (open/write/read/close/lseek/stat)");

    const char* fname = "SCTEST.TXT";

    int64_t fd = T_open(fname, O_WRONLY | O_CREAT | O_TRUNC, 0);
    check(fd >= 3, "sys_open(O_CREAT) allocates fd >= 3");
    if (fd < 3) { serial_print("  (skipping file I/O tests)\n"); return; }

    const char* content = "Phase3 Test Line\n";
    int64_t wn = T_write((int)fd, content, 17);
    check_eq64(wn, 17, "sys_write to FAT32 file returns 17");

    int64_t r = T_close((int)fd);
    check_eq64(r, SYSCALL_SUCCESS, "sys_close after write returns SUCCESS");

    fd = T_open(fname, O_RDONLY, 0);
    check(fd >= 3, "sys_open(O_RDONLY) of existing file succeeds");
    if (fd < 3) { serial_print("  (skipping read/seek/stat tests)\n"); return; }

    char buf[32];
    int64_t rn = T_read((int)fd, buf, 17);
    check_eq64(rn, 17, "sys_read returns 17 bytes");
    check(buf[0] == 'P', "first byte of read content is correct ('P')");

    int64_t pos = T_lseek((int)fd, 0, SEEK_SET);
    check_eq64(pos, 0, "sys_lseek(SEEK_SET,0) returns 0");

    rn = T_read((int)fd, buf, 5);
    check_eq64(rn, 5, "sys_read after seek returns 5 bytes");

    pos = T_lseek((int)fd, 0, SEEK_END);
    check(pos >= 17, "sys_lseek(SEEK_END,0) returns file size");

    ascent_stat_t st;
    r = T_fstat((int)fd, &st);
    check_eq64(r, SYSCALL_SUCCESS, "sys_fstat returns SUCCESS");
    check(st.st_mode & S_IFREG, "st_mode includes S_IFREG");
    check(st.st_size >= 17, "st_size >= 17 bytes");

    T_close((int)fd);

    r = T_stat(fname, &st);
    check_eq64(r, SYSCALL_SUCCESS, "sys_stat by path returns SUCCESS");

    r = T_open("NOEXIST.TXT", O_RDONLY, 0);
    check(r == ENOENT, "sys_open of non-existent file returns ENOENT");
}

static void test_pipe(void) {
    test_begin("7. PIPE (sys_pipe / read / write / close)");

    int pipefd[2] = { -1, -1 };
    int64_t r = T_pipe(pipefd);
    check_eq64(r, SYSCALL_SUCCESS, "sys_pipe() returns SUCCESS");
    check(pipefd[0] >= 3, "pipe read-end fd >= 3");
    check(pipefd[1] >= 3, "pipe write-end fd >= 3");
    check(pipefd[0] != pipefd[1], "pipe fds are distinct");

    const char* msg = "PIPEDATA";
    int64_t wn = T_write(pipefd[1], msg, 8);
    check_eq64(wn, 8, "write 8 bytes to pipe write-end");

    char buf[16];
    int64_t rn = T_read(pipefd[0], buf, 16);
    check_eq64(rn, 8, "read 8 bytes from pipe read-end");
    check(buf[0] == 'P' && buf[4] == 'D', "pipe data content is correct");

    T_close(pipefd[1]);
    rn = T_read(pipefd[0], buf, 16);
    check_eq64(rn, 0, "read after write-end closed returns 0 (EOF)");

    T_close(pipefd[0]);
}

static void test_dup(void) {
    test_begin("8. DUP / DUP2");

    int64_t newfd = T_dup(STDOUT_FD);
    check(newfd >= 3, "sys_dup(stdout) creates fd >= 3");

    if (newfd >= 3) {
        const char* msg = "dup test\n";
        int64_t wn = T_write((int)newfd, msg, 9);
        check_eq64(wn, 9, "sys_write through dup'd fd works");
        T_close((int)newfd);
    }

    int64_t r2 = T_dup2(STDOUT_FD, 5);
    check_eq64(r2, 5, "sys_dup2(stdout, 5) returns 5");
    if (r2 == 5) {
        const char* msg = "dup2 test\n";
        int64_t wn = T_write(5, msg, 10);
        check_eq64(wn, 10, "sys_write through dup2'd fd 5 works");
        T_close(5);
    }
}

static void test_memory(void) {
    test_begin("9. MEMORY (brk / mmap / munmap)");

    int64_t brk0 = T_brk(0);
    check(brk0 > 0, "sys_brk(NULL) returns a valid address");

    int64_t addr = T_mmap(0, 4096, 0, 0, -1, 0);
    check(addr > 0, "sys_mmap(4096) returns a non-zero address");

    if (addr > 0) {
        uint8_t* p = (uint8_t*)(uint64_t)addr;
        p[0]    = 0xAB;
        p[4095] = 0xCD;
        check(p[0] == 0xAB && p[4095] == 0xCD, "mmap'd memory is read/write accessible");

        int64_t r = T_munmap((void*)(uint64_t)addr, 4096);
        check_eq64(r, SYSCALL_SUCCESS, "sys_munmap returns SUCCESS");
    }
}

static void test_process_mgmt(void) {
    test_begin("10. PROCESS MANAGEMENT");

    int64_t pid = T_getpid();
    check(pid >= 0, "sys_getpid() returns valid PID");

    int64_t r = T_kill(99999, 9);
    check(r == EINVAL, "sys_kill(invalid_pid) returns EINVAL");

    r = T_kill(0, 9);
    check(r == EINVAL, "sys_kill(0) returns EINVAL");

    int status = -1;
    r = T_waitpid(99999, &status, 0);
    check(r == ECHILD, "sys_waitpid(non-existent) returns ECHILD");

    r = T_fork();
    check(r == ENOSYS, "sys_fork() returns ENOSYS (Phase 4 stub)");

    r = T_execve("test", NULL, NULL);
    check(r == ENOSYS, "sys_execve() returns ENOSYS (Phase 4 stub)");
}

static void test_shm(void) {
    test_begin("11. IPC: SHARED MEMORY");

    int64_t id0 = T_shmget(0, SHM_SEG_SIZE);
    check_eq64(id0, 0, "shmget(id=0) returns 0");

    int64_t id1 = T_shmget(1, SHM_SEG_SIZE);
    check_eq64(id1, 1, "shmget(id=1) returns 1");

    int64_t addr0 = T_shmmap(0);
    check(addr0 > 0, "shmmap(0) returns non-zero address");

    if (addr0 > 0) {
        uint8_t* shm = (uint8_t*)(uint64_t)addr0;
        shm[0] = 0xDE; shm[1] = 0xAD;

        int64_t addr0b = T_shmmap(0);
        uint8_t* shm2  = (uint8_t*)(uint64_t)addr0b;
        check(shm2[0] == 0xDE && shm2[1] == 0xAD,
              "shared memory data persists between shmmap calls");
    }

    int64_t bad = T_shmget(SHM_MAX_SEGS, 0);
    check(bad == EINVAL, "shmget(invalid_id) returns EINVAL");

    int64_t r = T_shmunmap(0);
    check_eq64(r, SYSCALL_SUCCESS, "shmunmap(0) returns SUCCESS");
    r = T_shmunmap(1);
    check_eq64(r, SYSCALL_SUCCESS, "shmunmap(1) returns SUCCESS");
}

static void test_msgqueue(void) {
    test_begin("12. IPC: MESSAGE QUEUE");

    const char* p1 = "HELLO";
    int64_t r = T_msgpost(0, p1, 5);
    check_eq64(r, SYSCALL_SUCCESS, "msgpost(qid=0, 5 bytes) returns SUCCESS");

    const char* p2 = "WORLD!";
    r = T_msgpost(0, p2, 6);
    check_eq64(r, SYSCALL_SUCCESS, "msgpost(qid=0, 6 bytes) returns SUCCESS");

    char buf[64];
    int64_t got = T_msgrecv(0, buf, 64);
    check_eq64(got, 5, "msgrecv returns 5 bytes for first message");
    check(buf[0] == 'H' && buf[4] == 'O', "first message data is correct ('HELLO')");

    got = T_msgrecv(0, buf, 64);
    check_eq64(got, 6, "msgrecv returns 6 bytes for second message");
    check(buf[0] == 'W', "second message data starts with 'W'");

    got = T_msgrecv(0, buf, 64);
    check(got == EAGAIN, "msgrecv on empty queue returns EAGAIN");

    r = T_msgpost(MSG_MAX_QUEUES, "x", 1);
    check(r == EINVAL, "msgpost(invalid_qid) returns EINVAL");
}

// KEY FIX: use only numbers >= SYSCALL_MAX — truly out-of-range
// 255 < SYSCALL_MAX(310) so it was hitting default→ENOSYS but
// also incrementing failed_syscalls — that was the 1 fake failure.
static void test_invalid_syscall(void) {
    test_begin("13. INVALID SYSCALL HANDLING");

    int64_t r = syscall_handler(SYSCALL_MAX, 0,0,0,0,0);
    check(r == ENOSYS, "syscall_num == SYSCALL_MAX returns ENOSYS");

    r = syscall_handler(SYSCALL_MAX + 1, 0,0,0,0,0);
    check(r == ENOSYS, "syscall_num == SYSCALL_MAX+1 returns ENOSYS");

    r = syscall_handler(0xFFFF, 0,0,0,0,0);
    check(r == ENOSYS, "syscall_num == 0xFFFF returns ENOSYS");
}

// =============================================================================
// SUMMARY & ENTRY POINT
// =============================================================================

static void print_summary(void) {
    char buf[16];
    serial_print("\n");
    serial_print("╔══════════════════════════════════════════╗\n");
    serial_print("║        PHASE 3 SYSCALL TEST SUMMARY      ║\n");
    serial_print("╠══════════════════════════════════════════╣\n");
    serial_print("║  PASSED : "); int_to_str(g_pass, buf); serial_print(buf); serial_print("\n");
    serial_print("║  FAILED : "); int_to_str(g_fail, buf); serial_print(buf); serial_print("\n");
    serial_print("║  TOTAL  : "); int_to_str(g_pass+g_fail, buf); serial_print(buf); serial_print("\n");
    serial_print("╚══════════════════════════════════════════╝\n\n");

    if (g_fail == 0)
        serial_print("[RESULT] ALL TESTS PASSED - Phase 3 syscalls OK!\n\n");
    else
        serial_print("[RESULT] SOME TESTS FAILED - check serial log above\n\n");
}

void syscall_kernel_test(void) {
    g_pass = 0;
    g_fail = 0;

    serial_print("\n");
    serial_print("══════════════════════════════════════════════\n");
    serial_print("    AscentOS Phase 3 - Syscall Test Suite     \n");
    serial_print("══════════════════════════════════════════════\n");

    // Reset stats so the counter block printed at the end
    // reflects only this run, not any earlier kernel activity.
    syscall_reset_stats();

    test_syscall_init();
    test_msr_config();
    test_ascent_basic();
    test_fd_table();
    test_write_stdout();
    test_file_io();
    test_pipe();
    test_dup();
    test_memory();
    test_process_mgmt();
    test_shm();
    test_msgqueue();
    test_invalid_syscall();

    print_summary();
    syscall_print_stats();
}

// =============================================================================
// USERMODE STUB  (Phase 4)
// =============================================================================
#ifdef USERSPACE
void usermode_syscall_test(void) {
    int fd = (int)open("UM_TEST.TXT", O_WRONLY | O_CREAT | O_TRUNC, 0);
    if (fd >= 0) { write(fd, "hello from usermode\n", 20); close(fd); }
    ascent_debug("usermode syscall test running");
    int shm_id = (int)ascent_shmget(2, 4096);
    if (shm_id >= 0) {
        void* p = (void*)(uint64_t)ascent_shmmap(shm_id);
        if ((uint64_t)p > 0) { volatile uint8_t* b = (volatile uint8_t*)p; b[0] = 0x42; }
        ascent_shmunmap(shm_id);
    }
    ascent_msgpost(0, "hello", 5);
    exit(0);
}
#endif