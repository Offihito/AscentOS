#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#define AT_FDCWD -100

int main() {
    printf("Testing extended access() / openat()...\n");

    // 1. Create a directory to test relative openat
    int tmp_dir_fd = open("/tmp", O_RDONLY);
    if (tmp_dir_fd < 0) {
        printf("Failed to open /tmp for dirfd test. errno = %d\n", errno);
        return 1;
    }

    // 2. Relative openat using dirfd
    int fd_rel = openat(tmp_dir_fd, "test_openat_rel.txt", O_CREAT | O_RDWR, 0666);
    if (fd_rel >= 0) {
        printf("openat(tmp_dir_fd, \"test_openat_rel.txt\") successful! fd = %d\n", fd_rel);
        write(fd_rel, "hello relative", 14);
        close(fd_rel);
    } else {
        printf("openat(tmp_dir_fd, \"test_openat_rel.txt\") failed! errno = %d\n", errno);
    }

    // 3. Verify it was created in /tmp
    int fd_abs = open("/tmp/test_openat_rel.txt", O_RDONLY);
    if (fd_abs >= 0) {
        printf("Verified relative path creation via absolute open!\n");
        close(fd_abs);
    } else {
        printf("Failed to verify relative creation! fd_abs errno = %d\n", errno);
    }

    // 4. Test strict access mask checks
    // We create a read-only file
    int ro_fd = open("/tmp/ro_file.txt", O_CREAT | O_RDWR, 0444);
    if (ro_fd >= 0) {
        close(ro_fd);
        if (access("/tmp/ro_file.txt", W_OK) != 0) {
            printf("access(/tmp/ro_file.txt, W_OK) correctly rejected! errno = %d\n", errno);
        } else {
            printf("access(/tmp/ro_file.txt, W_OK) INCORRECTLY succeeded!\n");
        }
    } else {
       printf("Failed to create ro_file.txt. errno = %d\n", errno);
    }

    close(tmp_dir_fd);
    return 0;
}
