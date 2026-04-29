#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <sys/wait.h>

#define SOCKET_PATH "/tmp/phase4_test.sock"

// ── Test 1: Sequential connect/accept ─────────────────────────────────
// Server forks, child is server (accept loop), parent forks clients.
// This avoids the "all forks then all accepts" pattern which is fragile.
#define NUM_CLIENTS 20

void test_concurrent_connections(void) {
    printf("=== Test 1: Concurrent connections (%d clients) ===\n", NUM_CLIENTS);

    unlink(SOCKET_PATH);

    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); exit(1); }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind"); exit(1);
    }
    if (listen(server_fd, 64) < 0) {
        perror("listen"); exit(1);
    }

    printf("Server listening on %s\n", SOCKET_PATH);

    // Fork the server process — it will accept connections
    pid_t server_pid = fork();
    if (server_pid == 0) {
        // ── Server child: accept NUM_CLIENTS connections ──
        int accepted = 0;
        for (int i = 0; i < NUM_CLIENTS; i++) {
            int client_fd = accept(server_fd, NULL, NULL);
            if (client_fd >= 0) {
                accepted++;
                close(client_fd);
            } else {
                perror("accept");
            }
        }
        close(server_fd);
        printf("Server accepted %d/%d connections\n", accepted, NUM_CLIENTS);
        exit(accepted == NUM_CLIENTS ? 0 : 1);
    }

    // ── Parent: fork client processes ──
    // Close server fd in parent (server child owns it now)
    // Actually we keep it open so clients can connect to the bound address.

    pid_t clients[NUM_CLIENTS];
    for (int i = 0; i < NUM_CLIENTS; i++) {
        clients[i] = fork();
        if (clients[i] == 0) {
            // Client child
            close(server_fd); // No need for listen socket
            int sock = socket(AF_UNIX, SOCK_STREAM, 0);
            if (sock < 0) { perror("client socket"); exit(1); }

            if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
                perror("client connect");
                exit(1);
            }

            // Connected!
            close(sock);
            exit(0);
        }
    }

    // Wait for all client children
    int client_ok = 0;
    for (int i = 0; i < NUM_CLIENTS; i++) {
        int status = 0;
        waitpid(clients[i], &status, 0);
        if (status == 0) client_ok++;
    }

    // Wait for server child
    int server_status = 0;
    waitpid(server_pid, &server_status, 0);

    close(server_fd);
    unlink(SOCKET_PATH);

    printf("Clients OK: %d/%d, Server exit: %d\n",
           client_ok, NUM_CLIENTS, (server_status >> 8) & 0xFF);

    if (client_ok == NUM_CLIENTS && server_status == 0) {
        printf("Test 1 PASSED\n\n");
    } else {
        printf("Test 1 FAILED\n\n");
    }
}

// ── Test 2: Simple 1-on-1 connect/accept ──────────────────────────────
void test_simple_connect(void) {
    printf("=== Test 2: Simple connect/accept ===\n");

    unlink(SOCKET_PATH);

    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); exit(1); }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind"); exit(1);
    }
    if (listen(server_fd, 5) < 0) {
        perror("listen"); exit(1);
    }

    pid_t pid = fork();
    if (pid == 0) {
        // Client child
        close(server_fd);
        int sock = socket(AF_UNIX, SOCK_STREAM, 0);
        if (sock < 0) { perror("client socket"); exit(1); }

        if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            perror("connect");
            exit(1);
        }

        printf("Client connected!\n");
        close(sock);
        exit(0);
    }

    // Server: accept one connection
    int client_fd = accept(server_fd, NULL, NULL);
    if (client_fd >= 0) {
        printf("Server accepted connection!\n");
        close(client_fd);
    } else {
        perror("accept");
    }

    int status = 0;
    waitpid(pid, &status, 0);

    close(server_fd);
    unlink(SOCKET_PATH);

    if (client_fd >= 0 && status == 0) {
        printf("Test 2 PASSED\n\n");
    } else {
        printf("Test 2 FAILED\n\n");
    }
}

// ── Test 3: Multiple sequential connections ───────────────────────────
void test_sequential_connections(void) {
    printf("=== Test 3: Sequential connections ===\n");

    unlink(SOCKET_PATH);

    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); exit(1); }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind"); exit(1);
    }
    if (listen(server_fd, 5) < 0) {
        perror("listen"); exit(1);
    }

    int count = 5;
    int success = 0;

    for (int i = 0; i < count; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            close(server_fd);
            int sock = socket(AF_UNIX, SOCK_STREAM, 0);
            if (sock < 0) exit(1);
            if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) exit(1);
            close(sock);
            exit(0);
        }

        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd >= 0) {
            success++;
            close(client_fd);
        }

        int status = 0;
        waitpid(pid, &status, 0);
    }

    close(server_fd);
    unlink(SOCKET_PATH);

    printf("Sequential: %d/%d succeeded\n", success, count);
    if (success == count) {
        printf("Test 3 PASSED\n\n");
    } else {
        printf("Test 3 FAILED\n\n");
    }
}

int main(void) {
    printf("===== Phase 4: Connection-Oriented Socket Stress Test =====\n\n");

    test_simple_connect();
    test_sequential_connections();
    test_concurrent_connections();

    printf("===== All Phase 4 tests completed =====\n");
    return 0;
}
