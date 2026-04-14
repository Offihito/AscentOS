#include <stdio.h>
#include <unistd.h>
#include <sys/utsname.h>

int main() {
    printf("Testing uname...\n");
    struct utsname name;
    if (uname(&name) == 0) {
        printf("sysname: %s\n", name.sysname);
        printf("nodename: %s\n", name.nodename);
        printf("release: %s\n", name.release);
        printf("version: %s\n", name.version);
        printf("machine: %s\n", name.machine);
    } else {
        printf("uname failed\n");
    }

    printf("\nTesting pipe...\n");
    int pipefd[2];
    if (pipe(pipefd) == 0) {
        printf("pipe created successfully: read_fd=%d, write_fd=%d\n", pipefd[0], pipefd[1]);
        
        char msg[] = "Hello from pipe!";
        write(pipefd[1], msg, sizeof(msg));
        
        char buf[32] = {0};
        read(pipefd[0], buf, sizeof(buf));
        printf("Read from pipe: %s\n", buf);
        
        close(pipefd[0]);
        close(pipefd[1]);
    } else {
        printf("pipe failed\n");
    }

    return 0;
}
