#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <assert.h>

int main() {
    printf("=== DUP and DUP2 Test ===\n\n");

    int fd_w = open("test_dup.txt", O_CREAT | O_WRONLY | O_TRUNC, 0666);
    if (fd_w < 0) {
        printf("open failed\n");
        return 1;
    }
    
    // Test dup
    int fd_dup = dup(fd_w);
    if (fd_dup < 0) {
        printf("dup failed\n");
        return 1;
    }
    
    printf("fd_w: %d, fd_dup: %d\n", fd_w, fd_dup);
    
    write(fd_w, "hello ", 6);
    write(fd_dup, "world\n", 6);
    
    close(fd_w);
    close(fd_dup);
    
    // Read and verify
    int fd_r = open("test_dup.txt", O_RDONLY, 0);
    char buf[16] = {0};
    read(fd_r, buf, 12);
    printf("Read from file (dup): %s", buf);
    close(fd_r);
    
    // Test dup2
    int fd_w2 = open("test_dup2.txt", O_CREAT | O_WRONLY | O_TRUNC, 0666);
    int new_fd = 10;
    int ret = dup2(fd_w2, new_fd);
    if (ret != new_fd) {
        printf("dup2 failed\n");
        return 1;
    }
    
    write(new_fd, "dup2 works\n", 11);
    
    close(fd_w2);
    close(new_fd);
    
    fd_r = open("test_dup2.txt", O_RDONLY, 0);
    memset(buf, 0, sizeof(buf));
    read(fd_r, buf, 11);
    printf("Read from file (dup2): %s", buf);
    close(fd_r);

    printf("\n=== Test Complete ===\n");
    return 0;
}
