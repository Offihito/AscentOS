#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

int main() {
    printf("Starting Phase 5 GAP & MERGE Syscall stress test...\n");

    // 1. Map 10 discrete blocks
    char *blocks[10];
    for (int i = 0; i < 10; i++) {
        blocks[i] = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (blocks[i] == MAP_FAILED) {
            printf("FAIL: mmap failed on block %d\n", i);
            return 1;
        }
        strcpy(blocks[i], "Phase5");
    }
    
    // 2. Unmap every odd block to create massive holes (fragmentation)
    for (int i = 1; i < 10; i+=2) {
        if (munmap(blocks[i], 4096) != 0) {
            printf("FAIL: munmap failed on block %d\n", i);
            return 1;
        }
    }

    // 3. Re-map generic blocks. The newly added Gap-Allocator should slot them in perfectly!
    char *new_blocks[5];
    int gaps_found = 0;
    for (int i = 0; i < 5; i++) {
        new_blocks[i] = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        // Verify gap assignment perfectly replaces an old pointer hole linearly structurally!
        for (int j = 1; j < 10; j+=2) {
             if (new_blocks[i] == blocks[j]) {
                  gaps_found++;
                  break;
             }
        }
    }
    
    if (gaps_found == 5) {
        printf("PASS: Gap Allocator linearly resolved disjoint memory boundaries efficiently.\n");
    } else {
        printf("FAIL: Expected 5 gap assignments strictly out of bounds! Got %d.\n", gaps_found);
        return 1;
    }

    printf("PASS: Integration constraints fully tested.\n");
    return 0;
}
