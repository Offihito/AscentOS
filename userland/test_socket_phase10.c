// ═══════════════════════════════════════════════════════════════════════════
// Phase 10: Socket Filesystem Integration Test Suite
// ═══════════════════════════════════════════════════════════════════════════
// Tests socket filesystem binding, S_IFSOCK file type, and unlink behavior

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <errno.h>
#include <fcntl.h>

#define UNIX_PATH_MAX 108
#define TEST_PATH "/tmp/phase10_test.sock"
#define TEST_PATH2 "/tmp/phase10_test2.sock"

// Test macros
#define TEST_START(name) printf("\n--- Test: %s ---\n", name)
#define TEST_END() printf("Test Finished\n")
#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        printf("  [FAIL] %s (line %d): %s\n", msg, __LINE__, errno ? strerror(errno) : "No error"); \
        return 1; \
    } \
    printf("  [OK] %s\n", msg); \
} while(0)

// ═══════════════════════════════════════════════════════════════════════════
// Test 1: Socket file type verification (S_IFSOCK)
// ═══════════════════════════════════════════════════════════════════════════
static int test_socket_file_type(void) {
    TEST_START("Socket file type (S_IFSOCK)");
    
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    TEST_ASSERT(sock >= 0, "socket created");
    
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, TEST_PATH, UNIX_PATH_MAX - 1);
    
    int ret = bind(sock, (struct sockaddr*)&addr, sizeof(addr));
    TEST_ASSERT(ret == 0, "bind succeeded");
    
    // Check file type with stat
    struct stat st;
    ret = stat(TEST_PATH, &st);
    TEST_ASSERT(ret == 0, "stat succeeded");
    
    // Verify S_IFSOCK is set
    int is_socket = S_ISSOCK(st.st_mode);
    printf("  File mode: 0%o, S_IFSOCK check: %d\n", st.st_mode, is_socket);
    TEST_ASSERT(is_socket, "S_ISSOCK returns true");
    
    // Cleanup
    unlink(TEST_PATH);
    close(sock);
    
    TEST_END();
    return 0;
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 2: Bind/unlink rapid cycles
// ═══════════════════════════════════════════════════════════════════════════
static int test_bind_unlink_cycles(void) {
    TEST_START("Bind/unlink rapid cycles");
    
    for (int i = 0; i < 100; i++) {
        int sock = socket(AF_UNIX, SOCK_STREAM, 0);
        TEST_ASSERT(sock >= 0, "socket created");
        
        struct sockaddr_un addr = { .sun_family = AF_UNIX };
        strncpy(addr.sun_path, TEST_PATH, UNIX_PATH_MAX - 1);
        
        int ret = bind(sock, (struct sockaddr*)&addr, sizeof(addr));
        if (ret < 0) {
            printf("  [FAIL] bind failed on iteration %d (errno=%d)\n", i, errno);
            close(sock);
            return 1;
        }
        
        ret = unlink(TEST_PATH);
        if (ret < 0) {
            printf("  [FAIL] unlink failed on iteration %d (errno=%d)\n", i, errno);
            close(sock);
            return 1;
        }
        
        close(sock);
    }
    
    printf("  [OK] 100 bind/unlink cycles completed\n");
    TEST_END();
    return 0;
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 3: Orphan socket (unlink while listening)
// ═══════════════════════════════════════════════════════════════════════════
static int test_orphan_socket(void) {
    TEST_START("Orphan socket (unlink while listening)");
    
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    TEST_ASSERT(sock >= 0, "socket created");
    
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, TEST_PATH, UNIX_PATH_MAX - 1);
    
    int ret = bind(sock, (struct sockaddr*)&addr, sizeof(addr));
    TEST_ASSERT(ret == 0, "bind succeeded");
    
    ret = listen(sock, 5);
    TEST_ASSERT(ret == 0, "listen succeeded");
    
    // Unlink while socket is active
    ret = unlink(TEST_PATH);
    TEST_ASSERT(ret == 0, "unlink succeeded while socket active");
    
    // File should no longer exist
    struct stat st;
    ret = stat(TEST_PATH, &st);
    TEST_ASSERT(ret < 0 && errno == ENOENT, "file no longer exists after unlink");
    
    // Socket should still be functional (orphaned)
    // Try to connect - should fail since file is gone
    int client = socket(AF_UNIX, SOCK_STREAM, 0);
    TEST_ASSERT(client >= 0, "client socket created");
    
    ret = connect(client, (struct sockaddr*)&addr, sizeof(addr));
    TEST_ASSERT(ret < 0 && errno == ECONNREFUSED, "connect fails with ECONNREFUSED (socket gone)");
    
    close(client);
    close(sock);
    
    TEST_END();
    return 0;
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 4: Bind to existing path fails
// ═══════════════════════════════════════════════════════════════════════════
static int test_bind_existing_fails(void) {
    TEST_START("Bind to existing path fails");
    
    // Create a regular file first
    int fd = open(TEST_PATH, O_CREAT | O_WRONLY, 0644);
    TEST_ASSERT(fd >= 0, "created regular file");
    write(fd, "test", 4);
    close(fd);
    
    // Try to bind socket to same path
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    TEST_ASSERT(sock >= 0, "socket created");
    
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, TEST_PATH, UNIX_PATH_MAX - 1);
    
    int ret = bind(sock, (struct sockaddr*)&addr, sizeof(addr));
    TEST_ASSERT(ret < 0 && errno == EADDRINUSE, "bind fails with EADDRINUSE");
    
    close(sock);
    unlink(TEST_PATH);
    
    TEST_END();
    return 0;
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 5: Connect to filesystem-bound socket
// ═══════════════════════════════════════════════════════════════════════════
static int test_connect_fs_socket(void) {
    TEST_START("Connect to filesystem-bound socket");
    
    // Create server socket
    int server = socket(AF_UNIX, SOCK_STREAM, 0);
    TEST_ASSERT(server >= 0, "server socket created");
    
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, TEST_PATH, UNIX_PATH_MAX - 1);
    
    int ret = bind(server, (struct sockaddr*)&addr, sizeof(addr));
    TEST_ASSERT(ret == 0, "server bind succeeded");
    
    ret = listen(server, 5);
    TEST_ASSERT(ret == 0, "listen succeeded");
    
    // Verify file exists
    struct stat st;
    ret = stat(TEST_PATH, &st);
    TEST_ASSERT(ret == 0, "socket file exists");
    TEST_ASSERT(S_ISSOCK(st.st_mode), "file is a socket");
    
    // Connect client
    int client = socket(AF_UNIX, SOCK_STREAM, 0);
    TEST_ASSERT(client >= 0, "client socket created");
    
    ret = connect(client, (struct sockaddr*)&addr, sizeof(addr));
    TEST_ASSERT(ret == 0, "connect succeeded");
    
    // Accept connection
    int accepted = accept(server, NULL, NULL);
    TEST_ASSERT(accepted >= 0, "accept succeeded");
    
    // Send data
    const char *msg = "hello";
    ret = write(client, msg, strlen(msg));
    TEST_ASSERT(ret == (int)strlen(msg), "write succeeded");
    
    // Receive data
    char buf[32];
    ret = read(accepted, buf, sizeof(buf));
    TEST_ASSERT(ret == (int)strlen(msg), "read succeeded");
    buf[ret] = '\0';
    TEST_ASSERT(strcmp(buf, msg) == 0, "received correct data");
    
    // Cleanup
    close(accepted);
    close(client);
    close(server);
    unlink(TEST_PATH);
    
    TEST_END();
    return 0;
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 6: Multiple sockets with different paths
// ═══════════════════════════════════════════════════════════════════════════
static int test_multiple_fs_sockets(void) {
    TEST_START("Multiple filesystem sockets");
    
    int socks[5];
    char paths[5][64];
    
    // Create multiple sockets
    for (int i = 0; i < 5; i++) {
        snprintf(paths[i], sizeof(paths[i]), "/tmp/sock_%d.sock", i);
        
        socks[i] = socket(AF_UNIX, SOCK_STREAM, 0);
        TEST_ASSERT(socks[i] >= 0, "socket created");
        
        struct sockaddr_un addr = { .sun_family = AF_UNIX };
        strncpy(addr.sun_path, paths[i], UNIX_PATH_MAX - 1);
        
        int ret = bind(socks[i], (struct sockaddr*)&addr, sizeof(addr));
        TEST_ASSERT(ret == 0, "bind succeeded");
        
        // Verify each socket file
        struct stat st;
        ret = stat(paths[i], &st);
        TEST_ASSERT(ret == 0, "stat succeeded");
        TEST_ASSERT(S_ISSOCK(st.st_mode), "file is a socket");
    }
    
    // Cleanup
    for (int i = 0; i < 5; i++) {
        unlink(paths[i]);
        close(socks[i]);
    }
    
    TEST_END();
    return 0;
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 7: Abstract vs filesystem namespace
// ═══════════════════════════════════════════════════════════════════════════
static int test_abstract_vs_filesystem(void) {
    TEST_START("Abstract vs filesystem namespace");
    
    // Create abstract socket (sun_path[0] = '\0')
    int abs_sock = socket(AF_UNIX, SOCK_STREAM, 0);
    TEST_ASSERT(abs_sock >= 0, "abstract socket created");
    
    struct sockaddr_un abs_addr = { .sun_family = AF_UNIX };
    abs_addr.sun_path[0] = '\0';
    strcpy(&abs_addr.sun_path[1], "abstract_test");
    
    int ret = bind(abs_sock, (struct sockaddr*)&abs_addr, sizeof(abs_addr));
    TEST_ASSERT(ret == 0, "abstract bind succeeded");
    
    // Abstract socket should NOT create a file
    struct stat st;
    ret = stat("/abstract_test", &st);
    TEST_ASSERT(ret < 0, "abstract socket has no filesystem entry");
    
    // Create filesystem socket
    int fs_sock = socket(AF_UNIX, SOCK_STREAM, 0);
    TEST_ASSERT(fs_sock >= 0, "filesystem socket created");
    
    struct sockaddr_un fs_addr = { .sun_family = AF_UNIX };
    strncpy(fs_addr.sun_path, TEST_PATH, UNIX_PATH_MAX - 1);
    
    ret = bind(fs_sock, (struct sockaddr*)&fs_addr, sizeof(fs_addr));
    TEST_ASSERT(ret == 0, "filesystem bind succeeded");
    
    // Filesystem socket SHOULD create a file
    ret = stat(TEST_PATH, &st);
    TEST_ASSERT(ret == 0, "filesystem socket has file entry");
    TEST_ASSERT(S_ISSOCK(st.st_mode), "file is a socket");
    
    // Cleanup
    close(abs_sock);
    unlink(TEST_PATH);
    close(fs_sock);
    
    TEST_END();
    return 0;
}

// ═══════════════════════════════════════════════════════════════════════════
// Main
// ═══════════════════════════════════════════════════════════════════════════
int main(void) {
    printf("═══════════════════════════════════════════════════════════════════════════\n");
    printf("Phase 10: Socket Filesystem Integration Test Suite\n");
    printf("═══════════════════════════════════════════════════════════════════════════\n");
    
    int passed = 0, failed = 0;
    
    if (test_socket_file_type() == 0) passed++; else failed++;
    if (test_bind_unlink_cycles() == 0) passed++; else failed++;
    if (test_orphan_socket() == 0) passed++; else failed++;
    if (test_bind_existing_fails() == 0) passed++; else failed++;
    if (test_connect_fs_socket() == 0) passed++; else failed++;
    if (test_multiple_fs_sockets() == 0) passed++; else failed++;
    if (test_abstract_vs_filesystem() == 0) passed++; else failed++;
    
    printf("\n═══════════════════════════════════════════════════════════════════════════\n");
    printf("Test Summary: %d passed, %d failed\n", passed, failed);
    printf("═══════════════════════════════════════════════════════════════════════════\n");
    
    return failed > 0 ? 1 : 0;
}
