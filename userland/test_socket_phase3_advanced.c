// ── Advanced AF_UNIX Phase 3 Test ───────────────────────────────────────────
// Tests: Multiprocess binding, deep path resolution, re-binding after unlink,
//        abstract vs filesystem isolation, and collision detection.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define UNIX_PATH_MAX 108

static int test_passed = 0;
static int test_failed = 0;

#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        printf("  FAIL: %s (line %d): %s\n", msg, __LINE__, strerror(errno)); \
        test_failed++; \
        return -1; \
    } \
    test_passed++; \
} while(0)

#define TEST_START(name) printf("\n=== Test: %s ===\n", name)
#define TEST_END() printf("  Passed: %d, Failed: %d\n", test_passed, test_failed)

// ── Test 1: Absolute vs Relative Paths ────────────────────────────────────────
static int test_path_resolution(void) {
    TEST_START("Path Resolution (Absolute vs Relative)");
    
    // 1. Relative path in /tmp
    chdir("/tmp");
    int fd1 = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr1;
    memset(&addr1, 0, sizeof(addr1));
    addr1.sun_family = AF_UNIX;
    strcpy(addr1.sun_path, "relative_socket.sock");
    
    unlink("relative_socket.sock");
    TEST_ASSERT(bind(fd1, (struct sockaddr*)&addr1, sizeof(addr1)) == 0, 
                "Should bind to relative path in /tmp");
    
    struct stat st;
    TEST_ASSERT(stat("/tmp/relative_socket.sock", &st) == 0, 
                "Socket file should exist at absolute path");
    TEST_ASSERT(S_ISSOCK(st.st_mode), "File should be a socket");
    
    // 2. Absolute path to the same location
    int fd2 = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr2;
    memset(&addr2, 0, sizeof(addr2));
    addr2.sun_family = AF_UNIX;
    strcpy(addr2.sun_path, "/tmp/relative_socket.sock");
    
    TEST_ASSERT(bind(fd2, (struct sockaddr*)&addr2, sizeof(addr2)) < 0, 
                "Should fail to bind to existing absolute path");
    TEST_ASSERT(errno == EADDRINUSE, "Should return EADDRINUSE");
    
    close(fd1);
    close(fd2);
    unlink("/tmp/relative_socket.sock");
    
    // 3. Invalid parent directory
    int fd3 = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr3;
    memset(&addr3, 0, sizeof(addr3));
    addr3.sun_family = AF_UNIX;
    strcpy(addr3.sun_path, "/nonexistent/path/socket.sock");
    
    TEST_ASSERT(bind(fd3, (struct sockaddr*)&addr3, sizeof(addr3)) < 0, 
                "Should fail for non-existent parent directory");
    // Depending on kernel implementation, might be ENOENT or other
    printf("  Non-existent parent returned errno=%d (%s)\n", errno, strerror(errno));
    
    close(fd3);
    TEST_END();
    return 0;
}

// ── Test 2: Abstract Namespace Isolation and Collision ────────────────────────
static int test_abstract_collision(void) {
    TEST_START("Abstract Namespace Collision & Isolation");
    
    int fd1 = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr1;
    memset(&addr1, 0, sizeof(addr1));
    addr1.sun_family = AF_UNIX;
    addr1.sun_path[0] = '\0';
    strcpy(addr1.sun_path + 1, "collision_test");
    
    TEST_ASSERT(bind(fd1, (struct sockaddr*)&addr1, sizeof(addr1)) == 0, 
                "Should bind to abstract address 'collision_test'");
    
    // Same name in another socket
    int fd2 = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr2;
    memset(&addr2, 0, sizeof(addr2));
    addr2.sun_family = AF_UNIX;
    addr2.sun_path[0] = '\0';
    strcpy(addr2.sun_path + 1, "collision_test");
    
    TEST_ASSERT(bind(fd2, (struct sockaddr*)&addr2, sizeof(addr2)) < 0, 
                "Should fail bind to identical abstract address");
    TEST_ASSERT(errno == EADDRINUSE, "Should return EADDRINUSE");
    
    // Different name but starts with same prefix
    int fd3 = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr3;
    memset(&addr3, 0, sizeof(addr3));
    addr3.sun_family = AF_UNIX;
    addr3.sun_path[0] = '\0';
    strcpy(addr3.sun_path + 1, "collision_test_longer");
    
    TEST_ASSERT(bind(fd3, (struct sockaddr*)&addr3, sizeof(addr3)) == 0, 
                "Should bind to 'collision_test_longer' (prefix match is not a collision)");
    
    // Cleaning up fd1 should allow fd2 to bind now
    close(fd1);
    TEST_ASSERT(bind(fd2, (struct sockaddr*)&addr2, sizeof(addr2)) == 0, 
                "Should bind to 'collision_test' after previous owner closed");
    
    close(fd2);
    close(fd3);
    TEST_END();
    return 0;
}

// ── Test 3: Multiprocess Binding ──────────────────────────────────────────────
static int test_multiprocess_bind(void) {
    TEST_START("Multiprocess Binding");
    
    const char *socket_path = "/tmp/multiprocess.sock";
    unlink(socket_path);
    
    int pid = fork();
    if (pid < 0) {
        perror("fork");
        return -1;
    }
    
    if (pid == 0) {
        // Child process
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strcpy(addr.sun_path, socket_path);
        
        // Wait a bit for parent to bind first
        usleep(100000); 
        
        if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
            printf("  Child correctly bound to socket (unexpected if parent bound first)\n");
            close(fd);
            exit(0);
        } else {
            if (errno == EADDRINUSE) {
                printf("  Child correctly saw EADDRINUSE\n");
                close(fd);
                exit(100); // Magic code for success (collision detected)
            } else {
                perror("child bind");
                exit(1);
            }
        }
    } else {
        // Parent process
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strcpy(addr.sun_path, socket_path);
        
        TEST_ASSERT(bind(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0, 
                    "Parent should bind successfully");
        
        int status;
        wait(&status);
        TEST_ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 100, 
                    "Child should have exited with EADDRINUSE status");
        
        close(fd);
        unlink(socket_path);
    }
    
    TEST_END();
    return 0;
}

// ── Test 4: Re-binding after Unlink (Without Closing) ─────────────────────────
static int test_rebind_unlink(void) {
    TEST_START("Re-binding after Unlink (No Close)");
    
    const char *path = "/tmp/rebind.sock";
    unlink(path);
    
    int fd1 = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, path);
    
    TEST_ASSERT(bind(fd1, (struct sockaddr*)&addr, sizeof(addr)) == 0, 
                "First bind should succeed");
    
    // Unlink the file. The socket is still bound in the kernel!
    unlink(path);
    
    int fd2 = socket(AF_UNIX, SOCK_STREAM, 0);
    TEST_ASSERT(bind(fd2, (struct sockaddr*)&addr, sizeof(addr)) < 0, 
                "Second bind should fail even if unlinked (socket still active in kernel)");
    TEST_ASSERT(errno == EADDRINUSE, "Should still return EADDRINUSE");
    
    close(fd1);
    // Now that fd1 is closed, the address is released in the kernel.
    TEST_ASSERT(bind(fd2, (struct sockaddr*)&addr, sizeof(addr)) == 0, 
                "Third bind should succeed after first socket is closed");
    
    close(fd2);
    unlink(path);
    TEST_END();
    return 0;
}

// ── Main ──────────────────────────────────────────────────────────────────────
int main(void) {
    printf("\n");
    printf("╔════════════════════════════════════════════════════════════╗\n");
    printf("║   Advanced Socket Test - Phase 3: AF_UNIX Binding          ║\n");
    printf("╚════════════════════════════════════════════════════════════╝\n");
    
    int total_passed = 0;
    int total_failed = 0;
    
    test_path_resolution();
    total_passed += test_passed; total_failed += test_failed;
    test_passed = test_failed = 0;
    
    test_abstract_collision();
    total_passed += test_passed; total_failed += test_failed;
    test_passed = test_failed = 0;
    
    test_multiprocess_bind();
    total_passed += test_passed; total_failed += test_failed;
    test_passed = test_failed = 0;
    
    test_rebind_unlink();
    total_passed += test_passed; total_failed += test_failed;
    
    printf("\n--- Advanced Phase 3 Summary ---\n");
    printf("Total Passed: %d, Total Failed: %d\n", total_passed, total_failed);
    
    return total_failed > 0 ? 1 : 0;
}
