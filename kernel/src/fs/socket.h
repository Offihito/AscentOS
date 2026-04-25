#ifndef FS_SOCKET_H
#define FS_SOCKET_H

#include "../net/tcp.h"
#include "../net/udp.h"
#include "../sched/wait.h"
#include "vfs.h"

// POSIX Domain
#define AF_UNIX 1
#define AF_INET 2

// POSIX Type
#define SOCK_STREAM 1
#define SOCK_DGRAM 2

// Standard Socket Address Structures
struct sockaddr {
  uint16_t sa_family;
  char sa_data[14];
};

struct sockaddr_un {
  uint16_t sun_family;
  char sun_path[108];
};

struct in_addr {
  uint32_t s_addr;
};

struct sockaddr_in {
  uint16_t sin_family;
  uint16_t sin_port;
  struct in_addr sin_addr;
  char sin_zero[8];
};

#define SOCKET_RX_BUF_SIZE 4096

// Unix SCM_RIGHTS support
struct cmsghdr {
  uint64_t cmsg_len;
  int cmsg_level;
  int cmsg_type;
};
#define SCM_RIGHTS 0x01
#define SOL_SOCKET 1

// Socket Data tied to vfs_node_t->device
struct socket_data {
  int domain;
  int type;
  int protocol;
  int net_id;          // Maps to TCP or UDP socket ID
  uint16_t local_port; // Local bound port (for UDP unbind)
  uint32_t remote_ip;  // For connected UDP
  uint16_t remote_port;

  uint16_t last_src_port;
  uint32_t last_src_ip;

  // AF_UNIX specific
  char sun_path[108];
  struct socket_data *peer;      // For connected stream sockets
  struct socket_data **accept_q; // For listening sockets
  uint32_t accept_q_len;
  uint32_t accept_q_max;

  // Pending FDs (AF_UNIX)
  vfs_node_t *pending_fds[8];
  uint32_t pending_fd_count;

  uint8_t rx_buffer[SOCKET_RX_BUF_SIZE];
  volatile uint32_t rx_head;
  volatile uint32_t rx_tail;
  bool closed;
  bool listening; // True if this socket is in listen mode
  wait_queue_t wait_queue;

  struct socket_data *next;
};

void socket_push_rx(struct socket_data *sock, const uint8_t *data,
                    uint16_t len);
uint32_t socket_pull_rx(struct socket_data *sock, uint8_t *buffer,
                        uint32_t size);

vfs_node_t *socket_create_node(int domain, int type, int protocol);
vfs_node_t *socket_create_node_from_data(struct socket_data *data);
void socket_list_add(struct socket_data *data);
struct socket_data *socket_find_unix(const char *path);

#endif
