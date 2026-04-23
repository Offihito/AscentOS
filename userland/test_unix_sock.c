#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <errno.h>

#define SOCKET_PATH "/tmp/test_sock"
#define TEST_MESSAGE "Hello from Unix Domain Socket!"

int main() {
    int server_fd, client_fd, accepted_fd;
    struct sockaddr_un addr;
    char buffer[128];
    pid_t pid;

    printf("[TEST] Starting AF_UNIX test...\n");

    // Create server socket
    server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket server");
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    // Bind
    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(server_fd);
        return 1;
    }

    // Listen
    if (listen(server_fd, 5) < 0) {
        perror("listen");
        close(server_fd);
        return 1;
    }

    printf("[SERVER] Listening on %s\n", SOCKET_PATH);

    // Fork a "client" process (AscentOS fork is basic, but let's see)
    // Actually, on AscentOS it might be easier to just do it sequentially or use another program.
    // Let's try fork.
    pid = fork();
    if (pid < 0) {
        perror("fork");
        close(server_fd);
        return 1;
    }

    if (pid == 0) {
        // Child: Client
        sleep(1); // Wait for server to be ready
        printf("[CLIENT] Connecting...\n");
        client_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (client_fd < 0) {
            perror("client socket");
            exit(1);
        }

        if (connect(client_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            perror("client connect");
            exit(1);
        }

        printf("[CLIENT] Connected! Sending message...\n");
        write(client_fd, TEST_MESSAGE, strlen(TEST_MESSAGE));
        close(client_fd);
        printf("[CLIENT] Done.\n");
        exit(0);
    } else {
        // Parent: Server
        printf("[SERVER] Waiting for accept...\n");
        accepted_fd = accept(server_fd, NULL, NULL);
        if (accepted_fd < 0) {
            perror("accept");
            close(server_fd);
            return 1;
        }

        printf("[SERVER] Accepted connection!\n");
        memset(buffer, 0, sizeof(buffer));
        int n = read(accepted_fd, buffer, sizeof(buffer) - 1);
        if (n > 0) {
            printf("[SERVER] Received: %s\n", buffer);
            if (strcmp(buffer, TEST_MESSAGE) == 0) {
                printf("[TEST] SUCCESS!\n");
            } else {
                printf("[TEST] FAILURE: Message mismatch\n");
            }
        } else {
            printf("[SERVER] Read failed or EOF\n");
        }

        close(accepted_fd);
        close(server_fd);
        
        // Wait for child
        int status;
        wait(&status);
    }

    return 0;
}
