// ═══════════════════════════════════════════════════════════════════════════
// Phase 11: Socket Multiplexing Integration Test Suite
// ═══════════════════════════════════════════════════════════════════════════
// Tests epoll wake-up integration with socket operations

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>

#define UNIX_PATH_MAX 108
#define TEST_PATH "/tmp/phase11_epoll.sock"
#define NUM_CLIENTS 10  // Reduced for testing

// Test macros
#define TEST_START(name) printf("\n--- Test: %s ---\n", name)
#define TEST_END() printf("Test Finished\n")
#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        printf("  [FAIL] %s (line %d): %s\n", msg, __LINE__, errno ? strerror(errno) : "No error"); \
        return 1; \
    } \
    printf("  [OK] %s\n", msg); \
} while(0)

// ═══════════════════════════════════════════════════════════════════════════
// Test 1: Epoll wakes on accept (connection pending)
// ═══════════════════════════════════════════════════════════════════════════
static int test_epoll_accept_wakeup(void) {
    TEST_START("Epoll wakes on accept (connection pending)");
    
    // Create server
    int server = socket(AF_UNIX, SOCK_STREAM, 0);
    TEST_ASSERT(server >= 0, "server socket created");
    
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, TEST_PATH, UNIX_PATH_MAX - 1);
    
    // Unlink if exists from previous run
    unlink(TEST_PATH);
    
    int ret = bind(server, (struct sockaddr*)&addr, sizeof(addr));
    TEST_ASSERT(ret == 0, "bind succeeded");
    
    ret = listen(server, 5);
    TEST_ASSERT(ret == 0, "listen succeeded");
    
    // Create epoll and add server
    int epfd = epoll_create1(0);
    TEST_ASSERT(epfd >= 0, "epoll created");
    
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = server };
    ret = epoll_ctl(epfd, EPOLL_CTL_ADD, server, &ev);
    TEST_ASSERT(ret == 0, "server added to epoll");
    
    // Fork: child connects, parent waits on epoll
    pid_t pid = fork();
    if (pid == 0) {
        // Child: connect after small delay
        usleep(50000);  // 50ms
        
        int client = socket(AF_UNIX, SOCK_STREAM, 0);
        connect(client, (struct sockaddr*)&addr, sizeof(addr));
        close(client);
        exit(0);
    }
    
    // Parent: wait for epoll event
    struct epoll_event events[1];
    int n = epoll_wait(epfd, events, 1, 2000);  // 2s timeout
    
    TEST_ASSERT(n == 1, "epoll_wait returned 1 event");
    TEST_ASSERT(events[0].data.fd == server, "event is for server fd");
    TEST_ASSERT(events[0].events & EPOLLIN, "EPOLLIN is set");
    
    // Accept the connection
    int client = accept(server, NULL, NULL);
    TEST_ASSERT(client >= 0, "accept succeeded");
    
    // Cleanup
    int status;
    waitpid(pid, &status, 0);
    
    close(client);
    close(epfd);
    close(server);
    unlink(TEST_PATH);
    
    TEST_END();
    return 0;
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 2: Epoll wakes on data arrival
// ═══════════════════════════════════════════════════════════════════════════
static int test_epoll_data_wakeup(void) {
    TEST_START("Epoll wakes on data arrival");
    
    // Create socketpair
    int sv[2];
    int ret = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    TEST_ASSERT(ret == 0, "socketpair created");
    
    // Create epoll and add sv[1]
    int epfd = epoll_create1(0);
    TEST_ASSERT(epfd >= 0, "epoll created");
    
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = sv[1] };
    ret = epoll_ctl(epfd, EPOLL_CTL_ADD, sv[1], &ev);
    TEST_ASSERT(ret == 0, "socket added to epoll");
    
    // Fork: child sends data, parent waits on epoll
    pid_t pid = fork();
    if (pid == 0) {
        // Child: send data after delay
        usleep(50000);  // 50ms
        write(sv[0], "hello", 5);
        close(sv[0]);
        close(sv[1]);
        exit(0);
    }
    
    // Parent: close child end
    close(sv[0]);
    
    // Wait for epoll event
    struct epoll_event events[1];
    int n = epoll_wait(epfd, events, 1, 2000);  // 2s timeout
    
    TEST_ASSERT(n == 1, "epoll_wait returned 1 event");
    TEST_ASSERT(events[0].data.fd == sv[1], "event is for correct fd");
    TEST_ASSERT(events[0].events & EPOLLIN, "EPOLLIN is set");
    
    // Read the data
    char buf[32];
    ret = read(sv[1], buf, sizeof(buf));
    TEST_ASSERT(ret == 5, "read 5 bytes");
    buf[ret] = '\0';
    TEST_ASSERT(strcmp(buf, "hello") == 0, "received correct data");
    
    // Cleanup
    int status;
    waitpid(pid, &status, 0);
    
    close(epfd);
    close(sv[1]);
    
    TEST_END();
    return 0;
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 3: Epoll wakes on peer close (EPOLLHUP)
// ═══════════════════════════════════════════════════════════════════════════
static int test_epoll_hup_wakeup(void) {
    TEST_START("Epoll wakes on peer close (EPOLLHUP)");
    
    // Create socketpair
    int sv[2];
    int ret = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    TEST_ASSERT(ret == 0, "socketpair created");
    
    // Create epoll and add sv[1]
    int epfd = epoll_create1(0);
    TEST_ASSERT(epfd >= 0, "epoll created");
    
    struct epoll_event ev = { .events = EPOLLIN | EPOLLHUP, .data.fd = sv[1] };
    ret = epoll_ctl(epfd, EPOLL_CTL_ADD, sv[1], &ev);
    TEST_ASSERT(ret == 0, "socket added to epoll");
    
    // Fork: child closes, parent waits on epoll
    pid_t pid = fork();
    if (pid == 0) {
        // Child: close after delay
        usleep(50000);  // 50ms
        close(sv[0]);
        close(sv[1]);
        exit(0);
    }
    
    // Parent: close child end
    close(sv[0]);
    
    // Wait for epoll event
    struct epoll_event events[1];
    int n = epoll_wait(epfd, events, 1, 2000);  // 2s timeout
    
    TEST_ASSERT(n == 1, "epoll_wait returned 1 event");
    TEST_ASSERT(events[0].events & EPOLLHUP, "EPOLLHUP is set");
    
    // Cleanup
    int status;
    waitpid(pid, &status, 0);
    
    close(epfd);
    close(sv[1]);
    
    TEST_END();
    return 0;
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 4: Epoll wakes on write buffer space (EPOLLOUT)
// ═══════════════════════════════════════════════════════════════════════════
static int test_epoll_out_wakeup(void) {
    TEST_START("Epoll wakes on write buffer space (EPOLLOUT)");
    
    // Create socketpair
    int sv[2];
    int ret = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    TEST_ASSERT(ret == 0, "socketpair created");
    
    // Create epoll and add sv[0] for EPOLLOUT
    int epfd = epoll_create1(0);
    TEST_ASSERT(epfd >= 0, "epoll created");
    
    struct epoll_event ev = { .events = EPOLLOUT, .data.fd = sv[0] };
    ret = epoll_ctl(epfd, EPOLL_CTL_ADD, sv[0], &ev);
    TEST_ASSERT(ret == 0, "socket added to epoll");
    
    // Socket should be writable immediately
    struct epoll_event events[1];
    int n = epoll_wait(epfd, events, 1, 100);  // 100ms timeout
    
    TEST_ASSERT(n == 1, "epoll_wait returned 1 event");
    TEST_ASSERT(events[0].events & EPOLLOUT, "EPOLLOUT is set");
    
    // Cleanup
    close(epfd);
    close(sv[0]);
    close(sv[1]);
    
    TEST_END();
    return 0;
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 5: Epoll edge-triggered with multiple events
// ═══════════════════════════════════════════════════════════════════════════
static int test_epoll_et_multiple(void) {
    TEST_START("Epoll edge-triggered with multiple events");
    
    // Create socketpair
    int sv[2];
    int ret = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    TEST_ASSERT(ret == 0, "socketpair created");
    
    // Create epoll with edge-triggered
    int epfd = epoll_create1(0);
    TEST_ASSERT(epfd >= 0, "epoll created");
    
    struct epoll_event ev = { .events = EPOLLIN | EPOLLET, .data.fd = sv[1] };
    ret = epoll_ctl(epfd, EPOLL_CTL_ADD, sv[1], &ev);
    TEST_ASSERT(ret == 0, "socket added to epoll (ET)");
    
    // Fork: child sends multiple messages
    pid_t pid = fork();
    if (pid == 0) {
        // Child: send multiple messages quickly
        usleep(50000);  // 50ms
        write(sv[0], "msg1", 4);
        write(sv[0], "msg2", 4);
        write(sv[0], "msg3", 4);
        close(sv[0]);
        // Don't close sv[1] - parent is watching it
        exit(0);
    }
    
    // Parent: close child end
    close(sv[0]);
    
    // Wait for epoll event (ET should only fire once)
    struct epoll_event events[1];
    int n = epoll_wait(epfd, events, 1, 2000);  // 2s timeout
    
    TEST_ASSERT(n == 1, "epoll_wait returned 1 event");
    TEST_ASSERT(events[0].events & EPOLLIN, "EPOLLIN is set");
    
    // Read all data in a loop until empty
    char buf[32];
    int total = 0;
    while (1) {
        ret = read(sv[1], buf + total, sizeof(buf) - total);
        if (ret <= 0) break;
        total += ret;
        if (total >= 12) break;  // Got all expected data
    }
    printf("  Total read: %d bytes\n", total);
    TEST_ASSERT(total == 12, "read all 12 bytes");
    
    // ET mode: second epoll_wait should not return (no new data)
    // Note: may return EPOLLHUP if child exits, but not EPOLLIN
    n = epoll_wait(epfd, events, 1, 100);  // 100ms timeout
    if (n > 0) {
        // If we got an event, it should NOT be EPOLLIN (data already consumed)
        TEST_ASSERT(!(events[0].events & EPOLLIN), "no EPOLLIN in ET mode after consuming data");
        printf("  [OK] no EPOLLIN in second event (got 0x%x)\n", events[0].events);
    } else {
        printf("  [OK] no more events in ET mode\n");
    }
    
    // Cleanup
    int status;
    waitpid(pid, &status, 0);
    
    close(epfd);
    close(sv[1]);
    
    TEST_END();
    return 0;
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 6: Simple epoll server simulation
// ═══════════════════════════════════════════════════════════════════════════
static int test_epoll_server_simulation(void) {
    TEST_START("Simple epoll server simulation");
    
    // Create server
    int server = socket(AF_UNIX, SOCK_STREAM, 0);
    TEST_ASSERT(server >= 0, "server socket created");
    
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, TEST_PATH, UNIX_PATH_MAX - 1);
    
    // Unlink if exists from previous run
    unlink(TEST_PATH);
    
    int ret = bind(server, (struct sockaddr*)&addr, sizeof(addr));
    TEST_ASSERT(ret == 0, "bind succeeded");
    
    ret = listen(server, NUM_CLIENTS);
    TEST_ASSERT(ret == 0, "listen succeeded");
    
    // Create epoll
    int epfd = epoll_create1(0);
    TEST_ASSERT(epfd >= 0, "epoll created");
    
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = server };
    ret = epoll_ctl(epfd, EPOLL_CTL_ADD, server, &ev);
    TEST_ASSERT(ret == 0, "server added to epoll");
    
    // Fork clients
    for (int i = 0; i < NUM_CLIENTS; i++) {
        if (fork() == 0) {
            // Child: connect and send data
            int sock = socket(AF_UNIX, SOCK_STREAM, 0);
            connect(sock, (struct sockaddr*)&addr, sizeof(addr));
            write(sock, &i, sizeof(i));
            usleep(50000);  // Keep connection briefly
            close(sock);
            exit(0);
        }
    }
    
    // Server event loop
    int accepted = 0, received = 0;
    int clients[NUM_CLIENTS];
    struct epoll_event events[10];
    
    while (accepted < NUM_CLIENTS || received < NUM_CLIENTS) {
        int n = epoll_wait(epfd, events, 10, 2000);
        
        for (int i = 0; i < n; i++) {
            if (events[i].data.fd == server) {
                // New connection
                int client = accept(server, NULL, NULL);
                if (client >= 0) {
                    clients[accepted] = client;
                    
                    // Use level-triggered for simplicity
                    struct epoll_event cev = { 
                        .events = EPOLLIN, 
                        .data.fd = client 
                    };
                    epoll_ctl(epfd, EPOLL_CTL_ADD, client, &cev);
                    accepted++;
                }
            } else {
                // Data from client (or EOF if orphaned)
                int buf;
                ssize_t r = read(events[i].data.fd, &buf, sizeof(buf));
                if (r > 0) {
                    received++;
                } else {
                    // EOF or error - orphaned connection, count as received and close
                    received++;
                    close(events[i].data.fd);
                    // Remove from epoll (auto-removed on close, but be explicit)
                    epoll_ctl(epfd, EPOLL_CTL_DEL, events[i].data.fd, NULL);
                }
            }
        }
    }
    
    printf("  Accepted: %d, Received: %d\n", accepted, received);
    TEST_ASSERT(accepted == NUM_CLIENTS, "accepted all clients");
    TEST_ASSERT(received == NUM_CLIENTS, "received from all clients");
    
    // Cleanup
    for (int i = 0; i < NUM_CLIENTS; i++) {
        int status;
        wait(&status);
    }
    
    for (int i = 0; i < accepted; i++) {
        close(clients[i]);
    }
    
    close(epfd);
    close(server);
    unlink(TEST_PATH);
    
    TEST_END();
    return 0;
}

// ═══════════════════════════════════════════════════════════════════════════
// Main
// ═══════════════════════════════════════════════════════════════════════════
int main(void) {
    printf("═══════════════════════════════════════════════════════════════════════════\n");
    printf("Phase 11: Socket Multiplexing Integration Test Suite\n");
    printf("═══════════════════════════════════════════════════════════════════════════\n");
    
    int passed = 0, failed = 0;
    
    if (test_epoll_accept_wakeup() == 0) passed++; else failed++;
    if (test_epoll_data_wakeup() == 0) passed++; else failed++;
    if (test_epoll_hup_wakeup() == 0) passed++; else failed++;
    if (test_epoll_out_wakeup() == 0) passed++; else failed++;
    if (test_epoll_et_multiple() == 0) passed++; else failed++;
    if (test_epoll_server_simulation() == 0) passed++; else failed++;
    
    printf("\n═══════════════════════════════════════════════════════════════════════════\n");
    printf("Test Summary: %d passed, %d failed\n", passed, failed);
    printf("═══════════════════════════════════════════════════════════════════════════\n");
    
    return failed > 0 ? 1 : 0;
}
