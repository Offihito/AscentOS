#include "net/tcp.h"
#include "net/ipv4.h"
#include "net/byteorder.h"
#include "net/netif.h"
#include "net/net.h"
#include "lib/string.h"
#include "console/console.h"

static tcp_socket_t sockets[MAX_TCP_SOCKETS];
static uint16_t next_local_port = 45000;

void tcp_init(void) {
    memset(sockets, 0, sizeof(sockets));
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
    sock->valid = true;
    sock->state = TCP_STATE_SYN_SENT;
    
    netif_t *nif = netif_get();
    sock->local_ip = nif ? nif->ip : 0;
    
    sock->local_port = next_local_port++;
    if (next_local_port > 60000) next_local_port = 45000;
    
    sock->remote_ip = ip;
    sock->remote_port = port;
    sock->seq_num = 0xAA00BB00; // Arbitrary ISN
    sock->ack_num = 0;
    sock->recv_callback = on_recv;

    int attempts = 5;
    while (attempts--) {
        tcp_send_segment(sock, TCP_FLAG_SYN, NULL, 0);

        // Wait for Handshake up to 1 second before retrying
        for (int wait = 0; wait < 1000; wait++) {
            net_poll();
            if (sock->state == TCP_STATE_ESTABLISHED) {
                return sock_id;
            }
            for (volatile int d = 0; d < 10000; d++); // ~1ms
        }
    }

    sock->valid = false;
    return -1; // Timeout
}

int tcp_send(int sock_id, const void *data, uint16_t len) {
    if (sock_id < 0 || sock_id >= MAX_TCP_SOCKETS) return -1;
    tcp_socket_t *sock = &sockets[sock_id];
    if (!sock->valid || sock->state != TCP_STATE_ESTABLISHED) return -1;

    uint32_t start_seq = sock->seq_num;
    int attempts = 5;
    while (attempts--) {
        tcp_send_segment(sock, TCP_FLAG_ACK | TCP_FLAG_PSH, data, len);

        // Wait for ACK
        for (int wait = 0; wait < 1000; wait++) {
            net_poll();
            if (sock->seq_num == start_seq + len) {
                return len; // Successfully ACKed
            }
            if (sock->state != TCP_STATE_ESTABLISHED) {
                return -1; // Socket closed prematurely
            }
            for (volatile int d = 0; d < 10000; d++);
        }
    }
    return -1; // Timeout
}

void tcp_close(int sock_id) {
    if (sock_id < 0 || sock_id >= MAX_TCP_SOCKETS) return;
    tcp_socket_t *sock = &sockets[sock_id];
    if (!sock->valid) return;

    if (sock->state == TCP_STATE_ESTABLISHED) {
        tcp_send_segment(sock, TCP_FLAG_FIN | TCP_FLAG_ACK, NULL, 0);
        sock->state = TCP_STATE_FIN_WAIT1;
        
        for (int wait = 0; wait < 3000; wait++) {
            net_poll();
            if (sock->state == TCP_STATE_CLOSED) break;
            for (volatile int d = 0; d < 10000; d++);
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
    for (int i = 0; i < MAX_TCP_SOCKETS; i++) {
        if (sockets[i].valid && sockets[i].local_port == dst_port && sockets[i].remote_ip == src_ip && sockets[i].remote_port == src_port) {
            sock = &sockets[i];
            break;
        }
    }

    if (!sock) return;

    // Reject packet if RST
    if (hdr->flags & TCP_FLAG_RST) {
        sock->state = TCP_STATE_CLOSED;
        return;
    }

    switch (sock->state) {
        case TCP_STATE_SYN_SENT:
            if ((hdr->flags & (TCP_FLAG_SYN | TCP_FLAG_ACK)) == (TCP_FLAG_SYN | TCP_FLAG_ACK)) {
                sock->ack_num = seq + 1; // Expected next seq from them
                sock->seq_num++;         // Consume our SYN's space
                sock->state = TCP_STATE_ESTABLISHED;
                tcp_send_segment(sock, TCP_FLAG_ACK, NULL, 0);
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
                    if (sock->recv_callback) {
                        sock->recv_callback(data, data_len);
                    }
                    sock->ack_num += data_len;
                    tcp_send_segment(sock, TCP_FLAG_ACK, NULL, 0);
                } else if ((int32_t)(seq - sock->ack_num) < 0) {
                    // Duplicate or old packet, ACK our expected sequence number again
                    tcp_send_segment(sock, TCP_FLAG_ACK, NULL, 0);
                }
            }

            if (hdr->flags & TCP_FLAG_FIN) {
                sock->ack_num++; // Consume FIN
                tcp_send_segment(sock, TCP_FLAG_ACK, NULL, 0);
                sock->state = TCP_STATE_CLOSED; // Simplified closure
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
