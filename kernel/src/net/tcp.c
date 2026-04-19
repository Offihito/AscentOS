#include "net/tcp.h"
#include "net/ipv4.h"
#include "net/byteorder.h"
#include "net/netif.h"
#include "net/net.h"
#include "lib/string.h"
#include "console/console.h"
#include "sched/sched.h"
#include "drivers/timer/pit.h"

static inline uint32_t tcp_generate_isn(void) {
    uint64_t tsc;
    __asm__ volatile("rdtsc" : "=A"(tsc));
    static uint32_t pseudo_random = 0x98765432;
    pseudo_random ^= pseudo_random << 13;
    pseudo_random ^= pseudo_random >> 17;
    pseudo_random ^= pseudo_random << 5;
    return (uint32_t)(tsc ^ pseudo_random);
}

static tcp_socket_t sockets[MAX_TCP_SOCKETS];
static uint16_t next_local_port = 45000;

void tcp_init(void) {
    memset(sockets, 0, sizeof(sockets));
    for (int i = 0; i < MAX_TCP_SOCKETS; i++) {
        wait_queue_init(&sockets[i].wait_queue);
    }
}

static uint16_t tcp_calculate_checksum(uint32_t src_ip, uint32_t dst_ip, const uint8_t *tcp_segment, uint16_t tcp_len) {
    uint32_t sum = 0;
    
    // Pseudo Header
    uint32_t sip = htonl(src_ip);
    uint32_t dip = htonl(dst_ip);
    sum += (sip >> 16) + (sip & 0xFFFF);
    sum += (dip >> 16) + (dip & 0xFFFF);
    sum += htons(PROTO_TCP);
    sum += htons(tcp_len);
    
    // TCP Segment
    const uint16_t *buf = (const uint16_t *)tcp_segment;
    int len = tcp_len;
    while (len > 1) {
        sum += *buf++;
        len -= 2;
    }
    
    if (len > 0) {
        sum += *(const uint8_t *)buf;
    }
    
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    
    return ~sum;
}

static void tcp_send_segment(tcp_socket_t *sock, uint8_t flags, const void *data, uint16_t len) {
    uint16_t total_len = sizeof(tcp_header_t) + len;
    uint8_t packet[total_len];
    memset(packet, 0, total_len);

    tcp_header_t *hdr = (tcp_header_t *)packet;
    hdr->src_port = htons(sock->local_port);
    hdr->dst_port = htons(sock->remote_port);
    hdr->seq_num = htonl(sock->seq_num);
    hdr->ack_num = (flags & TCP_FLAG_ACK) ? htonl(sock->ack_num) : 0;
    hdr->data_offset = (sizeof(tcp_header_t) / 4) << 4; // Length in 32-bit words
    hdr->flags = flags;
    hdr->window = htons(8192); // Basic window
    hdr->checksum = 0;
    hdr->urgent_ptr = 0;

    if (len > 0 && data) {
        memcpy(packet + sizeof(tcp_header_t), data, len);
    }

    hdr->checksum = tcp_calculate_checksum(sock->local_ip, sock->remote_ip, packet, total_len);

    ipv4_send_packet(sock->remote_ip, PROTO_TCP, packet, total_len);
}

int tcp_listen(uint16_t port, tcp_recv_cb_t on_recv) {
    int sock_id = -1;
    for (int i = 0; i < MAX_TCP_SOCKETS; i++) {
        if (!sockets[i].valid) {
            sock_id = i;
            break;
        }
    }
    if (sock_id == -1) return -1;

    tcp_socket_t *sock = &sockets[sock_id];
    memset(sock, 0, sizeof(tcp_socket_t));
    sock->valid = true;
    sock->state = TCP_STATE_LISTEN;
    
    netif_t *nif = netif_get();
    sock->local_ip = nif ? nif->ip : 0;
    sock->local_port = port;
    sock->recv_callback = on_recv;
    sock->parent_sock_id = -1;
    
    return sock_id;
}

int tcp_connect(uint32_t ip, uint16_t port, tcp_recv_cb_t on_recv) {
    int sock_id = -1;
    for (int i = 0; i < MAX_TCP_SOCKETS; i++) {
        if (!sockets[i].valid) {
            sock_id = i;
            break;
        }
    }
    if (sock_id == -1) return -1;

    tcp_socket_t *sock = &sockets[sock_id];
    memset(sock, 0, sizeof(tcp_socket_t));
    sock->parent_sock_id = -1;
    sock->valid = true;
    sock->state = TCP_STATE_SYN_SENT;
    
    netif_t *nif = netif_get();
    sock->local_ip = nif ? nif->ip : 0;
    
    sock->local_port = next_local_port++;
    if (next_local_port > 60000) next_local_port = 45000;
    
    sock->remote_ip = ip;
    sock->remote_port = port;
    sock->seq_num = tcp_generate_isn();
    sock->ack_num = 0;
    sock->recv_callback = on_recv;

    wait_queue_entry_t wq_entry;
    struct thread *current = sched_get_current();
    if (current) {
        wq_entry.thread = current;
        wq_entry.next = NULL;
        wait_queue_add(&sock->wait_queue, &wq_entry);
    }

    uint64_t start_ticks = pit_get_ticks();
    uint64_t last_retransmit = start_ticks;

    tcp_send_segment(sock, TCP_FLAG_SYN, NULL, 0);

    while (sock->state == TCP_STATE_SYN_SENT && sock->valid) {
        net_poll();
        uint64_t now = pit_get_ticks();
        
        if (now - start_ticks > 5000) { // 5 seconds timeout
            break;
        }

        if (now - last_retransmit > 1000) { // retransmit every 1 sec
            tcp_send_segment(sock, TCP_FLAG_SYN, NULL, 0);
            last_retransmit = now;
        }

        if (current && sock->state == TCP_STATE_SYN_SENT) {
            current->state = THREAD_BLOCKED;
            current->wakeup_ticks = now + 10;
            sched_yield();
        } else if (!current) {
            for (volatile int d = 0; d < 10000; d++);
        }
    }

    if (current) {
        wait_queue_remove(&sock->wait_queue, &wq_entry);
    }

    if (sock->state == TCP_STATE_ESTABLISHED) {
        return sock_id;
    }

    sock->valid = false;
    return -1; // Timeout
}

int tcp_send(int sock_id, const void *data, uint16_t len) {
    if (sock_id < 0 || sock_id >= MAX_TCP_SOCKETS) return -1;
    tcp_socket_t *sock = &sockets[sock_id];
    if (!sock->valid || sock->state != TCP_STATE_ESTABLISHED) return -1;

    uint32_t start_seq = sock->seq_num;
    
    wait_queue_entry_t wq_entry;
    struct thread *current = sched_get_current();
    if (current) {
        wq_entry.thread = current;
        wq_entry.next = NULL;
        wait_queue_add(&sock->wait_queue, &wq_entry);
    }

    uint64_t start_ticks = pit_get_ticks();
    uint64_t last_retransmit = start_ticks;

    tcp_send_segment(sock, TCP_FLAG_ACK | TCP_FLAG_PSH, data, len);

    while (sock->seq_num != start_seq + len && sock->state == TCP_STATE_ESTABLISHED && sock->valid) {
        net_poll();
        uint64_t now = pit_get_ticks();

        if (now - start_ticks > 5000) { // 5 sec timeout
            break;
        }

        if (now - last_retransmit > 1000) { // retransmit every 1 sec
            tcp_send_segment(sock, TCP_FLAG_ACK | TCP_FLAG_PSH, data, len);
            last_retransmit = now;
        }

        if (current && sock->seq_num != start_seq + len && sock->state == TCP_STATE_ESTABLISHED) {
            current->state = THREAD_BLOCKED;
            current->wakeup_ticks = now + 10;
            sched_yield();
        } else if (!current) {
            for (volatile int d = 0; d < 10000; d++);
        }
    }

    if (current) {
        wait_queue_remove(&sock->wait_queue, &wq_entry);
    }

    if (sock->seq_num == start_seq + len) {
        return len; // Successfully ACKed
    }
    return -1; // Timeout or Socket closed
}

void tcp_close(int sock_id) {
    if (sock_id < 0 || sock_id >= MAX_TCP_SOCKETS) return;
    tcp_socket_t *sock = &sockets[sock_id];
    if (!sock->valid) return;

    if (sock->state == TCP_STATE_ESTABLISHED) {
        tcp_send_segment(sock, TCP_FLAG_FIN | TCP_FLAG_ACK, NULL, 0);
        sock->state = TCP_STATE_FIN_WAIT1;
        
        wait_queue_entry_t wq_entry;
        struct thread *current = sched_get_current();
        if (current) {
            wq_entry.thread = current;
            wq_entry.next = NULL;
            wait_queue_add(&sock->wait_queue, &wq_entry);
        }

        uint64_t start_ticks = pit_get_ticks();
        uint64_t last_retransmit = start_ticks;

        while (sock->state != TCP_STATE_CLOSED && sock->valid) {
            net_poll();
            uint64_t now = pit_get_ticks();

            if (now - start_ticks > 3000) {
                break;
            }

            if ((sock->state == TCP_STATE_FIN_WAIT1 || sock->state == TCP_STATE_FIN_WAIT2) && now - last_retransmit > 1000) {
                tcp_send_segment(sock, TCP_FLAG_FIN | TCP_FLAG_ACK, NULL, 0);
                last_retransmit = now;
            }

            if (current && sock->state != TCP_STATE_CLOSED) {
                current->state = THREAD_BLOCKED;
                current->wakeup_ticks = now + 10;
                sched_yield();
            } else if (!current) {
                for (volatile int d = 0; d < 10000; d++);
            }
        }

        if (current) {
            wait_queue_remove(&sock->wait_queue, &wq_entry);
        }
    }
    
    sock->valid = false;
}

void tcp_handle_packet(const uint8_t *payload, uint16_t length, uint32_t src_ip, uint32_t dst_ip) {
    if (length < sizeof(tcp_header_t)) return;

    const tcp_header_t *hdr = (const tcp_header_t *)payload;
    uint16_t src_port = ntohs(hdr->src_port);
    uint16_t dst_port = ntohs(hdr->dst_port);
    uint32_t seq = ntohl(hdr->seq_num);
    uint32_t ack = ntohl(hdr->ack_num);
    uint8_t data_offset = (hdr->data_offset >> 4) * 4;
    
    if (data_offset < sizeof(tcp_header_t) || data_offset > length) return;

    uint16_t data_len = length - data_offset;
    const uint8_t *data = payload + data_offset;

    tcp_socket_t *sock = NULL;
    int listen_sock_idx = -1;
    
    for (int i = 0; i < MAX_TCP_SOCKETS; i++) {
        if (sockets[i].valid && sockets[i].local_port == dst_port) {
            if (sockets[i].remote_ip == src_ip && sockets[i].remote_port == src_port) {
                sock = &sockets[i];
                break;
            } else if (sockets[i].state == TCP_STATE_LISTEN) {
                listen_sock_idx = i;
            }
        }
    }

    if (!sock && listen_sock_idx >= 0) {
        if ((hdr->flags & TCP_FLAG_SYN) && !(hdr->flags & TCP_FLAG_ACK)) {
            int new_sock_idx = -1;
            for (int i = 0; i < MAX_TCP_SOCKETS; i++) {
                if (!sockets[i].valid) {
                    new_sock_idx = i;
                    break;
                }
            }
            if (new_sock_idx >= 0) {
                tcp_socket_t *listen_sock = &sockets[listen_sock_idx];
                if (listen_sock->accept_count < TCP_MAX_BACKLOG) {
                    tcp_socket_t *new_sock = &sockets[new_sock_idx];
                    memset(new_sock, 0, sizeof(tcp_socket_t));
                    new_sock->valid = true;
                    new_sock->state = TCP_STATE_SYN_RCVD;
                    new_sock->local_ip = listen_sock->local_ip;
                    new_sock->local_port = listen_sock->local_port;
                    new_sock->remote_ip = src_ip;
                    new_sock->remote_port = src_port;
                    new_sock->seq_num = tcp_generate_isn();
                    new_sock->ack_num = seq + 1;
                    new_sock->recv_callback = listen_sock->recv_callback;
                    new_sock->parent_sock_id = listen_sock_idx;
                    
                    tcp_send_segment(new_sock, TCP_FLAG_SYN | TCP_FLAG_ACK, NULL, 0);
                    wait_queue_wake_all(&listen_sock->wait_queue);
                    return;
                }
            }
        }
    }

    if (!sock) return;

    // Reject packet if RST
    if (hdr->flags & TCP_FLAG_RST) {
        sock->state = TCP_STATE_CLOSED;
        wait_queue_wake_all(&sock->wait_queue);
        return;
    }

    switch (sock->state) {
        case TCP_STATE_SYN_SENT:
            if ((hdr->flags & (TCP_FLAG_SYN | TCP_FLAG_ACK)) == (TCP_FLAG_SYN | TCP_FLAG_ACK)) {
                sock->ack_num = seq + 1; // Expected next seq from them
                sock->seq_num++;         // Consume our SYN's space
                sock->state = TCP_STATE_ESTABLISHED;
                tcp_send_segment(sock, TCP_FLAG_ACK, NULL, 0);
                wait_queue_wake_all(&sock->wait_queue);
            }
            break;

        case TCP_STATE_SYN_RCVD:
            if (hdr->flags & TCP_FLAG_ACK) {
                sock->state = TCP_STATE_ESTABLISHED;
                sock->seq_num = ack;
                
                if (sock->parent_sock_id >= 0) {
                    tcp_socket_t *psock = &sockets[sock->parent_sock_id];
                    if (psock->accept_count < TCP_MAX_BACKLOG) {
                        psock->accept_queue[psock->accept_count++] = (sock - sockets);
                    }
                }
                
                if (data_len > 0) {
                    if (sock->recv_callback) {
                        sock->recv_callback(sock - sockets, data, data_len);
                    }
                    sock->ack_num += data_len;
                    wait_queue_wake_all(&sock->wait_queue);
                }
            }
            break;

        case TCP_STATE_ESTABLISHED:
            if (hdr->flags & TCP_FLAG_ACK) {
                // If the ACK advances our sent window, update our seq_num
                if ((int32_t)(ack - sock->seq_num) > 0) {
                    sock->seq_num = ack;
                }
            }

            if (data_len > 0) {
                if (seq == sock->ack_num) {
                    // In-order data
                    tcp_send_segment(sock, TCP_FLAG_ACK, NULL, 0);
                    wait_queue_wake_all(&sock->wait_queue);
                } else if ((int32_t)(seq - sock->ack_num) < 0) {
                    // Duplicate or old packet, ACK our expected sequence number again
                    tcp_send_segment(sock, TCP_FLAG_ACK, NULL, 0);
                }
            }

            if (hdr->flags & TCP_FLAG_FIN) {
                sock->ack_num++; // Consume FIN
                tcp_send_segment(sock, TCP_FLAG_ACK, NULL, 0);
                sock->state = TCP_STATE_CLOSED; // Simplified closure
                wait_queue_wake_all(&sock->wait_queue);
            }
            break;

        case TCP_STATE_FIN_WAIT1:
            if (hdr->flags & TCP_FLAG_ACK) {
                sock->seq_num = ack;
                sock->state = TCP_STATE_FIN_WAIT2;
            }
            if (hdr->flags & TCP_FLAG_FIN) {
                sock->ack_num++;
                tcp_send_segment(sock, TCP_FLAG_ACK, NULL, 0);
                sock->state = TCP_STATE_CLOSED;
            }
            break;

        case TCP_STATE_FIN_WAIT2:
            if (hdr->flags & TCP_FLAG_FIN) {
                sock->ack_num++;
                tcp_send_segment(sock, TCP_FLAG_ACK, NULL, 0);
                sock->state = TCP_STATE_CLOSED;
            }
            break;

        default:
            break;
    }
}

int tcp_accept(int sock_id) {
    if (sock_id < 0 || sock_id >= MAX_TCP_SOCKETS) return -1;
    tcp_socket_t *sock = &sockets[sock_id];
    if (!sock->valid || sock->state != TCP_STATE_LISTEN) return -1;

    if (sock->accept_count > 0) {
        int new_sock_id = sock->accept_queue[0];
        for (int i = 1; i < sock->accept_count; i++) {
            sock->accept_queue[i - 1] = sock->accept_queue[i];
        }
        sock->accept_count--;
        return new_sock_id;
    }
    return -1;
}

int tcp_get_remote_info(int sock_id, uint32_t *ip, uint16_t *port) {
    if (sock_id < 0 || sock_id >= MAX_TCP_SOCKETS) return -1;
    tcp_socket_t *sock = &sockets[sock_id];
    if (!sock->valid) return -1;
    
    if (ip) *ip = sock->remote_ip;
    if (port) *port = sock->remote_port;
    return 0;
}
