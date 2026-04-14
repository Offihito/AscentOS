#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <sys/wait.h>

int main() {
    int pipefd[2];

    printf("=== Pipe + Fork Test ===\n\n");

    if (pipe(pipefd) != 0) {
        printf("pipe() failed!\n");
        return 1;
    }
    printf("Pipe created: read_fd=%d, write_fd=%d\n", pipefd[0], pipefd[1]);

    pid_t pid = fork();

    if (pid < 0) {
        printf("fork() failed!\n");
        return 1;
    }

    if (pid == 0) {
        // ── Child process ──
        close(pipefd[0]); // Close read end

        const char *msg = "Hello from child!";
        printf("[Child PID %d] Writing to pipe: \"%s\"\n", getpid(), msg);
        write(pipefd[1], msg, strlen(msg) + 1);

        close(pipefd[1]);
        _exit(0);
    } else {
        // ── Parent process ──
        close(pipefd[1]); // Close write end

        // Wait for child to finish writing before reading
        int status;
        waitpid(pid, &status, 0);
        printf("[Parent] Child exited with status %d\n", (status >> 8) & 0xFF);

        char buf[64] = {0};
        int n = read(pipefd[0], buf, sizeof(buf));
        printf("[Parent PID %d] Read %d bytes from pipe: \"%s\"\n", getpid(), n, buf);

        close(pipefd[0]);
    }

    printf("\n=== Test Complete ===\n");
    return 0;
}
