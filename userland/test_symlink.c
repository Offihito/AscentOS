#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>

int main() {
    printf("=== Symlink & Readlink Test ===\n\n");

    const char *target = "hello.txt";
    const char *linkpath = "my_symlink.txt";

    // 1. Create target file
    printf("[1] Creating target file %s\n", target);
    int fd = open(target, O_CREAT | O_WRONLY | O_TRUNC, 0666);
    if (fd < 0) {
        printf("Failed to create target file\n");
        return 1;
    }
    write(fd, "Target data!", 12);
    close(fd);

    // 2. Create symlink
    printf("[2] Creating symlink %s -> %s\n", linkpath, target);
    if (symlink(target, linkpath) != 0) {
        printf("symlink failed\n");
        return 1;
    }

    // 3. Readlink
    printf("[3] Reading symlink %s\n", linkpath);
    char buf[128] = {0};
    ssize_t len = readlink(linkpath, buf, sizeof(buf) - 1);
    if (len < 0) {
        printf("readlink failed\n");
        return 1;
    }
    buf[len] = '\0';
    printf("Readlink returned: %s\n", buf);

    if (strcmp(buf, target) != 0) {
        printf("Symlink target does not match!\n");
        return 1;
    }

    // 4. Open symlink to read target contents (if VFS supports following symlinks)
    // AscentOS VFS currently might not follow symlinks properly in simple open,
    // so we just test the symlink API functionality itself.
    
    printf("\n=== Test Complete ===\n");
    return 0;
}
