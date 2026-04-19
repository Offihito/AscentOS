#include "socket.h"
#include "../mm/heap.h"
#include "../lib/string.h"
#include "../sched/sched.h"

static struct socket_data *global_socket_list = NULL;

void socket_push_rx(struct socket_data *sock, const uint8_t *data, uint16_t len) {
    if (!sock || !data || sock->closed) return;
    for (uint16_t i = 0; i < len; i++) {
        uint32_t next_head = (sock->rx_head + 1) % SOCKET_RX_BUF_SIZE;
        if (next_head == sock->rx_tail) break; // Buffer full
        sock->rx_buffer[sock->rx_head] = data[i];
        sock->rx_head = next_head;
    }
    wait_queue_wake_all(&sock->wait_queue);
}

uint32_t socket_pull_rx(struct socket_data *sock, uint8_t *buffer, uint32_t size) {
    if (!sock || !buffer) return 0;
    while (sock->rx_head == sock->rx_tail) {
        if (sock->closed) return 0;
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
            socket_push_rx(curr, payload, length);
            return;
        }
        curr = curr->next;
    }
}

void udp_global_recv_cb(uint16_t local_port, const uint8_t *payload, uint16_t length, uint32_t src_ip, uint16_t src_port) {
    (void)src_ip; (void)src_port; // Currently ignoring sender info for basic receive
    struct socket_data *curr = global_socket_list;
    while (curr) {
        if (curr->type == SOCK_DGRAM && curr->local_port == local_port) {
            socket_push_rx(curr, payload, length);
            return;
        }
        curr = curr->next;
    }
}

static uint32_t socket_read(struct vfs_node *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    (void)offset;
    struct socket_data *sock = (struct socket_data *)node->device;
    if (!sock) return 0;
    return socket_pull_rx(sock, buffer, size);
}

static uint32_t socket_write(struct vfs_node *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    (void)offset;
    struct socket_data *sock = (struct socket_data *)node->device;
    if (!sock) return 0;
    
    if (sock->type == SOCK_STREAM && sock->net_id >= 0) {
        int w = tcp_send(sock->net_id, buffer, size);
        return w > 0 ? w : 0;
    }
    return 0; // standard write on UDP lacking sendto logic
}

static int socket_poll(struct vfs_node *node, int events) {
    struct socket_data *sock = (struct socket_data *)node->device;
    if (!sock) return 0;
    
    int revents = 0;
    if (events & POLLIN) {
        if (sock->rx_head != sock->rx_tail || sock->closed || sock->listening) {
            revents |= POLLIN;
        }
    }
    if (events & POLLOUT) {
        if (!sock->closed) {
            revents |= POLLOUT; // Sockets are generally writable in this simple stack
        }
    }
    return revents;
}

static void socket_close(vfs_node_t *node) {
    if (!node) return;
    struct socket_data *data = (struct socket_data *)node->device;
    if (data) {
        data->closed = true;
        if (data->type == SOCK_STREAM && data->net_id >= 0) {
            tcp_close(data->net_id);
        } else if (data->type == SOCK_DGRAM && data->local_port > 0) {
            udp_unbind(data->local_port);
        }
        
        // Remove from list
        if (global_socket_list == data) {
            global_socket_list = data->next;
        } else {
            struct socket_data *curr = global_socket_list;
            while (curr && curr->next != data) {
                curr = curr->next;
            }
            if (curr) curr->next = data->next;
        }
        
        kfree(data);
    }
    kfree(node);
}

vfs_node_t *socket_create_node(int domain, int type, int protocol) {
    vfs_node_t *node = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
    if (!node) return NULL;
    memset(node, 0, sizeof(vfs_node_t));

    node->flags = FS_SOCKET;
    node->mask = 0666;
    strcpy(node->name, "socket");
    node->close = socket_close;
    node->read = socket_read;
    node->write = socket_write;
    node->poll = socket_poll;

    struct socket_data *data = (struct socket_data *)kmalloc(sizeof(struct socket_data));
    if (data) {
        memset(data, 0, sizeof(struct socket_data));
        data->domain = domain;
        data->type = type;
        data->protocol = protocol;
        data->net_id = -1;
        
        data->next = global_socket_list;
        global_socket_list = data;
        
        wait_queue_init(&data->wait_queue);
        node->device = data;
    }

    return node;
}
