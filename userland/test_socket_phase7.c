// Phase 7: poll() Implementation Test Suite
// Tests socket poll functionality for AF_UNIX sockets

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

// Test 1: Basic poll on connected socket pair
static int test_poll_basic(void) {
    TEST_START("Basic poll on socket pair");
    
    int sv[2];
    int ret = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    TEST_ASSERT(ret == 0, "socketpair created");
    
    struct pollfd pfds[2];
    pfds[0].fd = sv[0];
    pfds[0].events = POLLIN | POLLOUT;
    pfds[0].revents = 0;
    pfds[1].fd = sv[1];
    pfds[1].events = POLLIN | POLLOUT;
    pfds[1].revents = 0;
    
    // Poll with 0 timeout (immediate)
    int ready = poll(pfds, 2, 0);
    TEST_ASSERT(ready >= 0, "poll returned without error");
    printf("  Immediate poll: %d ready\n", ready);
    
    // Both sockets should be writable (connected)
    TEST_ASSERT((pfds[0].revents & POLLOUT), "sv[0] is writable");
    TEST_ASSERT((pfds[1].revents & POLLOUT), "sv[1] is writable");
    
    close(sv[0]);
    close(sv[1]);
    TEST_END();
    return 0;
}

// Test 2: Poll for readability after write
static int test_poll_readability(void) {
    TEST_START("Poll for readability after write");
    
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    
    struct pollfd pfds[2];
    pfds[0].fd = sv[0];
    pfds[0].events = POLLIN;
    pfds[0].revents = 0;
    pfds[1].fd = sv[1];
    pfds[1].events = POLLIN;
    pfds[1].revents = 0;
    
    // Nothing to read initially
    int ready = poll(pfds, 2, 0);
    printf("  Before write: %d ready, revents[0]=%d, revents[1]=%d\n", 
           ready, pfds[0].revents, pfds[1].revents);
    TEST_ASSERT(!(pfds[0].revents & POLLIN), "sv[0] not readable yet");
    TEST_ASSERT(!(pfds[1].revents & POLLIN), "sv[1] not readable yet");
    
    // Write to sv[1], should make sv[0] readable
    char buf[] = "hello";
    ssize_t written = write(sv[1], buf, sizeof(buf));
    TEST_ASSERT(written == sizeof(buf), "write succeeded");
    
    // Poll sv[0] for readability
    pfds[0].fd = sv[0];
    pfds[0].events = POLLIN;
    pfds[0].revents = 0;
    ready = poll(pfds, 1, 0);
    printf("  After write: %d ready, revents[0]=%d\n", ready, pfds[0].revents);
    TEST_ASSERT((pfds[0].revents & POLLIN), "sv[0] is now readable");
    
    close(sv[0]);
    close(sv[1]);
    TEST_END();
    return 0;
}

// Test 3: Poll for POLLHUP after close
static int test_poll_hangup(void) {
    TEST_START("Poll for POLLHUP after close");
    
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    
    // Close one end
    close(sv[1]);
    
    struct pollfd pfd;
    pfd.fd = sv[0];
    pfd.events = POLLIN | POLLOUT | POLLHUP;
    pfd.revents = 0;
    
    int ready = poll(&pfd, 1, 0);
    printf("  After close: %d ready, revents=%d\n", ready, pfd.revents);
    TEST_ASSERT((pfd.revents & POLLHUP), "POLLHUP detected after peer close");
    
    close(sv[0]);
    TEST_END();
    return 0;
}

// Test 4: Poll on listening socket
static int test_poll_listening(void) {
    TEST_START("Poll on listening socket");
    
    int server = socket(AF_UNIX, SOCK_STREAM, 0);
    TEST_ASSERT(server >= 0, "socket created");
    
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, "/tmp/poll_listen.sock");
    unlink(addr.sun_path);
    
    int ret = bind(server, (struct sockaddr*)&addr, sizeof(addr));
    TEST_ASSERT(ret == 0, "bind succeeded");
    
    ret = listen(server, 5);
    TEST_ASSERT(ret == 0, "listen succeeded");
    
    struct pollfd pfd;
    pfd.fd = server;
    pfd.events = POLLIN;
    pfd.revents = 0;
    
    // No pending connections
    int ready = poll(&pfd, 1, 0);
    printf("  Before connect: %d ready, revents=%d\n", ready, pfd.revents);
    TEST_ASSERT(!(pfd.revents & POLLIN), "No pending connections");
    
    // Fork and connect
    pid_t pid = fork();
    if (pid == 0) {
        // Child
        int client = socket(AF_UNIX, SOCK_STREAM, 0);
        connect(client, (struct sockaddr*)&addr, sizeof(addr));
        sleep(1); // Keep connection alive
        close(client);
        exit(0);
    }
    
    // Give child time to connect
    usleep(100000); // 100ms
    
    // Now poll should show readability
    pfd.revents = 0;
    ready = poll(&pfd, 1, 0);
    printf("  After connect: %d ready, revents=%d\n", ready, pfd.revents);
    TEST_ASSERT((pfd.revents & POLLIN), "Pending connection detected");
    
    // Accept the connection
    int client = accept(server, NULL, NULL);
    TEST_ASSERT(client >= 0, "accept succeeded");
    
    close(client);
    close(server);
    unlink(addr.sun_path);
    wait(NULL);
    TEST_END();
    return 0;
}

// Test 5: Poll with timeout
static int test_poll_timeout(void) {
    TEST_START("Poll with timeout");
    
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    
    struct pollfd pfd;
    pfd.fd = sv[0];
    pfd.events = POLLIN; // Only looking for readability
    pfd.revents = 0;
    
    // Nothing to read, should timeout
    // Note: our implementation only yields once, so this is a basic test
    int ready = poll(&pfd, 1, 100); // 100ms timeout
    printf("  Poll with 100ms timeout: %d ready, revents=%d\n", ready, pfd.revents);
    TEST_ASSERT(ready == 0, "No events, should timeout");
    
    close(sv[0]);
    close(sv[1]);
    TEST_END();
    return 0;
}

// Test 6: Poll multiple sockets
static int test_poll_multiple(void) {
    TEST_START("Poll multiple sockets");
    
    int pairs[5][2];
    struct pollfd pfds[10];
    
    // Create 5 socket pairs
    for (int i = 0; i < 5; i++) {
        socketpair(AF_UNIX, SOCK_STREAM, 0, pairs[i]);
        pfds[i*2].fd = pairs[i][0];
        pfds[i*2].events = POLLIN | POLLOUT;
        pfds[i*2].revents = 0;
        pfds[i*2+1].fd = pairs[i][1];
        pfds[i*2+1].events = POLLIN | POLLOUT;
        pfds[i*2+1].revents = 0;
    }
    
    // Poll all - should all be writable
    int ready = poll(pfds, 10, 0);
    printf("  Initial poll: %d ready\n", ready);
    TEST_ASSERT(ready == 10, "All 10 sockets ready");
    
    // Write to pair 0 and 2
    write(pairs[0][1], "x", 1);
    write(pairs[2][1], "y", 1);
    
    // Reset and poll for readability only
    for (int i = 0; i < 10; i++) {
        pfds[i].events = POLLIN;
        pfds[i].revents = 0;
    }
    
    ready = poll(pfds, 10, 0);
    printf("  After writes: %d ready for reading\n", ready);
    TEST_ASSERT((pfds[0].revents & POLLIN), "pair[0][0] readable");
    TEST_ASSERT((pfds[4].revents & POLLIN), "pair[2][0] readable");
    
    // Cleanup
    for (int i = 0; i < 5; i++) {
        close(pairs[i][0]);
        close(pairs[i][1]);
    }
    TEST_END();
    return 0;
}

// Test 7: Poll on invalid fd
static int test_poll_invalid_fd(void) {
    TEST_START("Poll on invalid fd");
    
    struct pollfd pfd;
    pfd.fd = 999; // Invalid fd
    pfd.events = POLLIN;
    pfd.revents = 0;
    
    int ready = poll(&pfd, 1, 0);
    printf("  Poll invalid fd: %d ready, revents=%d\n", ready, pfd.revents);
    TEST_ASSERT((pfd.revents & POLLNVAL), "POLLNVAL for invalid fd");
    
    TEST_END();
    return 0;
}

// Test 8: Stress test - many polls
static int test_poll_stress(void) {
    TEST_START("Stress test - 1000 poll cycles");
    
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    
    struct pollfd pfds[2];
    pfds[0].fd = sv[0];
    pfds[1].fd = sv[1];
    
    for (int i = 0; i < 1000; i++) {
        pfds[0].events = POLLIN | POLLOUT;
        pfds[0].revents = 0;
        pfds[1].events = POLLIN | POLLOUT;
        pfds[1].revents = 0;
        
        poll(pfds, 2, 0);
    }
    
    printf("  [OK] Completed 1000 poll cycles\n");
    test_passed++;
    
    close(sv[0]);
    close(sv[1]);
    TEST_END();
    return 0;
}

// Test 9: Poll with shutdown
static int test_poll_shutdown(void) {
    TEST_START("Poll with shutdown");
    
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    
    // Shutdown write end
    shutdown(sv[1], SHUT_WR);
    
    struct pollfd pfd;
    pfd.fd = sv[0];
    pfd.events = POLLIN | POLLOUT | POLLHUP;
    pfd.revents = 0;
    
    int ready = poll(&pfd, 1, 0);
    printf("  After SHUT_WR: %d ready, revents=%d\n", ready, pfd.revents);
    // Should detect POLLHUP or POLLIN (EOF)
    TEST_ASSERT((pfd.revents & (POLLHUP | POLLIN)), "POLLHUP or POLLIN after shutdown");
    
    close(sv[0]);
    close(sv[1]);
    TEST_END();
    return 0;
}

int main(void) {
    printf("\n");
    printf("========================================\n");
    printf("  Phase 7: poll() Implementation\n");
    printf("  Test Suite\n");
    printf("========================================\n\n");
    
    int total_passed = 0;
    int total_failed = 0;
    
    test_poll_basic();
    total_passed += test_passed; total_failed += test_failed;
    test_passed = test_failed = 0;
    
    test_poll_readability();
    total_passed += test_passed; total_failed += test_failed;
    test_passed = test_failed = 0;
    
    test_poll_hangup();
    total_passed += test_passed; total_failed += test_failed;
    test_passed = test_failed = 0;
    
    test_poll_listening();
    total_passed += test_passed; total_failed += test_failed;
    test_passed = test_failed = 0;
    
    test_poll_timeout();
    total_passed += test_passed; total_failed += test_failed;
    test_passed = test_failed = 0;
    
    test_poll_multiple();
    total_passed += test_passed; total_failed += test_failed;
    test_passed = test_failed = 0;
    
    test_poll_invalid_fd();
    total_passed += test_passed; total_failed += test_failed;
    test_passed = test_failed = 0;
    
    test_poll_stress();
    total_passed += test_passed; total_failed += test_failed;
    test_passed = test_failed = 0;
    
    test_poll_shutdown();
    total_passed += test_passed; total_failed += test_failed;
    test_passed = test_failed = 0;
    
    printf("\n========================================\n");
    printf("  All Phase 7 Tests Completed\n");
    printf("  Total Passed: %d, Total Failed: %d\n", total_passed, total_failed);
    printf("========================================\n\n");
    
    return total_failed > 0 ? 1 : 0;
}
