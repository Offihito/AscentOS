// syscall_test.c — AscentOS Syscall Test v14
// Changes:
//   - v3: File I/O (lseek, fstat, O_APPEND, double-close, pipe, large write)
//   - v4: Advanced I/O (dup, dup2, fcntl, truncate, ftruncate, error fd modes)
//   - v5: Directory (mkdir, rmdir, chdir, getcwd, stat dir, rename, nested)
//   - v6: Time (clock_gettime, clock_getres, gettimeofday, nanosleep)
//   - v7: Process (fork/waitpid exit codes, getpid in child, getppid in child,
//          pipe across fork, fork fd inheritance, child writes file,
//          waitpid WNOHANG, double fork/zombie reap)
//   - v8: Process extended (deep fork chain, pipe echo 256B, waitpid(-1) any-child,
//          fork name stability 4-levels, pipe EOF on close, WNOHANG poll loop)
//   - v9: mmap/munmap (anon rw, zero-init, munmap, re-mmap, large, file-backed,
//          MAP_SHARED writeback, offset map, PROT_READ, mprotect upgrade)
//  - v10: Advanced FD (pipe2 O_CLOEXEC, pipe2 O_NONBLOCK, writev scatter,
//          writev byte count, getdents entry count, getdents find known file)
//  - v11: uname (sysname, machine, nodename fields; NULL ptr guard),
//         hard link / link(2) (new path readable, content matches source,
//         link to dir rejected with EPERM, link over existing file rejected)
//  - v12: brk/sbrk (Linux x86-64 ABI: brk(0) query, heap grow, sbrk(0) addr,
//          sbrk(+N) grow, sbrk(-N) shrink, sbrk write/read, double sbrk grow,
//          brk below current heap rejected, sbrk overflow guard)
//  - v13: readv (gather read into 1/3 iovec, byte count, partial fill, empty iov,
//          bad-fd guard), lseek extended (negative offset SEEK_END, SEEK_CUR
//          report, seek-past-EOF size unchanged, bad-fd guard, SEEK_SET=0
//          on write-only fd)
//  - v14: Stress tests — readv (1000-iteration scatter correctness, 64-iovec fan-out,
//          interleaved readv+lseek, pipe-backed readv, write-only fd guard),
//         lseek (1000-iteration seek+read sweep, alternating SEEK_END/SEEK_SET
//          boundary walk, seek+write hole fill, concurrent seek independence
//          via pipe-fd pair, SEEK_CUR accumulation over 500 steps)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <time.h>

// Linux x86-64 dirent64 layout for raw getdents64 syscall
struct linux_dirent64 {
    unsigned long long d_ino;
    long long          d_off;
    unsigned short     d_reclen;
    unsigned char      d_type;
    char               d_name[1];
};

#define SYS_GETDENTS64  78
#define SYS_PIPE2      293
#define SYS_OPENDIR    402   // AscentOS custom: open a dir fd for getdents

#ifndef O_CLOEXEC
#define O_CLOEXEC  0x80000
#endif
#ifndef O_NONBLOCK
#define O_NONBLOCK 0x800
#endif

// pipe2(pfd[2], flags)
static inline long _pipe2(int pfd[2], int flags) {
    long r;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"((long)SYS_PIPE2), "D"((long)pfd), "S"((long)flags)
        : "rcx", "r11", "memory");
    return r;
}

// opendir(path) -> dirfd  (AscentOS syscall 402)
static inline long _opendir(const char* path) {
    long r;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"((long)SYS_OPENDIR), "D"((long)path)
        : "rcx", "r11", "memory");
    return r;
}

// getdents64(fd, buf, count)
static inline long _getdents64(int fd, void* buf, int count) {
    long r;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"((long)SYS_GETDENTS64), "D"((long)fd), "S"((long)buf), "d"((long)count)
        : "rcx", "r11", "memory");
    return r;
}

// readv(fd, iov, iovcnt) — direct syscall 19, bypasses musl's __syscall_cp shim
#define SYS_READV 19
static inline long _readv(int fd, const struct iovec* iov, int iovcnt) {
    long r;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"((long)SYS_READV), "D"((long)fd), "S"((long)iov), "d"((long)iovcnt)
        : "rcx", "r11", "memory");
    return r;
}

// struct stat from sys/stat.h uses the Linux x86-64 ABI layout (144 bytes):
//   st_dev @ 0 (u64), st_ino @ 8 (u64), st_nlink @ 16 (u64),
//   st_mode @ 24 (u32), st_uid @ 28 (u32), st_gid @ 32 (u32),
//   st_rdev @ 40 (u64), st_size @ 48 (i64), ...
// sys_stat/sys_fstat now fill this layout directly — no custom struct needed.

#define SYS_GETTICKS  404

static inline long _sc0(long n) {
    long r;
    __asm__ volatile("syscall" :"=a"(r):"0"(n):"rcx","r11","memory");
    return r;
}

#define GRN "\033[32m"
#define RED "\033[31m"
#define RST "\033[0m"

static int pass = 0, fail = 0;

static void ok(const char* name) {
    printf("  " GRN "OK" RST "  %s\n", name);
    pass++;
    fflush(stdout);
}

static void ng(const char* name, long ret) {
    printf("  " RED "NG" RST "  %s  (ret=%ld)\n", name, ret);
    fail++;
    fflush(stdout);
}

// Helper: delete file, ignore errors
static void cleanup(const char* path) {
    unlink(path);
}

int main(void) {
    printf("=== AscentOS Syscall Test v13 ===\n\n");

    // ── SECTION 1: Basic syscalls ───────────────────────────────

    // 1. write to stdout
    {
        const char* msg = "  [write test]\n";
        long r = write(1, msg, strlen(msg));
        if (r > 0) ok("write(stdout)"); else ng("write(stdout)", r);
    }

    // 2. getpid — must return a positive PID
    {
        long r = getpid();
        if (r > 0) ok("getpid"); else ng("getpid", r);
    }

    // 3. getppid — must return >= 0
    {
        long r = getppid();
        if (r >= 0) ok("getppid"); else ng("getppid", r);
    }

    // 4. sched_yield — must return 0
    {
        long r = _sc0(24);
        if (r == 0) ok("sched_yield"); else ng("sched_yield", r);
    }

    // 5. getticks — must return >= 0
    {
        long r = _sc0(SYS_GETTICKS);
        if (r >= 0) ok("getticks(404)"); else ng("getticks(404)", r);
    }

    // 6. getticks monotonic — second reading must be >= first
    {
        long t0 = _sc0(SYS_GETTICKS);
        for (int i = 0; i < 100; i++) _sc0(24);
        long t1 = _sc0(SYS_GETTICKS);
        if (t1 >= t0) ok("getticks monotonic"); else ng("getticks monotonic", t1 - t0);
    }

    // 7-11. Basic file cycle: create, write, close, reopen, read, unlink
    {
        int fd = open("/tmp/sc_test.txt", O_WRONLY|O_CREAT|O_TRUNC, 0644);
        if (fd < 0) { ng("open(create)", fd); goto skip_file; }
        ok("open(O_CREAT)");

        long w = write(fd, "hello\n", 6);
        if (w == 6) ok("write(file)"); else ng("write(file)", w);

        long r = close(fd);
        if (r == 0) ok("close"); else ng("close", r);

        fd = open("/tmp/sc_test.txt", O_RDONLY, 0);
        if (fd < 0) { ng("open(O_RDONLY)", fd); goto skip_file; }
        ok("open(O_RDONLY)");

        char buf[16] = {0};
        r = read(fd, buf, sizeof(buf) - 1);
        if (r > 0 && buf[0] == 'h') ok("read(file)"); else ng("read(file)", r);

        close(fd);

        r = unlink("/tmp/sc_test.txt");
        if (r == 0) ok("unlink"); else ng("unlink", r);
        goto after_file;
    skip_file:
        printf("  -- file tests skipped\n");
    after_file:;
    }

    // 12-13. malloc + memset + free
    {
        char* p = malloc(1024);
        if (!p) { ng("malloc(1024)", 0); goto skip_mem; }
        memset(p, 0xAA, 1024);
        if (p[512] == (char)0xAA) ok("malloc+memset"); else ng("malloc+memset", 0);
        free(p);
        ok("free");
    skip_mem:;
    }

    // 14. getcwd — buffer must start with '/'
    {
        char cwd[256];
        memset(cwd, 0, sizeof(cwd));
        getcwd(cwd, sizeof(cwd));
        if (cwd[0] == '/') ok("getcwd");
        else ng("getcwd", (long)cwd[0]);
    }

    // 15. write to invalid fd — must return error, no panic
    {
        long r = write(999, "x", 1);
        if (r < 0) ok("write(bad_fd) -> error expected"); else ng("write(bad_fd)", r);
    }

    // ── SECTION 2: File I/O tests ───────────────────────────────
    printf("\n--- File I/O Tests ---\n");

    const char* TMP = "/tmp/sc_io_test.txt";

    // T1. lseek SEEK_SET — seek to beginning and read first byte
    {
        cleanup(TMP);
        int fd = open(TMP, O_RDWR|O_CREAT|O_TRUNC, 0644);
        if (fd < 0) { ng("lseek/open", fd); goto t1_end; }

        write(fd, "ABCDEF", 6);

        long pos = lseek(fd, 0, SEEK_SET);
        char c = 0;
        read(fd, &c, 1);

        if (pos == 0 && c == 'A') ok("lseek(SEEK_SET) + read");
        else ng("lseek(SEEK_SET) + read", pos);

        close(fd);
    t1_end:
        cleanup(TMP);
    }

    // T2. lseek SEEK_END — offset must equal file size
    {
        int fd = open(TMP, O_RDWR|O_CREAT|O_TRUNC, 0644);
        if (fd < 0) { ng("lseek(SEEK_END)/open", fd); goto t2_end; }

        write(fd, "HELLO", 5);
        long end = lseek(fd, 0, SEEK_END);

        if (end == 5) ok("lseek(SEEK_END) == file size");
        else ng("lseek(SEEK_END)", end);

        close(fd);
    t2_end:
        cleanup(TMP);
    }

    // T3. lseek SEEK_CUR — relative seek, read correct byte
    {
        int fd = open(TMP, O_RDWR|O_CREAT|O_TRUNC, 0644);
        if (fd < 0) { ng("lseek(SEEK_CUR)/open", fd); goto t3_end; }

        write(fd, "ABCDE", 5);
        lseek(fd, 0, SEEK_SET);
        lseek(fd, 2, SEEK_CUR);   // advance to 'C'
        char c = 0;
        read(fd, &c, 1);

        if (c == 'C') ok("lseek(SEEK_CUR) + read");
        else ng("lseek(SEEK_CUR)", (long)c);

        close(fd);
    t3_end:
        cleanup(TMP);
    }

    // T4. fstat — st_size must match bytes written
    // sys_fstat now fills the Linux x86-64 struct stat layout correctly.
    {
        int fd = open(TMP, O_RDWR|O_CREAT|O_TRUNC, 0644);
        if (fd < 0) { ng("fstat/open", fd); goto t4_end; }

        write(fd, "12345678", 8);   // 8 bytes

        struct stat st;
        memset(&st, 0, sizeof(st));
        long r = fstat(fd, &st);

        if (r == 0 && st.st_size == 8) ok("fstat -> st_size correct");
        else ng("fstat st_size", (long)st.st_size);

        close(fd);
    t4_end:
        cleanup(TMP);
    }

    // T5. fstat — st_mode must indicate a regular file (S_IFREG)
    {
        int fd = open(TMP, O_RDWR|O_CREAT|O_TRUNC, 0644);
        if (fd < 0) { ng("fstat(mode)/open", fd); goto t5_end; }

        write(fd, "x", 1);

        struct stat st;
        memset(&st, 0, sizeof(st));
        fstat(fd, &st);

        if (S_ISREG(st.st_mode)) ok("fstat -> S_IFREG");
        else ng("fstat st_mode", (long)st.st_mode);

        close(fd);
    t5_end:
        cleanup(TMP);
    }

    // T6. O_APPEND — each write must be appended to end of file
    // sys_write now seeks to ext2_file_size() before writing when O_APPEND is set.
    {
        int fd = open(TMP, O_WRONLY|O_CREAT|O_TRUNC, 0644);
        if (fd < 0) { ng("O_APPEND/create", fd); goto t6_end; }
        write(fd, "AAA", 3);
        close(fd);

        fd = open(TMP, O_WRONLY|O_APPEND, 0);
        if (fd < 0) { ng("O_APPEND/open", fd); goto t6_end; }
        write(fd, "BBB", 3);
        close(fd);

        // Read back: must be "AAABBB"
        fd = open(TMP, O_RDONLY, 0);
        char buf[16] = {0};
        read(fd, buf, 15);
        close(fd);

        if (buf[0] == 'A' && buf[3] == 'B' && buf[5] == 'B')
            ok("O_APPEND -> data appended correctly");
        else
            ng("O_APPEND content", (long)buf[3]);

    t6_end:
        cleanup(TMP);
    }

    // T7. write then read roundtrip — data read back must match exactly
    {
        int fd = open(TMP, O_RDWR|O_CREAT|O_TRUNC, 0644);
        if (fd < 0) { ng("write-then-read/open", fd); goto t7_end; }

        const char* msg = "roundtrip";
        write(fd, msg, 9);
        lseek(fd, 0, SEEK_SET);

        char buf[16] = {0};
        long r = read(fd, buf, 15);

        if (r == 9 && memcmp(buf, "roundtrip", 9) == 0)
            ok("write -> lseek(0) -> read roundtrip");
        else
            ng("write-then-read", r);

        close(fd);
    t7_end:
        cleanup(TMP);
    }

    // T8. double close — second close must return EBADF, no panic
    {
        int fd = open(TMP, O_WRONLY|O_CREAT|O_TRUNC, 0644);
        if (fd < 0) { ng("double-close/open", fd); goto t8_end; }
        close(fd);
        long r = close(fd);   // second close on same fd
        if (r < 0) ok("double-close -> EBADF expected");
        else ng("double-close expected error missing", r);
    t8_end:
        cleanup(TMP);
    }

    // T9. pipe write/read roundtrip
    {
        int pfd[2] = {-1, -1};
        long r = pipe(pfd);
        if (r != 0) { ng("pipe()", r); goto t9_end; }

        write(pfd[1], "pipe_ok", 7);
        close(pfd[1]);

        char buf[16] = {0};
        long n = read(pfd[0], buf, 15);
        close(pfd[0]);

        if (n == 7 && memcmp(buf, "pipe_ok", 7) == 0)
            ok("pipe write->read roundtrip");
        else
            ng("pipe read", n);
    t9_end:;
    }

    // T10. large write (4 KB) — full block written and read back correctly
    {
        int fd = open(TMP, O_RDWR|O_CREAT|O_TRUNC, 0644);
        if (fd < 0) { ng("large-write/open", fd); goto t10_end; }

        char *wbuf = malloc(4096);
        char *rbuf = malloc(4096);
        if (!wbuf || !rbuf) { ng("large-write/malloc", 0); goto t10_end; }

        memset(wbuf, 0x5A, 4096);
        long w = write(fd, wbuf, 4096);
        lseek(fd, 0, SEEK_SET);
        long n = read(fd, rbuf, 4096);

        if (w == 4096 && n == 4096 && rbuf[2048] == (char)0x5A)
            ok("4KB write -> read correct");
        else
            ng("large write/read", w < 4096 ? w : n);

        free(wbuf);
        free(rbuf);
        close(fd);
    t10_end:
        cleanup(TMP);
    }

    // ── SECTION 3: Advanced I/O tests ──────────────────────────
    printf("\n--- Advanced I/O Tests ---\n");

    // A1. dup — duplicated fd reads same file content
    {
        int fd = open(TMP, O_RDWR|O_CREAT|O_TRUNC, 0644);
        if (fd < 0) { ng("dup/open", fd); goto a1_end; }
        write(fd, "duptest", 7);
        lseek(fd, 0, SEEK_SET);

        int fd2 = dup(fd);
        if (fd2 < 0) { ng("dup()", fd2); close(fd); goto a1_end; }

        // fd2 shares the same offset as fd — read from fd2
        char buf[16] = {0};
        long n = read(fd2, buf, 15);
        if (n == 7 && memcmp(buf, "duptest", 7) == 0)
            ok("dup -> read same content");
        else
            ng("dup read", n);

        close(fd2);
        close(fd);
    a1_end:
        cleanup(TMP);
    }

    // A2. dup — fd and dup'd fd share file offset
    {
        int fd = open(TMP, O_RDWR|O_CREAT|O_TRUNC, 0644);
        if (fd < 0) { ng("dup(offset)/open", fd); goto a2_end; }
        write(fd, "ABCDE", 5);
        lseek(fd, 0, SEEK_SET);

        int fd2 = dup(fd);
        if (fd2 < 0) { ng("dup(offset)/dup", fd2); close(fd); goto a2_end; }

        // Read 2 bytes on fd — advances shared offset to 2
        char buf[8] = {0};
        read(fd, buf, 2);   // reads "AB", offset -> 2

        // Reading on fd2 must continue from offset 2, not restart
        char buf2[8] = {0};
        read(fd2, buf2, 1);  // must read 'C'
        if (buf2[0] == 'C') ok("dup -> shared offset");
        else ng("dup shared offset", (long)buf2[0]);

        close(fd2);
        close(fd);
    a2_end:
        cleanup(TMP);
    }

    // A3. dup2 — redirect fd to a specific number
    {
        int fd = open(TMP, O_RDWR|O_CREAT|O_TRUNC, 0644);
        if (fd < 0) { ng("dup2/open", fd); goto a3_end; }
        write(fd, "dup2ok", 6);
        lseek(fd, 0, SEEK_SET);

        int target = fd + 5;   // pick a free fd number far away
        int r2 = dup2(fd, target);
        if (r2 != target) { ng("dup2() return", r2); close(fd); goto a3_end; }

        char buf[16] = {0};
        long n = read(target, buf, 15);
        if (n == 6 && memcmp(buf, "dup2ok", 6) == 0)
            ok("dup2 -> read on target fd");
        else
            ng("dup2 read", n);

        close(target);
        close(fd);
    a3_end:
        cleanup(TMP);
    }

    // A4. dup2 on self — dup2(fd, fd) must return fd unchanged
    {
        int fd = open(TMP, O_RDWR|O_CREAT|O_TRUNC, 0644);
        if (fd < 0) { ng("dup2(self)/open", fd); goto a4_end; }

        int r = dup2(fd, fd);
        if (r == fd) ok("dup2(fd, fd) -> no-op returns fd");
        else ng("dup2(fd, fd)", r);

        close(fd);
    a4_end:
        cleanup(TMP);
    }

    // A5. fcntl F_GETFL — flags must include O_RDWR
    {
        int fd = open(TMP, O_RDWR|O_CREAT|O_TRUNC, 0644);
        if (fd < 0) { ng("fcntl(F_GETFL)/open", fd); goto a5_end; }

        int flags = fcntl(fd, F_GETFL, 0);
        // O_RDWR = 2; check the access mode bits (lowest 2 bits)
        if (flags >= 0 && (flags & O_ACCMODE) == O_RDWR)
            ok("fcntl(F_GETFL) -> O_RDWR");
        else
            ng("fcntl F_GETFL", flags);

        close(fd);
    a5_end:
        cleanup(TMP);
    }

    // A6. fcntl F_GETFD / F_SETFD — FD_CLOEXEC flag round-trip
    {
        int fd = open(TMP, O_RDWR|O_CREAT|O_TRUNC, 0644);
        if (fd < 0) { ng("fcntl(F_GETFD)/open", fd); goto a6_end; }

        // Clear FD_CLOEXEC first, then set it, then verify
        fcntl(fd, F_SETFD, 0);
        int before = fcntl(fd, F_GETFD, 0);
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        int after = fcntl(fd, F_GETFD, 0);

        if (before == 0 && (after & FD_CLOEXEC))
            ok("fcntl F_GETFD/F_SETFD -> FD_CLOEXEC round-trip");
        else
            ng("fcntl FD_CLOEXEC", after);

        close(fd);
    a6_end:
        cleanup(TMP);
    }

    // A7. ftruncate — shrink file, new size verified via fstat
    {
        int fd = open(TMP, O_RDWR|O_CREAT|O_TRUNC, 0644);
        if (fd < 0) { ng("ftruncate/open", fd); goto a7_end; }

        write(fd, "ABCDEFGHIJ", 10);   // 10 bytes

        int r = ftruncate(fd, 4);      // shrink to 4
        if (r != 0) { ng("ftruncate() call", r); close(fd); goto a7_end; }

        struct stat st;
        memset(&st, 0, sizeof(st));
        fstat(fd, &st);

        if (st.st_size == 4) ok("ftruncate -> st_size == 4");
        else ng("ftruncate st_size", (long)st.st_size);

        close(fd);
    a7_end:
        cleanup(TMP);
    }

    // A8. ftruncate — content after truncation point is gone
    {
        int fd = open(TMP, O_RDWR|O_CREAT|O_TRUNC, 0644);
        if (fd < 0) { ng("ftruncate(content)/open", fd); goto a8_end; }

        write(fd, "HELLO_WORLD", 11);
        ftruncate(fd, 5);              // keep only "HELLO"

        lseek(fd, 0, SEEK_SET);
        char buf[16] = {0};
        long n = read(fd, buf, 15);

        if (n == 5 && memcmp(buf, "HELLO", 5) == 0)
            ok("ftruncate -> truncated content correct");
        else
            ng("ftruncate content", n);

        close(fd);
    a8_end:
        cleanup(TMP);
    }

    // A9. truncate (by path) — size verified via stat
    {
        // Write 8 bytes, then truncate to 3 using path-based truncate()
        int fd = open(TMP, O_WRONLY|O_CREAT|O_TRUNC, 0644);
        if (fd < 0) { ng("truncate/open", fd); goto a9_end; }
        write(fd, "ABCDEFGH", 8);
        close(fd);

        int r = truncate(TMP, 3);
        if (r != 0) { ng("truncate() call", r); goto a9_end; }

        struct stat st;
        memset(&st, 0, sizeof(st));
        stat(TMP, &st);

        if (st.st_size == 3) ok("truncate(path) -> st_size == 3");
        else ng("truncate st_size", (long)st.st_size);

    a9_end:
        cleanup(TMP);
    }

    // A10. stat on regular file — S_ISREG and correct size
    {
        int fd = open(TMP, O_WRONLY|O_CREAT|O_TRUNC, 0644);
        if (fd < 0) { ng("stat(file)/open", fd); goto a10_end; }
        write(fd, "STAT_ME", 7);
        close(fd);

        struct stat st;
        memset(&st, 0, sizeof(st));
        int r = stat(TMP, &st);

        if (r == 0 && S_ISREG(st.st_mode) && st.st_size == 7)
            ok("stat(file) -> S_ISREG, size correct");
        else
            ng("stat(file)", (long)st.st_size);

    a10_end:
        cleanup(TMP);
    }

    // A11. write on O_RDONLY fd — must return error
    {
        int fd = open(TMP, O_WRONLY|O_CREAT|O_TRUNC, 0644);
        if (fd < 0) { ng("rdonly-write/create", fd); goto a11_end; }
        write(fd, "x", 1);
        close(fd);

        fd = open(TMP, O_RDONLY, 0);
        if (fd < 0) { ng("rdonly-write/open", fd); goto a11_end; }

        long r = write(fd, "y", 1);
        if (r < 0) ok("write on O_RDONLY fd -> error expected");
        else ng("write on O_RDONLY", r);

        close(fd);
    a11_end:
        cleanup(TMP);
    }

    // A12. read on O_WRONLY fd — must return error
    {
        int fd = open(TMP, O_WRONLY|O_CREAT|O_TRUNC, 0644);
        if (fd < 0) { ng("wronly-read/open", fd); goto a12_end; }

        char buf[4] = {0};
        long r = read(fd, buf, 4);
        if (r < 0) ok("read on O_WRONLY fd -> error expected");
        else ng("read on O_WRONLY", r);

        close(fd);
    a12_end:
        cleanup(TMP);
    }

    // A13. lseek on a pipe fd — must return error (pipes are not seekable)
    {
        int pfd[2] = {-1, -1};
        if (pipe(pfd) != 0) { ng("lseek(pipe)/pipe", -1); goto a13_end; }

        long r = lseek(pfd[0], 0, SEEK_SET);
        if (r < 0) ok("lseek on pipe fd -> error expected");
        else ng("lseek on pipe", r);

        close(pfd[0]);
        close(pfd[1]);
    a13_end:;
    }

    // A14. dup2 closes existing target fd before duplicating
    {
        // Open two separate files, dup2 second onto first's number
        int fd_a = open(TMP, O_RDWR|O_CREAT|O_TRUNC, 0644);
        if (fd_a < 0) { ng("dup2(close-target)/open_a", fd_a); goto a14_end; }
        write(fd_a, "FILE_A", 6);

        const char* TMP2 = "/tmp/sc_io_test2.txt";
        int fd_b = open(TMP2, O_RDWR|O_CREAT|O_TRUNC, 0644);
        if (fd_b < 0) { ng("dup2(close-target)/open_b", fd_b); close(fd_a); goto a14_end; }
        write(fd_b, "FILE_B", 6);
        lseek(fd_b, 0, SEEK_SET);

        // dup2(fd_b, fd_a): fd_a must be closed and replaced by fd_b
        int r = dup2(fd_b, fd_a);
        if (r != fd_a) { ng("dup2(close-target) return", r); close(fd_a); close(fd_b); goto a14_end; }

        // Reading fd_a must now yield FILE_B content
        char buf[16] = {0};
        long n = read(fd_a, buf, 15);
        if (n == 6 && memcmp(buf, "FILE_B", 6) == 0)
            ok("dup2 closes existing target fd");
        else
            ng("dup2 close-target read", n);

        close(fd_a);
        close(fd_b);
        unlink(TMP2);
    a14_end:
        cleanup(TMP);
    }

    // A15. ftruncate extend — growing a file fills with zeros
    {
        int fd = open(TMP, O_RDWR|O_CREAT|O_TRUNC, 0644);
        if (fd < 0) { ng("ftruncate(extend)/open", fd); goto a15_end; }

        write(fd, "AB", 2);
        ftruncate(fd, 6);   // extend from 2 to 6 bytes

        struct stat st;
        fstat(fd, &st);
        if (st.st_size != 6) { ng("ftruncate extend size", (long)st.st_size); close(fd); goto a15_end; }

        // Bytes 2-5 must be zero
        lseek(fd, 2, SEEK_SET);
        char buf[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        read(fd, buf, 4);
        if (buf[0] == 0 && buf[1] == 0 && buf[2] == 0 && buf[3] == 0)
            ok("ftruncate extend -> zero-filled");
        else
            ng("ftruncate extend zeros", (long)buf[0]);

        close(fd);
    a15_end:
        cleanup(TMP);
    }

    // ── SECTION 4: Directory tests ──────────────────────────────
    printf("\n--- Directory Tests ---\n");

    // Base test directory — everything lives under here
    const char* TDIR  = "/tmp/sc_dir_test";
    const char* TDIR2 = "/tmp/sc_dir_test2";

    // D1. mkdir — create a directory
    {
        rmdir(TDIR);   // clean up from any previous run
        int r = mkdir(TDIR, 0755);
        if (r == 0) ok("mkdir -> created");
        else ng("mkdir", r);
    }

    // D2. mkdir on existing path — must return error
    {
        int r = mkdir(TDIR, 0755);
        if (r < 0) ok("mkdir existing -> error expected");
        else ng("mkdir existing no error", r);
    }

    // D3. stat on directory — S_ISDIR must be set
    {
        struct stat st;
        memset(&st, 0, sizeof(st));
        int r = stat(TDIR, &st);
        if (r == 0 && S_ISDIR(st.st_mode))
            ok("stat(dir) -> S_ISDIR");
        else
            ng("stat(dir) mode", (long)st.st_mode);
    }

    // D4. chdir into the new directory
    {
        int r = chdir(TDIR);
        if (r == 0) ok("chdir -> success");
        else ng("chdir", r);
    }

    // D5. getcwd reflects the new working directory
    {
        char cwd[256];
        memset(cwd, 0, sizeof(cwd));
        getcwd(cwd, sizeof(cwd));

        // cwd must start with TDIR prefix
        int match = (strncmp(cwd, TDIR, strlen(TDIR)) == 0);
        if (match) ok("getcwd after chdir -> correct path");
        else ng("getcwd after chdir", (long)cwd[0]);
    }

    // D6. create a file inside the current (sub)directory using relative path
    {
        int fd = open("d6_file.txt", O_WRONLY|O_CREAT|O_TRUNC, 0644);
        if (fd < 0) { ng("create file in subdir", fd); goto d6_end; }
        long w = write(fd, "inside", 6);
        close(fd);
        if (w == 6) ok("create file in cwd subdir");
        else ng("write file in subdir", w);
    d6_end:;
    }

    // D7. stat on file created with relative path — must exist and be regular
    {
        struct stat st;
        memset(&st, 0, sizeof(st));
        // Use absolute path to be unambiguous
        char abs[300];
        snprintf(abs, sizeof(abs), "%s/d6_file.txt", TDIR);
        int r = stat(abs, &st);
        if (r == 0 && S_ISREG(st.st_mode) && st.st_size == 6)
            ok("stat file in subdir -> S_ISREG, size correct");
        else
            ng("stat file in subdir", (long)st.st_size);
    }

    // D8. chdir back to root so we can manipulate TDIR freely
    {
        int r = chdir("/");
        if (r == 0) ok("chdir('/') -> back to root");
        else ng("chdir('/')", r);
    }

    // D9. mkdir nested — create subdirectory inside TDIR
    {
        char nested[300];
        snprintf(nested, sizeof(nested), "%s/sub", TDIR);
        int r = mkdir(nested, 0755);
        if (r == 0) ok("mkdir nested subdir");
        else ng("mkdir nested", r);
    }

    // D10. stat nested dir — must be S_ISDIR
    {
        char nested[300];
        snprintf(nested, sizeof(nested), "%s/sub", TDIR);
        struct stat st;
        memset(&st, 0, sizeof(st));
        int r = stat(nested, &st);
        if (r == 0 && S_ISDIR(st.st_mode))
            ok("stat nested dir -> S_ISDIR");
        else
            ng("stat nested dir", r);
    }

    // D11. rmdir non-empty directory — must return error
    // TDIR still contains d6_file.txt and sub/, so rmdir should fail
    {
        int r = rmdir(TDIR);
        if (r < 0) ok("rmdir non-empty -> error expected");
        else ng("rmdir non-empty no error", r);
    }

    // D12. rmdir the empty nested dir — must succeed
    {
        char nested[300];
        snprintf(nested, sizeof(nested), "%s/sub", TDIR);
        int r = rmdir(nested);
        if (r == 0) ok("rmdir empty subdir -> success");
        else ng("rmdir empty subdir", r);
    }

    // D13. rename directory
    {
        // Remove TDIR2 if it exists from a previous run
        rmdir(TDIR2);

        int r = rename(TDIR, TDIR2);
        if (r == 0) ok("rename dir -> success");
        else ng("rename dir", r);
    }

    // D14. old name no longer accessible after rename
    {
        struct stat st;
        int r = stat(TDIR, &st);
        if (r < 0) ok("old dir name gone after rename");
        else ng("old dir still exists after rename", r);
    }

    // D15. new name is accessible and is a directory
    {
        struct stat st;
        memset(&st, 0, sizeof(st));
        int r = stat(TDIR2, &st);
        if (r == 0 && S_ISDIR(st.st_mode))
            ok("new dir name accessible after rename");
        else
            ng("new dir stat after rename", r);
    }

    // cleanup
    {
        char f[300];
        snprintf(f, sizeof(f), "%s/d6_file.txt", TDIR2);
        unlink(f);
        rmdir(TDIR2);
    }

    // ── SECTION 5: Time syscall tests ──────────────────────────
    printf("\n--- Time Tests ---\n");

    // T1. clock_gettime(CLOCK_REALTIME) — must succeed and return sane values
    {
        struct timespec ts;
        memset(&ts, 0xFF, sizeof(ts));   // poison with 0xFF to detect non-write
        int r = clock_gettime(CLOCK_REALTIME, &ts);
        if (r == 0 && ts.tv_sec >= 0 && ts.tv_nsec >= 0)
            ok("clock_gettime(CLOCK_REALTIME) -> success");
        else
            ng("clock_gettime(CLOCK_REALTIME)", r);
    }

    // T2. clock_gettime(CLOCK_MONOTONIC) — must succeed
    {
        struct timespec ts;
        memset(&ts, 0xFF, sizeof(ts));
        int r = clock_gettime(CLOCK_MONOTONIC, &ts);
        if (r == 0 && ts.tv_sec >= 0 && ts.tv_nsec >= 0)
            ok("clock_gettime(CLOCK_MONOTONIC) -> success");
        else
            ng("clock_gettime(CLOCK_MONOTONIC)", r);
    }

    // T3. clock_gettime(CLOCK_PROCESS_CPUTIME_ID) — must not crash
    {
        struct timespec ts;
        memset(&ts, 0, sizeof(ts));
        int r = clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
        // Kernel returns 0 (stub); result must be non-negative
        if (r == 0 && ts.tv_sec >= 0 && ts.tv_nsec >= 0)
            ok("clock_gettime(CLOCK_PROCESS_CPUTIME_ID) -> no crash");
        else
            ng("clock_gettime(CPUTIME)", r);
    }

    // T4. clock_gettime invalid clockid — must return error
    {
        struct timespec ts;
        int r = clock_gettime(9999, &ts);
        if (r < 0) ok("clock_gettime(invalid id) -> error expected");
        else ng("clock_gettime(invalid id)", r);
    }

    // T5. clock_gettime tv_nsec range — must be in [0, 999999999]
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        if (ts.tv_nsec >= 0 && ts.tv_nsec <= 999999999L)
            ok("clock_gettime tv_nsec in valid range");
        else
            ng("clock_gettime tv_nsec range", ts.tv_nsec);
    }

    // T6. CLOCK_MONOTONIC is non-decreasing across two calls
    {
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        // Burn a little time with yield loops
        for (int i = 0; i < 200; i++) _sc0(24);
        clock_gettime(CLOCK_MONOTONIC, &t1);

        long s0 = t0.tv_sec * 1000000000L + t0.tv_nsec;
        long s1 = t1.tv_sec * 1000000000L + t1.tv_nsec;
        if (s1 >= s0) ok("CLOCK_MONOTONIC is non-decreasing");
        else ng("CLOCK_MONOTONIC not monotonic", s1 - s0);
    }

    // T7. clock_getres(CLOCK_REALTIME) — must return resolution > 0
    {
        struct timespec res;
        memset(&res, 0xFF, sizeof(res));
        int r = clock_getres(CLOCK_REALTIME, &res);
        if (r == 0 && res.tv_sec >= 0 && res.tv_nsec > 0)
            ok("clock_getres(CLOCK_REALTIME) -> resolution > 0");
        else
            ng("clock_getres(CLOCK_REALTIME)", r);
    }

    // T8. clock_getres(CLOCK_MONOTONIC) — must match or be same as REALTIME
    {
        struct timespec res;
        memset(&res, 0, sizeof(res));
        int r = clock_getres(CLOCK_MONOTONIC, &res);
        if (r == 0 && res.tv_nsec > 0)
            ok("clock_getres(CLOCK_MONOTONIC) -> resolution > 0");
        else
            ng("clock_getres(CLOCK_MONOTONIC)", r);
    }

    // T9. clock_getres with NULL buf — must not crash (just validates clockid)
    {
        int r = clock_getres(CLOCK_MONOTONIC, NULL);
        if (r == 0) ok("clock_getres(NULL buf) -> no crash");
        else ng("clock_getres(NULL buf)", r);
    }

    // T10. gettimeofday — must return tv_sec >= 0 and tv_usec in [0, 999999]
    {
        struct timeval tv;
        memset(&tv, 0xFF, sizeof(tv));
        int r = gettimeofday(&tv, NULL);
        if (r == 0 && tv.tv_sec >= 0 && tv.tv_usec >= 0 && tv.tv_usec < 1000000)
            ok("gettimeofday -> valid tv_sec/tv_usec");
        else
            ng("gettimeofday", r);
    }

    // T11. gettimeofday is consistent with CLOCK_REALTIME
    // Both are tick-based on AscentOS, so they should return the same second.
    {
        struct timeval  tv;
        struct timespec ts;
        gettimeofday(&tv, NULL);
        clock_gettime(CLOCK_REALTIME, &ts);
        // Allow 1 second difference (timer tick boundary)
        long diff = tv.tv_sec - ts.tv_sec;
        if (diff >= -1 && diff <= 1)
            ok("gettimeofday consistent with CLOCK_REALTIME");
        else
            ng("gettimeofday vs CLOCK_REALTIME", diff);
    }

    // T12. nanosleep — sleep 1ms, must return 0 and not stall forever
    {
        struct timespec req = { .tv_sec = 0, .tv_nsec = 1000000 };  // 1 ms
        struct timespec rem = { .tv_sec = -1, .tv_nsec = -1 };
        int r = nanosleep(&req, &rem);
        // rem must be zeroed on clean return (no signal interrupted)
        if (r == 0 && rem.tv_sec == 0 && rem.tv_nsec == 0)
            ok("nanosleep(1ms) -> returns 0, rem zeroed");
        else
            ng("nanosleep(1ms)", r);
    }

    // T13. nanosleep(0,0) — zero sleep must return immediately
    {
        struct timespec req = { .tv_sec = 0, .tv_nsec = 0 };
        int r = nanosleep(&req, NULL);
        if (r == 0) ok("nanosleep(0,0) -> immediate return");
        else ng("nanosleep(0,0)", r);
    }

    // T14. nanosleep invalid tv_nsec — must return error (>= 1e9 is invalid)
    {
        struct timespec req = { .tv_sec = 0, .tv_nsec = 1000000000L };  // 1e9 = invalid
        int r = nanosleep(&req, NULL);
        if (r < 0) ok("nanosleep(invalid nsec) -> error expected");
        else ng("nanosleep(invalid nsec)", r);
    }

    // T15. Two sequential clock_gettime calls: second >= first (stress check)
    {
        struct timespec a, b;
        int ok_count = 0;
        for (int i = 0; i < 5; i++) {
            clock_gettime(CLOCK_MONOTONIC, &a);
            clock_gettime(CLOCK_MONOTONIC, &b);
            long na = a.tv_sec * 1000000000L + a.tv_nsec;
            long nb = b.tv_sec * 1000000000L + b.tv_nsec;
            if (nb >= na) ok_count++;
        }
        if (ok_count == 5) ok("clock_gettime monotonic across 5 rapid pairs");
        else ng("clock_gettime rapid monotonic", ok_count);
    }

    // ── SECTION 6: Process tests ────────────────────────────────
    printf("\n--- Process Tests ---\n");

    // P1. fork — returns child pid > 0 in parent, 0 in child
    {
        pid_t child = fork();
        if (child < 0) {
            ng("fork()", child);
        } else if (child == 0) {
            // Child: exit immediately with code 0
            _exit(0);
        } else {
            // Parent: wait and check
            int status = 0;
            waitpid(child, &status, 0);
            ok("fork -> parent gets child pid > 0");
        }
    }

    // P2. waitpid exit code — child exits with specific code, parent reads it
    {
        pid_t child = fork();
        if (child < 0) { ng("fork(exit code)", child); goto p2_end; }
        if (child == 0) _exit(42);

        int status = 0;
        waitpid(child, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 42)
            ok("waitpid -> exit code 42 received");
        else
            ng("waitpid exit code", WEXITSTATUS(status));
    p2_end:;
    }

    // P3. getpid in child — child's pid must differ from parent's
    {
        pid_t parent_pid = getpid();
        int pfd[2];
        pipe(pfd);

        pid_t child = fork();
        if (child < 0) { ng("fork(getpid child)", child); goto p3_end; }
        if (child == 0) {
            // Yield once so scheduler properly establishes child task context
            // before we call getpid() — task_get_current() may return NULL
            // on the very first syscall if the scheduler hasn't updated yet.
            _sc0(24);
            pid_t my_pid = getpid();
            write(pfd[1], &my_pid, sizeof(my_pid));
            close(pfd[1]);
            _exit(0);
        }

        close(pfd[1]);
        pid_t child_reported = 0;
        read(pfd[0], &child_reported, sizeof(child_reported));
        close(pfd[0]);
        waitpid(child, NULL, 0);

        if (child_reported != parent_pid && child_reported > 0)
            ok("getpid in child -> different from parent");
        else
            ng("getpid in child", child_reported);
    p3_end:;
    }

    // P4. getppid in child — must equal parent's pid
    {
        pid_t parent_pid = getpid();
        int pfd[2];
        pipe(pfd);

        pid_t child = fork();
        if (child < 0) { ng("fork(getppid child)", child); goto p4_end; }
        if (child == 0) {
            _sc0(24);   // yield to establish child scheduler context
            pid_t my_ppid = getppid();
            write(pfd[1], &my_ppid, sizeof(my_ppid));
            close(pfd[1]);
            _exit(0);
        }

        close(pfd[1]);
        pid_t child_ppid = 0;
        read(pfd[0], &child_ppid, sizeof(child_ppid));
        close(pfd[0]);
        waitpid(child, NULL, 0);

        if (child_ppid == parent_pid)
            ok("getppid in child -> equals parent pid");
        else
            ng("getppid in child", child_ppid);
    p4_end:;
    }

    // P5. pipe across fork — child writes, parent reads
    {
        int pfd[2];
        if (pipe(pfd) != 0) { ng("fork(pipe)/pipe", -1); goto p5_end; }

        pid_t child = fork();
        if (child < 0) { ng("fork(pipe)", child); goto p5_end; }
        if (child == 0) {
            close(pfd[0]);
            write(pfd[1], "hello_fork", 10);
            close(pfd[1]);
            _exit(0);
        }

        close(pfd[1]);
        char buf[16] = {0};
        long n = read(pfd[0], buf, 15);
        close(pfd[0]);
        waitpid(child, NULL, 0);

        if (n == 10 && memcmp(buf, "hello_fork", 10) == 0)
            ok("pipe across fork -> data received");
        else
            ng("pipe across fork read", n);
    p5_end:;
    }

    // P6. fork fd inheritance — child inherits parent's open file fd
    {
        const char* FORK_TMP = "/tmp/sc_fork_test.txt";
        int fd = open(FORK_TMP, O_WRONLY|O_CREAT|O_TRUNC, 0644);
        if (fd < 0) { ng("fork(fd inherit)/open", fd); goto p6_end; }

        pid_t child = fork();
        if (child < 0) { ng("fork(fd inherit)", child); close(fd); goto p6_end; }
        if (child == 0) {
            // Child writes through the inherited fd
            write(fd, "from_child", 10);
            close(fd);
            _exit(0);
        }

        close(fd);
        waitpid(child, NULL, 0);

        // Parent reads back what child wrote
        fd = open(FORK_TMP, O_RDONLY, 0);
        char buf[16] = {0};
        long n = read(fd, buf, 15);
        close(fd);
        unlink(FORK_TMP);

        if (n == 10 && memcmp(buf, "from_child", 10) == 0)
            ok("fork fd inheritance -> child write visible to parent");
        else
            ng("fork fd inherit read", n);
    p6_end:;
    }

    // P7. child writes a file, parent reads it after waitpid
    {
        const char* FORK_TMP2 = "/tmp/sc_fork_file.txt";
        unlink(FORK_TMP2);

        pid_t child = fork();
        if (child < 0) { ng("fork(child writes file)", child); goto p7_end; }
        if (child == 0) {
            int fd2 = open(FORK_TMP2, O_WRONLY|O_CREAT|O_TRUNC, 0644);
            write(fd2, "child_data", 10);
            close(fd2);
            _exit(0);
        }

        int status = 0;
        waitpid(child, &status, 0);

        int fd2 = open(FORK_TMP2, O_RDONLY, 0);
        char buf[16] = {0};
        long n = read(fd2, buf, 15);
        close(fd2);
        unlink(FORK_TMP2);

        if (WIFEXITED(status) && n == 10 && memcmp(buf, "child_data", 10) == 0)
            ok("child writes file -> parent reads after waitpid");
        else
            ng("child file write", n);
    p7_end:;
    }

    // P8. waitpid WNOHANG — child still running, must return 0 immediately
    {
        int pfd[2];
        pipe(pfd);
        pid_t child = fork();
        if (child < 0) { ng("fork(WNOHANG)", child); goto p8_end; }
        if (child == 0) {
            // Block until parent signals via pipe
            char c;
            read(pfd[0], &c, 1);
            _exit(0);
        }

        close(pfd[0]);
        // Child is blocked — WNOHANG must return 0 (not yet exited)
        int status = 0;
        pid_t r = waitpid(child, &status, WNOHANG);
        if (r == 0)
            ok("waitpid(WNOHANG) -> 0 while child running");
        else
            ng("waitpid(WNOHANG)", r);

        // Unblock child and clean up
        write(pfd[1], "x", 1);
        close(pfd[1]);
        waitpid(child, NULL, 0);
    p8_end:;
    }

    // P9. exit code 0 — WIFEXITED true, WEXITSTATUS == 0
    {
        pid_t child = fork();
        if (child < 0) { ng("fork(exit 0)", child); goto p9_end; }
        if (child == 0) _exit(0);

        int status = 0;
        waitpid(child, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
            ok("exit(0) -> WIFEXITED, WEXITSTATUS == 0");
        else
            ng("exit(0) status", status);
    p9_end:;
    }

    // P10. exit code 127 — boundary value (used by shells for "not found")
    {
        pid_t child = fork();
        if (child < 0) { ng("fork(exit 127)", child); goto p10_end; }
        if (child == 0) _exit(127);

        int status = 0;
        waitpid(child, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 127)
            ok("exit(127) -> WEXITSTATUS == 127");
        else
            ng("exit(127) status", WEXITSTATUS(status));
    p10_end:;
    }

    // P11. two sequential forks — both children reaped correctly
    {
        pid_t c1 = fork();
        if (c1 < 0) { ng("fork(seq c1)", c1); goto p11_end; }
        if (c1 == 0) _exit(1);

        pid_t c2 = fork();
        if (c2 < 0) {
            // Second fork failed (e.g. ENOMEM after many forks).
            // Still reap c1 to avoid zombie leak, then skip.
            waitpid(c1, NULL, 0);
            ng("fork(seq c2) - ENOMEM after many forks", c2);
            goto p11_end;
        }
        if (c2 == 0) _exit(2);

        int s1 = 0, s2 = 0;
        waitpid(c1, &s1, 0);
        waitpid(c2, &s2, 0);

        if (WIFEXITED(s1) && WEXITSTATUS(s1) == 1 &&
            WIFEXITED(s2) && WEXITSTATUS(s2) == 2)
            ok("two sequential forks -> both reaped with correct codes");
        else
            ng("sequential fork reap", WEXITSTATUS(s1) * 10 + WEXITSTATUS(s2));
    p11_end:;
    }

    // P12. deep fork chain — grandchild (fork inside forked child)
    // Parent forks child; child forks grandchild; grandchild exits 99;
    // child reaps grandchild then exits 55; parent reaps child and checks
    // both exit codes propagate correctly through two levels of waitpid.
    {
        int pfd[2];   // pipe: grandchild exit code relayed to parent
        if (pipe(pfd) < 0) { ng("fork-chain pipe", -1); goto p12_end; }

        pid_t child = fork();
        if (child < 0) { close(pfd[0]); close(pfd[1]); ng("fork(chain child)", child); goto p12_end; }
        if (child == 0) {
            // ── child ──
            close(pfd[0]);
            pid_t gc = fork();
            if (gc < 0) {
                write(pfd[1], "\xff", 1);   // signal failure
                close(pfd[1]);
                _exit(1);
            }
            if (gc == 0) {
                // ── grandchild ──
                close(pfd[1]);
                _exit(99);
            }
            // child waits for grandchild, relays its exit code
            int gstatus = 0;
            waitpid(gc, &gstatus, 0);
            unsigned char gcode = (unsigned char)WEXITSTATUS(gstatus);
            write(pfd[1], &gcode, 1);
            close(pfd[1]);
            _exit(55);
        }

        // ── parent ──
        close(pfd[1]);
        int cstatus = 0;
        waitpid(child, &cstatus, 0);

        unsigned char relayed = 0;
        int nr = read(pfd[0], &relayed, 1);
        close(pfd[0]);

        if (WIFEXITED(cstatus) && WEXITSTATUS(cstatus) == 55 &&
            nr == 1 && relayed == 99)
            ok("deep fork chain -> grandchild exit 99, child exit 55");
        else
            ng("deep fork chain", WEXITSTATUS(cstatus) * 1000 + relayed);
    p12_end:;
    }

    // P13. fork + pipe echo — child writes to pipe, parent reads back
    // Verifies that a larger payload (256 bytes) survives a fork+pipe round-trip,
    // exercising both the pipe buffer and fd inheritance across fork.
    {
        int pfd[2];
        if (pipe(pfd) < 0) { ng("echo-pipe", -1); goto p13_end; }

        // Build a known 256-byte pattern
        unsigned char pattern[256];
        for (int i = 0; i < 256; i++) pattern[i] = (unsigned char)(i & 0xFF);

        pid_t child = fork();
        if (child < 0) { close(pfd[0]); close(pfd[1]); ng("fork(echo-pipe)", child); goto p13_end; }
        if (child == 0) {
            close(pfd[0]);
            // Write in two halves to exercise multi-write
            write(pfd[1], pattern,       128);
            write(pfd[1], pattern + 128, 128);
            close(pfd[1]);
            _exit(0);
        }

        close(pfd[1]);
        unsigned char buf[256];
        int got = 0;
        while (got < 256) {
            int n = read(pfd[0], buf + got, 256 - got);
            if (n <= 0) break;
            got += n;
        }
        close(pfd[0]);
        waitpid(child, NULL, 0);

        int match = (got == 256);
        if (match)
            for (int i = 0; i < 256; i++)
                if (buf[i] != pattern[i]) { match = 0; break; }

        if (match)
            ok("fork + pipe echo -> 256-byte payload intact");
        else
            ng("fork pipe echo payload", got);
    p13_end:;
    }

    // P14. waitpid(-1) any-child — reap whichever child exits first
    // Fork two children with different exit codes; use waitpid(-1,...) twice.
    // Both must be reaped and their exit codes must be the set {3, 7}.
    {
        pid_t c1 = fork();
        if (c1 < 0) { ng("fork(any-child c1)", c1); goto p14_end; }
        if (c1 == 0) _exit(3);

        pid_t c2 = fork();
        if (c2 < 0) {
            waitpid(c1, NULL, 0);
            ng("fork(any-child c2)", c2);
            goto p14_end;
        }
        if (c2 == 0) _exit(7);

        int s1 = 0, s2 = 0;
        pid_t r1 = waitpid(-1, &s1, 0);
        pid_t r2 = waitpid(-1, &s2, 0);

        // r1 and r2 must be c1 and c2 in some order
        int codes_ok = 0;
        if ((r1 == c1 && r2 == c2) || (r1 == c2 && r2 == c1)) {
            int e1 = WEXITSTATUS(s1), e2 = WEXITSTATUS(s2);
            // The pair {e1,e2} must equal {3,7}
            if ((e1 == 3 && e2 == 7) || (e1 == 7 && e2 == 3))
                codes_ok = 1;
        }

        if (codes_ok)
            ok("waitpid(-1) any-child -> both reaped, codes {3,7}");
        else
            ng("waitpid(-1) any-child", WEXITSTATUS(s1)*10 + WEXITSTATUS(s2));
    p14_end:;
    }

    // P15. fork name stability — fork 4 levels deep, names must not corrupt
    // Each level forks the next, waits for it to exit 0, and exits 0 itself.
    // The original name-buffer overflow bug would corrupt task struct fields
    // causing either a kernel panic or a non-zero exit somewhere in the chain.
    // Simple exit(0) propagation makes the pass condition unambiguous.
    {
        pid_t l1 = fork();
        if (l1 < 0) { ng("fork(name-depth l1)", l1); goto p15_end; }
        if (l1 == 0) {
            pid_t l2 = fork();
            if (l2 < 0) _exit(1);
            if (l2 == 0) {
                pid_t l3 = fork();
                if (l3 < 0) _exit(1);
                if (l3 == 0) {
                    // Level 4: deepest — verify getpid() works and exit 0
                    _exit(getpid() > 0 ? 0 : 1);
                }
                int s = 0;
                pid_t r = waitpid(l3, &s, 0);
                _exit((r == l3 && WIFEXITED(s) && WEXITSTATUS(s) == 0) ? 0 : 1);
            }
            int s = 0;
            pid_t r = waitpid(l2, &s, 0);
            _exit((r == l2 && WIFEXITED(s) && WEXITSTATUS(s) == 0) ? 0 : 1);
        }

        int s = 0;
        waitpid(l1, &s, 0);
        if (WIFEXITED(s) && WEXITSTATUS(s) == 0)
            ok("fork name stability -> 4-level chain, no corruption");
        else
            ng("fork name stability", WEXITSTATUS(s));
    p15_end:;
    }

    // P16. fork + pipe: child closes write end, parent gets EOF
    // Verifies that when the only writer closes a pipe, the reader sees EOF
    // (read returns 0) rather than blocking forever.
    {
        int pfd[2];
        if (pipe(pfd) < 0) { ng("eof-pipe", -1); goto p16_end; }

        pid_t child = fork();
        if (child < 0) { ng("fork(eof-pipe)", child); goto p16_end; }
        if (child == 0) {
            close(pfd[0]);
            close(pfd[1]);   // close write end immediately — sends EOF
            _exit(0);
        }

        close(pfd[1]);   // parent also closes write end (only child was writer)
        char buf[4];
        int n = read(pfd[0], buf, sizeof(buf));
        close(pfd[0]);
        waitpid(child, NULL, 0);

        if (n == 0)
            ok("fork pipe EOF -> read returns 0 when writer closes");
        else
            ng("fork pipe EOF", n);
    p16_end:;
    }

    // P17. WNOHANG poll loop — parent spins until child actually exits
    // Verifies that repeated WNOHANG calls eventually converge to the real
    // exit code once the child has exited.
    {
        int pfd[2];
        if (pipe(pfd) < 0) { ng("wnohang-loop pipe", -1); goto p17_end; }

        pid_t child = fork();
        if (child < 0) { ng("fork(wnohang-loop)", child); goto p17_end; }
        if (child == 0) {
            // Wait for parent's "go" signal then exit
            char c; read(pfd[0], &c, 1);
            close(pfd[0]);
            _exit(88);
        }

        close(pfd[0]);

        // Confirm child is still running (WNOHANG should return 0)
        int status = 0;
        pid_t r = waitpid(child, &status, WNOHANG);
        int pre_ok = (r == 0);

        // Release child
        write(pfd[1], "g", 1);
        close(pfd[1]);

        // Poll until child exits (bounded to avoid infinite loop)
        int iters = 0;
        while (iters < 1000) {
            status = 0;
            r = waitpid(child, &status, WNOHANG);
            if (r == child) break;
            if (r < 0) break;
            iters++;
            // brief yield between polls
            struct timespec ts = {0, 1000000}; // 1ms
            nanosleep(&ts, NULL);
        }

        if (pre_ok && r == child && WIFEXITED(status) && WEXITSTATUS(status) == 88)
            ok("WNOHANG poll loop -> converges to exit code 88");
        else
            ng("WNOHANG poll loop", WEXITSTATUS(status));
    p17_end:;
    }

    // ── SECTION 7: mmap / munmap tests ─────────────────────────────────────
    printf("\n--- mmap / munmap Tests ---\n");

    // M1. Basic anonymous mmap — pointer must not be MAP_FAILED
    {
        void* p = mmap(NULL, 4096, PROT_READ|PROT_WRITE,
                       MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
        if (p != MAP_FAILED) ok("mmap anon -> valid pointer");
        else ng("mmap anon basic", -1);
        if (p != MAP_FAILED) munmap(p, 4096);
    }

    // M2. Anonymous map — zero-initialised
    {
        void* p = mmap(NULL, 4096, PROT_READ|PROT_WRITE,
                       MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) { ng("mmap anon(zero)/map", -1); goto m2_end; }
        unsigned char* b = (unsigned char*)p;
        int allzero = 1;
        for (int i = 0; i < 4096; i++) if (b[i] != 0) { allzero = 0; break; }
        if (allzero) ok("mmap anon -> zero-initialised");
        else         ng("mmap anon not zero", (long)b[0]);
        munmap(p, 4096);
    m2_end:;
    }

    // M3. Anonymous map — write/read full page
    {
        void* p = mmap(NULL, 4096, PROT_READ|PROT_WRITE,
                       MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) { ng("mmap anon(rw)/map", -1); goto m3_end; }
        unsigned char* b = (unsigned char*)p;
        for (int i = 0; i < 4096; i++) b[i] = (unsigned char)(i & 0xFF);
        int ok_flag = 1;
        for (int i = 0; i < 4096; i++)
            if (b[i] != (unsigned char)(i & 0xFF)) { ok_flag = 0; break; }
        if (ok_flag) ok("mmap anon -> write/read full page correct");
        else         ng("mmap anon rw mismatch", -1);
        munmap(p, 4096);
    m3_end:;
    }

    // M4. munmap returns 0
    {
        void* p = mmap(NULL, 4096, PROT_READ|PROT_WRITE,
                       MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) { ng("munmap(ret)/map", -1); goto m4_end; }
        int r = munmap(p, 4096);
        if (r == 0) ok("munmap -> returns 0");
        else        ng("munmap return value", r);
    m4_end:;
    }

    // M5. munmap + re-mmap usable
    {
        void* p1 = mmap(NULL, 4096, PROT_READ|PROT_WRITE,
                        MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
        if (p1 == MAP_FAILED) { ng("munmap(reuse)/map1", -1); goto m5_end; }
        munmap(p1, 4096);
        void* p2 = mmap(NULL, 4096, PROT_READ|PROT_WRITE,
                        MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
        if (p2 == MAP_FAILED) { ng("munmap(reuse)/map2", -1); goto m5_end; }
        ((char*)p2)[0] = 0x42;
        if (((char*)p2)[0] == 0x42) ok("munmap + re-mmap -> new mapping usable");
        else ng("munmap re-mmap content", -1);
        munmap(p2, 4096);
    m5_end:;
    }

    // M6. Large 16-page map — sentinel bytes + munmap
    {
        size_t sz = 16 * 4096;
        void* p = mmap(NULL, sz, PROT_READ|PROT_WRITE,
                       MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) { ng("munmap(large)/map", -1); goto m6_end; }
        unsigned char* b = (unsigned char*)p;
        b[0] = 0xAA; b[sz/2] = 0xBB; b[sz-1] = 0xCC;
        if (b[0] == 0xAA && b[sz/2] == 0xBB && b[sz-1] == 0xCC)
            ok("mmap 16-page -> sentinels at start/mid/end correct");
        else ng("mmap large sentinels", -1);
        int r = munmap(p, sz);
        if (r == 0) ok("munmap 16-page -> returns 0");
        else        ng("munmap large return", r);
    m6_end:;
    }

    const char* MMAP_FILE = "/tmp/sc_mmap_test.bin";

    // M7. File-backed MAP_PRIVATE read
    {
        cleanup(MMAP_FILE);
        int fd = open(MMAP_FILE, O_RDWR|O_CREAT|O_TRUNC, 0644);
        if (fd < 0) { ng("mmap(file)/open", fd); goto m7_end; }
        const char* payload = "FILEMMAP_OK";
        write(fd, payload, 11);
        void* p = mmap(NULL, 11, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);
        if (p == MAP_FAILED) { ng("mmap(file)/map", -1); goto m7_end; }
        if (memcmp(p, payload, 11) == 0)
            ok("mmap file-backed -> content matches written data");
        else ng("mmap file content mismatch", -1);
        munmap(p, 11);
    m7_end:
        cleanup(MMAP_FILE);
    }

    // M8. MAP_SHARED write — visible via read() after munmap
    {
        cleanup(MMAP_FILE);
        int fd = open(MMAP_FILE, O_RDWR|O_CREAT|O_TRUNC, 0644);
        if (fd < 0) { ng("mmap(shared-write)/open", fd); goto m8_end; }
        char zero[4096] = {0}; write(fd, zero, 4096);
        void* p = mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
        if (p == MAP_FAILED) { close(fd); ng("mmap(shared-write)/map", -1); goto m8_end; }
        memcpy(p, "SHARED_WR", 9);
        munmap(p, 4096);
        lseek(fd, 0, SEEK_SET);
        char buf[16] = {0}; read(fd, buf, 9);
        close(fd);
        if (memcmp(buf, "SHARED_WR", 9) == 0)
            ok("mmap MAP_SHARED write -> visible via read() after munmap");
        else ng("mmap shared write not persisted", (long)buf[0]);
    m8_end:
        cleanup(MMAP_FILE);
    }

    // M9. File offset=4096 — maps second page
    {
        cleanup(MMAP_FILE);
        int fd = open(MMAP_FILE, O_RDWR|O_CREAT|O_TRUNC, 0644);
        if (fd < 0) { ng("mmap(offset)/open", fd); goto m9_end; }
        char page[4096];
        memset(page, 'A', 4096); write(fd, page, 4096);
        memset(page, 'B', 4096); write(fd, page, 4096);
        void* p = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, fd, 4096);
        close(fd);
        if (p == MAP_FAILED) { ng("mmap(offset)/map", -1); goto m9_end; }
        unsigned char* b = (unsigned char*)p;
        int all_B = 1;
        for (int i = 0; i < 4096; i++) if (b[i] != 'B') { all_B = 0; break; }
        if (all_B) ok("mmap file offset=4096 -> maps second page correctly");
        else       ng("mmap offset page content", (long)b[0]);
        munmap(p, 4096);
    m9_end:
        cleanup(MMAP_FILE);
    }

    // M10. PROT_READ map — content readable
    {
        cleanup(MMAP_FILE);
        int fd = open(MMAP_FILE, O_RDWR|O_CREAT|O_TRUNC, 0644);
        if (fd < 0) { ng("prot_read/open", fd); goto m10_end; }
        write(fd, "READONLY", 8);
        void* p = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);
        if (p == MAP_FAILED) { ng("prot_read/map", -1); goto m10_end; }
        if (memcmp(p, "READONLY", 8) == 0) ok("PROT_READ map -> content readable");
        else ng("PROT_READ content wrong", -1);
        munmap(p, 4096);
    m10_end:
        cleanup(MMAP_FILE);
    }

    // M11. mprotect PROT_READ -> PROT_READ|PROT_WRITE
    {
        void* p = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) { ng("mprotect(upgrade)/map", -1); goto m11_end; }
        int r = mprotect(p, 4096, PROT_READ|PROT_WRITE);
        if (r != 0) { ng("mprotect upgrade -> returned error", r); munmap(p, 4096); goto m11_end; }
        ((char*)p)[0] = 0x55;
        if (((unsigned char*)p)[0] == 0x55)
            ok("mprotect PROT_READ -> PROT_READ|PROT_WRITE -> write ok");
        else ng("mprotect upgrade write failed", -1);
        munmap(p, 4096);
    m11_end:;
    }

    // ── SECTION 8: Advanced FD — pipe2, writev, getdents ───────────────────
    printf("\n--- Advanced FD Tests ---\n");

    // F1. pipe2 O_CLOEXEC — FD_CLOEXEC set on both ends
    {
        int pfd[2] = {-1, -1};
        long r = _pipe2(pfd, O_CLOEXEC);
        if (r != 0) { ng("pipe2(O_CLOEXEC)/call", r); goto f1_end; }
        int fl_r = fcntl(pfd[0], F_GETFD, 0);
        int fl_w = fcntl(pfd[1], F_GETFD, 0);
        close(pfd[0]); close(pfd[1]);
        if ((fl_r & FD_CLOEXEC) && (fl_w & FD_CLOEXEC))
            ok("pipe2(O_CLOEXEC) -> FD_CLOEXEC set on both ends");
        else ng("pipe2(O_CLOEXEC) flag missing", fl_r | fl_w);
    f1_end:;
    }

    // F2. pipe2 O_NONBLOCK — read on empty pipe returns error (EAGAIN)
    {
        int pfd[2] = {-1, -1};
        long r = _pipe2(pfd, O_NONBLOCK);
        if (r != 0) { ng("pipe2(O_NONBLOCK)/call", r); goto f2_end; }
        char buf[4];
        long n = read(pfd[0], buf, sizeof(buf));
        close(pfd[0]); close(pfd[1]);
        if (n < 0) ok("pipe2(O_NONBLOCK) -> read on empty pipe returns error (EAGAIN)");
        else ng("pipe2(O_NONBLOCK) read should have failed", n);
    f2_end:;
    }

    // F3. writev scatter — FOO+BAR+BAZ written in order
    {
        const char* WV_FILE = "/tmp/sc_writev_test.txt";
        cleanup(WV_FILE);
        int fd = open(WV_FILE, O_WRONLY|O_CREAT|O_TRUNC, 0644);
        if (fd < 0) { ng("writev(scatter)/open", fd); goto f3_end; }
        struct iovec iov[3];
        iov[0].iov_base = (void*)"FOO"; iov[0].iov_len = 3;
        iov[1].iov_base = (void*)"BAR"; iov[1].iov_len = 3;
        iov[2].iov_base = (void*)"BAZ"; iov[2].iov_len = 3;
        long w = writev(fd, iov, 3);
        close(fd);
        if (w != 9) { ng("writev scatter byte count", w); goto f3_end; }
        fd = open(WV_FILE, O_RDONLY, 0);
        if (fd < 0) { ng("writev(scatter)/reopen", fd); goto f3_end; }
        char buf[16] = {0}; read(fd, buf, 15); close(fd);
        if (memcmp(buf, "FOOBARBAZ", 9) == 0)
            ok("writev scatter -> FOO+BAR+BAZ written in order");
        else ng("writev scatter content wrong", (long)buf[0]);
    f3_end:
        cleanup(WV_FILE);
    }

    // F4. writev byte count — returns sum of iov_len (14)
    {
        const char* WV_FILE = "/tmp/sc_writev_test.txt";
        cleanup(WV_FILE);
        int fd = open(WV_FILE, O_WRONLY|O_CREAT|O_TRUNC, 0644);
        if (fd < 0) { ng("writev(bytecount)/open", fd); goto f4_end; }
        struct iovec iov[4];
        iov[0].iov_base = (void*)"A";       iov[0].iov_len = 1;
        iov[1].iov_base = (void*)"BCDE";    iov[1].iov_len = 4;
        iov[2].iov_base = (void*)"FG";      iov[2].iov_len = 2;
        iov[3].iov_base = (void*)"HIJKLMN"; iov[3].iov_len = 7;
        long w = writev(fd, iov, 4);
        close(fd);
        if (w == 14) ok("writev byte count -> returns sum of iov_len (14)");
        else ng("writev byte count wrong", w);
    f4_end:
        cleanup(WV_FILE);
    }

    // F5. getdents entry count — /tmp must yield at least . and .. (always present)
    // Uses SYS_OPENDIR (402) — kernel requires this for dir fds, not plain open()
    {
        long dfd = _opendir("/tmp");
        if (dfd < 0) { ng("getdents(count)/opendir", dfd); goto f5_end; }
        char buf[2048];
        long n = _getdents64((int)dfd, buf, sizeof(buf));
        close((int)dfd);
        if (n <= 0) { ng("getdents(count) returned <= 0", n); goto f5_end; }

        // Count ALL entries (including . and ..) — both must always be present
        int count = 0;
        long pos = 0;
        while (pos < n) {
            struct linux_dirent64* d = (struct linux_dirent64*)(buf + pos);
            if (d->d_reclen == 0) break;
            count++;
            pos += d->d_reclen;
        }

        if (count >= 2)
            ok("getdents -> /tmp yields at least . and .. entries");
        else
            ng("getdents count < 2", count);
    f5_end:;
    }

    // F6. getdents find known file — create file then find it in /tmp entries
    {
        const char* KNOWN     = "/tmp/sc_getdents_known.txt";
        const char* KNOWN_NAME = "sc_getdents_known.txt";
        cleanup(KNOWN);
        int kfd = open(KNOWN, O_WRONLY|O_CREAT|O_TRUNC, 0644);
        if (kfd < 0) { ng("getdents(find)/create", kfd); goto f6_end; }
        write(kfd, "x", 1); close(kfd);

        long dfd = _opendir("/tmp");
        if (dfd < 0) { ng("getdents(find)/opendir", dfd); goto f6_end; }
        char buf[4096];
        long n = _getdents64((int)dfd, buf, sizeof(buf));
        close((int)dfd);
        if (n <= 0) { ng("getdents(find) no entries", n); goto f6_end; }

        int found = 0;
        long pos = 0;
        while (pos < n) {
            struct linux_dirent64* d = (struct linux_dirent64*)(buf + pos);
            if (d->d_reclen == 0) break;
            if (strcmp(d->d_name, KNOWN_NAME) == 0) { found = 1; break; }
            pos += d->d_reclen;
        }
        if (found) ok("getdents -> known file found in /tmp entries");
        else ng("getdents known file not found", 0);
        cleanup(KNOWN);
    f6_end:;
    }

    // ── SECTION 9: uname ───────────────────────────────────────────────────
    printf("\n--- uname Tests ---\n");

    // U1. uname returns 0 and fills sysname with a non-empty string
    {
        struct utsname u;
        memset(&u, 0, sizeof(u));
        int r = uname(&u);
        if (r == 0 && u.sysname[0] != '\0')
            ok("uname -> returns 0 and sysname is non-empty");
        else
            ng("uname return/sysname", r);
    }

    // U2. machine field must be "x86_64" (Linux x86-64 ABI compatibility)
    {
        struct utsname u;
        memset(&u, 0, sizeof(u));
        uname(&u);
        if (strcmp(u.machine, "x86_64") == 0)
            ok("uname -> machine == \"x86_64\"");
        else
            ng("uname machine wrong", (long)u.machine[0]);
    }

    // U3. nodename field must be non-empty (hostname is set at boot)
    {
        struct utsname u;
        memset(&u, 0, sizeof(u));
        uname(&u);
        if (u.nodename[0] != '\0')
            ok("uname -> nodename is non-empty");
        else
            ng("uname nodename empty", 0);
    }

    // ── SECTION 10: hard link / link(2) ────────────────────────────────────
    // NOTE: AscentOS ext3 has no true inode hard links; link(2) is copy-based.
    // st_nlink == 2 is therefore NOT tested. Behaviour tested instead:
    //   - new path is readable after link()
    //   - content of new path matches source
    //   - linking a directory is rejected (EPERM)
    //   - linking over an existing path is rejected (EEXIST / -17)
    printf("\n--- hard link (link(2)) Tests ---\n");

    const char* LNK_SRC  = "/tmp/sc_link_src.txt";
    const char* LNK_DST  = "/tmp/sc_link_dst.txt";
    const char* LNK_PAYLOAD = "LINKTEST_CONTENT";
    const int   LNK_PAYLEN  = 16;

    // L1. link() returns 0 and new path becomes readable
    {
        cleanup(LNK_SRC); cleanup(LNK_DST);
        int fd = open(LNK_SRC, O_WRONLY|O_CREAT|O_TRUNC, 0644);
        if (fd < 0) { ng("link(readable)/create-src", fd); goto l1_end; }
        write(fd, LNK_PAYLOAD, LNK_PAYLEN);
        close(fd);

        int r = link(LNK_SRC, LNK_DST);
        if (r != 0) { ng("link(readable)/link()", r); goto l1_end; }

        // dst must now open successfully
        fd = open(LNK_DST, O_RDONLY, 0);
        if (fd >= 0) {
            close(fd);
            ok("link() -> new path is readable");
        } else {
            ng("link(readable) -> dst not openable", fd);
        }
    l1_end:
        cleanup(LNK_SRC); cleanup(LNK_DST);
    }

    // L2. content of new path matches original source content
    {
        cleanup(LNK_SRC); cleanup(LNK_DST);
        int fd = open(LNK_SRC, O_WRONLY|O_CREAT|O_TRUNC, 0644);
        if (fd < 0) { ng("link(content)/create-src", fd); goto l2_end; }
        write(fd, LNK_PAYLOAD, LNK_PAYLEN);
        close(fd);

        int r = link(LNK_SRC, LNK_DST);
        if (r != 0) { ng("link(content)/link()", r); goto l2_end; }

        char buf[32] = {0};
        fd = open(LNK_DST, O_RDONLY, 0);
        if (fd < 0) { ng("link(content)/open-dst", fd); goto l2_end; }
        read(fd, buf, LNK_PAYLEN);
        close(fd);

        if (memcmp(buf, LNK_PAYLOAD, LNK_PAYLEN) == 0)
            ok("link() -> dst content matches src content");
        else
            ng("link(content) content mismatch", (long)buf[0]);
    l2_end:
        cleanup(LNK_SRC); cleanup(LNK_DST);
    }

    // L3. link() on a directory must fail with EPERM (-1)
    {
        // /tmp is always present and is a directory
        int r = link("/tmp", "/tmp/sc_link_dir_dst");
        if (r < 0)
            ok("link(dir) -> rejected with error (EPERM expected)");
        else {
            ng("link(dir) should have failed", r);
            unlink("/tmp/sc_link_dir_dst");
        }
    }

    // L4. link() over an already-existing path must fail (EEXIST / -17)
    {
        cleanup(LNK_SRC); cleanup(LNK_DST);
        int fd = open(LNK_SRC, O_WRONLY|O_CREAT|O_TRUNC, 0644);
        if (fd < 0) { ng("link(eexist)/create-src", fd); goto l4_end; }
        write(fd, "x", 1); close(fd);

        fd = open(LNK_DST, O_WRONLY|O_CREAT|O_TRUNC, 0644);
        if (fd < 0) { ng("link(eexist)/create-dst", fd); goto l4_end; }
        write(fd, "y", 1); close(fd);

        // dst already exists — link() must refuse
        int r = link(LNK_SRC, LNK_DST);
        if (r < 0)
            ok("link() over existing path -> rejected (EEXIST expected)");
        else
            ng("link(eexist) should have returned error", r);
    l4_end:
        cleanup(LNK_SRC); cleanup(LNK_DST);
    }

    // ── SECTION 11: brk / sbrk ─────────────────────────────────────────────
    //
    // Linux x86-64 ABI for brk(2):
    //   syscall 12, arg1 = new_brk_addr
    //   - brk(0)      → returns current program break (never fails)
    //   - brk(addr)   → on success returns addr; on failure returns OLD break
    //                   (NEVER returns -errno — errno is set by libc wrapper)
    //
    // sbrk(3) is a libc shim:
    //   sbrk(0)  → current break  (calls brk(0))
    //   sbrk(+N) → old break      (calls brk(cur + N), returns old on success)
    //   sbrk(-N) → old break      (calls brk(cur - N), releases pages)
    //   sbrk(N)  → (void*)-1 on failure (sets errno = ENOMEM)
    //
    // All raw brk() invocations below go through the inline _sys_brk() wrapper
    // to exercise syscall 12 directly, matching AscentOS's SYS_BRK = 12.
    printf("\n--- brk / sbrk Tests ---\n");

    // Raw brk syscall wrapper — matches Linux x86-64 brk(2) exactly.
    // Returns the new (or current) program break; never a negative error code.
#define _sys_brk(addr) ({ \
    long _r; \
    __asm__ volatile("syscall" \
        : "=a"(_r) \
        : "0"(12L), "D"((long)(addr)) \
        : "rcx", "r11", "memory"); \
    (void*)_r; \
})

    // B1. brk(0) — query current break; must be a non-NULL canonical address
    {
        void* cur = _sys_brk(0);
        // Valid user-space address: above 4 KB (not NULL / low trap page),
        // below 128 TiB (Linux x86-64 user address space limit).
        if (cur > (void*)0x1000 && cur < (void*)0x0000800000000000ULL)
            ok("brk(0) -> returns valid current break address");
        else
            ng("brk(0) invalid address", (long)cur);
    }

    // B2. brk(cur + 4096) — grow heap by one page; kernel must honour it
    {
        void* before = _sys_brk(0);
        void* target = (char*)before + 4096;
        void* after  = _sys_brk(target);
        // On success the kernel returns exactly the requested address.
        if (after == target)
            ok("brk(cur+4096) -> heap grows by one page");
        else
            ng("brk(cur+4096) returned wrong address", (long)after);
        // Restore (best-effort; not fatal if it fails)
        _sys_brk(before);
    }

    // B3. brk(cur + 4096) — break after grow must be >= before
    {
        void* before = _sys_brk(0);
        void* target = (char*)before + 4096;
        _sys_brk(target);
        void* after = _sys_brk(0);   // re-query
        if (after >= before)
            ok("brk grow -> re-queried break is >= previous break");
        else
            ng("brk grow -> break went backwards", (long)(after - before));
        _sys_brk(before);
    }

    // B4. sbrk(0) — libc wrapper; must return same value as brk(0) syscall
    {
        void* raw = _sys_brk(0);
        void* lib = sbrk(0);
        // After any previous _sys_brk restores, both must agree.
        if (lib == raw)
            ok("sbrk(0) == brk(0) raw syscall");
        else
            ng("sbrk(0) != brk(0) raw", (long)((char*)lib - (char*)raw));
    }

    // B5. sbrk(+4096) — returns OLD break, new break must advance by 4096
    {
        void* old_brk = sbrk(0);
        void* ret     = sbrk(4096);
        void* new_brk = sbrk(0);
        if (ret == old_brk && (char*)new_brk == (char*)old_brk + 4096)
            ok("sbrk(+4096) -> returns old break, new break advances by 4096");
        else
            ng("sbrk(+4096) semantics wrong",
               (long)((char*)new_brk - (char*)old_brk));
        sbrk(-4096);   // release
    }

    // B6. sbrk(-N) — shrink: break must retreat by exactly N bytes
    {
        sbrk(8192);                   // allocate 8 KiB first
        void* before_shrink = sbrk(0);
        void* ret           = sbrk(-4096);
        void* after_shrink  = sbrk(0);
        int ok_flag = (ret == before_shrink) &&
                      ((char*)after_shrink == (char*)before_shrink - 4096);
        if (ok_flag)
            ok("sbrk(-4096) -> break retreats by 4096 bytes");
        else
            ng("sbrk(-4096) retreat wrong",
               (long)((char*)after_shrink - (char*)before_shrink));
        sbrk(-4096);   // release the first 8 KiB block
    }

    // B7. sbrk write/read — memory allocated via sbrk must be writable and readable
    {
        void* p = sbrk(4096);
        if (p == (void*)-1) { ng("sbrk(write/read)/alloc", -1); goto b7_end; }
        // Write a known pattern across the allocated page
        unsigned char* b = (unsigned char*)p;
        for (int i = 0; i < 4096; i++) b[i] = (unsigned char)(i & 0xFF);
        int ok_flag = 1;
        for (int i = 0; i < 4096; i++)
            if (b[i] != (unsigned char)(i & 0xFF)) { ok_flag = 0; break; }
        if (ok_flag)
            ok("sbrk alloc -> page is writable and readable");
        else
            ng("sbrk alloc -> read-back mismatch", 0);
        sbrk(-4096);
    b7_end:;
    }

    // B8. two consecutive sbrk(+N) calls — break must advance cumulatively
    {
        void* base = sbrk(0);
        sbrk(4096);
        sbrk(4096);
        void* top = sbrk(0);
        if ((char*)top == (char*)base + 8192)
            ok("two sbrk(+4096) calls -> break advances by 8192 total");
        else
            ng("double sbrk cumulative advance wrong",
               (long)((char*)top - (char*)base));
        sbrk(-8192);
    }

    // B9. brk below current break — Linux returns the CURRENT (unchanged) break,
    //     not an error code.  The break must NOT move below its current position.
    {
        // Establish a known position first
        void* cur = _sys_brk(0);
        // Request a break far below the current value (NULL / page 0)
        void* ret = _sys_brk((void*)0x1000);
        (void)ret;
        void* after = _sys_brk(0);
        // Kernel must leave break at cur (or higher), never at 0x1000
        if (after >= cur)
            ok("brk(addr < cur) -> break unchanged (Linux semantics)");
        else
            ng("brk(addr < cur) moved break backwards", (long)after);
    }

    // B10. sbrk(0) after malloc — sbrk must still return a valid non-NULL address
    //      (malloc may itself use brk; this ensures the two do not conflict
    //       in ways that corrupt the break pointer)
    {
        void* before = sbrk(0);
        void* m = malloc(1024);
        void* after  = sbrk(0);
        free(m);
        // break must be >= before (malloc may have advanced it), never behind
        if (after >= before && after != (void*)-1)
            ok("sbrk(0) after malloc -> break is valid and non-regressing");
        else
            ng("sbrk(0) after malloc invalid", (long)after);
    }

    // ── SECTION 12: readv (gather read) ────────────────────────────────────
    //
    // Linux x86-64 ABI for readv(2):
    //   syscall 19, arg1=fd, arg2=iov[], arg3=iovcnt
    //   Returns total bytes placed across all buffers, or -errno on error.
    //   Buffers are filled left-to-right; a short file fills only as many
    //   vectors as data permits.
    printf("\n--- readv Tests ---\n");

    const char* RV_FILE = "/tmp/sc_readv_test.txt";

    // RV1. readv into a single iovec — full content returned in one buffer
    {
        cleanup(RV_FILE);
        int fd = open(RV_FILE, O_RDWR|O_CREAT|O_TRUNC, 0644);
        if (fd < 0) { ng("readv(single)/open", fd); goto rv1_end; }

        write(fd, "HELLO", 5);
        lseek(fd, 0, SEEK_SET);

        char buf[16] = {0};
        struct iovec iov[1];
        iov[0].iov_base = buf;
        iov[0].iov_len  = sizeof(buf) - 1;

        long r = _readv(fd, iov, 1);

        if (r == 5 && memcmp(buf, "HELLO", 5) == 0)
            ok("readv(single iovec) -> full content read");
        else
            ng("readv(single iovec) content/count wrong", r);

        close(fd);
    rv1_end:
        cleanup(RV_FILE);
    }

    // RV2. readv scatter into 3 iovecs — each buffer filled in order
    {
        cleanup(RV_FILE);
        int fd = open(RV_FILE, O_RDWR|O_CREAT|O_TRUNC, 0644);
        if (fd < 0) { ng("readv(scatter)/open", fd); goto rv2_end; }

        write(fd, "FOOBARBAZ", 9);   // 9 bytes: 3+3+3
        lseek(fd, 0, SEEK_SET);

        char b0[4] = {0}, b1[4] = {0}, b2[4] = {0};
        struct iovec iov[3];
        iov[0].iov_base = b0; iov[0].iov_len = 3;
        iov[1].iov_base = b1; iov[1].iov_len = 3;
        iov[2].iov_base = b2; iov[2].iov_len = 3;

        long r = _readv(fd, iov, 3);

        if (r == 9 &&
            memcmp(b0, "FOO", 3) == 0 &&
            memcmp(b1, "BAR", 3) == 0 &&
            memcmp(b2, "BAZ", 3) == 0)
            ok("readv(3 iovecs) -> buffers filled in order (FOO/BAR/BAZ)");
        else
            ng("readv scatter content wrong", r);

        close(fd);
    rv2_end:
        cleanup(RV_FILE);
    }

    // RV3. readv byte count — return value equals sum of bytes actually read
    {
        cleanup(RV_FILE);
        int fd = open(RV_FILE, O_RDWR|O_CREAT|O_TRUNC, 0644);
        if (fd < 0) { ng("readv(bytecount)/open", fd); goto rv3_end; }

        // Write 14 bytes total: 1+4+2+7
        write(fd, "ABCDEFGHIJKLMN", 14);
        lseek(fd, 0, SEEK_SET);

        char b0[2]={0}, b1[5]={0}, b2[3]={0}, b3[8]={0};
        struct iovec iov[4];
        iov[0].iov_base = b0; iov[0].iov_len = 1;
        iov[1].iov_base = b1; iov[1].iov_len = 4;
        iov[2].iov_base = b2; iov[2].iov_len = 2;
        iov[3].iov_base = b3; iov[3].iov_len = 7;

        long r = _readv(fd, iov, 4);

        if (r == 14)
            ok("readv byte count -> returns sum of iov_len filled (14)");
        else
            ng("readv byte count wrong", r);

        close(fd);
    rv3_end:
        cleanup(RV_FILE);
    }

    // RV4. readv partial fill — file shorter than iovec capacity;
    //      return value == file size, trailing buffers stay zeroed
    {
        cleanup(RV_FILE);
        int fd = open(RV_FILE, O_RDWR|O_CREAT|O_TRUNC, 0644);
        if (fd < 0) { ng("readv(partial)/open", fd); goto rv4_end; }

        write(fd, "AB", 2);   // only 2 bytes
        lseek(fd, 0, SEEK_SET);

        char b0[4] = {0}, b1[4] = {0};
        struct iovec iov[2];
        iov[0].iov_base = b0; iov[0].iov_len = 4;   // first buf: capacity 4
        iov[1].iov_base = b1; iov[1].iov_len = 4;   // second buf: capacity 4

        long r = _readv(fd, iov, 2);

        // Must read exactly 2 bytes; b0[0..1]=="AB"; b0[2..3] and b1 stay zero
        if (r == 2 && b0[0] == 'A' && b0[1] == 'B' && b0[2] == '\0' && b1[0] == '\0')
            ok("readv partial fill -> returns actual bytes; trailing buffers zeroed");
        else
            ng("readv partial fill wrong", r);

        close(fd);
    rv4_end:
        cleanup(RV_FILE);
    }

    // RV5. readv with iovcnt==0 — must return 0 (no-op), not an error
    {
        cleanup(RV_FILE);
        int fd = open(RV_FILE, O_RDWR|O_CREAT|O_TRUNC, 0644);
        if (fd < 0) { ng("readv(empty)/open", fd); goto rv5_end; }

        write(fd, "X", 1);
        lseek(fd, 0, SEEK_SET);

        struct iovec iov[1];   // unused, but must be a valid ptr
        long r = _readv(fd, iov, 0);

        if (r == 0)
            ok("readv(iovcnt=0) -> returns 0 (no-op)");
        else
            ng("readv(iovcnt=0) unexpected return", r);

        close(fd);
    rv5_end:
        cleanup(RV_FILE);
    }

    // RV6. readv on a bad fd — must return an error (< 0), not crash
    {
        char buf[8] = {0};
        struct iovec iov[1];
        iov[0].iov_base = buf;
        iov[0].iov_len  = sizeof(buf);

        long r = _readv(999, iov, 1);

        if (r < 0)
            ok("readv(bad_fd) -> returns error (EBADF expected)");
        else
            ng("readv(bad_fd) should have failed", r);
    }

    // ── SECTION 13: lseek extended ─────────────────────────────────────────
    //
    // Linux x86-64 ABI for lseek(2):
    //   syscall 8, arg1=fd, arg2=offset, arg3=whence
    //   Returns new file offset (>= 0) on success, -errno on error.
    //   SEEK_SET=0, SEEK_CUR=1, SEEK_END=2
    printf("\n--- lseek Extended Tests ---\n");

    const char* LS_FILE = "/tmp/sc_lseek_ext_test.txt";

    // LS1. SEEK_END with negative offset — positions inside the file
    {
        cleanup(LS_FILE);
        int fd = open(LS_FILE, O_RDWR|O_CREAT|O_TRUNC, 0644);
        if (fd < 0) { ng("lseek(SEEK_END-neg)/open", fd); goto ls1_end; }

        write(fd, "ABCDE", 5);   // 5 bytes -> EOF at offset 5

        // Seek to 2 bytes before EOF -> offset 3 -> should read 'D'
        long pos = lseek(fd, -2, SEEK_END);
        char c = 0;
        read(fd, &c, 1);

        if (pos == 3 && c == 'D')
            ok("lseek(SEEK_END, -2) -> positions 2 bytes before EOF, reads correct byte");
        else
            ng("lseek(SEEK_END, -2) wrong position or byte", pos);

        close(fd);
    ls1_end:
        cleanup(LS_FILE);
    }

    // LS2. SEEK_CUR reports current position after reads
    {
        cleanup(LS_FILE);
        int fd = open(LS_FILE, O_RDWR|O_CREAT|O_TRUNC, 0644);
        if (fd < 0) { ng("lseek(SEEK_CUR-report)/open", fd); goto ls2_end; }

        write(fd, "ABCDEF", 6);
        lseek(fd, 0, SEEK_SET);

        char tmp[3];
        read(fd, tmp, 3);   // read 3 bytes -> position is now 3

        // lseek(fd, 0, SEEK_CUR) is the POSIX way to query current offset
        long pos = lseek(fd, 0, SEEK_CUR);

        if (pos == 3)
            ok("lseek(SEEK_CUR, 0) after read(3) -> reports offset 3");
        else
            ng("lseek(SEEK_CUR) report wrong", pos);

        close(fd);
    ls2_end:
        cleanup(LS_FILE);
    }

    // LS3. Seek past EOF — file size must NOT change until a write occurs
    {
        cleanup(LS_FILE);
        int fd = open(LS_FILE, O_RDWR|O_CREAT|O_TRUNC, 0644);
        if (fd < 0) { ng("lseek(past-EOF)/open", fd); goto ls3_end; }

        write(fd, "HI", 2);   // 2-byte file

        long pos = lseek(fd, 100, SEEK_SET);   // seek well past EOF

        struct stat st;
        memset(&st, 0, sizeof(st));
        fstat(fd, &st);

        // Position must be 100; file size must still be 2 (no implicit extension)
        if (pos == 100 && st.st_size == 2)
            ok("lseek past EOF -> offset advances but st_size unchanged");
        else
            ng("lseek past EOF: pos or size wrong", (long)st.st_size);

        close(fd);
    ls3_end:
        cleanup(LS_FILE);
    }

    // LS4. lseek on a bad fd — must return error (< 0), no crash
    {
        long r = lseek(999, 0, SEEK_SET);
        if (r < 0)
            ok("lseek(bad_fd) -> returns error (EBADF expected)");
        else
            ng("lseek(bad_fd) should have failed", r);
    }

    // LS5. lseek(SEEK_SET, 0) on a write-only fd — must succeed (returns 0)
    //      Seeking is independent of read/write permission.
    {
        cleanup(LS_FILE);
        int fd = open(LS_FILE, O_WRONLY|O_CREAT|O_TRUNC, 0644);
        if (fd < 0) { ng("lseek(wronly)/open", fd); goto ls5_end; }

        write(fd, "TEST", 4);

        long pos = lseek(fd, 0, SEEK_SET);

        if (pos == 0)
            ok("lseek(SEEK_SET, 0) on O_WRONLY fd -> returns 0 (seek independent of mode)");
        else
            ng("lseek on O_WRONLY fd wrong", pos);

        close(fd);
    ls5_end:
        cleanup(LS_FILE);
    }

    // LS6. Chained SEEK_CUR advances — cumulative offset must be exact
    {
        cleanup(LS_FILE);
        int fd = open(LS_FILE, O_RDWR|O_CREAT|O_TRUNC, 0644);
        if (fd < 0) { ng("lseek(chain)/open", fd); goto ls6_end; }

        write(fd, "ABCDEFGHIJ", 10);
        lseek(fd, 0, SEEK_SET);

        lseek(fd, 2, SEEK_CUR);   // -> 2
        lseek(fd, 3, SEEK_CUR);   // -> 5
        long pos = lseek(fd, 1, SEEK_CUR);   // -> 6

        char c = 0;
        read(fd, &c, 1);   // should read byte at offset 6 == 'G'

        if (pos == 6 && c == 'G')
            ok("lseek chained SEEK_CUR -> cumulative offset correct, reads right byte");
        else
            ng("lseek chain SEEK_CUR wrong", pos);

        close(fd);
    ls6_end:
        cleanup(LS_FILE);
    }

    // ── SECTION 14: readv stress tests ────────────────────────────────────────
    //
    // These tests exercise readv under repeated, varied, and adversarial
    // conditions to catch off-by-one errors, iovec accounting bugs, and
    // fd-state corruption that single-shot tests cannot expose.
    printf("\n--- readv Stress Tests ---\n");

    const char* SRV_FILE = "/tmp/sc_readv_stress.txt";

    // RS1. 1000-iteration scatter correctness
    //      Write a fixed 9-byte pattern, rewind, readv into 3 bufs, verify,
    //      repeat 1000 times. Catches position-tracking bugs that only surface
    //      after many seeks.
    {
        cleanup(SRV_FILE);
        int fd = open(SRV_FILE, O_RDWR|O_CREAT|O_TRUNC, 0644);
        if (fd < 0) { ng("readv-stress(iter)/open", fd); goto rs1_end; }

        write(fd, "FOOBARBAZ", 9);

        int ok_count = 0;
        for (int i = 0; i < 1000; i++) {
            lseek(fd, 0, SEEK_SET);
            char b0[3]={0}, b1[3]={0}, b2[3]={0};
            struct iovec iov[3];
            iov[0].iov_base = b0; iov[0].iov_len = 3;
            iov[1].iov_base = b1; iov[1].iov_len = 3;
            iov[2].iov_base = b2; iov[2].iov_len = 3;
            long r = _readv(fd, iov, 3);
            if (r == 9 &&
                memcmp(b0, "FOO", 3) == 0 &&
                memcmp(b1, "BAR", 3) == 0 &&
                memcmp(b2, "BAZ", 3) == 0)
                ok_count++;
        }

        if (ok_count == 1000)
            ok("readv stress: 1000 iterations scatter correct every time");
        else
            ng("readv stress: scatter mismatch in iteration loop", ok_count);

        close(fd);
    rs1_end:
        cleanup(SRV_FILE);
    }

    // RS2. 64-iovec fan-out
    //      Write 64 bytes (one per iovec), scatter-read into 64 single-byte
    //      buffers. Verifies the kernel correctly walks the full iov[] array
    //      even when iovcnt approaches IOV_MAX.
    {
        cleanup(SRV_FILE);
        int fd = open(SRV_FILE, O_RDWR|O_CREAT|O_TRUNC, 0644);
        if (fd < 0) { ng("readv-stress(64iov)/open", fd); goto rs2_end; }

        // Write bytes 0x00..0x3F so each is uniquely identifiable
        char wbuf[64];
        for (int i = 0; i < 64; i++) wbuf[i] = (char)i;
        write(fd, wbuf, 64);
        lseek(fd, 0, SEEK_SET);

        char rbuf[64];
        memset(rbuf, 0xFF, 64);
        struct iovec iov[64];
        for (int i = 0; i < 64; i++) {
            iov[i].iov_base = &rbuf[i];
            iov[i].iov_len  = 1;
        }

        long r = _readv(fd, iov, 64);

        int match = (r == 64);
        for (int i = 0; i < 64 && match; i++)
            if (rbuf[i] != (char)i) match = 0;

        if (match)
            ok("readv stress: 64-iovec fan-out, all bytes placed correctly");
        else
            ng("readv stress: 64-iovec fan-out mismatch", r);

        close(fd);
    rs2_end:
        cleanup(SRV_FILE);
    }

    // RS3. Interleaved readv + lseek — alternating readv and SEEK_SET in a loop
    //      ensures that lseek properly resets the file offset between readv calls
    //      and that readv does not cache or corrupt the position.
    {
        cleanup(SRV_FILE);
        int fd = open(SRV_FILE, O_RDWR|O_CREAT|O_TRUNC, 0644);
        if (fd < 0) { ng("readv-stress(interleave)/open", fd); goto rs3_end; }

        // 8 distinct 4-byte words side-by-side; we'll seek to each and readv it
        const char* pattern = "AAAABBBBCCCCDDDDEEEEFFFFGGGGHHHH";  // 8*4 = 32 bytes
        write(fd, pattern, 32);

        int errors = 0;
        // Read each 4-byte word in reverse order via lseek+readv
        for (int i = 7; i >= 0; i--) {
            lseek(fd, i * 4, SEEK_SET);
            char b0[2]={0}, b1[2]={0};
            struct iovec iov[2];
            iov[0].iov_base = b0; iov[0].iov_len = 2;
            iov[1].iov_base = b1; iov[1].iov_len = 2;
            long r = _readv(fd, iov, 2);
            // All 4 bytes at offset i*4 should be the same character
            char expected = pattern[i * 4];
            if (r != 4 || b0[0] != expected || b0[1] != expected ||
                b1[0] != expected || b1[1] != expected)
                errors++;
        }

        if (errors == 0)
            ok("readv stress: interleaved readv+lseek, all words correct");
        else
            ng("readv stress: interleaved readv+lseek mismatch count", errors);

        close(fd);
    rs3_end:
        cleanup(SRV_FILE);
    }

    // RS4. Pipe-backed readv — readv must work on pipe fds, not just regular files.
    //      Pipes are a common readv target (e.g. socket/network code paths) and
    //      exercise a different kernel code path than file-backed reads.
    {
        int pfd[2] = {-1, -1};
        if (pipe(pfd) != 0) { ng("readv-stress(pipe)/pipe", -1); goto rs4_end; }

        // Write 12 bytes in one shot; read them back with a 3-iovec readv
        write(pfd[1], "HELLOWORLD!!", 12);
        close(pfd[1]);

        char b0[5]={0}, b1[5]={0}, b2[3]={0};
        struct iovec iov[3];
        iov[0].iov_base = b0; iov[0].iov_len = 5;
        iov[1].iov_base = b1; iov[1].iov_len = 5;
        iov[2].iov_base = b2; iov[2].iov_len = 2;

        long r = _readv(pfd[0], iov, 3);
        close(pfd[0]);

        if (r == 12 &&
            memcmp(b0, "HELLO", 5) == 0 &&
            memcmp(b1, "WORLD", 5) == 0 &&
            b2[0] == '!' && b2[1] == '!')
            ok("readv stress: pipe-backed readv scatter correct");
        else
            ng("readv stress: pipe-backed readv wrong", r);

    rs4_end:;
    }

    // RS5. Write-only fd guard under load
    //      Attempt readv on an O_WRONLY fd 100 times; every call must return
    //      an error. A buggy implementation might succeed on some iterations
    //      if it fails to re-check mode on each entry.
    {
        cleanup(SRV_FILE);
        int fd = open(SRV_FILE, O_WRONLY|O_CREAT|O_TRUNC, 0644);
        if (fd < 0) { ng("readv-stress(wronly)/open", fd); goto rs5_end; }
        write(fd, "secret", 6);

        int guard_ok = 1;
        char buf[8];
        struct iovec iov[1];
        iov[0].iov_base = buf;
        iov[0].iov_len  = sizeof(buf);
        for (int i = 0; i < 100; i++) {
            if (_readv(fd, iov, 1) >= 0) { guard_ok = 0; break; }
        }

        if (guard_ok)
            ok("readv stress: O_WRONLY fd rejected every time (100 attempts)");
        else
            ng("readv stress: O_WRONLY fd allowed readv unexpectedly", 0);

        close(fd);
    rs5_end:
        cleanup(SRV_FILE);
    }

    // ── SECTION 15: lseek stress tests ────────────────────────────────────────
    printf("\n--- lseek Stress Tests ---\n");

    const char* SLS_FILE = "/tmp/sc_lseek_stress.txt";

    // SS1. 1000-iteration seek+read sweep
    //      Write 26 bytes ('A'..'Z'), then for 1000 iterations seek to a
    //      pseudo-random offset and verify the byte read matches the expected
    //      character. Catches position-tracking drift under sustained use.
    {
        cleanup(SLS_FILE);
        int fd = open(SLS_FILE, O_RDWR|O_CREAT|O_TRUNC, 0644);
        if (fd < 0) { ng("lseek-stress(sweep)/open", fd); goto ss1_end; }

        char alpha[26];
        for (int i = 0; i < 26; i++) alpha[i] = 'A' + i;
        write(fd, alpha, 26);

        int errors = 0;
        // Simple LCG for deterministic but varied offsets: x = (x*1103515245+12345) & 0x7fffffff
        unsigned int x = 0xdeadbeef;
        for (int i = 0; i < 1000; i++) {
            x = x * 1103515245u + 12345u;
            int off = (int)(x % 26);
            long pos = lseek(fd, off, SEEK_SET);
            char c = 0;
            read(fd, &c, 1);
            if (pos != off || c != ('A' + off)) errors++;
        }

        if (errors == 0)
            ok("lseek stress: 1000 random-offset seek+read, all correct");
        else
            ng("lseek stress: seek+read sweep had mismatches", errors);

        close(fd);
    ss1_end:
        cleanup(SLS_FILE);
    }

    // SS2. Alternating SEEK_END / SEEK_SET boundary walk
    //      Repeatedly bounce between the very start and very end of a file.
    //      Verifies that SEEK_END and SEEK_SET interact correctly and that
    //      the returned offset is always exact (no off-by-one at boundaries).
    {
        cleanup(SLS_FILE);
        int fd = open(SLS_FILE, O_RDWR|O_CREAT|O_TRUNC, 0644);
        if (fd < 0) { ng("lseek-stress(boundary)/open", fd); goto ss2_end; }

        write(fd, "BOUNDARY", 8);   // 8-byte file

        int errors = 0;
        for (int i = 0; i < 500; i++) {
            long end = lseek(fd, 0, SEEK_END);   // must be 8
            long beg = lseek(fd, 0, SEEK_SET);   // must be 0
            if (end != 8 || beg != 0) errors++;
        }

        if (errors == 0)
            ok("lseek stress: 500x SEEK_END/SEEK_SET boundary walk correct");
        else
            ng("lseek stress: boundary walk had position errors", errors);

        close(fd);
    ss2_end:
        cleanup(SLS_FILE);
    }

    // SS3. Seek + write hole fill
    //      Seek past EOF and write a single byte to create a sparse "hole",
    //      then seek back and verify: bytes in the hole read as zero (POSIX),
    //      the byte written at the far offset reads back correctly, and st_size
    //      equals the furthest byte written + 1.
    {
        cleanup(SLS_FILE);
        int fd = open(SLS_FILE, O_RDWR|O_CREAT|O_TRUNC, 0644);
        if (fd < 0) { ng("lseek-stress(hole)/open", fd); goto ss3_end; }

        write(fd, "X", 1);                    // offset 0: 'X'
        lseek(fd, 99, SEEK_SET);              // seek 98 bytes past EOF
        write(fd, "Z", 1);                    // offset 99: 'Z', creates hole

        // Check file size
        struct stat st;
        memset(&st, 0, sizeof(st));
        fstat(fd, &st);

        // Read back the hole byte (offset 50) and the sentinel bytes
        lseek(fd, 0, SEEK_SET);
        char first = 0;
        read(fd, &first, 1);

        lseek(fd, 50, SEEK_SET);
        char hole = 0xFF;
        read(fd, &hole, 1);

        lseek(fd, 99, SEEK_SET);
        char last = 0;
        read(fd, &last, 1);

        if (st.st_size == 100 && first == 'X' && hole == '\0' && last == 'Z')
            ok("lseek stress: seek+write hole — size correct, hole zeroed, sentinel correct");
        else
            ng("lseek stress: hole fill mismatch", (long)st.st_size);

        close(fd);
    ss3_end:
        cleanup(SLS_FILE);
    }

    // SS4. Two independent fds, independent positions
    //      Open the same file twice; seek each fd to a different offset and
    //      confirm that seeking one does NOT move the other. Tests that the
    //      kernel maintains per-open-file-description offsets correctly.
    {
        cleanup(SLS_FILE);
        int fd1 = open(SLS_FILE, O_RDWR|O_CREAT|O_TRUNC, 0644);
        if (fd1 < 0) { ng("lseek-stress(indep)/open1", fd1); goto ss4_end; }
        int fd2 = open(SLS_FILE, O_RDONLY, 0);
        if (fd2 < 0) { ng("lseek-stress(indep)/open2", fd2); close(fd1); goto ss4_end; }

        write(fd1, "ABCDEFGHIJ", 10);

        lseek(fd1, 2, SEEK_SET);   // fd1 at offset 2
        lseek(fd2, 7, SEEK_SET);   // fd2 at offset 7

        long p1 = lseek(fd1, 0, SEEK_CUR);   // should still be 2
        long p2 = lseek(fd2, 0, SEEK_CUR);   // should still be 7

        // Move fd1 to 0 — must NOT affect fd2
        lseek(fd1, 0, SEEK_SET);
        long p2_after = lseek(fd2, 0, SEEK_CUR);   // must still be 7

        if (p1 == 2 && p2 == 7 && p2_after == 7)
            ok("lseek stress: two fds on same file have independent positions");
        else
            ng("lseek stress: independent fd positions wrong", p2_after);

        close(fd1);
        close(fd2);
    ss4_end:
        cleanup(SLS_FILE);
    }

    // SS5. SEEK_CUR accumulation over 500 steps
    //      Starting at offset 0, apply 500 x SEEK_CUR(+1) advances and verify
    //      the final reported offset is exactly 500. Also checks that a single
    //      SEEK_CUR(-500) brings it back to 0. Exercises the cumulative
    //      arithmetic path in the kernel without any intermediate resets.
    {
        cleanup(SLS_FILE);
        int fd = open(SLS_FILE, O_RDWR|O_CREAT|O_TRUNC, 0644);
        if (fd < 0) { ng("lseek-stress(accumulate)/open", fd); goto ss5_end; }

        // Pre-fill 512 bytes so seeking within them is valid
        char filler[512];
        memset(filler, 0xCC, 512);
        write(fd, filler, 512);
        lseek(fd, 0, SEEK_SET);

        long pos = 0;
        int drift = 0;
        for (int i = 0; i < 500; i++) {
            pos = lseek(fd, 1, SEEK_CUR);
            if (pos != i + 1) { drift++; break; }
        }

        long back = lseek(fd, -500, SEEK_CUR);

        if (drift == 0 && pos == 500 && back == 0)
            ok("lseek stress: 500x SEEK_CUR(+1) then SEEK_CUR(-500) -> exact offsets");
        else
            ng("lseek stress: SEEK_CUR accumulation drift", pos);

        close(fd);
    ss5_end:
        cleanup(SLS_FILE);
    }

    printf("\n--- Result: %d OK, %d NG ---\n", pass, fail);
    return fail > 0 ? 1 : 0;
}