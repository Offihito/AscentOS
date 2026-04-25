#include "socket.h"
#include "../console/klog.h"
#include "../fs/vfs.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../net/net.h"
#include "../sched/sched.h"

static struct socket_data *global_socket_list = NULL;

void socket_push_rx(struct socket_data *sock, const uint8_t *data,
                    uint16_t len) {
  if (!sock || !data || sock->closed)
    return;

  for (uint16_t i = 0; i < len; i++) {
    uint32_t next_head = (sock->rx_head + 1) % SOCKET_RX_BUF_SIZE;
    if (next_head == sock->rx_tail)
      break; // Buffer full
    sock->rx_buffer[sock->rx_head] = data[i];
    sock->rx_head = next_head;
  }
  wait_queue_wake_all(&sock->wait_queue);
}

uint32_t socket_pull_rx(struct socket_data *sock, uint8_t *buffer,
                        uint32_t size) {
  if (!sock || !buffer)
    return 0;
  while (sock->rx_head == sock->rx_tail) {
    if (sock->closed)
      return 0;
    net_poll();
    sched_yield(); // Blocks waiting for data
  }
  uint32_t read = 0;
  while (read < size && sock->rx_head != sock->rx_tail) {
    buffer[read++] = sock->rx_buffer[sock->rx_tail];
    sock->rx_tail = (sock->rx_tail + 1) % SOCKET_RX_BUF_SIZE;
  }
  return read;
}

void tcp_global_recv_cb(int sock_id, const uint8_t *payload, uint16_t length) {
  struct socket_data *curr = global_socket_list;
  while (curr) {
    if (curr->type == SOCK_STREAM && curr->net_id == sock_id) {
      if (length == 0) {
        curr->closed = true;
        wait_queue_wake_all(&curr->wait_queue);
      } else {
        socket_push_rx(curr, payload, length);
      }
      return;
    }
    curr = curr->next;
  }
}

void udp_global_recv_cb(uint16_t local_port, const uint8_t *payload,
                        uint16_t length, uint32_t src_ip, uint16_t src_port) {
  (void)src_ip;
  (void)src_port; // Currently ignoring sender info for basic receive
  struct socket_data *curr = global_socket_list;
  while (curr) {
    if (curr->type == SOCK_DGRAM && curr->local_port == local_port) {
      curr->last_src_ip = src_ip;
      curr->last_src_port = src_port;
      socket_push_rx(curr, payload, length);
      return;
    }
    curr = curr->next;
  }
}

static uint32_t socket_read(struct vfs_node *node, uint32_t offset,
                            uint32_t size, uint8_t *buffer) {
  (void)offset;
  struct socket_data *sock = (struct socket_data *)node->device;
  if (!sock)
    return 0;
  return socket_pull_rx(sock, buffer, size);
}

static uint32_t socket_write(struct vfs_node *node, uint32_t offset,
                             uint32_t size, uint8_t *buffer) {
  (void)offset;
  struct socket_data *sock = (struct socket_data *)node->device;
  if (!sock || sock->closed)
    return 0;

  if (sock->domain == AF_INET) {
    if (sock->type == SOCK_STREAM && sock->net_id >= 0) {
      int w = tcp_send(sock->net_id, buffer, size);
      return w > 0 ? w : 0;
    }
  } else if (sock->domain == AF_UNIX) {
    if (sock->type == SOCK_STREAM) {
      if (sock->peer && !sock->peer->closed) {
        socket_push_rx(sock->peer, buffer, (uint16_t)size);
        return size;
      }
      return 0; // EPIPE
    }
  }
  return 0;
}

static int socket_poll(struct vfs_node *node, int events) {
  struct socket_data *sock = (struct socket_data *)node->device;
  if (!sock)
    return 0;

  int revents = 0;
  if (events & POLLIN) {
    if (sock->rx_head != sock->rx_tail || sock->closed) {
      revents |= POLLIN;
    }
    if (sock->listening) {
      if (sock->domain == AF_INET) {
        revents |= POLLIN;
      } else if (sock->domain == AF_UNIX) {
        if (sock->accept_q_len > 0)
          revents |= POLLIN;
      }
    }
  }
  if (events & POLLOUT) {
    if (!sock->closed && !sock->listening) {
      revents |= POLLOUT;
    }
  }

  return revents;
}

static void socket_close(vfs_node_t *node) {
  if (!node)
    return;
  struct socket_data *data = (struct socket_data *)node->device;
  if (data) {
    data->closed = true;

    // Wake up wait queues
    wait_queue_wake_all(&data->wait_queue);

    if (data->domain == AF_INET) {
      if (data->type == SOCK_STREAM && data->net_id >= 0) {
        tcp_close(data->net_id);
      } else if (data->type == SOCK_DGRAM && data->local_port > 0) {
        udp_unbind(data->local_port);
      }
    } else if (data->domain == AF_UNIX) {
      if (data->peer) {
        data->peer->closed = true;
        wait_queue_wake_all(&data->peer->wait_queue);
        data->peer->peer = NULL; // Break link
      }
      if (data->accept_q) {
        for (uint32_t i = 0; i < data->accept_q_len; i++) {
          if (data->accept_q[i]) {
            data->accept_q[i]->closed = true;
            wait_queue_wake_all(&data->accept_q[i]->wait_queue);
          }
        }
        kfree(data->accept_q);
      }
      // Clean up pending FDs
      for (uint32_t i = 0; i < data->pending_fd_count; i++) {
        if (data->pending_fds[i]) {
          vfs_close(data->pending_fds[i]);
        }
      }
    }

    // Remove from list
    if (global_socket_list == data) {
      global_socket_list = data->next;
    } else {
      struct socket_data *curr = global_socket_list;
      while (curr && curr->next != data) {
        curr = curr->next;
      }
      if (curr)
        curr->next = data->next;
    }

    kfree(data);
  }
  kfree(node);
}

struct socket_data *socket_find_unix(const char *path) {
  struct socket_data *curr = global_socket_list;
  while (curr) {
    if (curr->domain == AF_UNIX && strcmp(curr->sun_path, path) == 0) {
      return curr;
    }
    curr = curr->next;
  }
  return NULL;
}

void socket_list_add(struct socket_data *data) {
  if (!data)
    return;
  data->next = global_socket_list;
  global_socket_list = data;
}

vfs_node_t *socket_create_node_from_data(struct socket_data *data) {
  if (!data)
    return NULL;

  vfs_node_t *node = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
  if (!node)
    return NULL;
  memset(node, 0, sizeof(vfs_node_t));

  node->flags = FS_SOCKET;
  node->mask = 0666;
  strcpy(node->name, "socket");
  node->close = socket_close;
  node->read = socket_read;
  node->write = socket_write;
  node->poll = socket_poll;
  node->device = data;

  return node;
}

vfs_node_t *socket_create_node(int domain, int type, int protocol) {
  struct socket_data *data =
      (struct socket_data *)kmalloc(sizeof(struct socket_data));
  if (!data)
    return NULL;

  memset(data, 0, sizeof(struct socket_data));
  data->domain = domain;
  data->type = type;
  data->protocol = protocol;
  data->net_id = -1;
  wait_queue_init(&data->wait_queue);

  socket_list_add(data);
  return socket_create_node_from_data(data);
}
