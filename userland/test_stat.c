// test_stat.c - Test program for stat/fstat syscalls
// Build: gcc -static -o test_stat.elf test_stat.c

#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>

// Simple number to string conversion
static void print_num(uint64_t n) {
    char buf[24];
    int i = 23;
    buf[i] = '\n';
    if (n == 0) {
        buf[--i] = '0';
    } else {
        while (n > 0) {
            buf[--i] = '0' + (n % 10);
            n /= 10;
        }
    }
    write(1, &buf[i], 24 - i);
}

static void print_str(const char *s) {
    int len = 0;
    while (s[len]) len++;
    write(1, s, len);
}

static void print_oct(uint32_t n) {
    char buf[12];
    int i = 11;
    buf[i] = '\n';
    if (n == 0) {
        buf[--i] = '0';
    } else {
        while (n > 0) {
            buf[--i] = '0' + (n & 7);
            n >>= 3;
        }
    }
    write(1, &buf[i], 11 - i);
}

static void print_stat(const char *name, struct stat *st) {
    print_str("=== ");
    print_str(name);
    print_str(" ===\n");
    
    print_str("  st_mode:  ");
    print_oct(st->st_mode);
    print_str("  st_size:  ");
    print_num(st->st_size);
    print_str("  st_ino:   ");
    print_num(st->st_ino);
    print_str("  st_uid:   ");
    print_num(st->st_uid);
    print_str("  st_gid:   ");
    print_num(st->st_gid);
    print_str("  st_nlink: ");
    print_num(st->st_nlink);
    print_str("  st_blksize: ");
    print_num(st->st_blksize);
    print_str("  st_blocks:  ");
    print_num(st->st_blocks);
    
    // File type
    print_str("  type: ");
    if (S_ISREG(st->st_mode))  print_str("regular file\n");
    else if (S_ISDIR(st->st_mode)) print_str("directory\n");
    else if (S_ISCHR(st->st_mode)) print_str("character device\n");
    else if (S_ISBLK(st->st_mode)) print_str("block device\n");
    else if (S_ISFIFO(st->st_mode)) print_str("FIFO/pipe\n");
    else if (S_ISLNK(st->st_mode)) print_str("symlink\n");
    else print_str("unknown\n");
    
    print_str("\n");
}

int main(int argc, char **argv) {
    struct stat st;
    int fd;
    int ret;
    
    print_str("\n=== stat/fstat syscall test ===\n\n");
    
    // Test 1: stat on a known file (on mounted disk at /mnt)
    print_str("Test 1: stat(\"/mnt/hello.txt\")\n");
    ret = stat("/mnt/hello.txt", &st);
    if (ret == 0) {
        print_stat("/mnt/hello.txt", &st);
    } else {
        print_str("  stat failed (file may not exist)\n\n");
    }
    
    // Test 2: stat on root directory
    print_str("Test 2: stat(\"/\")\n");
    ret = stat("/", &st);
    if (ret == 0) {
        print_stat("/", &st);
    } else {
        print_str("  stat failed\n\n");
    }
    
    // Test 3: stat on non-existent file
    print_str("Test 3: stat(\"/nonexistent\") - should fail\n");
    ret = stat("/nonexistent", &st);
    if (ret == 0) {
        print_str("  ERROR: stat should have failed!\n\n");
    } else {
        print_str("  correctly returned error\n\n");
    }
    
    // Test 4: fstat on stdout (fd 1)
    print_str("Test 4: fstat(1) - stdout\n");
    ret = fstat(1, &st);
    if (ret == 0) {
        print_stat("stdout", &st);
    } else {
        print_str("  fstat failed\n\n");
    }
    
    // Test 5: fstat on stdin (fd 0)
    print_str("Test 5: fstat(0) - stdin\n");
    ret = fstat(0, &st);
    if (ret == 0) {
        print_stat("stdin", &st);
    } else {
        print_str("  fstat failed\n\n");
    }
    
    // Test 6: open a file and use fstat (on mounted disk at /mnt)
    print_str("Test 6: open + fstat(\"/mnt/hello.txt\")\n");
    fd = open("/mnt/hello.txt", O_RDONLY);
    if (fd >= 0) {
        print_str("  opened fd: ");
        print_num(fd);
        ret = fstat(fd, &st);
        if (ret == 0) {
            print_stat("opened file", &st);
        } else {
            print_str("  fstat failed\n\n");
        }
        close(fd);
    } else {
        print_str("  open failed (file may not exist)\n\n");
    }
    
    // Test 7: fstat on invalid fd
    print_str("Test 7: fstat(99) - invalid fd, should fail\n");
    ret = fstat(99, &st);
    if (ret == 0) {
        print_str("  ERROR: fstat should have failed!\n\n");
    } else {
        print_str("  correctly returned error\n\n");
    }
    
    print_str("=== test complete ===\n");
    return 0;
}
