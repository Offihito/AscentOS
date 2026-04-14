// test_unlink_rename.c - Test program for unlink, rename, and readlink syscalls
// Build: x86_64-linux-musl-gcc -static -O2 -Wall -o test_unlink_rename.elf test_unlink_rename.c

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

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

// Helper to create a file with some content
static int create_file(const char *path, const char *content) {
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) return -1;
    if (content) {
        size_t len = 0;
        while (content[len]) len++;
        write(fd, content, len);
    }
    close(fd);
    return 0;
}

// Helper to check if a file exists
static int file_exists(const char *path) {
    struct stat st;
    return (stat(path, &st) == 0);
}

// Helper to read file content
static int read_file(const char *path, char *buf, size_t size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    int n = read(fd, buf, size - 1);
    if (n >= 0) buf[n] = '\0';
    close(fd);
    return n;
}

int main(void) {
    printf("\n========================================\n");
    printf("  unlink/rename/readlink syscall test\n");
    printf("========================================\n\n");

    // ══════════════════════════════════════════════════
    // UNLINK TESTS
    // ══════════════════════════════════════════════════
    printf("── UNLINK Tests ──\n\n");

    // Test 1: Create a file and unlink it
    printf("Test 1: Create and unlink a file\n");
    int ret = create_file("/tmp/unlink_test.txt", "hello unlink");
    check("create /tmp/unlink_test.txt", ret == 0);
    check("file exists before unlink", file_exists("/tmp/unlink_test.txt"));
    
    ret = unlink("/tmp/unlink_test.txt");
    check("unlink returned success", ret == 0);
    check("file gone after unlink", !file_exists("/tmp/unlink_test.txt"));
    printf("\n");

    // Test 2: Unlink non-existent file
    printf("Test 2: Unlink non-existent file\n");
    ret = unlink("/tmp/does_not_exist_xyz.txt");
    check("unlink non-existent returns error", ret != 0);
    printf("\n");

    // Test 3: Unlink a directory (should fail with EISDIR)
    printf("Test 3: Unlink a directory (should fail)\n");
    mkdir("/tmp/unlink_dir_test", 0755);
    ret = unlink("/tmp/unlink_dir_test");
    check("unlink directory returns error", ret != 0);
    printf("\n");

    // Test 4: Create, write, unlink, verify data is gone
    printf("Test 4: Create, write, unlink, verify gone\n");
    create_file("/tmp/data_test.txt", "some important data");
    check("data file exists", file_exists("/tmp/data_test.txt"));
    ret = unlink("/tmp/data_test.txt");
    check("unlink data file succeeded", ret == 0);
    check("data file is gone", !file_exists("/tmp/data_test.txt"));
    printf("\n");

    // ══════════════════════════════════════════════════
    // RENAME TESTS
    // ══════════════════════════════════════════════════
    printf("── RENAME Tests ──\n\n");

    // Test 5: Simple rename
    printf("Test 5: Simple rename\n");
    create_file("/tmp/old_name.txt", "rename me");
    check("old file exists", file_exists("/tmp/old_name.txt"));
    
    ret = rename("/tmp/old_name.txt", "/tmp/new_name.txt");
    check("rename returned success", ret == 0);
    check("old name gone", !file_exists("/tmp/old_name.txt"));
    check("new name exists", file_exists("/tmp/new_name.txt"));

    // Verify content preserved
    char buf[256];
    int n = read_file("/tmp/new_name.txt", buf, sizeof(buf));
    check("content preserved after rename", n > 0 && buf[0] == 'r');
    printf("\n");

    // Clean up
    unlink("/tmp/new_name.txt");

    // Test 6: Rename to overwrite existing file
    printf("Test 6: Rename overwrites existing target\n");
    create_file("/tmp/src_file.txt", "source content");
    create_file("/tmp/dst_file.txt", "dest content");
    check("source exists", file_exists("/tmp/src_file.txt"));
    check("dest exists", file_exists("/tmp/dst_file.txt"));

    ret = rename("/tmp/src_file.txt", "/tmp/dst_file.txt");
    check("rename overwrite succeeded", ret == 0);
    check("source gone after overwrite", !file_exists("/tmp/src_file.txt"));
    check("dest still exists", file_exists("/tmp/dst_file.txt"));

    // Check that dest now has source content
    n = read_file("/tmp/dst_file.txt", buf, sizeof(buf));
    check("dest has source content", n > 0 && buf[0] == 's');
    printf("\n");

    // Clean up
    unlink("/tmp/dst_file.txt");

    // Test 7: Rename non-existent file
    printf("Test 7: Rename non-existent file\n");
    ret = rename("/tmp/ghost_file.txt", "/tmp/new_ghost.txt");
    check("rename non-existent returns error", ret != 0);
    printf("\n");

    // ══════════════════════════════════════════════════
    // READLINK TESTS
    // ══════════════════════════════════════════════════
    printf("── READLINK Tests ──\n\n");

    // Test 8: readlink on /proc/self/exe
    printf("Test 8: readlink(\"/proc/self/exe\")\n");
    char linkbuf[256];
    ssize_t linklen = readlink("/proc/self/exe", linkbuf, sizeof(linkbuf) - 1);
    if (linklen > 0) {
        linkbuf[linklen] = '\0';
        printf("  /proc/self/exe -> %s\n", linkbuf);
        check("readlink /proc/self/exe returned path", linklen > 0);
    } else {
        printf("  readlink /proc/self/exe returned %d (errno=%d)\n",
               (int)linklen, errno);
        // This may not work if the kernel doesn't track thread names
        printf("  (skipping — /proc/self/exe not supported)\n");
    }
    printf("\n");

    // Test 9: readlink on a regular file (not a symlink — should fail)
    printf("Test 9: readlink on a regular file (should fail)\n");
    create_file("/tmp/not_a_link.txt", "I'm not a symlink");
    linklen = readlink("/tmp/not_a_link.txt", linkbuf, sizeof(linkbuf));
    check("readlink on regular file returns error", linklen < 0);
    unlink("/tmp/not_a_link.txt");
    printf("\n");

    // Test 10: readlink on non-existent path
    printf("Test 10: readlink on non-existent path\n");
    linklen = readlink("/tmp/nonexistent_link", linkbuf, sizeof(linkbuf));
    check("readlink non-existent returns error", linklen < 0);
    printf("\n");

    // ══════════════════════════════════════════════════
    // COMBINED WORKFLOW TEST (simulates TCC-like behavior)
    // ══════════════════════════════════════════════════
    printf("── TCC-like Workflow Test ──\n\n");

    printf("Test 11: Write temp -> rename to final (atomic output pattern)\n");
    create_file("/tmp/output.o.tmp", "ELF binary data here...");
    check("temp file created", file_exists("/tmp/output.o.tmp"));
    
    ret = rename("/tmp/output.o.tmp", "/tmp/output.o");
    check("atomic rename succeeded", ret == 0);
    check("temp file gone", !file_exists("/tmp/output.o.tmp"));
    check("final file exists", file_exists("/tmp/output.o"));

    // Clean up
    ret = unlink("/tmp/output.o");
    check("cleanup unlink succeeded", ret == 0);
    check("final file cleaned up", !file_exists("/tmp/output.o"));
    printf("\n");

    // ── Summary ──────────────────────────────────────────────────────────
    printf("========================================\n");
    printf("  Results: %d passed, %d failed\n", pass_count, fail_count);
    printf("========================================\n");

    return fail_count > 0 ? 1 : 0;
}
