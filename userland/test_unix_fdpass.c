#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <fcntl.h>

#define TEST_FILE "/tmp/test_fdpass_data"
#define SOCKET_PATH "/tmp/test_fdpass_sock"

void server() {
    int server_fd, client_fd;
    struct sockaddr_un addr;
    char buf[128];
    struct msghdr msg = {0};
    struct iovec iov[1];
    char control[128]; // Use explicit size for simpler userland test
    struct cmsghdr *cmsg;

    unlink(SOCKET_PATH);

    server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        exit(1);
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path)-1);

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        exit(1);
    }

    listen(server_fd, 5);
    printf("[SERVER] Waiting for connection...\n");

    client_fd = accept(server_fd, NULL, NULL);
    if (client_fd < 0) {
        perror("accept");
        exit(1);
    }
    printf("[SERVER] Accepted connection!\n");

    // Receive message with FD
    iov[0].iov_base = buf;
    iov[0].iov_len = sizeof(buf);
    msg.msg_iov = iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);

    ssize_t n = recvmsg(client_fd, &msg, 0);
    if (n < 0) {
        perror("recvmsg");
        exit(1);
    }
    buf[n] = '\0';
    printf("[SERVER] Received message: %s\n", buf);

    cmsg = (struct cmsghdr *)control;
    if (msg.msg_controllen >= sizeof(struct cmsghdr) && cmsg->cmsg_level == 1 && cmsg->cmsg_type == 1) {
        int passed_fd = *((int*)((char*)cmsg + sizeof(struct cmsghdr)));
        printf("[SERVER] Received FD: %d\n", passed_fd);

        // Try to read from the passed FD
        char file_buf[128];
        lseek(passed_fd, 0, SEEK_SET);
        ssize_t rn = read(passed_fd, file_buf, sizeof(file_buf)-1);
        if (rn >= 0) {
            file_buf[rn] = '\0';
            printf("[SERVER] Read from passed FD: %s\n", file_buf);
        } else {
            perror("read passed fd");
        }
        close(passed_fd);
    } else {
        printf("[SERVER] No FD received! (controllen=%d)\n", (int)msg.msg_controllen);
    }

    close(client_fd);
    close(server_fd);
}

void client() {
    int client_fd;
    struct sockaddr_un addr;
    struct msghdr msg = {0};
    struct iovec iov[1];
    char control[128];
    struct cmsghdr *cmsg;

    sleep(1); // Wait for server

    // Create a file to pass
    int file_fd = open(TEST_FILE, O_CREAT | O_RDWR | O_TRUNC, 0644);
    write(file_fd, "Passed FD Content!", 18);

    client_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path)-1);

    printf("[CLIENT] Connecting...\n");
    if (connect(client_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect");
        exit(1);
    }
    printf("[CLIENT] Connected!\n");

    // Send message with FD
    char *text = "Here is an FD!";
    iov[0].iov_base = text;
    iov[0].iov_len = strlen(text);
    msg.msg_iov = iov;
    msg.msg_iovlen = 1;

    memset(control, 0, sizeof(control));
    msg.msg_control = control;
    msg.msg_controllen = sizeof(struct cmsghdr) + sizeof(int);
    
    cmsg = (struct cmsghdr *)control;
    cmsg->cmsg_len = msg.msg_controllen;
    cmsg->cmsg_level = 1; // SOL_SOCKET
    cmsg->cmsg_type = 1;  // SCM_RIGHTS
    *((int*)((char*)cmsg + sizeof(struct cmsghdr))) = file_fd;

    printf("[CLIENT] Sending message with FD %d...\n", file_fd);
    if (sendmsg(client_fd, &msg, 0) < 0) {
        perror("sendmsg");
        exit(1);
    }

    printf("[CLIENT] Done.\n");
    close(file_fd);
    close(client_fd);
}

int main() {
    pid_t pid = fork();
    if (pid == 0) {
        client();
    } else {
        server();
        wait(NULL);
        printf("[TEST] AF_UNIX FD Passing SUCCESS!\n");
    }
    return 0;
}
