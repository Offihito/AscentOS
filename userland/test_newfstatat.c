// test_newfstatat.c - Test program for newfstatat (fstatat) syscall 262
// Build: x86_64-linux-musl-gcc -static -O2 -Wall -o test_newfstatat.elf test_newfstatat.c

#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

// May not be defined in all musl versions
#ifndef AT_EMPTY_PATH
#define AT_EMPTY_PATH 0x1000
#endif

static const char *filetype_str(mode_t mode) {
    if (S_ISREG(mode))  return "regular file";
    if (S_ISDIR(mode))  return "directory";
    if (S_ISCHR(mode))  return "character device";
    if (S_ISBLK(mode))  return "block device";
    if (S_ISFIFO(mode)) return "FIFO/pipe";
    if (S_ISLNK(mode))  return "symlink";
    return "unknown";
}

static void print_stat(const char *label, struct stat *st) {
    printf("  === %s ===\n", label);
    printf("    type:     %s\n", filetype_str(st->st_mode));
    printf("    mode:     0%o\n", st->st_mode & 07777);
    printf("    size:     %lld\n", (long long)st->st_size);
    printf("    ino:      %llu\n", (unsigned long long)st->st_ino);
    printf("    nlink:    %llu\n", (unsigned long long)st->st_nlink);
    printf("    blksize:  %lld\n", (long long)st->st_blksize);
    printf("    blocks:   %lld\n", (long long)st->st_blocks);
    printf("\n");
}

static int pass_count = 0;
static int fail_count = 0;

static void check(const char *name, int condition) {
    if (condition) {
        printf("[PASS] %s\n", name);
        pass_count++;
    } else {
        printf("[FAIL] %s (errno=%d)\n", name, errno);
        fail_count++;
    }
}

int main(void) {
    struct stat st;
    int ret;

    printf("\n========================================\n");
    printf("  newfstatat (fstatat) syscall test\n");
    printf("========================================\n\n");

    // ── Test 1: fstatat with AT_FDCWD and absolute path ──────────────────
    printf("Test 1: fstatat(AT_FDCWD, \"/\", ...)\n");
    ret = fstatat(AT_FDCWD, "/", &st, 0);
    check("stat root directory", ret == 0);
    if (ret == 0) {
        check("root is a directory", S_ISDIR(st.st_mode));
        print_stat("/", &st);
    }

    // ── Test 2: fstatat with AT_FDCWD on a known file ────────────────────
    printf("Test 2: fstatat(AT_FDCWD, \"/hello.txt\", ...)\n");
    ret = fstatat(AT_FDCWD, "/hello.txt", &st, 0);
    if (ret == 0) {
        check("stat /hello.txt", 1);
        check("/hello.txt is a regular file", S_ISREG(st.st_mode));
        check("/hello.txt has non-zero size", st.st_size > 0);
        print_stat("/hello.txt", &st);
    } else {
        printf("  (file may not exist on disk, skipping) errno=%d\n\n", errno);
    }

    // ── Test 3: fstatat with AT_FDCWD on non-existent path ───────────────
    printf("Test 3: fstatat(AT_FDCWD, \"/nonexistent_xyz\", ...) - should fail\n");
    ret = fstatat(AT_FDCWD, "/nonexistent_xyz", &st, 0);
    check("stat non-existent returns error", ret != 0);
    printf("\n");

    // ── Test 4: fstatat with a real dirfd ────────────────────────────────
    printf("Test 4: open(\"/\") then fstatat(dirfd, \"hello.txt\", ...)\n");
    int dirfd = open("/", O_RDONLY);
    if (dirfd >= 0) {
        check("opened / as dirfd", 1);
        ret = fstatat(dirfd, "hello.txt", &st, 0);
        if (ret == 0) {
            check("fstatat(dirfd, \"hello.txt\") succeeded", 1);
            check("hello.txt via dirfd is regular file", S_ISREG(st.st_mode));
            print_stat("hello.txt (via dirfd)", &st);
        } else {
            printf("  (hello.txt not on disk, skipping) errno=%d\n\n", errno);
        }
        close(dirfd);
    } else {
        printf("  Could not open / as dirfd, errno=%d\n\n", errno);
    }

    // ── Test 5: fstatat with AT_EMPTY_PATH (fstat-like) ──────────────────
    printf("Test 5: AT_EMPTY_PATH on an open fd\n");
    int fd = open("/hello.txt", O_RDONLY);
    if (fd >= 0) {
        ret = fstatat(fd, "", &st, AT_EMPTY_PATH);
        check("fstatat(fd, \"\", AT_EMPTY_PATH) succeeded", ret == 0);
        if (ret == 0) {
            check("AT_EMPTY_PATH reports regular file", S_ISREG(st.st_mode));
            print_stat("hello.txt (AT_EMPTY_PATH)", &st);
        }
        close(fd);
    } else {
        printf("  (hello.txt not on disk, skipping AT_EMPTY_PATH test)\n\n");
    }

    // ── Test 6: fstatat on /docs directory ───────────────────────────────
    printf("Test 6: fstatat(AT_FDCWD, \"/docs\", ...)\n");
    ret = fstatat(AT_FDCWD, "/docs", &st, 0);
    if (ret == 0) {
        check("stat /docs", 1);
        check("/docs is a directory", S_ISDIR(st.st_mode));
        print_stat("/docs", &st);
    } else {
        printf("  (/docs may not exist, skipping) errno=%d\n\n", errno);
    }

    // ── Test 7: fstatat with invalid dirfd ───────────────────────────────
    printf("Test 7: fstatat(999, \"hello.txt\", ...) - invalid dirfd\n");
    ret = fstatat(999, "hello.txt", &st, 0);
    check("invalid dirfd returns error", ret != 0);
    printf("\n");

    // ── Test 8: Compare fstatat vs fstat on same fd ──────────────────────
    printf("Test 8: Compare fstatat(AT_EMPTY_PATH) vs fstat on same fd\n");
    fd = open("/hello.txt", O_RDONLY);
    if (fd >= 0) {
        struct stat st1, st2;
        int r1 = fstat(fd, &st1);
        int r2 = fstatat(fd, "", &st2, AT_EMPTY_PATH);
        if (r1 == 0 && r2 == 0) {
            check("both calls succeeded", 1);
            check("same st_mode", st1.st_mode == st2.st_mode);
            check("same st_size", st1.st_size == st2.st_size);
            check("same st_ino",  st1.st_ino  == st2.st_ino);
        } else {
            printf("  one of the calls failed (r1=%d r2=%d)\n", r1, r2);
        }
        close(fd);
    } else {
        printf("  (hello.txt not on disk, skipping comparison test)\n\n");
    }

    // ── Summary ──────────────────────────────────────────────────────────
    printf("\n========================================\n");
    printf("  Results: %d passed, %d failed\n", pass_count, fail_count);
    printf("========================================\n");

    return fail_count > 0 ? 1 : 0;
}
