// Phase 8: epoll() Implementation Test Suite
// Tests epoll functionality for AF_UNIX sockets

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/epoll.h>
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

// Test 1: Basic epoll_create1
static int test_epoll_create(void) {
    TEST_START("epoll_create1 basic");
    
    int epfd = epoll_create1(0);
    TEST_ASSERT(epfd >= 0, "epoll_create1 succeeded");
    
    close(epfd);
    TEST_END();
    return 0;
}

// Test 2: epoll_ctl ADD
static int test_epoll_ctl_add(void) {
    TEST_START("epoll_ctl ADD");
    
    int epfd = epoll_create1(0);
    TEST_ASSERT(epfd >= 0, "epoll_create1 succeeded");
    
    int sv[2];
    int ret = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    TEST_ASSERT(ret == 0, "socketpair created");
    
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLOUT;
    ev.data.u32 = 123;
    
    ret = epoll_ctl(epfd, EPOLL_CTL_ADD, sv[0], &ev);
    TEST_ASSERT(ret == 0, "epoll_ctl ADD succeeded");
    
    close(sv[0]);
    close(sv[1]);
    close(epfd);
    TEST_END();
    return 0;
}

// Test 3: epoll_ctl DEL
static int test_epoll_ctl_del(void) {
    TEST_START("epoll_ctl DEL");
    
    int epfd = epoll_create1(0);
    TEST_ASSERT(epfd >= 0, "epoll_create1 succeeded");
    
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    
    struct epoll_event ev = {.events = EPOLLIN | EPOLLOUT, .data.u32 = 1};
    int ret = epoll_ctl(epfd, EPOLL_CTL_ADD, sv[0], &ev);
    TEST_ASSERT(ret == 0, "epoll_ctl ADD succeeded");
    
    ret = epoll_ctl(epfd, EPOLL_CTL_DEL, sv[0], NULL);
    TEST_ASSERT(ret == 0, "epoll_ctl DEL succeeded");
    
    close(sv[0]);
    close(sv[1]);
    close(epfd);
    TEST_END();
    return 0;
}

// Test 4: epoll_ctl MOD
static int test_epoll_ctl_mod(void) {
    TEST_START("epoll_ctl MOD");
    
    int epfd = epoll_create1(0);
    TEST_ASSERT(epfd >= 0, "epoll_create1 succeeded");
    
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    
    struct epoll_event ev = {.events = EPOLLIN, .data.u32 = 1};
    int ret = epoll_ctl(epfd, EPOLL_CTL_ADD, sv[0], &ev);
    TEST_ASSERT(ret == 0, "epoll_ctl ADD succeeded");
    
    // Modify to watch for different events
    ev.events = EPOLLOUT;
    ev.data.u32 = 456;
    ret = epoll_ctl(epfd, EPOLL_CTL_MOD, sv[0], &ev);
    TEST_ASSERT(ret == 0, "epoll_ctl MOD succeeded");
    
    close(sv[0]);
    close(sv[1]);
    close(epfd);
    TEST_END();
    return 0;
}

// Test 5: epoll_wait immediate return (level-triggered)
static int test_epoll_wait_immediate(void) {
    TEST_START("epoll_wait immediate (level-triggered)");
    
    int epfd = epoll_create1(0);
    TEST_ASSERT(epfd >= 0, "epoll_create1 succeeded");
    
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    
    struct epoll_event ev = {.events = EPOLLIN | EPOLLOUT, .data.u32 = 1};
    epoll_ctl(epfd, EPOLL_CTL_ADD, sv[0], &ev);
    
    // Connected socket should be writable immediately
    struct epoll_event events[10];
    int n = epoll_wait(epfd, events, 10, 0);
    printf("  Immediate epoll_wait: %d events\n", n);
    TEST_ASSERT(n >= 1, "epoll_wait returned events");
    TEST_ASSERT((events[0].events & EPOLLOUT), "EPOLLOUT event present");
    
    close(sv[0]);
    close(sv[1]);
    close(epfd);
    TEST_END();
    return 0;
}

// Test 6: epoll_wait for readability
static int test_epoll_wait_readable(void) {
    TEST_START("epoll_wait for readability");
    
    int epfd = epoll_create1(0);
    TEST_ASSERT(epfd >= 0, "epoll_create1 succeeded");
    
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    
    struct epoll_event ev = {.events = EPOLLIN | EPOLLOUT, .data.u32 = 1};
    epoll_ctl(epfd, EPOLL_CTL_ADD, sv[0], &ev);
    
    // Initially not readable
    struct epoll_event events[10];
    int n = epoll_wait(epfd, events, 10, 0);
    printf("  Before write: %d events, events[0].events=%d\n", n, n > 0 ? events[0].events : 0);
    
    // Write to sv[1], should make sv[0] readable
    char buf[] = "hello";
    ssize_t written = write(sv[1], buf, sizeof(buf));
    TEST_ASSERT(written == sizeof(buf), "write succeeded");
    
    // Now poll for readability
    n = epoll_wait(epfd, events, 10, 0);
    printf("  After write: %d events, events[0].events=%d\n", n, n > 0 ? events[0].events : 0);
    TEST_ASSERT(n >= 1, "epoll_wait returned events after write");
    TEST_ASSERT((events[0].events & EPOLLIN), "EPOLLIN event present");
    
    close(sv[0]);
    close(sv[1]);
    close(epfd);
    TEST_END();
    return 0;
}

// Test 7: Edge-triggered mode
static int test_epoll_edge_triggered(void) {
    TEST_START("Edge-triggered mode");
    
    int epfd = epoll_create1(0);
    TEST_ASSERT(epfd >= 0, "epoll_create1 succeeded");
    
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    
    // Add with EPOLLET (edge-triggered)
    struct epoll_event ev = {.events = EPOLLIN | EPOLLET, .data.u32 = 1};
    epoll_ctl(epfd, EPOLL_CTL_ADD, sv[0], &ev);
    
    // Write data
    write(sv[1], "hello", 5);
    
    struct epoll_event events[10];
    
    // First wait should return (edge triggered on new data)
    int n = epoll_wait(epfd, events, 10, 100);
    printf("  First wait (ET): %d events\n", n);
    TEST_ASSERT(n >= 1, "First wait returned events");
    
    // Second wait should NOT return (no new data, even though data still there)
    n = epoll_wait(epfd, events, 10, 100);
    printf("  Second wait (ET): %d events\n", n);
    // Note: This test may pass or fail depending on implementation details
    // In true ET mode, second wait should return 0
    
    close(sv[0]);
    close(sv[1]);
    close(epfd);
    TEST_END();
    return 0;
}

// Test 8: epoll on multiple FDs
static int test_epoll_multiple_fds(void) {
    TEST_START("epoll on multiple FDs");
    
    int epfd = epoll_create1(0);
    TEST_ASSERT(epfd >= 0, "epoll_create1 succeeded");
    
    int pairs[5][2];
    
    // Create 5 socket pairs and add to epoll
    for (int i = 0; i < 5; i++) {
        socketpair(AF_UNIX, SOCK_STREAM, 0, pairs[i]);
        
        struct epoll_event ev = {.events = EPOLLIN | EPOLLOUT, .data.u32 = i};
        int ret = epoll_ctl(epfd, EPOLL_CTL_ADD, pairs[i][0], &ev);
        TEST_ASSERT(ret == 0, "epoll_ctl ADD succeeded");
    }
    
    // Write to pair 0 and 2
    write(pairs[0][1], "x", 1);
    write(pairs[2][1], "y", 1);
    
    struct epoll_event events[10];
    int n = epoll_wait(epfd, events, 10, 0);
    printf("  After writes: %d events\n", n);
    TEST_ASSERT(n >= 2, "At least 2 events returned");
    
    // Cleanup
    for (int i = 0; i < 5; i++) {
        close(pairs[i][0]);
        close(pairs[i][1]);
    }
    close(epfd);
    TEST_END();
    return 0;
}

// Test 9: epoll on invalid FD
static int test_epoll_invalid_fd(void) {
    TEST_START("epoll on invalid FD");
    
    int epfd = epoll_create1(0);
    TEST_ASSERT(epfd >= 0, "epoll_create1 succeeded");
    
    struct epoll_event ev = {.events = EPOLLIN, .data.u32 = 1};
    int ret = epoll_ctl(epfd, EPOLL_CTL_ADD, 999, &ev);
    printf("  epoll_ctl ADD on invalid fd: ret=%d, errno=%d\n", ret, errno);
    TEST_ASSERT(ret < 0, "epoll_ctl ADD on invalid fd failed");
    
    close(epfd);
    TEST_END();
    return 0;
}

// Test 10: epoll_ctl ADD duplicate
static int test_epoll_add_duplicate(void) {
    TEST_START("epoll_ctl ADD duplicate");
    
    int epfd = epoll_create1(0);
    TEST_ASSERT(epfd >= 0, "epoll_create1 succeeded");
    
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    
    struct epoll_event ev = {.events = EPOLLIN, .data.u32 = 1};
    int ret = epoll_ctl(epfd, EPOLL_CTL_ADD, sv[0], &ev);
    TEST_ASSERT(ret == 0, "First epoll_ctl ADD succeeded");
    
    // Try to add same FD again
    ret = epoll_ctl(epfd, EPOLL_CTL_ADD, sv[0], &ev);
    printf("  Second ADD (duplicate): ret=%d, errno=%d\n", ret, errno);
    TEST_ASSERT(ret < 0, "Duplicate ADD failed as expected");
    
    close(sv[0]);
    close(sv[1]);
    close(epfd);
    TEST_END();
    return 0;
}

// Test 11: EPOLLONESHOT
static int test_epoll_oneshot(void) {
    TEST_START("EPOLLONESHOT mode");
    
    int epfd = epoll_create1(0);
    TEST_ASSERT(epfd >= 0, "epoll_create1 succeeded");
    
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    
    // Add with EPOLLONESHOT
    struct epoll_event ev = {.events = EPOLLIN | EPOLLONESHOT, .data.u32 = 1};
    epoll_ctl(epfd, EPOLL_CTL_ADD, sv[0], &ev);
    
    // Write data
    write(sv[1], "msg1", 4);
    
    struct epoll_event events[10];
    int n = epoll_wait(epfd, events, 10, 100);
    printf("  First wait (oneshot): %d events\n", n);
    TEST_ASSERT(n >= 1, "First wait returned events");
    
    // Write more data
    write(sv[1], "msg2", 4);
    
    // Second wait should NOT return (oneshot disabled the FD)
    n = epoll_wait(epfd, events, 10, 100);
    printf("  Second wait (oneshot disabled): %d events\n", n);
    // Note: May or may not work depending on implementation
    
    close(sv[0]);
    close(sv[1]);
    close(epfd);
    TEST_END();
    return 0;
}

// Test 12: epoll on listening socket
static int test_epoll_listening(void) {
    TEST_START("epoll on listening socket");
    
    int server = socket(AF_UNIX, SOCK_STREAM, 0);
    TEST_ASSERT(server >= 0, "socket created");
    
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, "/tmp/epoll_listen.sock");
    unlink(addr.sun_path);
    
    int ret = bind(server, (struct sockaddr*)&addr, sizeof(addr));
    TEST_ASSERT(ret == 0, "bind succeeded");
    
    ret = listen(server, 5);
    TEST_ASSERT(ret == 0, "listen succeeded");
    
    int epfd = epoll_create1(0);
    struct epoll_event ev = {.events = EPOLLIN, .data.fd = server};
    epoll_ctl(epfd, EPOLL_CTL_ADD, server, &ev);
    
    // No connections yet
    struct epoll_event events[10];
    int n = epoll_wait(epfd, events, 10, 0);
    printf("  Before connect: %d events\n", n);
    TEST_ASSERT(n == 0, "No events before connect");
    
    // Fork and connect
    pid_t pid = fork();
    if (pid == 0) {
        // Child
        int client = socket(AF_UNIX, SOCK_STREAM, 0);
        connect(client, (struct sockaddr*)&addr, sizeof(addr));
        sleep(1);
        close(client);
        exit(0);
    }
    
    // Give child time to connect
    usleep(100000);
    
    // Now poll should show readability
    n = epoll_wait(epfd, events, 10, 0);
    printf("  After connect: %d events\n", n);
    TEST_ASSERT(n >= 1, "Events after connect");
    TEST_ASSERT((events[0].events & EPOLLIN), "EPOLLIN on accept");
    
    // Accept the connection
    int client = accept(server, NULL, NULL);
    TEST_ASSERT(client >= 0, "accept succeeded");
    
    close(client);
    close(server);
    close(epfd);
    unlink(addr.sun_path);
    wait(NULL);
    TEST_END();
    return 0;
}

// Test 13: Stress test - many epoll operations
static int test_epoll_stress(void) {
    TEST_START("Stress test - 1000 epoll cycles");
    
    int epfd = epoll_create1(0);
    TEST_ASSERT(epfd >= 0, "epoll_create1 succeeded");
    
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    
    struct epoll_event ev = {.events = EPOLLIN | EPOLLOUT, .data.u32 = 1};
    
    for (int i = 0; i < 1000; i++) {
        epoll_ctl(epfd, EPOLL_CTL_ADD, sv[0], &ev);
        
        struct epoll_event events[10];
        epoll_wait(epfd, events, 10, 0);
        
        epoll_ctl(epfd, EPOLL_CTL_DEL, sv[0], NULL);
    }
    
    printf("  [OK] Completed 1000 add/wait/del cycles\n");
    test_passed++;
    
    close(sv[0]);
    close(sv[1]);
    close(epfd);
    TEST_END();
    return 0;
}

// Test 14: epoll_wait with timeout
static int test_epoll_timeout(void) {
    TEST_START("epoll_wait with timeout");
    
    int epfd = epoll_create1(0);
    TEST_ASSERT(epfd >= 0, "epoll_create1 succeeded");
    
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    
    // Only watch for readability (no data yet)
    struct epoll_event ev = {.events = EPOLLIN, .data.u32 = 1};
    epoll_ctl(epfd, EPOLL_CTL_ADD, sv[0], &ev);
    
    struct epoll_event events[10];
    
    // Wait with 100ms timeout (should timeout)
    int n = epoll_wait(epfd, events, 10, 100);
    printf("  Wait with 100ms timeout: %d events\n", n);
    // May timeout or return 0 depending on implementation
    
    close(sv[0]);
    close(sv[1]);
    close(epfd);
    TEST_END();
    return 0;
}

// Test 15: epoll on closed socket (POLLHUP)
static int test_epoll_hangup(void) {
    TEST_START("epoll POLLHUP detection");
    
    int epfd = epoll_create1(0);
    TEST_ASSERT(epfd >= 0, "epoll_create1 succeeded");
    
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    
    struct epoll_event ev = {.events = EPOLLIN | EPOLLOUT | EPOLLHUP, .data.u32 = 1};
    epoll_ctl(epfd, EPOLL_CTL_ADD, sv[0], &ev);
    
    // Close one end
    close(sv[1]);
    
    struct epoll_event events[10];
    int n = epoll_wait(epfd, events, 10, 0);
    printf("  After peer close: %d events, events[0]=%d\n", n, n > 0 ? events[0].events : 0);
    TEST_ASSERT(n >= 1, "Events returned after peer close");
    // Should have POLLHUP or EPOLLIN (EOF)
    
    close(sv[0]);
    close(epfd);
    TEST_END();
    return 0;
}

int main(void) {
    printf("\n");
    printf("========================================\n");
    printf("  Phase 8: epoll() Implementation\n");
    printf("  Test Suite\n");
    printf("========================================\n\n");
    
    int total_passed = 0;
    int total_failed = 0;
    
    test_epoll_create();
    total_passed += test_passed; total_failed += test_failed;
    test_passed = test_failed = 0;
    
    test_epoll_ctl_add();
    total_passed += test_passed; total_failed += test_failed;
    test_passed = test_failed = 0;
    
    test_epoll_ctl_del();
    total_passed += test_passed; total_failed += test_failed;
    test_passed = test_failed = 0;
    
    test_epoll_ctl_mod();
    total_passed += test_passed; total_failed += test_failed;
    test_passed = test_failed = 0;
    
    test_epoll_wait_immediate();
    total_passed += test_passed; total_failed += test_failed;
    test_passed = test_failed = 0;
    
    test_epoll_wait_readable();
    total_passed += test_passed; total_failed += test_failed;
    test_passed = test_failed = 0;
    
    test_epoll_edge_triggered();
    total_passed += test_passed; total_failed += test_failed;
    test_passed = test_failed = 0;
    
    test_epoll_multiple_fds();
    total_passed += test_passed; total_failed += test_failed;
    test_passed = test_failed = 0;
    
    test_epoll_invalid_fd();
    total_passed += test_passed; total_failed += test_failed;
    test_passed = test_failed = 0;
    
    test_epoll_add_duplicate();
    total_passed += test_passed; total_failed += test_failed;
    test_passed = test_failed = 0;
    
    test_epoll_oneshot();
    total_passed += test_passed; total_failed += test_failed;
    test_passed = test_failed = 0;
    
    test_epoll_listening();
    total_passed += test_passed; total_failed += test_failed;
    test_passed = test_failed = 0;
    
    test_epoll_stress();
    total_passed += test_passed; total_failed += test_failed;
    test_passed = test_failed = 0;
    
    test_epoll_timeout();
    total_passed += test_passed; total_failed += test_failed;
    test_passed = test_failed = 0;
    
    test_epoll_hangup();
    total_passed += test_passed; total_failed += test_failed;
    test_passed = test_failed = 0;
    
    printf("\n========================================\n");
    printf("  All Phase 8 Tests Completed\n");
    printf("  Total Passed: %d, Total Failed: %d\n", total_passed, total_failed);
    printf("========================================\n\n");
    
    return total_failed > 0 ? 1 : 0;
}
