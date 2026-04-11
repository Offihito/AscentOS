#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/syscall.h>

// Color codes for output
#define GREEN   "\033[32m"
#define RED     "\033[31m"
#define YELLOW  "\033[33m"
#define RESET   "\033[0m"

// Test counters
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_PASS(msg) \
    do { \
        printf(GREEN "[PASS]" RESET " %s\n", msg); \
        tests_passed++; \
    } while (0)

#define TEST_FAIL(msg, ...) \
    do { \
        printf(RED "[FAIL]" RESET " " msg "\n", ##__VA_ARGS__); \
        tests_failed++; \
    } while (0)

#define TEST_INFO(msg) printf(YELLOW "[INFO]" RESET " %s\n", msg)

// ─────────────────────────────────────────────────────────────────────────────
// TEST 1: Basic MAP_PRIVATE allocation and write/read
// ─────────────────────────────────────────────────────────────────────────────
void test_private_basic(void) {
    TEST_INFO("TEST 1: Basic MAP_PRIVATE allocation and write/read");
    
    // Allocate 4 pages of private anonymous memory
    size_t size = 4096 * 4;
    uint8_t *ptr = (uint8_t *)mmap(NULL, size, PROT_READ | PROT_WRITE,
                                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    
    if (ptr == (uint8_t *)MAP_FAILED) {
        TEST_FAIL("mmap with MAP_PRIVATE | MAP_ANONYMOUS");
        return;
    }
    TEST_PASS("mmap with MAP_PRIVATE | MAP_ANONYMOUS");
    
    // Write pattern to memory
    for (uint32_t i = 0; i < size; i++) {
        ptr[i] = (uint8_t)(i & 0xFF);
    }
    
    // Verify pattern
    int pattern_ok = 1;
    for (uint32_t i = 0; i < size; i++) {
        if (ptr[i] != (uint8_t)(i & 0xFF)) {
            pattern_ok = 0;
            break;
        }
    }
    
    if (pattern_ok) {
        TEST_PASS("MAP_PRIVATE memory readable/writable with correct values");
    } else {
        TEST_FAIL("MAP_PRIVATE memory pattern verification");
    }
    
    // Cleanup
    if (munmap(ptr, size) == 0) {
        TEST_PASS("munmap MAP_PRIVATE memory");
    } else {
        TEST_FAIL("munmap MAP_PRIVATE memory");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// TEST 2: Basic MAP_SHARED allocation and write/read
// ─────────────────────────────────────────────────────────────────────────────
void test_shared_basic(void) {
    TEST_INFO("TEST 2: Basic MAP_SHARED allocation and write/read");
    
    size_t size = 4096 * 2;
    uint32_t *ptr = (uint32_t *)mmap(NULL, size, PROT_READ | PROT_WRITE,
                                      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    
    if (ptr == (uint32_t *)MAP_FAILED) {
        TEST_FAIL("mmap with MAP_SHARED | MAP_ANONYMOUS");
        return;
    }
    TEST_PASS("mmap with MAP_SHARED | MAP_ANONYMOUS");
    
    // Write values
    for (uint32_t i = 0; i < size / sizeof(uint32_t); i++) {
        ptr[i] = 0xDEADBEEF + i;
    }
    
    // Verify values
    int values_ok = 1;
    for (uint32_t i = 0; i < size / sizeof(uint32_t); i++) {
        if (ptr[i] != (0xDEADBEEF + i)) {
            values_ok = 0;
            break;
        }
    }
    
    if (values_ok) {
        TEST_PASS("MAP_SHARED memory readable/writable with correct values");
    } else {
        TEST_FAIL("MAP_SHARED memory value verification");
    }
    
    // Cleanup
    if (munmap(ptr, size) == 0) {
        TEST_PASS("munmap MAP_SHARED memory");
    } else {
        TEST_FAIL("munmap MAP_SHARED memory");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// TEST 3: Invalid flag combinations should fail
// ─────────────────────────────────────────────────────────────────────────────
void test_invalid_flags(void) {
    TEST_INFO("TEST 3: Invalid flag combinations");
    
    // Case 1: Both MAP_SHARED and MAP_PRIVATE (invalid)
    uint8_t *ptr = (uint8_t *)mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                                    MAP_SHARED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == (uint8_t *)MAP_FAILED) {
        TEST_PASS("mmap rejects MAP_SHARED | MAP_PRIVATE");
    } else {
        TEST_FAIL("mmap should reject MAP_SHARED | MAP_PRIVATE");
        munmap(ptr, 4096);
    }
    
    // Case 2: Neither MAP_SHARED nor MAP_PRIVATE (invalid)
    ptr = (uint8_t *)mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                          MAP_ANONYMOUS, -1, 0);
    if (ptr == (uint8_t *)MAP_FAILED) {
        TEST_PASS("mmap rejects MAP_ANONYMOUS without MAP_SHARED/MAP_PRIVATE");
    } else {
        TEST_FAIL("mmap should reject MAP_ANONYMOUS without MAP_SHARED/MAP_PRIVATE");
        munmap(ptr, 4096);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// TEST 4: MAP_PRIVATE persistence (write/read within same process)
// ─────────────────────────────────────────────────────────────────────────────
void test_private_persistence(void) {
    TEST_INFO("TEST 4: MAP_PRIVATE persistence");
    
    size_t size = 4096 * 2;
    uint32_t *ptr = (uint32_t *)mmap(NULL, size, PROT_READ | PROT_WRITE,
                                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    
    if (ptr == (uint32_t *)MAP_FAILED) {
        TEST_FAIL("Initial mmap for private persistence test");
        return;
    }
    
    // Write initial value
    *ptr = 0x12345678;
    ptr[100] = 0xDEADBEEF;
    
    // Verify values persist
    if (*ptr == 0x12345678 && ptr[100] == 0xDEADBEEF) {
        TEST_PASS("MAP_PRIVATE values persist across multiple accesses");
    } else {
        TEST_FAIL("MAP_PRIVATE persistence check failed");
    }
    
    // Modify and verify again
    *ptr = 0x87654321;
    if (*ptr == 0x87654321) {
        TEST_PASS("MAP_PRIVATE can be modified multiple times");
    } else {
        TEST_FAIL("MAP_PRIVATE modification failed");
    }
    
    munmap(ptr, size);
}

// ─────────────────────────────────────────────────────────────────────────────
// TEST 5: MAP_SHARED persistence (write/read within same process)
// ─────────────────────────────────────────────────────────────────────────────
void test_shared_persistence(void) {
    TEST_INFO("TEST 5: MAP_SHARED persistence");
    
    size_t size = 4096 * 2;
    uint32_t *ptr = (uint32_t *)mmap(NULL, size, PROT_READ | PROT_WRITE,
                                      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    
    if (ptr == (uint32_t *)MAP_FAILED) {
        TEST_FAIL("Initial mmap for shared persistence test");
        return;
    }
    
    // Write initial value
    *ptr = 0xAAAAAAAA;
    ptr[200] = 0xBBBBBBBB;
    
    // Verify values persist
    if (*ptr == 0xAAAAAAAA && ptr[200] == 0xBBBBBBBB) {
        TEST_PASS("MAP_SHARED values persist across multiple accesses");
    } else {
        TEST_FAIL("MAP_SHARED persistence check failed");
    }
    
    // Modify and verify again
    *ptr = 0xCCCCCCCC;
    if (*ptr == 0xCCCCCCCC) {
        TEST_PASS("MAP_SHARED can be modified multiple times");
    } else {
        TEST_FAIL("MAP_SHARED modification failed");
    }
    
    munmap(ptr, size);
}

// ─────────────────────────────────────────────────────────────────────────────
// TEST 6: Multiple allocations and independence
// ─────────────────────────────────────────────────────────────────────────────
void test_multiple_allocations(void) {
    TEST_INFO("TEST 6: Multiple independent allocations");
    
    size_t size1 = 4096, size2 = 8192, size3 = 2048;
    
    uint32_t *private1 = (uint32_t *)mmap(NULL, size1, PROT_READ | PROT_WRITE,
                                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    uint32_t *shared1 = (uint32_t *)mmap(NULL, size2, PROT_READ | PROT_WRITE,
                                          MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    uint32_t *private2 = (uint32_t *)mmap(NULL, size3, PROT_READ | PROT_WRITE,
                                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    
    if (private1 == (uint32_t *)MAP_FAILED || shared1 == (uint32_t *)MAP_FAILED ||
        private2 == (uint32_t *)MAP_FAILED) {
        TEST_FAIL("Multiple mmap allocations");
        return;
    }
    TEST_PASS("Multiple mmap allocations succeeded");
    
    // Write distinct patterns to each
    private1[0] = 0x11111111;
    shared1[0]  = 0x22222222;
    private2[0] = 0x33333333;
    
    // Verify they're independent
    if (private1[0] == 0x11111111 && shared1[0] == 0x22222222 && 
        private2[0] == 0x33333333) {
        TEST_PASS("Multiple allocations are independent");
    } else {
        TEST_FAIL("Multiple allocations are corrupted");
    }
    
    // Cleanup
    munmap(private1, size1);
    munmap(shared1, size2);
    munmap(private2, size3);
}

// ─────────────────────────────────────────────────────────────────────────────
// TEST 7: Allocate large memory blocks
// ─────────────────────────────────────────────────────────────────────────────
void test_large_allocations(void) {
    TEST_INFO("TEST 7: Large memory allocations");
    
    // Allocate 1MB of private memory
    size_t large_size = 1024 * 1024;
    uint8_t *large_private = (uint8_t *)mmap(NULL, large_size, PROT_READ | PROT_WRITE,
                                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    
    if (large_private == (uint8_t *)MAP_FAILED) {
        TEST_FAIL("1MB MAP_PRIVATE allocation");
        return;
    }
    TEST_PASS("1MB MAP_PRIVATE allocation");
    
    // Write to first, middle, and last pages
    large_private[0] = 0xAA;
    large_private[large_size / 2] = 0xBB;
    large_private[large_size - 1] = 0xCC;
    
    int pattern_ok = (large_private[0] == 0xAA && 
                      large_private[large_size / 2] == 0xBB &&
                      large_private[large_size - 1] == 0xCC);
    
    if (pattern_ok) {
        TEST_PASS("Large allocation read/write across all regions");
    } else {
        TEST_FAIL("Large allocation read/write failed");
    }
    
    munmap(large_private, large_size);
}

// ─────────────────────────────────────────────────────────────────────────────
// TEST 8: Protection flags work correctly
// ─────────────────────────────────────────────────────────────────────────────
void test_protection_flags(void) {
    TEST_INFO("TEST 8: Protection flags (PROT_READ only)");
    
    size_t size = 4096;
    
    // Allocate with read-only protection
    uint8_t *ptr = (uint8_t *)mmap(NULL, size, PROT_READ,
                                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    
    if (ptr == (uint8_t *)MAP_FAILED) {
        TEST_FAIL("mmap with PROT_READ only");
        return;
    }
    TEST_PASS("mmap with PROT_READ only");
    
    // Reading should work (memory is zero-initialized)
    if (ptr[100] == 0) {
        TEST_PASS("Can read from PROT_READ memory");
    } else {
        TEST_FAIL("Cannot read from PROT_READ memory");
    }
    
    // Note: Writing would cause a segfault, which we can't easily test here
    // In a full test suite, this would be in a subprocess
    
    munmap(ptr, size);
}

// ─────────────────────────────────────────────────────────────────────────────
// TEST 9: Verify zero-initialization of anonymous mappings
// ─────────────────────────────────────────────────────────────────────────────
void test_zero_initialization(void) {
    TEST_INFO("TEST 9: Anonymous mappings zero-initialized");
    
    size_t size = 8192;
    uint8_t *ptr = (uint8_t *)mmap(NULL, size, PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    
    if (ptr == (uint8_t *)MAP_FAILED) {
        TEST_FAIL("mmap for zero-init test");
        return;
    }
    
    // Check that memory is all zeros
    int all_zero = 1;
    for (size_t i = 0; i < size; i++) {
        if (ptr[i] != 0) {
            all_zero = 0;
            break;
        }
    }
    
    if (all_zero) {
        TEST_PASS("Anonymous memory is properly zero-initialized");
    } else {
        TEST_FAIL("Anonymous memory contains non-zero values");
    }
    
    munmap(ptr, size);
}

// ─────────────────────────────────────────────────────────────────────────────
// TEST 10: munmap with various sizes
// ─────────────────────────────────────────────────────────────────────────────
void test_munmap_various_sizes(void) {
    TEST_INFO("TEST 10: munmap with various allocation sizes");
    
    size_t sizes[] = {4096, 8192, 16384, 65536, 0};
    
    for (int i = 0; sizes[i] > 0; i++) {
        uint8_t *ptr = (uint8_t *)mmap(NULL, sizes[i], PROT_READ | PROT_WRITE,
                                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        
        if (ptr == (uint8_t *)MAP_FAILED) {
            printf(RED "[FAIL]" RESET " mmap %zu bytes\n", sizes[i]);
            tests_failed++;
            continue;
        }
        
        if (munmap(ptr, sizes[i]) == 0) {
            printf(GREEN "[PASS]" RESET " munmap %zu bytes\n", sizes[i]);
            tests_passed++;
        } else {
            printf(RED "[FAIL]" RESET " munmap %zu bytes (returned error)\n", sizes[i]);
            tests_failed++;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Main test runner
// ─────────────────────────────────────────────────────────────────────────────
int main(void) {
    printf("\n");
    printf("╔════════════════════════════════════════════════════════════════╗\n");
    printf("║     MAP_SHARED and MAP_PRIVATE Functionality Test Suite       ║\n");
    printf("╚════════════════════════════════════════════════════════════════╝\n");
    printf("\n");
    
    test_private_basic();
    printf("\n");
    
    test_shared_basic();
    printf("\n");
    
    test_invalid_flags();
    printf("\n");
    
    test_private_persistence();
    printf("\n");
    
    test_shared_persistence();
    printf("\n");
    
    test_multiple_allocations();
    printf("\n");
    
    test_large_allocations();
    printf("\n");
    
    test_protection_flags();
    printf("\n");
    
    test_zero_initialization();
    printf("\n");
    
    test_munmap_various_sizes();
    printf("\n");
    
    // Summary
    printf("╔════════════════════════════════════════════════════════════════╗\n");
    printf("║                         Test Summary                           ║\n");
    printf("╠════════════════════════════════════════════════════════════════╣\n");
    printf("║ " GREEN "Passed: %-54d" RESET "║\n", tests_passed);
    printf("║ " RED "Failed: %-54d" RESET "║\n", tests_failed);
    printf("╚════════════════════════════════════════════════════════════════╝\n");
    printf("\n");
    
    return tests_failed == 0 ? 0 : 1;
}
