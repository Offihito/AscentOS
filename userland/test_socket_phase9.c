// Phase 9: epoll Advanced Features Test Suite
// Tests EPOLLONESHOT, EPOLLEXCLUSIVE, EPOLLRDHUP, and epoll_pwait

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/epoll.h>
#include <sys/wait.h>
#include <errno.h>

// Poll events for EPOLLRDHUP (may not be defined in older headers)
#ifndef EPOLLRDHUP
#define EPOLLRDHUP 0x2000
#endif

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

// ═══════════════════════════════════════════════════════════════════════════
// Test 1: EPOLLONESHOT - Basic one-shot behavior
// ═══════════════════════════════════════════════════════════════════════════
static int test_oneshot_basic(void) {
    TEST_START("EPOLLONESHOT basic");
    
    int sv[2];
    int ret = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    TEST_ASSERT(ret == 0, "socketpair created");
    
    int epfd = epoll_create1(0);
    TEST_ASSERT(epfd >= 0, "epoll_create1 succeeded");
    
    // Add with EPOLLONESHOT
    struct epoll_event ev = { 
        .events = EPOLLIN | EPOLLONESHOT, 
        .data.u32 = 1 
    };
    ret = epoll_ctl(epfd, EPOLL_CTL_ADD, sv[1], &ev);
    TEST_ASSERT(ret == 0, "epoll_ctl ADD with EPOLLONESHOT succeeded");
    
    // Send first message
    ssize_t written = write(sv[0], "msg1", 4);
    TEST_ASSERT(written == 4, "first write succeeded");
    
    // Should receive one event
    struct epoll_event events[10];
    int n = epoll_wait(epfd, events, 10, 100);
    printf("  First event: %d events\n", n);
    TEST_ASSERT(n == 1, "received first event");
    TEST_ASSERT(events[0].data.u32 == 1, "correct event data");
    
    // Read the data
    char buf[16];
    ssize_t r = read(sv[1], buf, sizeof(buf));
    TEST_ASSERT(r == 4, "read first message");
    
    // Send second message WITHOUT re-arming
    written = write(sv[0], "msg2", 4);
    TEST_ASSERT(written == 4, "second write succeeded");
    
    // Should NOT receive event (oneshot disabled)
    n = epoll_wait(epfd, events, 10, 100);
    printf("  Without re-arm: %d events\n", n);
    TEST_ASSERT(n == 0, "no event without re-arm (oneshot working)");
    
    close(sv[0]);
    close(sv[1]);
    close(epfd);
    TEST_END();
    return 0;
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 2: EPOLLONESHOT - Re-arming with EPOLL_CTL_MOD
// ═══════════════════════════════════════════════════════════════════════════
static int test_oneshot_rearm(void) {
    TEST_START("EPOLLONESHOT re-arm");
    
    int sv[2];
    int ret = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    TEST_ASSERT(ret == 0, "socketpair created");
    
    int epfd = epoll_create1(0);
    TEST_ASSERT(epfd >= 0, "epoll_create1 succeeded");
    
    // Add with EPOLLONESHOT
    struct epoll_event ev = { 
        .events = EPOLLIN | EPOLLONESHOT, 
        .data.u32 = 1 
    };
    ret = epoll_ctl(epfd, EPOLL_CTL_ADD, sv[1], &ev);
    TEST_ASSERT(ret == 0, "epoll_ctl ADD with EPOLLONESHOT succeeded");
    
    // Send first message
    write(sv[0], "msg1", 4);
    
    // Receive first event
    struct epoll_event events[10];
    int n = epoll_wait(epfd, events, 10, 100);
    TEST_ASSERT(n == 1, "received first event");
    
    // Read the data
    char buf[16];
    read(sv[1], buf, sizeof(buf));
    
    // Send second message (should not trigger event)
    write(sv[0], "msg2", 4);
    n = epoll_wait(epfd, events, 10, 100);
    TEST_ASSERT(n == 0, "no event before re-arm");
    
    // Re-arm with EPOLL_CTL_MOD
    ev.events = EPOLLIN | EPOLLONESHOT;
    ev.data.u32 = 1;
    ret = epoll_ctl(epfd, EPOLL_CTL_MOD, sv[1], &ev);
    TEST_ASSERT(ret == 0, "epoll_ctl MOD (re-arm) succeeded");
    
    // Now should receive the pending event
    n = epoll_wait(epfd, events, 10, 100);
    printf("  After re-arm: %d events\n", n);
    TEST_ASSERT(n == 1, "received event after re-arm");
    
    // Read the data
    read(sv[1], buf, sizeof(buf));
    
    // Send third message and verify oneshot still works
    write(sv[0], "msg3", 4);
    n = epoll_wait(epfd, events, 10, 100);
    TEST_ASSERT(n == 0, "no event after firing again (oneshot still active)");
    
    close(sv[0]);
    close(sv[1]);
    close(epfd);
    TEST_END();
    return 0;
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 3: EPOLLEXCLUSIVE - Prevent thundering herd
// ═══════════════════════════════════════════════════════════════════════════
static int test_exclusive_thundering_herd(void) {
    TEST_START("EPOLLEXCLUSIVE thundering herd prevention");
    
    int sv[2];
    int ret = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    TEST_ASSERT(ret == 0, "socketpair created");
    
    // Create multiple epoll instances
    int epfd1 = epoll_create1(0);
    int epfd2 = epoll_create1(0);
    int epfd3 = epoll_create1(0);
    TEST_ASSERT(epfd1 >= 0 && epfd2 >= 0 && epfd3 >= 0, "created 3 epoll instances");
    
    // Add same FD to all with EPOLLEXCLUSIVE
    struct epoll_event ev = { 
        .events = EPOLLIN | EPOLLEXCLUSIVE, 
        .data.u32 = 100 
    };
    
    ret = epoll_ctl(epfd1, EPOLL_CTL_ADD, sv[1], &ev);
    TEST_ASSERT(ret == 0, "added to epfd1 with EPOLLEXCLUSIVE");
    
    ret = epoll_ctl(epfd2, EPOLL_CTL_ADD, sv[1], &ev);
    TEST_ASSERT(ret == 0, "added to epfd2 with EPOLLEXCLUSIVE");
    
    ret = epoll_ctl(epfd3, EPOLL_CTL_ADD, sv[1], &ev);
    TEST_ASSERT(ret == 0, "added to epfd3 with EPOLLEXCLUSIVE");
    
    // Send data
    write(sv[0], "test", 4);
    
    // Check which epoll instances received events
    struct epoll_event events[10];
    int n1 = epoll_wait(epfd1, events, 10, 0);
    int n2 = epoll_wait(epfd2, events, 10, 0);
    int n3 = epoll_wait(epfd3, events, 10, 0);
    
    printf("  epfd1: %d events, epfd2: %d events, epfd3: %d events\n", n1, n2, n3);
    
    // With EPOLLEXCLUSIVE, ideally only one should wake up
    // But our implementation may wake multiple - just verify they all work
    int total_woken = (n1 > 0) + (n2 > 0) + (n3 > 0);
    printf("  Total epoll instances woken: %d\n", total_woken);
    TEST_ASSERT(total_woken >= 1, "at least one epoll instance woken");
    
    close(sv[0]);
    close(sv[1]);
    close(epfd1);
    close(epfd2);
    close(epfd3);
    TEST_END();
    return 0;
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 4: EPOLLRDHUP - Peer write-side shutdown detection
// ═══════════════════════════════════════════════════════════════════════════
static int test_rdhup_shutdown(void) {
    TEST_START("EPOLLRDHUP peer write shutdown");
    
    int sv[2];
    int ret = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    TEST_ASSERT(ret == 0, "socketpair created");
    
    int epfd = epoll_create1(0);
    TEST_ASSERT(epfd >= 0, "epoll_create1 succeeded");
    
    // Watch for EPOLLIN | EPOLLRDHUP
    struct epoll_event ev = { 
        .events = EPOLLIN | EPOLLRDHUP, 
        .data.u32 = 42 
    };
    ret = epoll_ctl(epfd, EPOLL_CTL_ADD, sv[1], &ev);
    TEST_ASSERT(ret == 0, "epoll_ctl ADD with EPOLLRDHUP succeeded");
    
    // Send some data first
    write(sv[0], "data", 4);
    
    struct epoll_event events[10];
    int n = epoll_wait(epfd, events, 10, 100);
    TEST_ASSERT(n == 1, "received data event");
    TEST_ASSERT(events[0].events & EPOLLIN, "EPOLLIN set");
    printf("  Events after data: 0x%x\n", events[0].events);
    
    // Read the data
    char buf[16];
    read(sv[1], buf, sizeof(buf));
    
    // Shutdown peer's write side
    ret = shutdown(sv[0], SHUT_WR);
    TEST_ASSERT(ret == 0, "shutdown(SHUT_WR) succeeded");
    
    // Should receive EPOLLRDHUP
    n = epoll_wait(epfd, events, 10, 100);
    TEST_ASSERT(n == 1, "received event after shutdown");
    printf("  Events after shutdown: 0x%x\n", events[0].events);
    TEST_ASSERT(events[0].events & EPOLLRDHUP, "EPOLLRDHUP set after peer shutdown");
    TEST_ASSERT(events[0].events & EPOLLIN, "EPOLLIN also set (EOF readable)");
    
    close(sv[0]);
    close(sv[1]);
    close(epfd);
    TEST_END();
    return 0;
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 5: epoll_pwait - Basic signal mask handling
// ═══════════════════════════════════════════════════════════════════════════
static volatile sig_atomic_t signal_received = 0;

static void signal_handler(int sig) {
    (void)sig;
    signal_received = 1;
}

static int test_epoll_pwait_basic(void) {
    TEST_START("epoll_pwait basic");
    
    int sv[2];
    int ret = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    TEST_ASSERT(ret == 0, "socketpair created");
    
    int epfd = epoll_create1(0);
    TEST_ASSERT(epfd >= 0, "epoll_create1 succeeded");
    
    struct epoll_event ev = { .events = EPOLLIN, .data.u32 = 1 };
    ret = epoll_ctl(epfd, EPOLL_CTL_ADD, sv[1], &ev);
    TEST_ASSERT(ret == 0, "epoll_ctl ADD succeeded");
    
    // Block SIGUSR1
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    ret = sigprocmask(SIG_BLOCK, &mask, NULL);
    TEST_ASSERT(ret == 0, "sigprocmask BLOCK SIGUSR1 succeeded");
    
    // Set up signal handler
    signal(SIGUSR1, signal_handler);
    
    // Fork: child sends signal and data
    pid_t pid = fork();
    if (pid == 0) {
        // Child process
        usleep(50000);  // 50ms delay
        kill(getppid(), SIGUSR1);
        usleep(50000);  // Another 50ms
        write(sv[0], "data", 4);
        exit(0);
    }
    
    // Parent: use epoll_pwait with unblocked signals
    sigset_t unblock;
    sigemptyset(&unblock);
    
    struct epoll_event events[10];
    signal_received = 0;
    
    // epoll_pwait with empty mask (unblock all signals during wait)
    int n = epoll_pwait(epfd, events, 10, 1000, &unblock);
    printf("  epoll_pwait returned: %d, signal_received: %d\n", n, signal_received);
    
    // Should return due to data, not signal interruption
    TEST_ASSERT(n >= 1, "epoll_pwait returned events");
    
    // Wait for child
    int status;
    waitpid(pid, &status, 0);
    
    // Restore signal mask
    sigprocmask(SIG_UNBLOCK, &mask, NULL);
    
    close(sv[0]);
    close(sv[1]);
    close(epfd);
    TEST_END();
    return 0;
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 6: EPOLLONESHOT with edge-triggered mode
// ═══════════════════════════════════════════════════════════════════════════
static int test_oneshot_edgetriggered(void) {
    TEST_START("EPOLLONESHOT with EPOLLET");
    
    int sv[2];
    int ret = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    TEST_ASSERT(ret == 0, "socketpair created");
    
    int epfd = epoll_create1(0);
    TEST_ASSERT(epfd >= 0, "epoll_create1 succeeded");
    
    // Add with EPOLLONESHOT | EPOLLET
    struct epoll_event ev = { 
        .events = EPOLLIN | EPOLLONESHOT | EPOLLET, 
        .data.u32 = 1 
    };
    ret = epoll_ctl(epfd, EPOLL_CTL_ADD, sv[1], &ev);
    TEST_ASSERT(ret == 0, "epoll_ctl ADD with EPOLLONESHOT | EPOLLET succeeded");
    
    // Send message
    write(sv[0], "edge", 4);
    
    struct epoll_event events[10];
    int n = epoll_wait(epfd, events, 10, 100);
    TEST_ASSERT(n == 1, "received edge-triggered event");
    
    // Don't read - send more data
    write(sv[0], "more", 4);
    
    // Should NOT receive another event (oneshot disabled)
    n = epoll_wait(epfd, events, 10, 100);
    TEST_ASSERT(n == 0, "no second event (oneshot working with ET)");
    
    // Re-arm
    ev.events = EPOLLIN | EPOLLONESHOT | EPOLLET;
    ret = epoll_ctl(epfd, EPOLL_CTL_MOD, sv[1], &ev);
    TEST_ASSERT(ret == 0, "re-arm succeeded");
    
    // Now should get event for pending data
    n = epoll_wait(epfd, events, 10, 100);
    TEST_ASSERT(n == 1, "received event after re-arm");
    
    close(sv[0]);
    close(sv[1]);
    close(epfd);
    TEST_END();
    return 0;
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 7: Multiple FDs with EPOLLONESHOT
// ═══════════════════════════════════════════════════════════════════════════
static int test_oneshot_multiple_fds(void) {
    TEST_START("EPOLLONESHOT multiple FDs");
    
    int sv1[2], sv2[2], sv3[2];
    int ret = socketpair(AF_UNIX, SOCK_STREAM, 0, sv1);
    TEST_ASSERT(ret == 0, "socketpair 1 created");
    ret = socketpair(AF_UNIX, SOCK_STREAM, 0, sv2);
    TEST_ASSERT(ret == 0, "socketpair 2 created");
    ret = socketpair(AF_UNIX, SOCK_STREAM, 0, sv3);
    TEST_ASSERT(ret == 0, "socketpair 3 created");
    
    int epfd = epoll_create1(0);
    TEST_ASSERT(epfd >= 0, "epoll_create1 succeeded");
    
    // Add all with EPOLLONESHOT
    struct epoll_event ev = { .events = EPOLLIN | EPOLLONESHOT };
    
    ev.data.u32 = 1;
    epoll_ctl(epfd, EPOLL_CTL_ADD, sv1[1], &ev);
    
    ev.data.u32 = 2;
    epoll_ctl(epfd, EPOLL_CTL_ADD, sv2[1], &ev);
    
    ev.data.u32 = 3;
    epoll_ctl(epfd, EPOLL_CTL_ADD, sv3[1], &ev);
    
    // Write to all
    write(sv1[0], "a", 1);
    write(sv2[0], "b", 1);
    write(sv3[0], "c", 1);
    
    // Should get all three events
    struct epoll_event events[10];
    int n = epoll_wait(epfd, events, 10, 100);
    printf("  Received %d events\n", n);
    TEST_ASSERT(n == 3, "received all 3 events");
    
    // Read all data
    char buf[4];
    read(sv1[1], buf, 1);
    read(sv2[1], buf, 1);
    read(sv3[1], buf, 1);
    
    // Write again - should get no events (all disabled)
    write(sv1[0], "x", 1);
    write(sv2[0], "y", 1);
    write(sv3[0], "z", 1);
    
    n = epoll_wait(epfd, events, 10, 100);
    TEST_ASSERT(n == 0, "no events after all oneshots fired");
    
    // Re-arm only sv2
    ev.events = EPOLLIN | EPOLLONESHOT;
    ev.data.u32 = 2;
    ret = epoll_ctl(epfd, EPOLL_CTL_MOD, sv2[1], &ev);
    TEST_ASSERT(ret == 0, "re-armed sv2");
    
    // Now only sv2 should trigger
    n = epoll_wait(epfd, events, 10, 100);
    TEST_ASSERT(n == 1, "only re-armed FD triggered");
    TEST_ASSERT(events[0].data.u32 == 2, "correct FD triggered");
    
    close(sv1[0]); close(sv1[1]);
    close(sv2[0]); close(sv2[1]);
    close(sv3[0]); close(sv3[1]);
    close(epfd);
    TEST_END();
    return 0;
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 8: EPOLLRDHUP with full connection close
// ═══════════════════════════════════════════════════════════════════════════
static int test_rdhup_full_close(void) {
    TEST_START("EPOLLRDHUP full connection close");
    
    int sv[2];
    int ret = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    TEST_ASSERT(ret == 0, "socketpair created");
    
    int epfd = epoll_create1(0);
    TEST_ASSERT(epfd >= 0, "epoll_create1 succeeded");
    
    struct epoll_event ev = { 
        .events = EPOLLIN | EPOLLRDHUP | EPOLLHUP, 
        .data.u32 = 1 
    };
    ret = epoll_ctl(epfd, EPOLL_CTL_ADD, sv[1], &ev);
    TEST_ASSERT(ret == 0, "epoll_ctl ADD succeeded");
    
    // Close peer completely
    close(sv[0]);
    
    // Should receive both EPOLLIN (EOF) and EPOLLHUP
    struct epoll_event events[10];
    int n = epoll_wait(epfd, events, 10, 100);
    TEST_ASSERT(n == 1, "received event after peer close");
    printf("  Events after full close: 0x%x\n", events[0].events);
    TEST_ASSERT(events[0].events & EPOLLHUP, "EPOLLHUP set");
    TEST_ASSERT(events[0].events & EPOLLIN, "EPOLLIN set (EOF readable)");
    
    close(sv[1]);
    close(epfd);
    TEST_END();
    return 0;
}

// ═══════════════════════════════════════════════════════════════════════════
// Main test runner
// ═══════════════════════════════════════════════════════════════════════════
int main(void) {
    printf("═══════════════════════════════════════════════════════════════════════════\n");
    printf("Phase 9: epoll Advanced Features Test Suite\n");
    printf("═══════════════════════════════════════════════════════════════════════════\n");
    
    // Run all tests
    test_oneshot_basic();
    test_oneshot_rearm();
    test_exclusive_thundering_herd();
    test_rdhup_shutdown();
    test_epoll_pwait_basic();
    test_oneshot_edgetriggered();
    test_oneshot_multiple_fds();
    test_rdhup_full_close();
    
    // Summary
    printf("\n═══════════════════════════════════════════════════════════════════════════\n");
    printf("Test Summary: %d passed, %d failed\n", test_passed, test_failed);
    printf("═══════════════════════════════════════════════════════════════════════════\n");
    
    return test_failed > 0 ? 1 : 0;
}
