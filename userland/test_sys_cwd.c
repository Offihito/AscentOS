#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

int main() {
    printf("Testing getcwd and chdir...\n");

    char buf[256];
    if (getcwd(buf, sizeof(buf)) != NULL) {
        printf("Current directory: %s\n", buf);
    } else {
        printf("getcwd failed! errno = %d\n", errno);
    }

    // Try creating a file in current dir
    int fd = open("test_file_cwd.txt", O_CREAT | O_RDWR, 0666);
    if (fd >= 0) {
        printf("Successfully created test_file_cwd.txt in CWD!\n");
        close(fd);
    } else {
        printf("Failed to create test_file_cwd.txt in CWD! errno = %d\n", errno);
    }

    // Try changing directory to /tmp
    if (chdir("/tmp") == 0) {
        printf("Successfully changed directory to /tmp\n");
    } else {
        printf("chdir to /tmp failed! errno = %d\n", errno);
    }

    if (getcwd(buf, sizeof(buf)) != NULL) {
        printf("New directory: %s\n", buf);
    } else {
        printf("getcwd after chdir failed! errno = %d\n", errno);
    }

    // Create file inside /tmp via relative path
    int fd2 = open("test_file_tmp.txt", O_CREAT | O_RDWR, 0666);
    if (fd2 >= 0) {
        printf("Successfully created test_file_tmp.txt relatively in new CWD!\n");
        close(fd2);
    } else {
        printf("Failed to create test_file_tmp.txt in new CWD! errno = %d\n", errno);
    }

    // Verify it actually exists at /tmp/test_file_tmp.txt
    int fd3 = open("/tmp/test_file_tmp.txt", O_RDONLY);
    if (fd3 >= 0) {
        printf("Verified existence of /tmp/test_file_tmp.txt via absolute path!\n");
        close(fd3);
    } else {
        printf("Could not verify absolute existence! errno = %d\n", errno);
    }

    // Test relative chdir (..)
    if (chdir("..") == 0) {
        printf("Successfully changed directory to ..\n");
    } else {
        printf("chdir to .. failed! errno = %d\n", errno);
    }

    if (getcwd(buf, sizeof(buf)) != NULL) {
        printf("Directory after .. is: %s\n", buf);
    } else {
        printf("getcwd after .. failed! errno = %d\n", errno);
    }

    return 0;
}
