#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdint.h>

#define PAGE_SIZE 4096
#define NUM_PAGES 512
#define SIZE (NUM_PAGES * PAGE_SIZE)

int main() {
    printf("[COW TEST] STARTING - 2MB Memory Allocation\n");

    // 1. Allocate 2MB
    uint8_t *ptr = mmap(NULL, SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    // 2. Initialize with unique pattern
    printf("[COW TEST] Initializing memory with 0x42...\n");
    for (size_t i = 0; i < SIZE; i++) {
        ptr[i] = (uint8_t)(0x42);
    }

    // 3. Fork multiple children
    const int NUM_CHILDREN = 3;
    printf("[COW TEST] Forking %d children...\n", NUM_CHILDREN);

    for (int i = 0; i < NUM_CHILDREN; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            return 1;
        }

        if (pid == 0) {
            // --- CHILD ---
            uint8_t my_pattern = (uint8_t)(0xA0 + i);
            printf("[Child %d] PID %d started. Verifying shared pattern...\n", i, getpid());
            
            // Verify parent's pattern
            for (size_t j = 0; j < SIZE; j += PAGE_SIZE) {
                if (ptr[j] != 0x42) {
                    printf("[Child %d] FAILURE: Memory corruption! Expected 0x42 at offset %zu, got 0x%02X\n", i, (size_t)j, ptr[j]);
                    exit(1);
                }
            }

            printf("[Child %d] Modification (CoW) with pattern 0x%02X...\n", i, my_pattern);
            for (size_t j = 0; j < SIZE; j++) {
                ptr[j] = my_pattern;
            }

            // Verify own pattern
            for (size_t j = 0; j < SIZE; j += PAGE_SIZE) {
                if (ptr[j] != my_pattern) {
                    printf("[Child %d] FAILURE: Own write verification failed at %zu!\n", i, (size_t)j);
                    exit(1);
                }
            }

            printf("[Child %d] Success. Exiting.\n", i);
            exit(0);
        }
    }

    // --- PARENT ---
    printf("[Parent] Waiting for children to finish CoW operations...\n");
    for (int i = 0; i < NUM_CHILDREN; i++) {
        int status;
        wait(&status);
        if (WEXITSTATUS(status) != 0) {
            printf("[Parent] Child failed!\n");
            return 1;
        }
    }

    printf("[Parent] Verifying isolation (Memory should still be 0x42)...\n");
    for (size_t i = 0; i < SIZE; i += PAGE_SIZE) {
        if (ptr[i] != 0x42) {
            printf("[Parent] FAILURE: Isolation breached at offset %zu! Got 0x%02X\n", i, ptr[i]);
            return 1;
        }
    }

    printf("[Parent] ALL TESTS PASSED!\n");
    munmap(ptr, SIZE);
    return 0;
}
