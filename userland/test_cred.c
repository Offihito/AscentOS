#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>

int main() {
    printf("=== Credentials & Umask Test ===\n\n");

    uid_t uid = getuid();
    gid_t gid = getgid();
    uid_t euid = geteuid();
    gid_t egid = getegid();

    printf("Parent Credentials:\n");
    printf(" UID:  %d\n", uid);
    printf(" GID:  %d\n", gid);
    printf(" EUID: %d\n", euid);
    printf(" EGID: %d\n\n", egid);

    mode_t old_mask = umask(0077);
    printf("Original umask: %04o\n", old_mask);
    mode_t new_mask = umask(0077);
    printf("New umask:      %04o\n\n", new_mask);

    if (new_mask != 0077) {
        printf("Error: umask did not update correctly!\n");
        return 1;
    }

    printf("Forking to test inheritance...\n");
    pid_t child = fork();
    if (child == 0) {
        // Child
        uid_t c_uid = getuid();
        mode_t c_mask = umask(0);

        printf("Child Credentials:\n");
        printf(" UID:   %d\n", c_uid);
        printf(" Umask: %04o\n", c_mask);

        if (c_uid != uid || c_mask != 0077) {
            printf("Error: Child did not inherit correctly!\n");
            _exit(1);
        }
        
        printf("Inheritance OK!\n");
        _exit(0);
    } else if (child > 0) {
        int status;
        waitpid(child, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            printf("\n=== Test Complete ===\n");
            return 0;
        } else {
            printf("\nChild failed.\n");
            return 1;
        }
    } else {
        printf("Fork failed!\n");
        return 1;
    }
}
