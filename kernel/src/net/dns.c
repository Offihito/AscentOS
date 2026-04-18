#include "net/dns.h"
#include "net/udp.h"
#include "net/net.h"
#include "net/byteorder.h"
#include "net/netif.h"
#include "lib/string.h"
#include "console/console.h"

// QEMU user-mode network built-in DNS server (10.0.2.3)
#define DEFAULT_DNS_SERVER IP4(10, 0, 2, 3)

static uint16_t next_dns_id = 0x1000;
static uint16_t next_local_port = 50000;

static volatile bool dns_resolved = false;
static uint32_t dns_resolved_ip = 0;

static void dns_recv_cb(uint16_t local_port, const uint8_t *payload, uint16_t length, uint32_t src_ip, uint16_t src_port) {
    (void)local_port;
    (void)src_ip;
    if (src_port != DNS_PORT) return;
    console_puts("[DNS] Received response packet!\n");
    if (length < sizeof(dns_header_t)) return;

    const dns_header_t *hdr = (const dns_header_t *)payload;
    // Check if it's a response (QR bit is MSB of flags in big-endian, so 0x8000)
    if (!(ntohs(hdr->flags) & 0x8000)) return; 

    uint16_t ans_count = ntohs(hdr->ans_count);
    if (ans_count == 0) {
        return; // No answers
    }

    const uint8_t *p = payload + sizeof(dns_header_t);
    const uint8_t *end = payload + length;

    // Skip question section (assuming 1 question, as we send)
    while (p < end && *p != 0) {
        p++;
    }
    p += 1 + 4; // Skip null byte, Type (2), and Class (2)

    // Rough parser for the Answer section
    for (int i = 0; i < ans_count; i++) {
        if (p >= end) break;
        
        // Skip Name (can be a 16-bit pointer starting with 11, or sequence of labels)
        if ((*p & 0xC0) == 0xC0) {
            p += 2;
        } else {
            while (p < end && *p != 0) p++;
            p++;
        }

        if (p + 10 > end) break;

        uint16_t type = (p[0] << 8) | p[1];
        uint16_t dclass = (p[2] << 8) | p[3];
        // ttl = 4 bytes (p[4..7])
        uint16_t data_len = (p[8] << 8) | p[9];
        p += 10;

        if (type == 1 && dclass == 1 && data_len == 4 && p + 4 <= end) {
            // Got an A record!
            dns_resolved_ip = IP4(p[0], p[1], p[2], p[3]); // Host byte order
            dns_resolved = true;
            return;
        }
        
        p += data_len;
    }
}

static void format_dns_name(uint8_t *qname, const char *host) {
    uint8_t *len_byte = qname++;
    int count = 0;
    while (*host) {
        if (*host == '.') {
            *len_byte = count;
            len_byte = qname++;
            count = 0;
        } else {
            *qname++ = *host;
            count++;
        }
        host++;
    }
    *len_byte = count;
    *qname++ = 0;
}

int dns_resolve_A_record(const char *domain, uint32_t *out_ip) {
    if (!domain || !out_ip) return -1;

    dns_resolved = false;
    dns_resolved_ip = 0;

    uint16_t local_port = next_local_port++;
    if (next_local_port > 60000) next_local_port = 50000;

    udp_bind(local_port, dns_recv_cb);

    uint8_t packet[256];
    memset(packet, 0, sizeof(packet));

    dns_header_t *hdr = (dns_header_t *)packet;
    hdr->id = htons(next_dns_id++); // Incrementing ID
    hdr->flags = htons(0x0100); // Standard Query, Recursion Desired
    hdr->q_count = htons(1);

    uint8_t *qname = packet + sizeof(dns_header_t);
    format_dns_name(qname, domain);

    uint8_t *qinfo = qname + strlen(domain) + 2;
    // Type A (1)
    qinfo[0] = 0;
    qinfo[1] = 1;
    // Class IN (1)
    qinfo[2] = 0;
    qinfo[3] = 1;

    uint16_t packet_len = (qinfo - packet) + 4;

    uint32_t target_dns = DEFAULT_DNS_SERVER;
    
    // We attempt to send a few times for ARPs to settle
    int attempts = 5;
    while (attempts--) {
        if (udp_send_packet(target_dns, local_port, DNS_PORT, packet, packet_len) == 0) {
            break; // Send technically succeeded (might still be dropped or no response, but we sent it)
        }
        
        // Give ARP a moment
        for (int d = 0; d < 1000; d++) {
            net_poll();
            for(volatile int k=0; k<1000; k++);
        }
    }

    // Wait for reply via net_poll loop
    for (int wait = 0; wait < 6000; wait++) {
        net_poll();
        if (dns_resolved) {
            *out_ip = dns_resolved_ip;
            udp_unbind(local_port);
            return 0;
        }
        // ~1ms delay
        for (volatile int d = 0; d < 10000; d++); 
    }

    udp_unbind(local_port);
    return -1; // Timeout
}
