#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

#define ITERATIONS 500
#define BUFFER_SIZE 4096
#define TEST_FILE "/tmp/io_leak_test.tmp"

void print_file(const char* path, const char* title) {
    printf("\n--- %s ---\n", title);
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        printf("Could not open %s: %s\n", path, strerror(errno));
        return;
    }
    char buf[4096];
    int n;
    while ((n = read(fd, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        printf("%s", buf);
    }
    close(fd);
    printf("\n----------------------\n");
}

int main() {
    char buffer[BUFFER_SIZE];
    memset(buffer, 'C', BUFFER_SIZE);

    printf("AscentOS IO Leak Test\n");
    printf("=====================\n");
    printf("Target: Open, Read, Write, Close\n");
    printf("Iterations: %d\n", ITERATIONS);

    print_file("/proc/meminfo", "Initial MemInfo");
    print_file("/proc/heapinfo", "Initial HeapInfo");

    for (int i = 0; i < ITERATIONS; i++) {
        // 1. Stress Open(O_CREAT) + Write + Close
        int fd = open(TEST_FILE, O_CREAT | O_WRONLY | O_TRUNC, 0666);
        if (fd < 0) {
            fprintf(stderr, "\n[FATAL] Iteration %d: open(WR) failed: %s\n", i, strerror(errno));
            exit(1);
        }
        if (write(fd, buffer, BUFFER_SIZE) != BUFFER_SIZE) {
            fprintf(stderr, "\n[FATAL] Iteration %d: write failed: %s\n", i, strerror(errno));
            exit(1);
        }
        close(fd);

        // 2. Stress Open + Read + Close
        fd = open(TEST_FILE, O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "\n[FATAL] Iteration %d: open(RD) failed: %s\n", i, strerror(errno));
            exit(1);
        }
        if (read(fd, buffer, BUFFER_SIZE) != BUFFER_SIZE) {
            fprintf(stderr, "\n[FATAL] Iteration %d: read failed: %s\n", i, strerror(errno));
            exit(1);
        }
        close(fd);

        // 3. Stress /proc/meminfo open/close (known potential leak due to dynamic vfs_node allocation)
        fd = open("/proc/meminfo", O_RDONLY);
        if (fd >= 0) {
            read(fd, buffer, 128);
            close(fd);
        }

        // 4. Cleanup
        if (unlink(TEST_FILE) < 0) {
            fprintf(stderr, "\n[FATAL] Iteration %d: unlink failed: %s\n", i, strerror(errno));
            exit(1);
        }

        if (i > 0 && i % 100 == 0) {
            printf("Progress: %d/%d...\n", i, ITERATIONS);
        }
    }

    printf("\nTest finished. Checking final state...\n");
    print_file("/proc/meminfo", "Final MemInfo");
    print_file("/proc/heapinfo", "Final HeapInfo");

    printf("\nIf 'Free Objs' decreased significantly for specific slab sizes,\nor 'Big Allocations' increased, a leak exists.\n");
    
    return 0;
}
