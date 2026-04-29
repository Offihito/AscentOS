#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <sys/wait.h>
#include <fcntl.h>

#define TEST_SOCK_1 "/tmp/stress_p4_1.sock"
#define TEST_SOCK_2 "/tmp/stress_p4_2.sock"

void cleanup() {
    unlink(TEST_SOCK_1);
    unlink(TEST_SOCK_2);
}

// Helper to create a listening socket
int create_server(const char* path, int backlog) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path)-1);

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(fd); return -1;
    }
    if (listen(fd, backlog) < 0) {
        perror("listen"); close(fd); return -1;
    }
    return fd;
}

// ── Test A: Backlog Exhaustion ───────────────────────────────────────
// Fill the backlog and verify subsequent connections are refused.
void test_backlog_exhaustion() {
    printf("--- Test A: Backlog Exhaustion ---\n");
    cleanup();

    int backlog = 3;
    int srv = create_server(TEST_SOCK_1, backlog);
    if (srv < 0) exit(1);

    int clients[backlog + 2];
    int count = 0;

    printf("Filling backlog of %d (using non-blocking connects)...\n", backlog);
    for (int i = 0; i < backlog; i++) {
        clients[i] = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strcpy(addr.sun_path, TEST_SOCK_1);

        int res = connect(clients[i], (struct sockaddr*)&addr, sizeof(addr));
        if (res == 0 || (res < 0 && errno == EINPROGRESS)) {
            printf("  [OK] Client %d queued in backlog\n", i);
            count++;
        } else {
            perror("  [FAIL] Unexpected connect failure");
        }
    }

    // This one should be refused
    printf("Testing overflow client...\n");
    int extra = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, TEST_SOCK_1);

    if (connect(extra, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        if (errno == ECONNREFUSED) {
            printf("  [PASS] Overflow client refused as expected (ECONNREFUSED)\n");
        } else {
            printf("  [FAIL] Overflow client failed with wrong error: %d\n", errno);
        }
    } else {
        printf("  [FAIL] Overflow client connected despite full backlog!\n");
    }

    // Accept one and see if we can connect again
    int acc = accept(srv, NULL, NULL);
    if (acc >= 0) {
        printf("  [OK] Accepted one client, backlog should have space now\n");
        // Use non-blocking for this too to avoid single-process connect/accept deadlock
        int extra_nb = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (connect(extra_nb, (struct sockaddr*)&addr, sizeof(addr)) == 0 || errno == EINPROGRESS) {
            printf("  [PASS] New client connected/queued after accept\n");
            close(extra_nb);
        } else {
            perror("  [FAIL] New client could not connect after accept");
        }
        close(acc);
    } else {
        perror("  [FAIL] Accept failed");
    }

    close(extra); // Close the one that was refused
    for (int i = 0; i < count; i++) close(clients[i]);
    close(srv);
    printf("Test A Finished\n\n");
}

// ── Test B: Non-blocking Accept & Connect ───────────────────────────
void test_nonblocking_ops() {
    printf("--- Test B: Non-blocking Ops ---\n");
    cleanup();

    int srv = create_server(TEST_SOCK_1, 5);
    // Set server non-blocking
    int flags = fcntl(srv, F_GETFL, 0);
    fcntl(srv, F_SETFL, flags | O_NONBLOCK);

    printf("Testing non-blocking accept on empty queue...\n");
    int acc = accept(srv, NULL, NULL);
    if (acc < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        printf("  [PASS] Accept returned EAGAIN/EWOULDBLOCK\n");
    } else {
        printf("  [FAIL] Accept did not return EAGAIN (ret=%d, err=%d)\n", acc, errno);
    }

    printf("Testing non-blocking connect (immediate)...\n");
    int clu = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, TEST_SOCK_1);

    int res = connect(clu, (struct sockaddr*)&addr, sizeof(addr));
    // Since it's AF_UNIX and the server is local, some kernels connect immediately.
    // Ours currently returns EINPROGRESS if SOCK_NONBLOCK is set.
    if (res < 0) {
        if (errno == EINPROGRESS) {
            printf("  [OK] Connect returned EINPROGRESS\n");
            // Since we don't have poll implementation for 'writable' on connect yet in some kernels,
            // we check state by calling connect again or accept.
        } else {
            perror("  [FAIL] Connect failed");
        }
    } else {
        printf("  [OK] Connect finished immediately\n");
    }

    acc = accept(srv, NULL, NULL);
    if (acc >= 0) {
        printf("  [PASS] Server accepted non-blocking client\n");
        close(acc);
    } else {
        perror("  [FAIL] Server could not accept client");
    }

    close(clu);
    close(srv);
    printf("Test B Finished\n\n");
}

// ── Test C: Multi-Server Chaos ──────────────────────────────────────
#define NUM_CLIENTS_PER_SRV 15
void test_multiserver_chaos() {
    printf("--- Test C: Multi-Server Chaos (%d total clients) ---\n", NUM_CLIENTS_PER_SRV*2);
    cleanup();

    pid_t srv1_pid = fork();
    if (srv1_pid == 0) {
        int srv = create_server(TEST_SOCK_1, 32);
        for (int i = 0; i < NUM_CLIENTS_PER_SRV; i++) {
            int c = accept(srv, NULL, NULL);
            if (c >= 0) close(c);
        }
        exit(0);
    }

    pid_t srv2_pid = fork();
    if (srv2_pid == 0) {
        int srv = create_server(TEST_SOCK_2, 32);
        for (int i = 0; i < NUM_CLIENTS_PER_SRV; i++) {
            int c = accept(srv, NULL, NULL);
            if (c >= 0) close(c);
        }
        exit(0);
    }

    // Give servers a moment
    usleep(100000);

    printf("Forking clients for two servers simultaneously...\n");
    pid_t clients[NUM_CLIENTS_PER_SRV * 2];
    for (int i = 0; i < NUM_CLIENTS_PER_SRV * 2; i++) {
        clients[i] = fork();
        if (clients[i] == 0) {
            const char* target = (i % 2 == 0) ? TEST_SOCK_1 : TEST_SOCK_2;
            int sock = socket(AF_UNIX, SOCK_STREAM, 0);
            struct sockaddr_un addr;
            memset(&addr, 0, sizeof(addr));
            addr.sun_family = AF_UNIX;
            strcpy(addr.sun_path, target);
            if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
                exit(1);
            }
            close(sock);
            exit(0);
        }
    }

    int failures = 0;
    for (int i = 0; i < NUM_CLIENTS_PER_SRV * 2; i++) {
        int status;
        waitpid(clients[i], &status, 0);
        if (WEXITSTATUS(status) != 0) failures++;
    }

    waitpid(srv1_pid, NULL, 0);
    waitpid(srv2_pid, NULL, 0);

    if (failures == 0) {
        printf("  [PASS] All %d clients connected and were accepted across 2 servers\n", NUM_CLIENTS_PER_SRV*2);
    } else {
        printf("  [FAIL] %d clients failed connection\n", failures);
    }

    cleanup();
    printf("Test C Finished\n\n");
}

int main() {
    printf("===== Detailed Socket Stress Test (Phase 4) =====\n\n");

    test_backlog_exhaustion();
    test_nonblocking_ops();
    test_multiserver_chaos();

    printf("===== All Detailed Tests Completed =====\n");
    return 0;
}
