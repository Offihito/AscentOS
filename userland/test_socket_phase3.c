#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <errno.h>
#include <assert.h>

#define UNIX_PATH_MAX 108

void test_abstract_namespace(void) {
    printf("Testing abstract namespace...\n");
    int socks[200]; // Adjusted for new MAX_FDS=256
    char abstract_name[UNIX_PATH_MAX];
    
    // Create many abstract sockets
    for (int i = 0; i < 200; i++) {
        socks[i] = socket(AF_UNIX, SOCK_STREAM, 0);
        if (socks[i] < 0) {
            perror("socket");
            exit(1);
        }
        
        memset(abstract_name, 0, sizeof(abstract_name));
        snprintf(abstract_name + 1, sizeof(abstract_name) - 1, "socket_%d", i);
        
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        memcpy(addr.sun_path, abstract_name, sizeof(addr.sun_path));
        
        int ret = bind(socks[i], (struct sockaddr*)&addr, sizeof(struct sockaddr_un));
        if (ret != 0) {
            fprintf(stderr, "bind %d failed: %s\n", i, strerror(errno));
            exit(1);
        }
    }
    printf("Created 200 abstract sockets\n");
    
    // Verify collision detection
    int collision_sock = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un collision_addr;
    memset(&collision_addr, 0, sizeof(collision_addr));
    collision_addr.sun_family = AF_UNIX;
    collision_addr.sun_path[0] = '\0';
    strcpy(collision_addr.sun_path + 1, "socket_0");
    
    if (bind(collision_sock, (struct sockaddr*)&collision_addr, sizeof(collision_addr)) == 0) {
        fprintf(stderr, "Collision detection failed! (socket_0 was already bound)\n");
        exit(1);
    }
    if (errno != EADDRINUSE) {
        fprintf(stderr, "Expected EADDRINUSE, got %d (%s)\n", errno, strerror(errno));
        exit(1);
    }
    printf("Collision detection verified\n");
    
    // Cleanup
    for (int i = 0; i < 200; i++) close(socks[i]);
    close(collision_sock);
}

void test_filesystem_sockets(void) {
    printf("Testing filesystem sockets...\n");
    int socks[100];
    char path[UNIX_PATH_MAX];
    
    for (int i = 0; i < 100; i++) {
        socks[i] = socket(AF_UNIX, SOCK_STREAM, 0);
        snprintf(path, sizeof(path), "/tmp/test_socket_%d.sock", i);
        
        unlink(path); // Ensure it doesn't exist
        
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
        
        if (bind(socks[i], (struct sockaddr*)&addr, sizeof(addr)) != 0) {
            fprintf(stderr, "bind to %s failed: %s\n", path, strerror(errno));
            exit(1);
        }
    }
    printf("Created 100 filesystem sockets\n");
    
    // Verify files exist and are sockets
    for (int i = 0; i < 100; i++) {
        snprintf(path, sizeof(path), "/tmp/test_socket_%d.sock", i);
        struct stat st;
        if (stat(path, &st) != 0) {
            fprintf(stderr, "stat %s failed\n", path);
            exit(1);
        }
        if (!S_ISSOCK(st.st_mode)) {
            fprintf(stderr, "%s is not a socket (mode %o)\n", path, st.st_mode);
            exit(1);
        }
    }
    printf("Verified 100 socket files\n");
    
    // Cleanup
    for (int i = 0; i < 100; i++) {
        snprintf(path, sizeof(path), "/tmp/test_socket_%d.sock", i);
        unlink(path);
        close(socks[i]);
    }
}

int main() {
    printf("--- Socket Phase 3 Stress Test ---\n");
    test_abstract_namespace();
    test_filesystem_sockets();
    printf("--- All Phase 3 tests passed! ---\n");
    return 0;
}
