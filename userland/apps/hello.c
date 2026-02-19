#include "../libc/libc.h"  // veya sadece: #include "../libc/stdio.h"

int main(void) {
    printf("Hello from ring3 AscentOS :3\n");
    printf("PID: %d\n", getpid());
    return 0;
}