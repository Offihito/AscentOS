#include "syscall.h"
#include "../sched/sched.h"
#include "../fs/vfs.h"
#include "../fs/socket.h"
#include "../console/klog.h"
#include <stdint.h>

static int alloc_fd(struct thread *t) {
    for (int i = 0; i < MAX_FDS; i++) {
        if (t->fds[i] == NULL) {
            return i;
        }
    }
    return -1;
}

static uint64_t sys_socket(uint64_t domain, uint64_t type, uint64_t protocol, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;

    if (domain != AF_INET) {
        return (uint64_t)-97; // EAFNOSUPPORT
    }
    if (type != SOCK_STREAM && type != SOCK_DGRAM) {
        return (uint64_t)-22; // EINVAL
    }

    struct thread *t = sched_get_current();
    if (!t) return (uint64_t)-1;

    int fd = alloc_fd(t);
    if (fd < 0) return (uint64_t)-24; // EMFILE

    vfs_node_t *node = socket_create_node((int)domain, (int)type, (int)protocol);
    if (!node) return (uint64_t)-12; // ENOMEM

    t->fds[fd] = node;
    t->fd_offsets[fd] = 0;

    return fd;
}

extern void tcp_global_recv_cb(int sock_id, const uint8_t *payload, uint16_t length);
extern void udp_global_recv_cb(uint16_t local_port, const uint8_t *payload, uint16_t length, uint32_t src_ip, uint16_t src_port);

static uint64_t sys_connect(uint64_t fd, uint64_t addr_ptr, uint64_t addrlen, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;

    struct thread *t = sched_get_current();
    if (!t || fd >= MAX_FDS || !t->fds[fd]) {
        return (uint64_t)-9; // EBADF
    }
    
    vfs_node_t *node = t->fds[fd];
    if (node->flags != FS_SOCKET) {
        return (uint64_t)-88; // ENOTSOCK
    }

    struct socket_data *sock = (struct socket_data *)node->device;
    if (!sock) return (uint64_t)-22; // EINVAL

    if (addrlen < sizeof(struct sockaddr_in)) {
        return (uint64_t)-22; // EINVAL
    }
    
    struct sockaddr_in *addr = (struct sockaddr_in *)addr_ptr;
    if (addr->sin_family != AF_INET) {
        return (uint64_t)-97; // EAFNOSUPPORT
    }

    // Convert network byte order to host byte order if kernel requires it
    // Wait, let's assume tcp_connect handles the exact IP and port values.
    // IP and Port conversion logic:
    // NTOHS is __builtin_bswap16
    // NTOHL is __builtin_bswap32
    uint32_t ip = __builtin_bswap32(addr->sin_addr.s_addr);
    uint16_t port = __builtin_bswap16(addr->sin_port);

    if (sock->type == SOCK_STREAM) {
        int net_id = tcp_connect(ip, port, tcp_global_recv_cb);
        if (net_id < 0) {
            return (uint64_t)-111; // ECONNREFUSED or Timeout
        }
        sock->net_id = net_id;
        return 0;
    }

    return (uint64_t)-95; // EOPNOTSUPP for now
}

static uint64_t sys_bind(uint64_t fd, uint64_t addr_ptr, uint64_t addrlen, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;

    struct thread *t = sched_get_current();
    if (!t || fd >= MAX_FDS || !t->fds[fd]) {
        return (uint64_t)-9; // EBADF
    }
    
    vfs_node_t *node = t->fds[fd];
    if (node->flags != FS_SOCKET) {
        return (uint64_t)-88; // ENOTSOCK
    }

    struct socket_data *sock = (struct socket_data *)node->device;
    if (!sock) return (uint64_t)-22; // EINVAL

    if (addrlen < sizeof(struct sockaddr_in)) {
        return (uint64_t)-22; // EINVAL
    }
    
    struct sockaddr_in *addr = (struct sockaddr_in *)addr_ptr;
    if (addr->sin_family != AF_INET) {
        return (uint64_t)-97; // EAFNOSUPPORT
    }

    uint16_t port = __builtin_bswap16(addr->sin_port);

    if (sock->type == SOCK_DGRAM) {
        int net_id = udp_bind(port, udp_global_recv_cb);
        if (net_id < 0) {
            return (uint64_t)-98; // EADDRINUSE
        }
        sock->net_id = net_id;
        sock->local_port = port;
        return 0;
    }

    if (sock->type == SOCK_STREAM) {
        // For TCP, just store the port; actual listen socket created in sys_listen
        sock->local_port = port;
        return 0;
    }

    return (uint64_t)-95; // EOPNOTSUPP
}

static uint64_t sys_sendto(uint64_t fd, uint64_t buf_ptr, uint64_t len, uint64_t flags, uint64_t addr_ptr, uint64_t addrlen) {
    (void)flags;

    struct thread *t = sched_get_current();
    if (!t || fd >= MAX_FDS || !t->fds[fd]) return (uint64_t)-9;
    
    vfs_node_t *node = t->fds[fd];
    struct socket_data *sock = (struct socket_data *)node->device;
    if (!sock) return (uint64_t)-22;

    if (sock->type == SOCK_DGRAM) {
        if (!addr_ptr || addrlen < sizeof(struct sockaddr_in)) return (uint64_t)-22;
        struct sockaddr_in *addr = (struct sockaddr_in *)addr_ptr;
        uint32_t ip = __builtin_bswap32(addr->sin_addr.s_addr);
        uint16_t dport = __builtin_bswap16(addr->sin_port);
        int w = udp_send_packet(ip, sock->local_port, dport, (const void *)buf_ptr, (uint16_t)len);
        return w == 0 ? len : -1;
    } else {
        // TCP ignores addr
        return node->write(node, 0, len, (uint8_t *)buf_ptr);
    }
}

static uint64_t sys_recvfrom(uint64_t fd, uint64_t buf_ptr, uint64_t len, uint64_t flags, uint64_t addr_ptr, uint64_t addrlen_ptr) {
    (void)flags; (void)addr_ptr; (void)addrlen_ptr;
    // For now we don't return the sender's addr in recvfrom (simplified)

    struct thread *t = sched_get_current();
    if (!t || fd >= MAX_FDS || !t->fds[fd]) return (uint64_t)-9;
    
    vfs_node_t *node = t->fds[fd];
    return node->read(node, 0, len, (uint8_t *)buf_ptr);
}

static uint64_t sys_listen(uint64_t fd, uint64_t backlog, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)backlog; (void)a2; (void)a3; (void)a4; (void)a5;

    struct thread *t = sched_get_current();
    if (!t || fd >= MAX_FDS || !t->fds[fd]) return (uint64_t)-9;

    vfs_node_t *node = t->fds[fd];
    if (node->flags != FS_SOCKET) return (uint64_t)-88;

    struct socket_data *sock = (struct socket_data *)node->device;
    if (!sock) return (uint64_t)-22;

    if (sock->type != SOCK_STREAM) return (uint64_t)-95; // EOPNOTSUPP

    if (sock->local_port == 0) return (uint64_t)-22; // Must bind first

    // Create listening TCP socket in the kernel network stack
    int net_id = tcp_listen(sock->local_port, tcp_global_recv_cb);
    if (net_id < 0) return (uint64_t)-98; // EADDRINUSE

    sock->net_id = net_id;
    sock->listening = true;

    klog_puts("[SOCKET] listen on port ");
    klog_uint64(sock->local_port);
    klog_puts(", net_id=");
    klog_uint64(net_id);
    klog_puts("\n");

    return 0;
}

static uint64_t sys_accept(uint64_t fd, uint64_t addr_ptr, uint64_t addrlen_ptr, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;

    struct thread *t = sched_get_current();
    if (!t || fd >= MAX_FDS || !t->fds[fd]) return (uint64_t)-9;

    vfs_node_t *node = t->fds[fd];
    if (node->flags != FS_SOCKET) return (uint64_t)-88;

    struct socket_data *sock = (struct socket_data *)node->device;
    if (!sock || !sock->listening) return (uint64_t)-22;

    // Block until a connection arrives in the TCP accept queue
    int new_net_id = -1;
    for (int tries = 0; tries < 30000; tries++) {
        new_net_id = tcp_accept(sock->net_id);
        if (new_net_id >= 0) break;
        sched_yield();
    }

    if (new_net_id < 0) return (uint64_t)-11; // EAGAIN

    klog_puts("[SOCKET] accepted connection, new net_id=");
    klog_uint64(new_net_id);
    klog_puts("\n");

    // Create a new socket VFS node for the accepted connection
    vfs_node_t *new_node = socket_create_node(AF_INET, SOCK_STREAM, 0);
    if (!new_node) return (uint64_t)-12; // ENOMEM

    struct socket_data *new_sock = (struct socket_data *)new_node->device;
    new_sock->net_id = new_net_id;

    // Fill caller's addr if provided
    if (addr_ptr && addrlen_ptr) {
        uint32_t remote_ip = 0;
        uint16_t remote_port = 0;
        tcp_get_remote_info(new_net_id, &remote_ip, &remote_port);

        struct sockaddr_in peer;
        peer.sin_family = AF_INET;
        peer.sin_port = __builtin_bswap16(remote_port);
        peer.sin_addr.s_addr = __builtin_bswap32(remote_ip);

        struct sockaddr_in *uaddr = (struct sockaddr_in *)addr_ptr;
        *uaddr = peer;

        uint32_t *ulen = (uint32_t *)addrlen_ptr;
        *ulen = sizeof(struct sockaddr_in);
    }

    // Allocate a file descriptor for the accepted socket
    int new_fd = alloc_fd(t);
    if (new_fd < 0) {
        // No FDs available
        return (uint64_t)-24; // EMFILE
    }

    t->fds[new_fd] = new_node;
    t->fd_offsets[new_fd] = 0;

    return new_fd;
}

void syscall_register_net(void) {
    syscall_register(SYS_SOCKET, sys_socket);
    syscall_register(SYS_CONNECT, sys_connect);
    syscall_register(SYS_ACCEPT, sys_accept);
    syscall_register(SYS_SENDTO, sys_sendto);
    syscall_register(SYS_RECVFROM, sys_recvfrom);
    syscall_register(SYS_BIND, sys_bind);
    syscall_register(SYS_LISTEN, sys_listen);
    klog_puts("[OK] Net Syscalls registered (socket/connect/accept/listen/sendto/recvfrom/bind).\n");
}
