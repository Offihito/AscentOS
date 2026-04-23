#include "../console/klog.h"
#include "../fs/socket.h"
#include "../fs/vfs.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../net/netif.h"
#include "../sched/sched.h"
#include "syscall.h"
#include <stdint.h>

static uint64_t sys_socket(uint64_t domain, uint64_t type, uint64_t protocol,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;

  klog_puts("[SOCKET] domain=");
  klog_uint64(domain);
  klog_puts(" type=");
  klog_uint64(type);
  klog_puts("\n");

  if (domain != AF_INET && domain != AF_UNIX) {
    return (uint64_t)-97; // EAFNOSUPPORT
  }
  if (type != SOCK_STREAM && type != SOCK_DGRAM) {
    return (uint64_t)-22; // EINVAL
  }

  struct thread *t = sched_get_current();
  if (!t)
    return (uint64_t)-1;

  int fd = alloc_fd(t);
  if (fd < 0)
    return (uint64_t)-24; // EMFILE

  vfs_node_t *node = socket_create_node((int)domain, (int)type, (int)protocol);
  if (!node)
    return (uint64_t)-12; // ENOMEM

  t->fds[fd] = node;
  t->fd_offsets[fd] = 0;

  klog_puts("[SOCKET] created fd=");
  klog_uint64(fd);
  klog_puts("\n");

  return fd;
}

extern void tcp_global_recv_cb(int sock_id, const uint8_t *payload,
                               uint16_t length);
extern void udp_global_recv_cb(uint16_t local_port, const uint8_t *payload,
                               uint16_t length, uint32_t src_ip,
                               uint16_t src_port);

static uint64_t sys_connect(uint64_t fd, uint64_t addr_ptr, uint64_t addrlen,
                            uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;

  struct thread *t = sched_get_current();
  if (!t || fd >= MAX_FDS || !t->fds[fd]) {
    return (uint64_t)-9; // EBADF
  }

  vfs_node_t *node = t->fds[fd];
  if (node->flags != FS_SOCKET) {
    return (uint64_t)-88; // ENOTSOCK
  }

  struct socket_data *sock = (struct socket_data *)node->device;
  if (!sock)
    return (uint64_t)-22; // EINVAL

  struct sockaddr *addr = (struct sockaddr *)addr_ptr;
  if (sock->domain == AF_INET) {
    if (addr->sa_family != AF_INET) {
      return (uint64_t)-97; // EAFNOSUPPORT
    }
    struct sockaddr_in *sin = (struct sockaddr_in *)addr_ptr;
    uint32_t ip = __builtin_bswap32(sin->sin_addr.s_addr);
    uint16_t port = __builtin_bswap16(sin->sin_port);

    if (sock->type == SOCK_STREAM) {
      int net_id = tcp_connect(ip, port, tcp_global_recv_cb);
      if (net_id < 0) {
        return (uint64_t)-111; // ECONNREFUSED
      }
      sock->net_id = net_id;
      return 0;
    }

    if (sock->type == SOCK_DGRAM) {
      sock->remote_ip = ip;
      sock->remote_port = port;
      if (sock->local_port == 0) {
        int p = udp_bind(0, udp_global_recv_cb);
        if (p < 0)
          return (uint64_t)-98;
        sock->local_port = (uint16_t)p;
      }
      return 0;
    }
  } else if (sock->domain == AF_UNIX) {
    struct sockaddr_un *sun = (struct sockaddr_un *)addr_ptr;
    if (sun->sun_family != AF_UNIX)
      return (uint64_t)-97;

    // Handle abstract UNIX sockets (sun_path[0] == '\0')
    char lookup_path[108];
    if (sun->sun_path[0] == '\0') {
      // Abstract socket - copy the abstract name
      uint32_t name_len = (uint32_t)addrlen - 2;
      if (name_len > 107) name_len = 107;
      memcpy(lookup_path, sun->sun_path, name_len);
      lookup_path[name_len] = '\0';
    } else {
      // Path-based socket
      vfs_node_t *sock_node = vfs_resolve_path(sun->sun_path);
      if (!sock_node || (sock_node->flags & 0xFF) != FS_SOCKET) {
        if (sock_node)
          kfree(sock_node);
        return (uint64_t)-111; // ECONNREFUSED
      }
      kfree(sock_node);
      strcpy(lookup_path, sun->sun_path);
    }

    struct socket_data *server = socket_find_unix(lookup_path);
    if (!server || !server->listening)
      return (uint64_t)-111;

    if (sock->type == SOCK_STREAM) {
      if (server->accept_q_len >= server->accept_q_max)
        return (uint64_t)-11; // EAGAIN

      // Create only the socket data for the peer
      struct socket_data *peer_sock =
          (struct socket_data *)kmalloc(sizeof(struct socket_data));
      if (!peer_sock)
        return (uint64_t)-12;
      memset(peer_sock, 0, sizeof(struct socket_data));
      peer_sock->domain = AF_UNIX;
      peer_sock->type = SOCK_STREAM;
      peer_sock->net_id = -1;
      wait_queue_init(&peer_sock->wait_queue);

      socket_list_add(peer_sock);

      peer_sock->peer = sock;
      sock->peer = peer_sock;

      server->accept_q[server->accept_q_len++] = peer_sock;
      wait_queue_wake_all(&server->wait_queue);
      return 0;
    }
  }

  return (uint64_t)-95; // EOPNOTSUPP
}

static uint64_t sys_bind(uint64_t fd, uint64_t addr_ptr, uint64_t addrlen,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;

  struct thread *t = sched_get_current();
  if (!t || fd >= MAX_FDS || !t->fds[fd]) {
    return (uint64_t)-9; // EBADF
  }

  vfs_node_t *node = t->fds[fd];
  if (node->flags != FS_SOCKET) {
    return (uint64_t)-88; // ENOTSOCK
  }

  struct socket_data *sock = (struct socket_data *)node->device;
  if (!sock)
    return (uint64_t)-22; // EINVAL

  struct sockaddr *addr = (struct sockaddr *)addr_ptr;
  if (sock->domain == AF_INET) {
    if (addr->sa_family != AF_INET) {
      return (uint64_t)-97; // EAFNOSUPPORT
    }
    struct sockaddr_in *sin = (struct sockaddr_in *)addr_ptr;
    uint16_t port = __builtin_bswap16(sin->sin_port);

    if (sock->type == SOCK_DGRAM) {
      int p = udp_bind(port, udp_global_recv_cb);
      if (p < 0)
        return (uint64_t)-98;
      sock->local_port = (uint16_t)p;
      return 0;
    }
    if (sock->type == SOCK_STREAM) {
      sock->local_port = port;
      return 0;
    }
  } else if (sock->domain == AF_UNIX) {
    struct sockaddr_un *sun = (struct sockaddr_un *)addr_ptr;
    if (sun->sun_family != AF_UNIX)
      return (uint64_t)-97;

    // Handle abstract UNIX sockets (sun_path[0] == '\0')
    if (sun->sun_path[0] == '\0') {
      klog_puts("[BIND] AF_UNIX abstract socket\n");
      uint32_t name_len = (uint32_t)addrlen - 2;
      if (name_len > 107) name_len = 107;
      memcpy(sock->sun_path, sun->sun_path, name_len);
      sock->sun_path[name_len] = '\0';
      return 0;
    }

    klog_puts("[BIND] AF_UNIX path=");
    klog_puts(sun->sun_path);
    klog_puts("\n");

    if (vfs_resolve_path(sun->sun_path))
      return (uint64_t)-98; // EADDRINUSE

    // Split path into directory and name
    char dir_path[128];
    char file_name[128];
    const char *last_slash = strrchr(sun->sun_path, '/');

    if (!last_slash) {
      strcpy(dir_path, ".");
      strcpy(file_name, sun->sun_path);
    } else {
      size_t dir_len = (size_t)(last_slash - sun->sun_path);
      if (dir_len == 0)
        strcpy(dir_path, "/");
      else {
        strncpy(dir_path, sun->sun_path, dir_len);
        dir_path[dir_len] = '\0';
      }
      strcpy(file_name, last_slash + 1);
    }

    struct thread *t = sched_get_current();
    vfs_node_t *cwd = vfs_resolve_path(t->cwd_path);
    vfs_node_t *dir = vfs_resolve_path_at(cwd ? cwd : fs_root, dir_path);
    if (!dir) {
      klog_puts("[BIND] ENOENT: parent dir not found: ");
      klog_puts(dir_path);
      klog_puts("\n");
      return (uint64_t)-2; // ENOENT
    }

    int err = vfs_mknod(dir, file_name, 0666, FS_SOCKET, sock);
    if (err < 0) {
      klog_puts("[BIND] mknod failed for socket: ");
      klog_puts(file_name);
      klog_puts("\n");
      return (uint64_t)-1;
    }

    strncpy(sock->sun_path, sun->sun_path, 107);
    sock->sun_path[107] = '\0';
    return 0;
  }

  return (uint64_t)-95; // EOPNOTSUPP
}

static uint64_t sys_sendto(uint64_t fd, uint64_t buf_ptr, uint64_t len,
                           uint64_t flags, uint64_t addr_ptr,
                           uint64_t addrlen) {
  (void)flags;

  struct thread *t = sched_get_current();
  if (!t || fd >= MAX_FDS || !t->fds[fd])
    return (uint64_t)-9;

  vfs_node_t *node = t->fds[fd];
  struct socket_data *sock = (struct socket_data *)node->device;
  if (!sock)
    return (uint64_t)-22;

  if (sock->type == SOCK_DGRAM) {
    uint32_t ip;
    uint16_t dport;

    if (addr_ptr && addrlen >= sizeof(struct sockaddr_in)) {
      struct sockaddr_in *addr = (struct sockaddr_in *)addr_ptr;
      ip = __builtin_bswap32(addr->sin_addr.s_addr);
      dport = __builtin_bswap16(addr->sin_port);
    } else if (sock->remote_port != 0) {
      ip = sock->remote_ip;
      dport = sock->remote_port;
    } else {
      return (uint64_t)-22; // EINVAL
    }

    // Auto-bind to ephemeral port if not already bound
    if (sock->local_port == 0) {
      int p = udp_bind(0, udp_global_recv_cb);
      if (p < 0)
        return (uint64_t)-98;
      sock->local_port = p;
    }

    int w = udp_send_packet(ip, sock->local_port, dport, (const void *)buf_ptr,
                            (uint16_t)len);
    return w == 0 ? len : -1;
  } else {
    // TCP ignores addr
    return node->write(node, 0, len, (uint8_t *)buf_ptr);
  }
}

static uint64_t sys_recvfrom(uint64_t fd, uint64_t buf_ptr, uint64_t len,
                             uint64_t flags, uint64_t addr_ptr,
                             uint64_t addrlen_ptr) {
  (void)flags;
  struct thread *t = sched_get_current();
  if (!t || fd >= MAX_FDS || !t->fds[fd])
    return (uint64_t)-9;

  vfs_node_t *node = t->fds[fd];
  struct socket_data *sock = (struct socket_data *)node->device;

  uint32_t bytes_read = node->read(node, 0, len, (uint8_t *)buf_ptr);

  if (bytes_read > 0 && addr_ptr && sock && sock->type == SOCK_DGRAM) {
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = __builtin_bswap16(sock->last_src_port);
    sin.sin_addr.s_addr = __builtin_bswap32(sock->last_src_ip);

    uint32_t addrlen = sizeof(struct sockaddr_in);
    if (addrlen_ptr) {
      uint32_t user_addrlen = *(uint32_t *)addrlen_ptr;
      if (user_addrlen < addrlen)
        addrlen = user_addrlen;
      *(uint32_t *)addrlen_ptr = sizeof(struct sockaddr_in);
    }
    memcpy((void *)addr_ptr, &sin, addrlen);
  }

  return bytes_read;
}

static uint64_t sys_listen(uint64_t fd, uint64_t backlog, uint64_t a2,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  struct thread *t = sched_get_current();
  if (!t || fd >= MAX_FDS || !t->fds[fd])
    return (uint64_t)-9;

  vfs_node_t *node = t->fds[fd];
  if (node->flags != FS_SOCKET)
    return (uint64_t)-88;

  struct socket_data *sock = (struct socket_data *)node->device;
  if (!sock)
    return (uint64_t)-22;

  klog_puts("[LISTEN] fd=");
  klog_uint64(fd);
  klog_puts(" domain=");
  klog_uint64(sock->domain == AF_UNIX ? 1 : 2);
  klog_puts(" backlog=");
  klog_uint64(backlog);
  klog_puts("\n");

  if (sock->domain == AF_INET) {
    if (sock->type != SOCK_STREAM)
      return (uint64_t)-95; // EOPNOTSUPP

    if (sock->local_port == 0)
      return (uint64_t)-22; // Must bind first

    // Create listening TCP socket in the kernel network stack
    int net_id = tcp_listen(sock->local_port, tcp_global_recv_cb);
    if (net_id < 0)
      return (uint64_t)-98; // EADDRINUSE

    sock->net_id = net_id;
    sock->listening = true;
    return 0;
  } else if (sock->domain == AF_UNIX) {
    if (sock->type != SOCK_STREAM)
      return (uint64_t)-95;

    uint32_t q_max = (uint32_t)backlog;
    if (q_max == 0)
      q_max = 5;
    if (q_max > 128)
      q_max = 128;

    sock->accept_q =
        (struct socket_data **)kmalloc(sizeof(struct socket_data *) * q_max);
    if (!sock->accept_q)
      return (uint64_t)-12; // ENOMEM

    memset(sock->accept_q, 0, sizeof(struct socket_data *) * q_max);
    sock->accept_q_len = 0;
    sock->accept_q_max = q_max;
    sock->listening = true;
    return 0;
  }
  return (uint64_t)-95;
}

static uint64_t sys_accept(uint64_t fd, uint64_t addr_ptr, uint64_t addrlen_ptr,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;

  struct thread *t = sched_get_current();
  if (!t || fd >= MAX_FDS || !t->fds[fd])
    return (uint64_t)-9;

  vfs_node_t *node = t->fds[fd];
  if (node->flags != FS_SOCKET)
    return (uint64_t)-88;

  struct socket_data *sock = (struct socket_data *)node->device;
  if (!sock || !sock->listening)
    return (uint64_t)-22;

  if (sock->domain == AF_INET) {
    // Block until a connection arrives in the TCP accept queue
    int new_net_id = -1;
    for (int tries = 0; tries < 30000; tries++) {
      new_net_id = tcp_accept(sock->net_id);
      if (new_net_id >= 0)
        break;
      sched_yield();
    }

    if (new_net_id < 0)
      return (uint64_t)-11; // EAGAIN

    // Create a new socket VFS node for the accepted connection
    vfs_node_t *new_node = socket_create_node(AF_INET, SOCK_STREAM, 0);
    if (!new_node)
      return (uint64_t)-12; // ENOMEM

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

    int new_fd = alloc_fd(t);
    if (new_fd < 0)
      return (uint64_t)-24;

    t->fds[new_fd] = new_node;
    t->fd_offsets[new_fd] = 0;

    return new_fd;
  } else if (sock->domain == AF_UNIX) {
    while (sock->accept_q_len == 0) {
      if (sock->closed)
        return (uint64_t)-9;
      sched_yield();
    }

    struct socket_data *peer_sock = sock->accept_q[0];
    for (uint32_t i = 1; i < sock->accept_q_len; i++) {
      sock->accept_q[i - 1] = sock->accept_q[i];
    }
    sock->accept_q_len--;

    vfs_node_t *new_node = socket_create_node_from_data(peer_sock);
    if (!new_node)
      return (uint64_t)-12;

    int new_fd = alloc_fd(t);
    if (new_fd < 0)
      return (uint64_t)-24;
    t->fds[new_fd] = new_node;

    return new_fd;
  }
  return (uint64_t)-95;
}

struct iovec {
  void *iov_base;
  uint64_t iov_len;
};

struct msghdr {
  void *msg_name;
  uint32_t msg_namelen;
  struct iovec *msg_iov;
  uint64_t msg_iovlen;
  void *msg_control;
  uint64_t msg_controllen;
  int msg_flags;
};

static uint64_t sys_sendmsg(uint64_t fd, uint64_t msg_ptr, uint64_t flags,
                            uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  struct msghdr *msg = (struct msghdr *)msg_ptr;
  if (!msg || msg->msg_iovlen == 0)
    return (uint64_t)-22;

  struct thread *t = sched_get_current();
  if (!t || fd >= MAX_FDS || !t->fds[fd])
    return (uint64_t)-9;
  vfs_node_t *node = t->fds[fd];
  if (node->flags != FS_SOCKET)
    return (uint64_t)-88;
  struct socket_data *sock = (struct socket_data *)node->device;

  // Handle data transfer
  struct iovec *iov = msg->msg_iov;
  uint64_t total_sent = 0;
  for (uint64_t i = 0; i < msg->msg_iovlen; i++) {
    uint64_t sent =
        sys_sendto(fd, (uint64_t)iov[i].iov_base, iov[i].iov_len, flags, 0, 0);
    if ((int64_t)sent < 0)
      return sent;
    total_sent += sent;
  }

  // Handle FD passing (SCM_RIGHTS)
  if (sock->domain == AF_UNIX && sock->peer && msg->msg_control &&
      msg->msg_controllen >= sizeof(struct cmsghdr)) {
    struct cmsghdr *cmsg = (struct cmsghdr *)msg->msg_control;
    if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
      int *fds = (int *)((uint8_t *)cmsg + sizeof(struct cmsghdr));
      int num_fds = (cmsg->cmsg_len - sizeof(struct cmsghdr)) / sizeof(int);

      struct socket_data *peer = sock->peer;
      for (int i = 0; i < num_fds && peer->pending_fd_count < 8; i++) {
        int send_fd = fds[i];
        if (send_fd >= 0 && send_fd < MAX_FDS && t->fds[send_fd]) {
          // Reference the node
          vfs_node_t *n = t->fds[send_fd];
          vfs_open(n); // Increment refcount
          peer->pending_fds[peer->pending_fd_count++] = n;
        }
      }
    }
  }

  return total_sent;
}

static uint64_t sys_recvmsg(uint64_t fd, uint64_t msg_ptr, uint64_t flags,
                            uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  struct msghdr *msg = (struct msghdr *)msg_ptr;
  if (!msg || msg->msg_iovlen == 0)
    return (uint64_t)-22;

  struct thread *t = sched_get_current();
  if (!t || fd >= MAX_FDS || !t->fds[fd])
    return (uint64_t)-9;
  vfs_node_t *node = t->fds[fd];
  if (node->flags != FS_SOCKET)
    return (uint64_t)-88;
  struct socket_data *sock = (struct socket_data *)node->device;

  // Handle data transfer
  struct iovec *iov = msg->msg_iov;
  uint32_t addrlen = msg->msg_namelen;
  uint64_t total_recv = 0;
  // For simplicity, we only receive into the first iov if multiple are present,
  // or it needs more complex logic. Let's do just the first for now.
  total_recv = sys_recvfrom(fd, (uint64_t)iov[0].iov_base, iov[0].iov_len,
                            flags, (uint64_t)msg->msg_name, (uint64_t)&addrlen);
  msg->msg_namelen = addrlen;

  // Handle received FDs
  if (sock->domain == AF_UNIX && sock->pending_fd_count > 0 &&
      msg->msg_control && msg->msg_controllen >= sizeof(struct cmsghdr)) {
    struct cmsghdr *cmsg = (struct cmsghdr *)msg->msg_control;
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;

    int *fds = (int *)((uint8_t *)cmsg + sizeof(struct cmsghdr));
    int num_to_deliver = sock->pending_fd_count;
    int max_can_fit =
        (msg->msg_controllen - sizeof(struct cmsghdr)) / sizeof(int);
    if (num_to_deliver > max_can_fit)
      num_to_deliver = max_can_fit;

    for (int i = 0; i < num_to_deliver; i++) {
      int new_fd = alloc_fd(t);
      if (new_fd >= 0) {
        t->fds[new_fd] = sock->pending_fds[i];
        fds[i] = new_fd;
      } else {
        vfs_close(sock->pending_fds[i]);
        fds[i] = -1;
      }
    }
    cmsg->cmsg_len = sizeof(struct cmsghdr) + num_to_deliver * sizeof(int);

    // Shift remaining
    for (uint32_t i = num_to_deliver; i < sock->pending_fd_count; i++) {
      sock->pending_fds[i - num_to_deliver] = sock->pending_fds[i];
    }
    sock->pending_fd_count -= num_to_deliver;
  }

  return total_recv;
}

static uint64_t sys_getsockname(uint64_t fd, uint64_t addr_ptr,
                                uint64_t addrlen_ptr, uint64_t a3, uint64_t a4,
                                uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;

  struct thread *t = sched_get_current();
  if (!t || fd >= MAX_FDS || !t->fds[fd])
    return (uint64_t)-9; // EBADF

  vfs_node_t *node = t->fds[fd];
  struct socket_data *sock = (struct socket_data *)node->device;
  if (!sock)
    return (uint64_t)-88; // ENOTSOCK

  struct sockaddr_in sin;
  memset(&sin, 0, sizeof(sin));
  sin.sin_family = AF_INET;
  sin.sin_port = __builtin_bswap16(sock->local_port);
  // Get local netif IP
  netif_t *nif = netif_get();
  sin.sin_addr.s_addr = __builtin_bswap32(nif ? nif->ip : 0);

  uint32_t addrlen = sizeof(struct sockaddr_in);
  if (addrlen_ptr) {
    uint32_t user_addrlen = *(uint32_t *)addrlen_ptr;
    if (user_addrlen < addrlen)
      addrlen = user_addrlen;
    *(uint32_t *)addrlen_ptr = sizeof(struct sockaddr_in);
  }
  memcpy((void *)addr_ptr, &sin, addrlen);

  return 0;
}

// ── setsockopt (syscall 54) ─────────────────────────────────────────────────
// Common socket option constants
#define SO_REUSEADDR 2
#define SO_ERROR 4
#define SO_KEEPALIVE 9
#define SO_SNDBUF 7
#define SO_RCVBUF 8
#define SO_REUSEPORT 15
#define SO_TYPE 3
#define SO_LINGER 13

// TCP level options
#define IPPROTO_TCP 6
#define TCP_NODELAY 1

static uint64_t sys_setsockopt(uint64_t fd, uint64_t level, uint64_t optname,
                               uint64_t optval, uint64_t optlen, uint64_t a5) {
  (void)a5;

  struct thread *t = sched_get_current();
  if (!t || fd >= MAX_FDS || !t->fds[fd])
    return (uint64_t)-9; // EBADF

  vfs_node_t *node = t->fds[fd];
  if (node->flags != FS_SOCKET)
    return (uint64_t)-88; // ENOTSOCK

  struct socket_data *sock = (struct socket_data *)node->device;
  if (!sock)
    return (uint64_t)-22; // EINVAL

  klog_puts("[SOCKOPT] setsockopt fd=");
  klog_uint64(fd);
  klog_puts(" level=");
  klog_uint64(level);
  klog_puts(" optname=");
  klog_uint64(optname);
  klog_puts("\n");

  // Accept all known options silently (store nothing for now)
  if (level == SOL_SOCKET) {
    switch (optname) {
    case SO_REUSEADDR:
    case SO_REUSEPORT:
    case SO_KEEPALIVE:
    case SO_SNDBUF:
    case SO_RCVBUF:
    case SO_LINGER:
      return 0; // accepted, no-op
    default:
      return 0; // accept unknown SOL_SOCKET options too
    }
  }

  if (level == IPPROTO_TCP) {
    switch (optname) {
    case TCP_NODELAY:
      return 0; // accepted, no-op
    default:
      return 0;
    }
  }

  // Accept any other level/optname combination
  return 0;
}

// ── getsockopt (syscall 55) ─────────────────────────────────────────────────
static uint64_t sys_getsockopt(uint64_t fd, uint64_t level, uint64_t optname,
                               uint64_t optval, uint64_t optlen_ptr,
                               uint64_t a5) {
  (void)a5;

  struct thread *t = sched_get_current();
  if (!t || fd >= MAX_FDS || !t->fds[fd])
    return (uint64_t)-9; // EBADF

  vfs_node_t *node = t->fds[fd];
  if (node->flags != FS_SOCKET)
    return (uint64_t)-88; // ENOTSOCK

  struct socket_data *sock = (struct socket_data *)node->device;
  if (!sock)
    return (uint64_t)-22; // EINVAL

  if (!optval || !optlen_ptr)
    return (uint64_t)-14; // EFAULT

  uint32_t *ulen = (uint32_t *)optlen_ptr;

  if (level == SOL_SOCKET) {
    int val = 0;
    switch (optname) {
    case SO_TYPE:
      val = sock->type;
      break;
    case SO_ERROR:
      val = 0; // no pending error
      break;
    case SO_REUSEADDR:
    case SO_REUSEPORT:
    case SO_KEEPALIVE:
      val = 0; // not enabled
      break;
    case SO_SNDBUF:
    case SO_RCVBUF:
      val = SOCKET_RX_BUF_SIZE;
      break;
    default:
      val = 0;
      break;
    }

    uint32_t copy_len = *ulen;
    if (copy_len > sizeof(int))
      copy_len = sizeof(int);
    memcpy((void *)optval, &val, copy_len);
    *ulen = sizeof(int);
    return 0;
  }

  // For unknown levels, return 0
  int val = 0;
  uint32_t copy_len = *ulen;
  if (copy_len > sizeof(int))
    copy_len = sizeof(int);
  memcpy((void *)optval, &val, copy_len);
  *ulen = sizeof(int);
  return 0;
}

void syscall_register_net(void) {
  syscall_register(SYS_SOCKET, sys_socket);
  syscall_register(SYS_CONNECT, sys_connect);
  syscall_register(SYS_ACCEPT, sys_accept);
  syscall_register(SYS_SENDTO, sys_sendto);
  syscall_register(SYS_RECVFROM, sys_recvfrom);
  syscall_register(SYS_SENDMSG, sys_sendmsg);
  syscall_register(SYS_RECVMSG, sys_recvmsg);
  syscall_register(SYS_BIND, sys_bind);
  syscall_register(SYS_LISTEN, sys_listen);
  syscall_register(SYS_GETSOCKNAME, sys_getsockname);
  syscall_register(SYS_SETSOCKOPT, sys_setsockopt);
  syscall_register(SYS_GETSOCKOPT, sys_getsockopt);
  klog_puts("[OK] Net Syscalls registered "
            "(socket/connect/accept/listen/sendto/recvfrom/sendmsg/recvmsg/"
            "bind/getsockname/setsockopt/getsockopt).\n");
}
