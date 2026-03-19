#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    printf("Hello from ring3 AscentOS \n");
    printf("PID: %d\n", getpid());

    char* buf = malloc(64);
    if (!buf) {
        printf("Error: malloc failed\n");
        return 1;
    }
    strcpy(buf, "musl malloc working!");
    printf("malloc : %s\n", buf);
    free(buf);

    char msg[32];
    sprintf(msg, "pid=%d fmt ok", getpid());
    printf("sprintf: %s\n", msg);

    printf("--- All Tests Passed ---\n");
    return 0;
}