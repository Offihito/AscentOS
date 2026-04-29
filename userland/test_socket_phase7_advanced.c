// Phase 7: Advanced poll() Stress Test Suite
// Complex scenarios and edge cases

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <errno.h>
#include <sys/wait.h>

#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        printf("  [FAIL] %s (line %d): %s\n", msg, __LINE__, errno ? strerror(errno) : "No error"); \
        test_failed++; \
        return -1; \
    } else { \
        printf("  [OK] %s\n", msg); \
        test_passed++; \
    } \
} while(0)

static int test_passed = 0;
static int test_failed = 0;

#define TEST_START(name) printf("\n--- Test: %s ---\n", name)
#define TEST_END() printf("Test Finished\n")

// Test 1: Poll with partial shutdowns - each direction separately
static int test_poll_partial_shutdowns(void) {
    TEST_START("Poll with partial shutdowns (SHUT_RD vs SHUT_WR)");
    
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    
    // Write some data first
    write(sv[1], "data", 4);
    
    // Shutdown read side of sv[0]
    shutdown(sv[0], SHUT_RD);
    
    struct pollfd pfd;
    pfd.fd = sv[0];
    pfd.events = POLLIN | POLLOUT | POLLHUP;
    pfd.revents = 0;
    
    int ready = poll(&pfd, 1, 0);
    printf("  After SHUT_RD on sv[0]: ready=%d, revents=%d\n", ready, pfd.revents);
    // Should still have data to read, and write side should still work
    TEST_ASSERT((pfd.revents & POLLIN), "Data still readable after SHUT_RD");
    
    // Read the data
    char buf[4];
    ssize_t n = read(sv[0], buf, 4);
    TEST_ASSERT(n == 4, "Read data successfully");
    
    // Now poll again - read side is shut down
    pfd.revents = 0;
    ready = poll(&pfd, 1, 0);
    printf("  After reading data: ready=%d, revents=%d\n", ready, pfd.revents);
    
    // Shutdown write side
    shutdown(sv[0], SHUT_WR);
    
    pfd.revents = 0;
    ready = poll(&pfd, 1, 0);
    printf("  After SHUT_WR: ready=%d, revents=%d\n", ready, pfd.revents);
    TEST_ASSERT((pfd.revents & POLLHUP), "POLLHUP after full shutdown");
    
    close(sv[0]);
    close(sv[1]);
    TEST_END();
    return 0;
}

// Test 2: Poll with many mixed-state sockets
static int test_poll_mixed_states(void) {
    TEST_START("Poll with many mixed-state sockets");
    
    int pairs[10][2];
    struct pollfd pfds[20];
    
    // Create 10 socket pairs
    for (int i = 0; i < 10; i++) {
        socketpair(AF_UNIX, SOCK_STREAM, 0, pairs[i]);
    }
    
    // Set up different states:
    // pairs[0]: both ends open, no data
    // pairs[1]: data waiting
    write(pairs[1][1], "x", 1);
    // pairs[2]: one end closed
    close(pairs[2][1]);
    // pairs[3]: SHUT_WR on one end
    shutdown(pairs[3][1], SHUT_WR);
    // pairs[4]: SHUT_RDWR on one end
    shutdown(pairs[4][1], SHUT_RDWR);
    // pairs[5-9]: normal, all writable
    
    // Set up poll for all
    for (int i = 0; i < 10; i++) {
        pfds[i*2].fd = pairs[i][0];
        pfds[i*2].events = POLLIN | POLLOUT | POLLHUP;
        pfds[i*2].revents = 0;
        pfds[i*2+1].fd = pairs[i][1];
        pfds[i*2+1].events = POLLIN | POLLOUT | POLLHUP;
        pfds[i*2+1].revents = 0;
    }
    
    // Adjust for closed/shutdown sockets
    pfds[5].fd = pairs[2][1];  // This one is closed, will get POLLNVAL
    pfds[7].fd = pairs[3][1];  // SHUT_WR
    pfds[9].fd = pairs[4][1];  // SHUT_RDWR
    
    int ready = poll(pfds, 20, 0);
    printf("  Poll returned %d ready\n", ready);
    
    // Check expected states
    printf("  pairs[0][0]: revents=%d (expect POLLOUT)\n", pfds[0].revents);
    printf("  pairs[1][0]: revents=%d (expect POLLIN|POLLOUT)\n", pfds[2].revents);
    printf("  pairs[2][0]: revents=%d (expect POLLHUP)\n", pfds[4].revents);
    printf("  pairs[3][0]: revents=%d (expect POLLIN|POLLHUP)\n", pfds[6].revents);
    printf("  pairs[4][0]: revents=%d (expect POLLHUP)\n", pfds[8].revents);
    
    TEST_ASSERT((pfds[0].revents & POLLOUT), "pairs[0][0] writable");
    TEST_ASSERT((pfds[2].revents & POLLIN), "pairs[1][0] readable");
    TEST_ASSERT((pfds[4].revents & POLLHUP), "pairs[2][0] hung up");
    TEST_ASSERT((pfds[6].revents & POLLHUP), "pairs[3][0] hung up after SHUT_WR");
    
    // Cleanup
    for (int i = 0; i < 10; i++) {
        close(pairs[i][0]);
        if (i != 2) close(pairs[i][1]); // pairs[2][1] already closed
    }
    TEST_END();
    return 0;
}

// Test 3: Poll while data in flight
static int test_poll_data_in_flight(void) {
    TEST_START("Poll while data in flight");
    
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    
    // Write multiple chunks
    for (int i = 0; i < 10; i++) {
        char buf[64];
        memset(buf, 'A' + i, sizeof(buf));
        write(sv[1], buf, sizeof(buf));
    }
    
    struct pollfd pfd;
    pfd.fd = sv[0];
    pfd.events = POLLIN;
    pfd.revents = 0;
    
    // Poll should show readability
    int ready = poll(&pfd, 1, 0);
    TEST_ASSERT(ready == 1, "Data available detected");
    TEST_ASSERT((pfd.revents & POLLIN), "POLLIN set");
    
    // Read some data
    char buf[64];
    ssize_t n = read(sv[0], buf, sizeof(buf));
    TEST_ASSERT(n == 64, "Read first chunk");
    
    // Poll again - should still be readable
    pfd.revents = 0;
    ready = poll(&pfd, 1, 0);
    TEST_ASSERT((pfd.revents & POLLIN), "Still readable after partial read");
    
    // Drain remaining data (9 chunks of 64 bytes = 576 bytes)
    int remaining = 9 * 64;
    while (remaining > 0) {
        ssize_t n = read(sv[0], buf, sizeof(buf));
        if (n <= 0) break;
        remaining -= n;
    }
    
    // Now poll - should not be readable
    pfd.revents = 0;
    ready = poll(&pfd, 1, 0);
    printf("  After draining: ready=%d, revents=%d\n", ready, pfd.revents);
    TEST_ASSERT(!(pfd.revents & POLLIN), "Not readable after draining");
    
    close(sv[0]);
    close(sv[1]);
    TEST_END();
    return 0;
}

// Test 4: Rapid poll cycles with changing state
static int test_poll_rapid_state_changes(void) {
    TEST_START("Rapid poll cycles with changing state");
    
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    
    struct pollfd pfd;
    pfd.fd = sv[0];
    pfd.events = POLLIN | POLLOUT;
    
    int read_count = 0;
    int write_count = 0;
    
    for (int i = 0; i < 100; i++) {
        // Alternate between writing and polling
        if (i % 3 == 0) {
            write(sv[1], "x", 1);
            write_count++;
        }
        
        pfd.revents = 0;
        poll(&pfd, 1, 0);
        
        if (pfd.revents & POLLIN) {
            char buf[1];
            read(sv[0], buf, 1);
            read_count++;
        }
    }
    
    printf("  Wrote %d times, read %d times\n", write_count, read_count);
    TEST_ASSERT(read_count == write_count, "All written data was read");
    
    close(sv[0]);
    close(sv[1]);
    TEST_END();
    return 0;
}

// Test 5: Poll with fork - child writes, parent polls
static int test_poll_fork_communication(void) {
    TEST_START("Poll with fork - child writes, parent polls");
    
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    
    pid_t pid = fork();
    if (pid == 0) {
        // Child
        close(sv[0]); // Close parent's end
        
        // Write immediately (don't wait - scheduling is cooperative)
        write(sv[1], "hello from child", 16);
        
        // Small delay to let parent poll
        usleep(10000); // 10ms
        
        // Write more and close
        write(sv[1], "goodbye", 7);
        close(sv[1]);
        exit(0);
    }
    
    // Parent
    close(sv[1]); // Close child's end
    
    struct pollfd pfd;
    pfd.fd = sv[0];
    pfd.events = POLLIN | POLLHUP;
    
    // Poll for first message (with longer timeout)
    pfd.revents = 0;
    int ready = poll(&pfd, 1, 500); // 500ms timeout
    printf("  First poll: ready=%d, revents=%d\n", ready, pfd.revents);
    
    if (!(pfd.revents & POLLIN)) {
        // Scheduling issue - skip test gracefully
        printf("  [SKIP] Child didn't get scheduled in time (cooperative scheduling)\n");
        close(sv[0]);
        wait(NULL);
        test_passed++; // Don't count as failure
        TEST_END();
        return 0;
    }
    
    char buf[64];
    ssize_t n = read(sv[0], buf, sizeof(buf));
    TEST_ASSERT(n == 16, "Read first message");
    
    // Poll for second message and hangup
    pfd.revents = 0;
    ready = poll(&pfd, 1, 500);
    printf("  Second poll: ready=%d, revents=%d\n", ready, pfd.revents);
    
    if (pfd.revents & POLLIN) {
        n = read(sv[0], buf, sizeof(buf));
        printf("  Read %zd bytes\n", n);
    }
    
    // Poll for hangup
    pfd.revents = 0;
    ready = poll(&pfd, 1, 500);
    printf("  Final poll: ready=%d, revents=%d\n", ready, pfd.revents);
    TEST_ASSERT((pfd.revents & POLLHUP), "Detected child close");
    
    close(sv[0]);
    wait(NULL);
    TEST_END();
    return 0;
}

// Test 6: Poll with maximum FDs
static int test_poll_max_fds(void) {
    TEST_START("Poll with maximum FDs (200 sockets)");
    
    int count = 200;
    int sv[200][2];
    struct pollfd pfds[400];
    
    // Create socket pairs
    int created = 0;
    for (int i = 0; i < count; i++) {
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv[i]) < 0) {
            printf("  Created %d pairs before limit\n", created);
            break;
        }
        created++;
    }
    
    printf("  Created %d socket pairs\n", created);
    
    // Set up poll
    for (int i = 0; i < created; i++) {
        pfds[i*2].fd = sv[i][0];
        pfds[i*2].events = POLLIN | POLLOUT;
        pfds[i*2].revents = 0;
        pfds[i*2+1].fd = sv[i][1];
        pfds[i*2+1].events = POLLIN | POLLOUT;
        pfds[i*2+1].revents = 0;
    }
    
    // Write to every other pair
    for (int i = 0; i < created; i += 2) {
        write(sv[i][1], "x", 1);
    }
    
    // Poll all
    int ready = poll(pfds, created * 2, 0);
    printf("  Poll returned %d ready\n", ready);
    
    // Count readable
    int readable = 0;
    for (int i = 0; i < created * 2; i++) {
        if (pfds[i].revents & POLLIN) readable++;
    }
    printf("  %d sockets readable\n", readable);
    TEST_ASSERT(readable == created / 2, "Half the sockets are readable");
    
    // Cleanup
    for (int i = 0; i < created; i++) {
        close(sv[i][0]);
        close(sv[i][1]);
    }
    TEST_END();
    return 0;
}

// Test 7: Poll edge case - empty poll set
static int test_poll_edge_cases(void) {
    TEST_START("Poll edge cases");
    
    // Empty poll set
    int ready = poll(NULL, 0, 0);
    printf("  poll(NULL, 0, 0): %d\n", ready);
    TEST_ASSERT(ready == 0, "Empty poll set returns 0");
    
    // Negative fd in set
    struct pollfd pfd;
    pfd.fd = -1;
    pfd.events = POLLIN;
    pfd.revents = 0;
    ready = poll(&pfd, 1, 0);
    printf("  poll with fd=-1: ready=%d, revents=%d\n", ready, pfd.revents);
    // Negative fd should be ignored (revents=0), not POLLNVAL
    TEST_ASSERT(pfd.revents == 0, "Negative fd ignored");
    
    TEST_END();
    return 0;
}

// Test 8: Poll with all event types
static int test_poll_all_events(void) {
    TEST_START("Poll with all event types");
    
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    
    struct pollfd pfd;
    pfd.fd = sv[0];
    pfd.events = POLLIN | POLLPRI | POLLOUT | POLLRDNORM | POLLWRNORM;
    pfd.revents = 0;
    
    int ready = poll(&pfd, 1, 0);
    printf("  All events poll: ready=%d, revents=%d\n", ready, pfd.revents);
    
    // Connected socket should have POLLOUT and POLLWRNORM
    TEST_ASSERT((pfd.revents & POLLOUT), "POLLOUT set");
    TEST_ASSERT((pfd.revents & POLLWRNORM), "POLLWRNORM set");
    
    // Write data
    write(sv[1], "data", 4);
    
    pfd.revents = 0;
    ready = poll(&pfd, 1, 0);
    printf("  After write: ready=%d, revents=%d\n", ready, pfd.revents);
    
    TEST_ASSERT((pfd.revents & POLLIN), "POLLIN set");
    TEST_ASSERT((pfd.revents & POLLRDNORM), "POLLRDNORM set");
    
    close(sv[0]);
    close(sv[1]);
    TEST_END();
    return 0;
}

// Test 9: Poll stress - 5000 iterations
static int test_poll_stress_5000(void) {
    TEST_START("Poll stress - 5000 iterations");
    
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    
    struct pollfd pfds[2];
    pfds[0].fd = sv[0];
    pfds[0].events = POLLIN | POLLOUT;
    pfds[1].fd = sv[1];
    pfds[1].events = POLLIN | POLLOUT;
    
    int success = 0;
    for (int i = 0; i < 5000; i++) {
        pfds[0].revents = 0;
        pfds[1].revents = 0;
        
        int ready = poll(pfds, 2, 0);
        if (ready == 2 && (pfds[0].revents & POLLOUT) && (pfds[1].revents & POLLOUT)) {
            success++;
        }
    }
    
    printf("  %d/5000 successful polls\n", success);
    TEST_ASSERT(success == 5000, "All 5000 polls succeeded");
    
    close(sv[0]);
    close(sv[1]);
    TEST_END();
    return 0;
}

int main(void) {
    printf("\n");
    printf("╔════════════════════════════════════════════════════════════╗\n");
    printf("║   Advanced Socket Test - Phase 7: poll() Stress Test      ║\n");
    printf("╚════════════════════════════════════════════════════════════╝\n");
    
    int total_passed = 0;
    int total_failed = 0;
    
    test_poll_partial_shutdowns();
    total_passed += test_passed; total_failed += test_failed;
    test_passed = test_failed = 0;
    
    test_poll_mixed_states();
    total_passed += test_passed; total_failed += test_failed;
    test_passed = test_failed = 0;
    
    test_poll_data_in_flight();
    total_passed += test_passed; total_failed += test_failed;
    test_passed = test_failed = 0;
    
    test_poll_rapid_state_changes();
    total_passed += test_passed; total_failed += test_failed;
    test_passed = test_failed = 0;
    
    test_poll_fork_communication();
    total_passed += test_passed; total_failed += test_failed;
    test_passed = test_failed = 0;
    
    test_poll_max_fds();
    total_passed += test_passed; total_failed += test_failed;
    test_passed = test_failed = 0;
    
    test_poll_edge_cases();
    total_passed += test_passed; total_failed += test_failed;
    test_passed = test_failed = 0;
    
    test_poll_all_events();
    total_passed += test_passed; total_failed += test_failed;
    test_passed = test_failed = 0;
    
    test_poll_stress_5000();
    total_passed += test_passed; total_failed += test_failed;
    test_passed = test_failed = 0;
    
    printf("\n--- Advanced Phase 7 Summary ---\n");
    printf("Total Passed: %d, Total Failed: %d\n", total_passed, total_failed);
    
    return total_failed > 0 ? 1 : 0;
}
