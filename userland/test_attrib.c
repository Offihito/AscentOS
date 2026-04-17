#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <string.h>

int main() {
    printf("=== Attributes & Directory Test ===\n\n");

    const char *dir_name = "test_dir_attr";

    // 1. mkdir
    printf("[1] Creating directory %s\n", dir_name);
    if (mkdir(dir_name, 0777) != 0) {
        printf("mkdir failed\n");
        return 1;
    }

    struct stat st;
    if (stat(dir_name, &st) != 0) {
        printf("stat failed\n");
        return 1;
    }
    printf("Directory mode: %o\n", st.st_mode & 0777);

    // 2. chmod
    printf("[2] Changing mode to 0755\n");
    if (chmod(dir_name, 0755) != 0) {
        printf("chmod failed\n");
        return 1;
    }
    if (stat(dir_name, &st) != 0) {
        printf("stat failed\n");
        return 1;
    }
    printf("New directory mode: %o\n", st.st_mode & 0777);

    // 3. chown
    printf("[3] Changing owner to 1000:1000\n");
    if (chown(dir_name, 1000, 1000) != 0) {
        printf("chown failed\n");
        return 1;
    }
    if (stat(dir_name, &st) != 0) {
        printf("stat failed\n");
        return 1;
    }
    printf("New owner: UID %d, GID %d\n", st.st_uid, st.st_gid);

    // 4. rmdir
    printf("[4] Removing directory %s\n", dir_name);
    if (rmdir(dir_name) != 0) {
        printf("rmdir failed\n");
        return 1;
    }

    // Verify removed
    if (stat(dir_name, &st) == 0) {
        printf("Error: Directory still exists!\n");
        return 1;
    } else {
        printf("Directory successfully removed.\n");
    }

    printf("\n=== Test Complete ===\n");
    return 0;
}
