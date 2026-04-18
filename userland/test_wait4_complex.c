#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>

int main() {
    printf("[WAIT4_TEST] Starting complex wait4 test\n");

    printf("[WAIT4_TEST] Forking Child 1...\n");
    pid_t child1 = fork();
    if (child1 == 0) {
        printf("[WAIT4_TEST] Child 1 (PID %d) starting, will exit with status 10\n", getpid());
        // Simple delay loop since we don't know if sleep() is reliable
        for(volatile int i=0; i<10000000; i++);
        exit(10);
    }

    printf("[WAIT4_TEST] Forking Child 2...\n");
    pid_t child2 = fork();
    if (child2 == 0) {
        printf("[WAIT4_TEST] Child 2 (PID %d) starting, will exit with status 20\n", getpid());
        for(volatile int i=0; i<20000000; i++);
        exit(20);
    }

    // Test 1: WNOHANG
    int status;
    pid_t res = wait4(-1, &status, WNOHANG, NULL);
    if (res == 0) {
        printf("[WAIT4_TEST] Test 1 (WNOHANG) PASSED: No zombie found yet as expected\n");
    } else if (res > 0) {
        printf("[WAIT4_TEST] Test 1 (WNOHANG) NOTE: Child %d exited very quickly\n", res);
    } else {
        printf("[WAIT4_TEST] Test 1 (WNOHANG) FAILED: Unexpected return %d, errno %d\n", res, errno);
    }

    // Test 2: Wait for specific child (child 2)
    printf("[WAIT4_TEST] Test 2: Waiting for Child 2 (PID %d)...\n", child2);
    res = wait4(child2, &status, 0, NULL);
    if (res == child2 && WEXITSTATUS(status) == 20) {
        printf("[WAIT4_TEST] Test 2 PASSED: Reaped Child 2 with status 20\n");
    } else {
        printf("[WAIT4_TEST] Test 2 FAILED: Reaped %d with status %d (expected %d, status 20)\n", res, WEXITSTATUS(status), child2);
    }

    // Test 3: Wait for remaining child (child 1)
    printf("[WAIT4_TEST] Test 3: Waiting for remaining child...\n");
    res = wait4(-1, &status, 0, NULL);
    if (res == child1 && WEXITSTATUS(status) == 10) {
        printf("[WAIT4_TEST] Test 3 PASSED: Reaped Child 1 with status 10\n");
    } else {
        printf("[WAIT4_TEST] Test 3 FAILED: Reaped %d with status %d (expected %d, status 10)\n", res, WEXITSTATUS(status), child1);
    }

    // Test 4: ECHILD
    res = wait4(-1, &status, 0, NULL);
    if (res == -1) {
        printf("[WAIT4_TEST] Test 4 (ECHILD) PASSED (returned -1)\n");
    } else {
        printf("[WAIT4_TEST] Test 4 (ECHILD) FAILED: res=%d\n", res);
    }

    printf("[WAIT4_TEST] Complex wait4 test COMPLETED\n");
    return 0;
}
