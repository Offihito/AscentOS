#include "../libc/libc.h"

int main(void) {
    puts("Fork testi basliyor...");

    pid_t child = fork();

    if (child == 0) {
        // Child
        printf("[child] PID=%d\n", getpid());
        puts("[child] Bitti.");
        exit(0);
    } else {
        // Parent
        printf("[parent] Child PID=%d, bekliyorum...\n", child);
        waitpid(child, 0, 0);
        puts("[parent] Child bitti.");
    }

    return 0;
}