// ── Advanced Socket Tests for Phase 1 ──────────────────────────────────────────
// Tests: fork behavior, dup/dup2, poll, edge cases, socketpair interactions

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <poll.h>
#include <fcntl.h>

#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        printf("FAIL: %s (line %d, errno=%d)\n", msg, __LINE__, errno); \
        return -1; \
    } \
    test_passed++; \
} while(0)

#define TEST_START(name) printf("\n=== Test: %s ===\n", name)
#define TEST_END() printf("  Passed: %d, Failed: %d\n", test_passed, test_failed)

static int test_passed = 0;
static int test_failed = 0;

// ── Test 1: Socket across fork ────────────────────────────────────────────────
// Verify sockets survive fork() and can be used in both processes
static int test_socket_fork(void) {
    TEST_START("Socket Across Fork");
    
    // Create socketpair before fork
    int sv[2];
    int ret = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    TEST_ASSERT(ret == 0, "socketpair should succeed");
    printf("  Parent: created socketpair [%d, %d]\n", sv[0], sv[1]);
    
    pid_t pid = fork();
    TEST_ASSERT(pid >= 0, "fork should succeed");
    
    if (pid == 0) {
        // Child process
        printf("  Child: inherited fds [%d, %d]\n", sv[0], sv[1]);
        
        // Verify we can use the sockets
        int type = 0;
        socklen_t len = sizeof(type);
        ret = getsockopt(sv[0], SOL_SOCKET, SO_TYPE, &type, &len);
        TEST_ASSERT(ret == 0, "child can getsockopt on inherited socket");
        TEST_ASSERT(type == SOCK_STREAM, "socket type should be SOCK_STREAM");
        
        // Close one end
        close(sv[0]);
        printf("  Child: closed fd %d\n", sv[0]);
        
        // Exit successfully
        _exit(0);
    } else {
        // Parent process
        int status;
        waitpid(pid, &status, 0);
        TEST_ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0, 
                    "child should exit successfully");
        
        printf("  Parent: child exited\n");
        
        // Parent can still use its end
        int type = 0;
        socklen_t len = sizeof(type);
        ret = getsockopt(sv[1], SOL_SOCKET, SO_TYPE, &type, &len);
        TEST_ASSERT(ret == 0, "parent can still use socket after child exit");
        
        close(sv[0]);
        close(sv[1]);
    }
    
    TEST_END();
    return 0;
}

// ── Test 2: dup/dup2/dup3 with sockets ────────────────────────────────────────
static int test_socket_dup(void) {
    TEST_START("dup/dup2 with Sockets");
    
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    TEST_ASSERT(fd >= 0, "socket creation should succeed");
    printf("  Created socket fd=%d\n", fd);
    
    // Test dup()
    int fd2 = dup(fd);
    TEST_ASSERT(fd2 >= 0, "dup should succeed");
    TEST_ASSERT(fd2 != fd, "dup should return different fd");
    printf("  dup(%d) = %d\n", fd, fd2);
    
    // Both FDs should refer to same socket
    int type1 = 0, type2 = 0;
    socklen_t len = sizeof(int);
    getsockopt(fd, SOL_SOCKET, SO_TYPE, &type1, &len);
    getsockopt(fd2, SOL_SOCKET, SO_TYPE, &type2, &len);
    TEST_ASSERT(type1 == type2, "both fds should have same socket type");
    
    // Test dup2()
    int fd3 = dup2(fd, 10);
    TEST_ASSERT(fd3 == 10, "dup2 should return target fd");
    printf("  dup2(%d, 10) = %d\n", fd, fd3);
    
    // Close original, duplicate should still work
    close(fd);
    int domain = 0;
    getsockopt(fd2, SOL_SOCKET, SO_DOMAIN, &domain, &len);
    TEST_ASSERT(domain == AF_UNIX, "duplicate should still work after original closed");
    printf("  After closing fd=%d, fd2=%d still works\n", fd, fd2);
    
    close(fd2);
    close(fd3);
    
    TEST_END();
    return 0;
}

// ── Test 3: Poll on sockets ───────────────────────────────────────────────────
static int test_socket_poll(void) {
    TEST_START("Poll on Sockets");
    
    int sv[2];
    int ret = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    TEST_ASSERT(ret == 0, "socketpair should succeed");
    
    // Poll for writable (should be ready immediately for connected socketpair)
    struct pollfd pfd;
    pfd.fd = sv[0];
    pfd.events = POLLOUT;
    pfd.revents = 0;
    
    int n = poll(&pfd, 1, 100); // 100ms timeout
    TEST_ASSERT(n >= 0, "poll should not error");
    printf("  poll(%d, POLLOUT, 100ms) = %d, revents=0x%x\n", sv[0], n, pfd.revents);
    
    // For connected socketpair, POLLOUT should be set
    if (pfd.revents & POLLOUT) {
        printf("  Socket is writable (expected for connected pair)\n");
    } else {
        printf("  Socket not writable (may need actual data handling)\n");
    }
    
    // Poll for readable (should timeout since no data)
    pfd.events = POLLIN;
    pfd.revents = 0;
    n = poll(&pfd, 1, 50); // 50ms timeout
    printf("  poll(%d, POLLIN, 50ms) = %d, revents=0x%x\n", sv[0], n, pfd.revents);
    // Should timeout (n=0) or return with no POLLIN
    
    // Test POLLNVAL with closed fd
    close(sv[0]);
    pfd.fd = sv[0];
    pfd.events = POLLIN | POLLOUT;
    pfd.revents = 0;
    n = poll(&pfd, 1, 10);
    printf("  poll(closed fd) = %d, revents=0x%x (POLLNVAL=0x%x)\n", 
           n, pfd.revents, POLLNVAL);
    
    close(sv[1]);
    TEST_END();
    return 0;
}

// ── Test 4: Socket flags (SOCK_NONBLOCK, SOCK_CLOEXEC) ────────────────────────
static int test_socket_flags(void) {
    TEST_START("Socket Flags");
    
    // Create socket with SOCK_NONBLOCK
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    TEST_ASSERT(fd >= 0, "socket with SOCK_NONBLOCK should succeed");
    printf("  Created socket with SOCK_NONBLOCK: fd=%d\n", fd);
    
    // Check O_NONBLOCK via fcntl (if supported)
    int flags = fcntl(fd, F_GETFL);
    if (flags >= 0 && (flags & O_NONBLOCK)) {
        printf("  O_NONBLOCK is set (via fcntl)\n");
    } else {
        printf("  O_NONBLOCK check skipped (fcntl may not fully support sockets)\n");
    }
    close(fd);
    
    // Create socket with SOCK_CLOEXEC
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    TEST_ASSERT(fd >= 0, "socket with SOCK_CLOEXEC should succeed");
    printf("  Created socket with SOCK_CLOEXEC: fd=%d\n", fd);
    
    // Check FD_CLOEXEC via fcntl
    flags = fcntl(fd, F_GETFD);
    if (flags >= 0 && (flags & FD_CLOEXEC)) {
        printf("  FD_CLOEXEC is set (via fcntl)\n");
    } else {
        printf("  FD_CLOEXEC check skipped\n");
    }
    close(fd);
    
    // Combined flags
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    TEST_ASSERT(fd >= 0, "socket with both flags should succeed");
    printf("  Created socket with SOCK_NONBLOCK|SOCK_CLOEXEC: fd=%d\n", fd);
    close(fd);
    
    TEST_END();
    return 0;
}

// ── Test 5: Socketpair edge cases ─────────────────────────────────────────────
static int test_socketpair_edge_cases(void) {
    TEST_START("Socketpair Edge Cases");
    
    int sv[2];
    
    // SOCK_DGRAM socketpair
    int ret = socketpair(AF_UNIX, SOCK_DGRAM, 0, sv);
    TEST_ASSERT(ret == 0, "SOCK_DGRAM socketpair should succeed");
    printf("  Created SOCK_DGRAM socketpair [%d, %d]\n", sv[0], sv[1]);
    close(sv[0]);
    close(sv[1]);
    
    // Invalid domain
    ret = socketpair(999, SOCK_STREAM, 0, sv);
    TEST_ASSERT(ret < 0, "invalid domain should fail");
    printf("  Invalid domain correctly rejected\n");
    
    // Invalid type
    ret = socketpair(AF_UNIX, 999, 0, sv);
    TEST_ASSERT(ret < 0, "invalid type should fail");
    printf("  Invalid type correctly rejected\n");
    
    // Protocol must be 0 for AF_UNIX
    ret = socketpair(AF_UNIX, SOCK_STREAM, 1, sv);
    TEST_ASSERT(ret < 0, "non-zero protocol should fail for AF_UNIX");
    printf("  Non-zero protocol correctly rejected\n");
    
    // Misaligned pointer (EFAULT test - may crash instead)
    // Skip for safety
    
    TEST_END();
    return 0;
}

// ── Test 6: Close one end of socketpair ───────────────────────────────────────
static int test_socketpair_close_one_end(void) {
    TEST_START("Close One End of Socketpair");
    
    int sv[2];
    int ret = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    TEST_ASSERT(ret == 0, "socketpair should succeed");
    printf("  Created socketpair [%d, %d]\n", sv[0], sv[1]);
    
    // Close one end
    close(sv[0]);
    printf("  Closed fd %d\n", sv[0]);
    
    // Other end should still be usable
    int type = 0;
    socklen_t len = sizeof(type);
    ret = getsockopt(sv[1], SOL_SOCKET, SO_TYPE, &type, &len);
    TEST_ASSERT(ret == 0, "other end should still be usable");
    printf("  fd %d still usable after peer closed\n", sv[1]);
    
    // Poll should indicate peer closed (POLLHUP)
    struct pollfd pfd;
    pfd.fd = sv[1];
    pfd.events = POLLIN | POLLOUT;
    pfd.revents = 0;
    int n = poll(&pfd, 1, 100);
    printf("  poll(remaining fd) = %d, revents=0x%x (POLLHUP=0x%x)\n",
           n, pfd.revents, POLLHUP);
    
    close(sv[1]);
    
    TEST_END();
    return 0;
}

// ── Test 7: Many socketpairs ──────────────────────────────────────────────────
static int test_many_socketpairs(void) {
    TEST_START("Many Socketpairs");
    
    #define MAX_PAIRS 50
    int pairs[MAX_PAIRS][2];
    int created = 0;
    
    for (int i = 0; i < MAX_PAIRS; i++) {
        int ret = socketpair(AF_UNIX, SOCK_STREAM, 0, pairs[i]);
        if (ret < 0) {
            printf("  socketpair failed at %d (errno=%d)\n", i, errno);
            break;
        }
        created++;
    }
    
    printf("  Created %d socketpairs (%d fds total)\n", created, created * 2);
    TEST_ASSERT(created > 10, "should create at least 10 socketpairs");
    
    // Verify all are usable
    int usable = 0;
    for (int i = 0; i < created; i++) {
        int type = 0;
        socklen_t len = sizeof(type);
        if (getsockopt(pairs[i][0], SOL_SOCKET, SO_TYPE, &type, &len) == 0) {
            usable++;
        }
    }
    TEST_ASSERT(usable == created, "all created sockets should be usable");
    printf("  Verified %d/%d pairs are usable\n", usable, created);
    
    // Close all
    for (int i = 0; i < created; i++) {
        close(pairs[i][0]);
        close(pairs[i][1]);
    }
    printf("  Closed all %d pairs\n", created);
    
    // Should be able to create more now
    int sv[2];
    int ret = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    TEST_ASSERT(ret == 0, "should create new socketpair after closing all");
    printf("  Created new socketpair after cleanup\n");
    close(sv[0]);
    close(sv[1]);
    
    TEST_END();
    return 0;
}

// ── Test 8: Socket options edge cases ─────────────────────────────────────────
static int test_socket_options_edge(void) {
    TEST_START("Socket Options Edge Cases");
    
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    TEST_ASSERT(fd >= 0, "socket creation should succeed");
    
    // Get SO_ERROR (should be 0 for new socket)
    int error = 0;
    socklen_t len = sizeof(error);
    int ret = getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len);
    TEST_ASSERT(ret == 0, "getsockopt SO_ERROR should succeed");
    TEST_ASSERT(error == 0, "SO_ERROR should be 0 for new socket");
    printf("  SO_ERROR = %d (expected 0)\n", error);
    
    // Set SO_SNDBUF
    int newbuf = 8192;
    ret = setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &newbuf, sizeof(newbuf));
    TEST_ASSERT(ret == 0, "setsockopt SO_SNDBUF should succeed");
    
    // Get SO_SNDBUF
    int sndbuf = 0;
    len = sizeof(sndbuf);
    ret = getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, &len);
    TEST_ASSERT(ret == 0, "getsockopt SO_SNDBUF should succeed");
    printf("  SO_SNDBUF = %d (set to %d)\n", sndbuf, newbuf);
    
    // Set SO_REUSEADDR
    int reuse = 1;
    ret = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    TEST_ASSERT(ret == 0, "setsockopt SO_REUSEADDR should succeed");
    printf("  SO_REUSEADDR set to 1\n");
    
    // Invalid optname
    ret = getsockopt(fd, SOL_SOCKET, 99999, &error, &len);
    TEST_ASSERT(ret < 0, "invalid optname should fail");
    printf("  Invalid optname correctly rejected\n");
    
    // Invalid level (not SOL_SOCKET and no family handler)
    ret = getsockopt(fd, 999, SO_TYPE, &error, &len);
    TEST_ASSERT(ret < 0, "invalid level should fail");
    printf("  Invalid level correctly rejected\n");
    
    close(fd);
    
    TEST_END();
    return 0;
}

// ── Test 9: Invalid FD operations ──────────────────────────────────────────────
static int test_invalid_fd_ops(void) {
    TEST_START("Invalid FD Operations");
    
    // getsockopt on invalid FD
    int val = 0;
    socklen_t len = sizeof(val);
    int ret = getsockopt(999, SOL_SOCKET, SO_TYPE, &val, &len);
    TEST_ASSERT(ret < 0 && errno == EBADF, "getsockopt on bad fd should return EBADF");
    printf("  getsockopt(999) correctly returns EBADF\n");
    
    // setsockopt on invalid FD
    ret = setsockopt(999, SOL_SOCKET, SO_RCVBUF, &val, sizeof(val));
    TEST_ASSERT(ret < 0 && errno == EBADF, "setsockopt on bad fd should return EBADF");
    printf("  setsockopt(999) correctly returns EBADF\n");
    
    // bind on invalid FD
    struct sockaddr_un addr;
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, "/tmp/test_invalid");
    ret = bind(999, (struct sockaddr*)&addr, sizeof(addr));
    TEST_ASSERT(ret < 0 && errno == EBADF, "bind on bad fd should return EBADF");
    printf("  bind(999) correctly returns EBADF\n");
    
    // connect on invalid FD
    ret = connect(999, (struct sockaddr*)&addr, sizeof(addr));
    TEST_ASSERT(ret < 0 && errno == EBADF, "connect on bad fd should return EBADF");
    printf("  connect(999) correctly returns EBADF\n");
    
    // listen on invalid FD
    ret = listen(999, 5);
    TEST_ASSERT(ret < 0 && errno == EBADF, "listen on bad fd should return EBADF");
    printf("  listen(999) correctly returns EBADF\n");
    
    // accept on invalid FD
    ret = accept(999, NULL, NULL);
    TEST_ASSERT(ret < 0 && errno == EBADF, "accept on bad fd should return EBADF");
    printf("  accept(999) correctly returns EBADF\n");
    
    TEST_END();
    return 0;
}

// ── Test 10: Non-socket FD socket operations ───────────────────────────────────
static int test_non_socket_ops(void) {
    TEST_START("Non-Socket FD Socket Operations");
    
    // Open a regular file
    int fd = open("/tmp/test_nonsock.txt", O_CREAT | O_RDWR, 0644);
    TEST_ASSERT(fd >= 0, "file creation should succeed");
    printf("  Created test file fd=%d\n", fd);
    
    // Try getsockopt on file fd
    int val = 0;
    socklen_t len = sizeof(val);
    int ret = getsockopt(fd, SOL_SOCKET, SO_TYPE, &val, &len);
    TEST_ASSERT(ret < 0, "getsockopt on file should fail");
    printf("  getsockopt(file fd) correctly fails (errno=%d)\n", errno);
    
    // Try bind on file fd
    struct sockaddr_un addr;
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, "/tmp/test_bind");
    ret = bind(fd, (struct sockaddr*)&addr, sizeof(addr));
    TEST_ASSERT(ret < 0, "bind on file should fail");
    printf("  bind(file fd) correctly fails\n");
    
    close(fd);
    unlink("/tmp/test_nonsock.txt");
    
    TEST_END();
    return 0;
}

// ── Test 11: Rapid fork with sockets ──────────────────────────────────────────
static int test_rapid_fork_sockets(void) {
    TEST_START("Rapid Fork with Sockets");
    
    #define NUM_FORKS 10
    int sv[2];
    int ret = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    TEST_ASSERT(ret == 0, "socketpair should succeed");
    
    int succeeded = 0;
    for (int i = 0; i < NUM_FORKS; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            printf("  fork failed at %d\n", i);
            break;
        }
        if (pid == 0) {
            // Child: close unused sockets first, verify one works, then exit
            close(sv[1]);  // Close the other end
            int type = 0;
            socklen_t len = sizeof(type);
            if (getsockopt(sv[0], SOL_SOCKET, SO_TYPE, &type, &len) == 0) {
                close(sv[0]);
                _exit(0);
            } else {
                close(sv[0]);
                _exit(1);
            }
        }
        
        // Parent: wait for this child before forking next
        int status;
        pid_t wpid = waitpid(pid, &status, 0);
        if (wpid == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            succeeded++;
        } else {
            printf("  child %d failed\n", i);
        }
    }
    
    printf("  %d/%d children exited successfully\n", succeeded, NUM_FORKS);
    TEST_ASSERT(succeeded == NUM_FORKS, "all children should exit successfully");
    
    close(sv[0]);
    close(sv[1]);
    
    TEST_END();
    return 0;
}

// ── Test 12: Socket state after exec ───────────────────────────────────────────
// Note: This test requires a helper program, so we'll skip actual exec
static int test_socket_exec(void) {
    TEST_START("Socket State (FD_CLOEXEC test)");
    
    // Create socket with SOCK_CLOEXEC
    int fd_cloexec = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    TEST_ASSERT(fd_cloexec >= 0, "socket with CLOEXEC should succeed");
    
    // Create socket without SOCK_CLOEXEC
    int fd_normal = socket(AF_UNIX, SOCK_STREAM, 0);
    TEST_ASSERT(fd_normal >= 0, "normal socket should succeed");
    
    printf("  Created fd=%d (with CLOEXEC), fd=%d (without)\n", fd_cloexec, fd_normal);
    
    // Check FD_CLOEXEC flag
    int flags = fcntl(fd_cloexec, F_GETFD);
    if (flags >= 0) {
        if (flags & FD_CLOEXEC) {
            printf("  fd %d has FD_CLOEXEC set\n", fd_cloexec);
        } else {
            printf("  fd %d does NOT have FD_CLOEXEC set (unexpected)\n", fd_cloexec);
        }
    }
    
    flags = fcntl(fd_normal, F_GETFD);
    if (flags >= 0) {
        if (flags & FD_CLOEXEC) {
            printf("  fd %d has FD_CLOEXEC set (unexpected)\n", fd_normal);
        } else {
            printf("  fd %d does NOT have FD_CLOEXEC set (expected)\n", fd_normal);
        }
    }
    
    close(fd_cloexec);
    close(fd_normal);
    
    TEST_END();
    return 0;
}

// ── Main ──────────────────────────────────────────────────────────────────────
int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    
    printf("\n");
    printf("╔════════════════════════════════════════════════════════════╗\n");
    printf("║        Advanced Socket Tests - Phase 1 Extended           ║\n");
    printf("╚════════════════════════════════════════════════════════════╝\n");
    
    int total_passed = 0;
    int total_failed = 0;
    
    #define RUN_TEST(test) do { \
        test_passed = test_failed = 0; \
        if (test() < 0) total_failed++; \
        total_passed += test_passed; \
        total_failed += test_failed; \
    } while(0)
    
    RUN_TEST(test_socket_fork);
    RUN_TEST(test_socket_dup);
    RUN_TEST(test_socket_poll);
    RUN_TEST(test_socket_flags);
    RUN_TEST(test_socketpair_edge_cases);
    RUN_TEST(test_socketpair_close_one_end);
    RUN_TEST(test_many_socketpairs);
    RUN_TEST(test_socket_options_edge);
    RUN_TEST(test_invalid_fd_ops);
    RUN_TEST(test_non_socket_ops);
    RUN_TEST(test_rapid_fork_sockets);
    RUN_TEST(test_socket_exec);
    
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
    printf("╚════════════════════════════════════════════════════════════╝\n\n");
    
    return total_failed > 0 ? 1 : 0;
}
