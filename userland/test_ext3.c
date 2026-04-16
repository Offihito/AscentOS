#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

int main() {
    printf("[USERLAND] Ext3 Journal Stress Test\n");
    const char *path = "/tmp/journal_test.txt";
    
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    printf("[USERLAND] Writing data to %s...\n", path);
    const char *data = "This is a journaled write test data block.\n";
    for (int i = 0; i < 10; i++) {
        write(fd, data, strlen(data));
    }

    // We don't have fsync implemented yet properly for journal, 
    // but the kernel's ext2_write_block is synchronous currently (PIO/DMA sync).
    
    printf("[USERLAND] Write complete. Now unlinking...\n");
    close(fd);
    
    // Unlink will trigger metadata updates (bitmaps, inode changes)
    if (unlink(path) == 0) {
        printf("[USERLAND] Success: File unlinked.\n");
    } else {
        perror("unlink");
    }

    return 0;
}
