// ── Socket Stress Test for Phase 1: Socket Infrastructure ──────────────────────
// Tests: socket creation, FD exhaustion, rapid create/close, FD reuse

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>

#define ITERATIONS 10000
#define RAPID_CYCLES 50000

// Simple assert macro that prints and tracks failures
static int test_passed = 0;
static int test_failed = 0;

#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        printf("FAIL: %s (line %d): %s\n", msg, __LINE__, strerror(errno)); \
        test_failed++; \
        return -1; \
    } \
    test_passed++; \
} while(0)

#define TEST_START(name) printf("\n=== Test: %s ===\n", name)
#define TEST_END() printf("  Passed: %d, Failed: %d\n", test_passed, test_failed)

// ── Test 1: FD Exhaustion ─────────────────────────────────────────────────────
// Create as many sockets as possible until we run out of FDs
static int test_fd_exhaustion(void) {
    TEST_START("FD Exhaustion");
    
    int fds[ITERATIONS];
    int i;
    
    // Create as many sockets as possible
    for (i = 0; i < ITERATIONS; i++) {
        fds[i] = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fds[i] < 0) {
            printf("  Socket creation failed at iteration %d (errno=%d)\n", i, errno);
            break;
        }
    }
    
    int created = i;
    printf("  Created %d sockets before exhaustion\n", created);
    TEST_ASSERT(created > 0, "Should create at least one socket");
    
    // Close all sockets
    for (int j = 0; j < created; j++) {
        close(fds[j]);
    }
    
    // Verify FD reuse - should be able to create new socket now
    int new_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    TEST_ASSERT(new_fd >= 0, "Should be able to create socket after closing");
    printf("  FD reuse verified: new_fd=%d\n", new_fd);
    close(new_fd);
    
    TEST_END();
    return 0;
}

// ── Test 2: Rapid Create/Close ────────────────────────────────────────────────
// Test that we can rapidly create and close sockets without leaks
static int test_rapid_create_close(void) {
    TEST_START("Rapid Create/Close");
    
    for (int i = 0; i < RAPID_CYCLES; i++) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        TEST_ASSERT(fd >= 0, "Socket creation should succeed");
        close(fd);
        
        // Progress indicator every 10000 iterations
        if (i % 10000 == 0) {
            printf("  Progress: %d/%d cycles\n", i, RAPID_CYCLES);
        }
    }
    
    printf("  Rapid create/close: %d cycles completed\n", RAPID_CYCLES);
    TEST_END();
    return 0;
}

// ── Test 3: Socket Types ───────────────────────────────────────────────────────
// Test creating different socket types
static int test_socket_types(void) {
    TEST_START("Socket Types");
    
    // SOCK_STREAM
    int stream_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    TEST_ASSERT(stream_fd >= 0, "SOCK_STREAM creation should succeed");
    printf("  SOCK_STREAM fd=%d\n", stream_fd);
    close(stream_fd);
    
    // SOCK_DGRAM
    int dgram_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    TEST_ASSERT(dgram_fd >= 0, "SOCK_DGRAM creation should succeed");
    printf("  SOCK_DGRAM fd=%d\n", dgram_fd);
    close(dgram_fd);
    
    // Invalid type should fail
    int invalid_fd = socket(AF_UNIX, 999, 0);
    TEST_ASSERT(invalid_fd < 0, "Invalid socket type should fail");
    printf("  Invalid type correctly rejected\n");
    
    // Invalid domain should fail
    int invalid_domain = socket(99, SOCK_STREAM, 0);
    TEST_ASSERT(invalid_domain < 0, "Invalid domain should fail");
    printf("  Invalid domain correctly rejected\n");
    
    // Invalid protocol for AF_UNIX should fail
    int invalid_proto = socket(AF_UNIX, SOCK_STREAM, 1);
    TEST_ASSERT(invalid_proto < 0, "Invalid protocol should fail");
    printf("  Invalid protocol correctly rejected\n");
    
    TEST_END();
    return 0;
}

// ── Test 4: Socketpair Creation ───────────────────────────────────────────────
// Test socketpair() syscall
static int test_socketpair(void) {
    TEST_START("Socketpair");
    
    int sv[2];
    int ret = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    TEST_ASSERT(ret == 0, "socketpair should succeed");
    printf("  Created socketpair: [%d, %d]\n", sv[0], sv[1]);
    
    // Both FDs should be valid
    TEST_ASSERT(sv[0] >= 0, "First FD should be valid");
    TEST_ASSERT(sv[1] >= 0, "Second FD should be valid");
    
    // FDs should be different
    TEST_ASSERT(sv[0] != sv[1], "FDs should be different");
    
    close(sv[0]);
    close(sv[1]);
    
    // Test invalid domain
    ret = socketpair(99, SOCK_STREAM, 0, sv);
    TEST_ASSERT(ret < 0, "socketpair with invalid domain should fail");
    
    // Test invalid type
    ret = socketpair(AF_UNIX, 999, 0, sv);
    TEST_ASSERT(ret < 0, "socketpair with invalid type should fail");
    
    TEST_END();
    return 0;
}

// ── Test 5: Multiple Socketpairs ──────────────────────────────────────────────
// Create many socketpairs to stress the system
static int test_multiple_socketpairs(void) {
    TEST_START("Multiple Socketpairs");
    
    #define NUM_PAIRS 100
    int pairs[NUM_PAIRS][2];
    int created = 0;
    
    for (int i = 0; i < NUM_PAIRS; i++) {
        int ret = socketpair(AF_UNIX, SOCK_STREAM, 0, pairs[i]);
        if (ret < 0) {
            printf("  socketpair failed at iteration %d\n", i);
            break;
        }
        created++;
    }
    
    printf("  Created %d socketpairs\n", created);
    TEST_ASSERT(created > 0, "Should create at least one socketpair");
    
    // Close all
    for (int i = 0; i < created; i++) {
        close(pairs[i][0]);
        close(pairs[i][1]);
    }
    
    TEST_END();
    return 0;
}

// ── Test 6: Socket Options ─────────────────────────────────────────────────────
// Test getsockopt for socket-level options
static int test_socket_options(void) {
    TEST_START("Socket Options");
    
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    TEST_ASSERT(fd >= 0, "Socket creation should succeed");
    
    // Get SO_TYPE
    int type = 0;
    socklen_t len = sizeof(type);
    int ret = getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &len);
    TEST_ASSERT(ret == 0, "getsockopt SO_TYPE should succeed");
    TEST_ASSERT(type == SOCK_STREAM, "SO_TYPE should be SOCK_STREAM");
    printf("  SO_TYPE = %d (expected SOCK_STREAM=%d)\n", type, SOCK_STREAM);
    
    // Get SO_DOMAIN
    int domain = 0;
    len = sizeof(domain);
    ret = getsockopt(fd, SOL_SOCKET, SO_DOMAIN, &domain, &len);
    TEST_ASSERT(ret == 0, "getsockopt SO_DOMAIN should succeed");
    TEST_ASSERT(domain == AF_UNIX, "SO_DOMAIN should be AF_UNIX");
    printf("  SO_DOMAIN = %d (expected AF_UNIX=%d)\n", domain, AF_UNIX);
    
    // Get SO_PROTOCOL
    int protocol = 0;
    len = sizeof(protocol);
    ret = getsockopt(fd, SOL_SOCKET, SO_PROTOCOL, &protocol, &len);
    TEST_ASSERT(ret == 0, "getsockopt SO_PROTOCOL should succeed");
    printf("  SO_PROTOCOL = %d\n", protocol);
    
    // Get SO_RCVBUF
    int rcvbuf = 0;
    len = sizeof(rcvbuf);
    ret = getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, &len);
    TEST_ASSERT(ret == 0, "getsockopt SO_RCVBUF should succeed");
    printf("  SO_RCVBUF = %d\n", rcvbuf);
    
    // Set SO_RCVBUF
    int new_buf = 32768;
    ret = setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &new_buf, sizeof(new_buf));
    TEST_ASSERT(ret == 0, "setsockopt SO_RCVBUF should succeed");
    
    // Verify change
    ret = getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, &len);
    TEST_ASSERT(ret == 0, "getsockopt after set should succeed");
    printf("  SO_RCVBUF after set = %d\n", rcvbuf);
    
    close(fd);
    TEST_END();
    return 0;
}

// ── Test 7: Mixed Socket Types ────────────────────────────────────────────────
// Create a mix of socket types simultaneously
static int test_mixed_types(void) {
    TEST_START("Mixed Socket Types");
    
    #define NUM_MIXED 100
    int streams[NUM_MIXED];
    int dgrams[NUM_MIXED];
    int pairs[NUM_MIXED][2];
    
    int s_created = 0, d_created = 0, p_created = 0;
    
    // Create all types
    for (int i = 0; i < NUM_MIXED; i++) {
        streams[i] = socket(AF_UNIX, SOCK_STREAM, 0);
        if (streams[i] >= 0) s_created++;
        
        dgrams[i] = socket(AF_UNIX, SOCK_DGRAM, 0);
        if (dgrams[i] >= 0) d_created++;
        
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, pairs[i]) == 0) {
            p_created++;
        }
    }
    
    printf("  Created: %d streams, %d dgrams, %d pairs\n", 
           s_created, d_created, p_created);
    
    TEST_ASSERT(s_created > 0, "Should create some stream sockets");
    TEST_ASSERT(d_created > 0, "Should create some dgram sockets");
    TEST_ASSERT(p_created > 0, "Should create some socketpairs");
    
    // Close all
    for (int i = 0; i < NUM_MIXED; i++) {
        if (streams[i] >= 0) close(streams[i]);
        if (dgrams[i] >= 0) close(dgrams[i]);
        if (pairs[i][0] >= 0) close(pairs[i][0]);
        if (pairs[i][1] >= 0) close(pairs[i][1]);
    }
    
    TEST_END();
    return 0;
}

// ── Main ──────────────────────────────────────────────────────────────────────
int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    
    printf("\n");
    printf("╔════════════════════════════════════════════════════════════╗\n");
    printf("║     Socket Stress Test - Phase 1: Socket Infrastructure    ║\n");
    printf("╚════════════════════════════════════════════════════════════╝\n");
    printf("\n");
    
    int total_passed = 0;
    int total_failed = 0;
    
    // Run all tests
    test_fd_exhaustion();
    total_passed += test_passed;
    total_failed += test_failed;
    test_passed = test_failed = 0;
    
    test_rapid_create_close();
    total_passed += test_passed;
    total_failed += test_failed;
    test_passed = test_failed = 0;
    
    test_socket_types();
    total_passed += test_passed;
    total_failed += test_failed;
    test_passed = test_failed = 0;
    
    test_socketpair();
    total_passed += test_passed;
    total_failed += test_failed;
    test_passed = test_failed = 0;
    
    test_multiple_socketpairs();
    total_passed += test_passed;
    total_failed += test_failed;
    test_passed = test_failed = 0;
    
    test_socket_options();
    total_passed += test_passed;
    total_failed += test_failed;
    test_passed = test_failed = 0;
    
    test_mixed_types();
    total_passed += test_passed;
    total_failed += test_failed;
    
    // Summary
    printf("\n");
    printf("╔════════════════════════════════════════════════════════════╗\n");
    printf("║                      TEST SUMMARY                          ║\n");
    printf("╠════════════════════════════════════════════════════════════╣\n");
    printf("║  Total Passed: %-12d                            ║\n", total_passed);
    printf("║  Total Failed: %-12d                            ║\n", total_failed);
    printf("╠════════════════════════════════════════════════════════════╣\n");
    if (total_failed == 0) {
        printf("║              ALL TESTS PASSED!                             ║\n");
    } else {
        printf("║              SOME TESTS FAILED                             ║\n");
    }
    printf("╚════════════════════════════════════════════════════════════╝\n");
    printf("\n");
    
    return total_failed > 0 ? 1 : 0;
}
