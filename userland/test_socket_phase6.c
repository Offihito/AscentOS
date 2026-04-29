// Phase 6: Socket Options & Control - Stress Test
// Tests setsockopt/getsockopt, shutdown, and poll operations

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define TEST_SOCK "/tmp/test_p6.sock"

// Socket option constants (may not be in headers)
#ifndef SO_PASSCRED
#define SO_PASSCRED 16
#endif
#ifndef SO_PEERCRED
#define SO_PEERCRED 17
#endif
#ifndef SO_ACCEPTCONN
#define SO_ACCEPTCONN 30
#endif
#ifndef SO_SNDBUFFORCE
#define SO_SNDBUFFORCE 32
#endif
#ifndef SO_RCVBUFFORCE
#define SO_RCVBUFFORCE 33
#endif

// Shutdown constants
#ifndef SHUT_RD
#define SHUT_RD 0
#define SHUT_WR 1
#define SHUT_RDWR 2
#endif

// ── Test 1: Basic getsockopt for socket info ──────────────────────────────────
void test_getsockopt_basic(void) {
    printf("--- Test 1: Basic getsockopt (SO_TYPE, SO_DOMAIN, SO_PROTOCOL) ---\n");

    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        printf("  [SKIP] Cannot create socket\n");
        return;
    }

    int val;
    socklen_t len = sizeof(val);

    // SO_TYPE
    if (getsockopt(sock, SOL_SOCKET, SO_TYPE, &val, &len) == 0) {
        printf("  [OK] SO_TYPE = %d (expected 1)\n", val);
    } else {
        perror("  [FAIL] getsockopt SO_TYPE");
    }

    // SO_DOMAIN
    if (getsockopt(sock, SOL_SOCKET, SO_DOMAIN, &val, &len) == 0) {
        printf("  [OK] SO_DOMAIN = %d (expected 1)\n", val);
    } else {
        perror("  [FAIL] getsockopt SO_DOMAIN");
    }

    // SO_PROTOCOL
    if (getsockopt(sock, SOL_SOCKET, SO_PROTOCOL, &val, &len) == 0) {
        printf("  [OK] SO_PROTOCOL = %d (expected 0)\n", val);
    } else {
        perror("  [FAIL] getsockopt SO_PROTOCOL");
    }

    // SO_ERROR
    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &val, &len) == 0) {
        printf("  [OK] SO_ERROR = %d (expected 0)\n", val);
    } else {
        perror("  [FAIL] getsockopt SO_ERROR");
    }

    close(sock);
    printf("Test 1 Finished\n\n");
}

// ── Test 2: Buffer size options ───────────────────────────────────────────────
void test_buffer_options(void) {
    printf("--- Test 2: Buffer Size Options (SO_RCVBUF, SO_SNDBUF) ---\n");

    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return;
    }

    int val;
    socklen_t len = sizeof(val);

    // Get default buffer sizes
    getsockopt(sock, SOL_SOCKET, SO_RCVBUF, &val, &len);
    printf("  Default SO_RCVBUF = %d\n", val);

    getsockopt(sock, SOL_SOCKET, SO_SNDBUF, &val, &len);
    printf("  Default SO_SNDBUF = %d\n", val);

    // Set new buffer sizes
    int new_rcvbuf = 32768;
    if (setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &new_rcvbuf, sizeof(new_rcvbuf)) == 0) {
        getsockopt(sock, SOL_SOCKET, SO_RCVBUF, &val, &len);
        printf("  [OK] Set SO_RCVBUF to %d, got %d\n", new_rcvbuf, val);
    } else {
        perror("  [FAIL] setsockopt SO_RCVBUF");
    }

    int new_sndbuf = 16384;
    if (setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &new_sndbuf, sizeof(new_sndbuf)) == 0) {
        getsockopt(sock, SOL_SOCKET, SO_SNDBUF, &val, &len);
        printf("  [OK] Set SO_SNDBUF to %d, got %d\n", new_sndbuf, val);
    } else {
        perror("  [FAIL] setsockopt SO_SNDBUF");
    }

    // Test buffer limits (request huge buffer, should be capped)
    int huge_buf = 100 * 1024 * 1024; // 100MB
    if (setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &huge_buf, sizeof(huge_buf)) == 0) {
        getsockopt(sock, SOL_SOCKET, SO_RCVBUF, &val, &len);
        printf("  [OK] Requested 100MB RCVBUF, got %d (should be capped)\n", val);
    }

    close(sock);
    printf("Test 2 Finished\n\n");
}

// ── Test 3: SO_ACCEPTCONN on listening socket ──────────────────────────────────
void test_acceptconn_option(void) {
    printf("--- Test 3: SO_ACCEPTCONN (listening socket detection) ---\n");

    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return;
    }

    int val;
    socklen_t len = sizeof(val);

    // Before listen
    if (getsockopt(sock, SOL_SOCKET, SO_ACCEPTCONN, &val, &len) == 0) {
        printf("  Before listen: SO_ACCEPTCONN = %d (expected 0)\n", val);
    }

    // Bind and listen
    unlink(TEST_SOCK);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, TEST_SOCK);

    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(sock);
        return;
    }

    listen(sock, 5);

    // After listen
    if (getsockopt(sock, SOL_SOCKET, SO_ACCEPTCONN, &val, &len) == 0) {
        printf("  After listen: SO_ACCEPTCONN = %d (expected 1)\n", val);
    }

    close(sock);
    unlink(TEST_SOCK);
    printf("Test 3 Finished\n\n");
}

// ── Test 4: SO_REUSEADDR option ───────────────────────────────────────────────
void test_reuseaddr_option(void) {
    printf("--- Test 4: SO_REUSEADDR Option ---\n");

    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return;
    }

    int val = 1;
    socklen_t len = sizeof(val);

    // Set SO_REUSEADDR
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val)) == 0) {
        printf("  [OK] Set SO_REUSEADDR = 1\n");
    } else {
        perror("  [FAIL] setsockopt SO_REUSEADDR");
    }

    // Get it back
    if (getsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &val, &len) == 0) {
        printf("  [OK] Got SO_REUSEADDR = %d\n", val);
    }

    close(sock);
    printf("Test 4 Finished\n\n");
}

// ── Test 5: SO_PASSCRED option ────────────────────────────────────────────────
void test_passcred_option(void) {
    printf("--- Test 5: SO_PASSCRED Option ---\n");

    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return;
    }

    int val = 1;
    socklen_t len = sizeof(val);

    // Set SO_PASSCRED
    if (setsockopt(sock, SOL_SOCKET, SO_PASSCRED, &val, sizeof(val)) == 0) {
        printf("  [OK] Set SO_PASSCRED = 1\n");
    } else {
        perror("  [FAIL] setsockopt SO_PASSCRED");
    }

    // Get it back
    if (getsockopt(sock, SOL_SOCKET, SO_PASSCRED, &val, &len) == 0) {
        printf("  [OK] Got SO_PASSCRED = %d\n", val);
    }

    close(sock);
    printf("Test 5 Finished\n\n");
}

// ── Test 6: SO_PEERCRED on connected sockets ───────────────────────────────────
void test_peercred_option(void) {
    printf("--- Test 6: SO_PEERCRED (peer credentials) ---\n");

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        perror("socketpair");
        return;
    }

    // Struct for credentials (platform-specific size)
    struct {
        int pid;
        int uid;
        int gid;
    } cred;

    socklen_t len = sizeof(cred);

    if (getsockopt(sv[0], SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0) {
        printf("  [OK] SO_PEERCRED: pid=%d, uid=%d, gid=%d\n", cred.pid, cred.uid, cred.gid);
    } else {
        perror("  [FAIL] getsockopt SO_PEERCRED");
    }

    close(sv[0]);
    close(sv[1]);
    printf("Test 6 Finished\n\n");
}

// ── Test 7: Shutdown operations ───────────────────────────────────────────────
void test_shutdown_operations(void) {
    printf("--- Test 7: Shutdown Operations ---\n");

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        perror("socketpair");
        return;
    }

    // Test SHUT_WR - should still be able to receive
    printf("  Testing SHUT_WR...\n");
    if (shutdown(sv[0], SHUT_WR) == 0) {
        printf("  [OK] shutdown(sv[0], SHUT_WR) succeeded\n");

        // Send from other end
        send(sv[1], "hello", 5, 0);

        char buf[10];
        ssize_t n = recv(sv[0], buf, sizeof(buf), 0);
        if (n > 0) {
            printf("  [OK] Still received %zd bytes after SHUT_WR\n", n);
        }
    } else {
        perror("  [FAIL] shutdown SHUT_WR");
    }

    close(sv[0]);
    close(sv[1]);

    // Test SHUT_RDWR
    printf("  Testing SHUT_RDWR...\n");
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        perror("socketpair");
        return;
    }

    if (shutdown(sv[0], SHUT_RDWR) == 0) {
        printf("  [OK] shutdown(sv[0], SHUT_RDWR) succeeded\n");
    } else {
        perror("  [FAIL] shutdown SHUT_RDWR");
    }

    close(sv[0]);
    close(sv[1]);
    printf("Test 7 Finished\n\n");
}

// ── Test 8: Timeout options ───────────────────────────────────────────────────
void test_timeout_options(void) {
    printf("--- Test 8: Timeout Options (SO_RCVTIMEO, SO_SNDTIMEO) ---\n");

    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return;
    }

    // Set receive timeout using simple int (milliseconds)
    int timeout_ms = 5000;
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout_ms, sizeof(timeout_ms)) == 0) {
        printf("  [OK] Set SO_RCVTIMEO = %d ms\n", timeout_ms);
    } else {
        perror("  [FAIL] setsockopt SO_RCVTIMEO");
    }

    // Set send timeout
    if (setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout_ms, sizeof(timeout_ms)) == 0) {
        printf("  [OK] Set SO_SNDTIMEO = %d ms\n", timeout_ms);
    } else {
        perror("  [FAIL] setsockopt SO_SNDTIMEO");
    }

    // Get them back
    int val;
    socklen_t len = sizeof(val);
    if (getsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &val, &len) == 0) {
        printf("  [OK] Got SO_RCVTIMEO = %d ms\n", val);
    }

    if (getsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &val, &len) == 0) {
        printf("  [OK] Got SO_SNDTIMEO = %d ms\n", val);
    }

    close(sock);
    printf("Test 8 Finished\n\n");
}

// ── Test 9: Stress test - many sockets with options ────────────────────────────
void test_many_sockets_with_options(void) {
    printf("--- Test 9: Stress Test - 1000 Sockets with Options ---\n");

    #define NUM_SOCKETS 1000
    int socks[NUM_SOCKETS];
    int success = 0;

    for (int i = 0; i < NUM_SOCKETS; i++) {
        socks[i] = socket(AF_UNIX, SOCK_STREAM, 0);
        if (socks[i] < 0) {
            printf("  [FAIL] Failed at socket %d\n", i);
            break;
        }

        // Set various options
        int buf_size = 4096 * (i % 16 + 1);
        setsockopt(socks[i], SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));

        int reuse = 1;
        setsockopt(socks[i], SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        success++;
    }

    printf("  Created %d sockets with options\n", success);

    // Verify options on each
    int verified = 0;
    for (int i = 0; i < success; i++) {
        int val;
        socklen_t len = sizeof(val);
        if (getsockopt(socks[i], SOL_SOCKET, SO_REUSEADDR, &val, &len) == 0 && val == 1) {
            verified++;
        }
    }

    printf("  Verified options on %d/%d sockets\n", verified, success);

    // Close all
    for (int i = 0; i < success; i++) {
        close(socks[i]);
    }

    printf("Test 9 Finished\n\n");
}

// ── Test 10: Rapid setsockopt/getsockopt cycles ───────────────────────────────
void test_rapid_option_cycles(void) {
    printf("--- Test 10: Rapid setsockopt/getsockopt Cycles ---\n");

    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return;
    }

    int val;
    socklen_t len = sizeof(val);

    for (int i = 0; i < 10000; i++) {
        // Set buffer size
        int buf = 4096 + (i % 1000);
        setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &buf, sizeof(buf));

        // Read it back
        getsockopt(sock, SOL_SOCKET, SO_RCVBUF, &val, &len);
    }

    printf("  [OK] Completed 10000 set/get cycles\n");
    close(sock);
    printf("Test 10 Finished\n\n");
}

// ── Test 11: Invalid option handling ───────────────────────────────────────────
void test_invalid_options(void) {
    printf("--- Test 11: Invalid Option Handling ---\n");

    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return;
    }

    int val = 0;
    socklen_t len = sizeof(val);

    // Try invalid option (should fail with ENOPROTOOPT)
    int ret = getsockopt(sock, SOL_SOCKET, 9999, &val, &len);
    if (ret < 0) {
        printf("  [OK] Invalid getsockopt returned error (expected)\n");
    } else {
        printf("  [FAIL] Invalid getsockopt should have failed\n");
    }

    ret = setsockopt(sock, SOL_SOCKET, 9999, &val, sizeof(val));
    if (ret < 0) {
        printf("  [OK] Invalid setsockopt returned error (expected)\n");
    } else {
        printf("  [FAIL] Invalid setsockopt should have failed\n");
    }

    // Try with invalid level
    ret = getsockopt(sock, 999, SO_TYPE, &val, &len);
    if (ret < 0) {
        printf("  [OK] Invalid level getsockopt returned error (expected)\n");
    }

    close(sock);
    printf("Test 11 Finished\n\n");
}

// ── Test 12: Options on different socket types ─────────────────────────────────
void test_options_on_socket_types(void) {
    printf("--- Test 12: Options on Different Socket Types ---\n");

    // SOCK_STREAM
    int stream = socket(AF_UNIX, SOCK_STREAM, 0);
    if (stream >= 0) {
        int val;
        socklen_t len = sizeof(val);
        if (getsockopt(stream, SOL_SOCKET, SO_TYPE, &val, &len) == 0 && val == SOCK_STREAM) {
            printf("  [OK] SOCK_STREAM: SO_TYPE correct\n");
        }
        close(stream);
    }

    // SOCK_DGRAM
    int dgram = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (dgram >= 0) {
        int val;
        socklen_t len = sizeof(val);
        if (getsockopt(dgram, SOL_SOCKET, SO_TYPE, &val, &len) == 0 && val == SOCK_DGRAM) {
            printf("  [OK] SOCK_DGRAM: SO_TYPE correct\n");
        }
        close(dgram);
    }

    printf("Test 12 Finished\n\n");
}

// ── Main ───────────────────────────────────────────────────────────────────────
int main(void) {
    printf("========================================\n");
    printf("  Phase 6: Socket Options & Control\n");
    printf("  Stress Test Suite\n");
    printf("========================================\n\n");

    test_getsockopt_basic();
    test_buffer_options();
    test_acceptconn_option();
    test_reuseaddr_option();
    test_passcred_option();
    test_peercred_option();
    test_shutdown_operations();
    test_timeout_options();
    test_many_sockets_with_options();
    test_rapid_option_cycles();
    test_invalid_options();
    test_options_on_socket_types();

    printf("========================================\n");
    printf("  All Phase 6 Tests Completed\n");
    printf("========================================\n");

    return 0;
}
