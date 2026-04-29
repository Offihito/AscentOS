#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <sys/wait.h>

#define TEST_SOCK "/tmp/test_p5.sock"

void test_basic_io() {
    printf("--- Test A: Basic I/O (Ping-Pong) ---\n");
    unlink(TEST_SOCK);

    int srv_listener = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, TEST_SOCK);

    bind(srv_listener, (struct sockaddr*)&addr, sizeof(addr));
    listen(srv_listener, 5);

    pid_t pid = fork();
    if (pid == 0) {
        // Client
        usleep(100000);
        int clu = socket(AF_UNIX, SOCK_STREAM, 0);
        connect(clu, (struct sockaddr*)&addr, sizeof(addr));

        const char* msg = "Hello from Client!";
        printf("  [Client] Sending: %s\n", msg);
        send(clu, msg, strlen(msg), 0);

        char buf[128];
        int n = recv(clu, buf, sizeof(buf)-1, 0);
        if (n > 0) {
            buf[n] = '\0';
            printf("  [Client] Received: %s\n", buf);
        }
        close(clu);
        exit(0);
    }

    // Server
    int srv_conn = accept(srv_listener, NULL, NULL);
    char buf[128];
    int n = recv(srv_conn, buf, sizeof(buf)-1, 0);
    if (n > 0) {
        buf[n] = '\0';
        printf("  [Server] Received: %s\n", buf);
        
        const char* reply = "Hello from Server!";
        printf("  [Server] Sending: %s\n", reply);
        send(srv_conn, reply, strlen(reply), 0);
    }

    waitpid(pid, NULL, 0);
    close(srv_conn);
    close(srv_listener);
    printf("Test A Finished\n\n");
}

void test_large_transfer() {
    printf("--- Test B: Large Data Transfer (64KB+) ---\n");
    unlink(TEST_SOCK);

    int srv_listener = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, TEST_SOCK);
    if (bind(srv_listener, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind"); exit(1);
    }
    listen(srv_listener, 1);

    pid_t pid = fork();
    if (pid == 0) {
        // Client
        usleep(100000);
        int clu = socket(AF_UNIX, SOCK_STREAM, 0);
        connect(clu, (struct sockaddr*)&addr, sizeof(addr));

        size_t size = 128 * 1024; // 128KB (Buffer is 64KB)
        char* data = malloc(size);
        for(size_t i=0; i<size; i++) data[i] = (char)(i % 256);

        printf("  [Client] Sending 128KB of data (should block halfway)...\n");
        ssize_t total = 0;
        while (total < (ssize_t)size) {
            ssize_t n = send(clu, data + total, size - total, 0);
            if (n < 0) { perror("send"); break; }
            total += n;
            // printf("  [Client] Sent %ld/%ld\n", total, (ssize_t)size);
        }
        printf("  [Client] Finished sending %ld bytes\n", total);
        free(data);
        close(clu);
        exit(0);
    }

    // Server
    int srv_conn = accept(srv_listener, NULL, NULL);
    size_t size = 128 * 1024;
    char* buf = malloc(size);
    ssize_t total = 0;

    printf("  [Server] Receiving 128KB of data...\n");
    while (total < (ssize_t)size) {
        ssize_t n = recv(srv_conn, buf + total, size - total, 0);
        if (n <= 0) break;
        total += n;
    }
    printf("  [Server] Received %ld bytes total\n", total);

    // Verify
    int errors = 0;
    for(size_t i=0; i<size; i++) {
        if (buf[i] != (char)(i % 256)) {
            errors++;
            if (errors < 5) printf("  [ERROR] Data mismatch at %ld: expected %d, got %d\n", i, (int)(i%256), (int)buf[i]);
        }
    }

    if (errors == 0) printf("  [PASS] Data integrity verified\n");
    else printf("  [FAIL] %d mismatches found\n", errors);

    waitpid(pid, NULL, 0);
    close(srv_conn);
    close(srv_listener);
    free(buf);
    printf("Test B Finished\n\n");
}

int main() {
    printf("===== AF_UNIX Socket I/O Test (Phase 5) =====\n\n");
    test_basic_io();
    test_large_transfer();
    printf("===== All Phase 5 Tests Completed =====\n");
    return 0;
}
