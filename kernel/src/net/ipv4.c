#include "net/ipv4.h"
#include "net/ethernet.h"
#include "net/netif.h"
#include "net/arp.h"
#include "net/checksum.h"
#include "net/byteorder.h"
#include "net/icmp.h"
#include "net/udp.h"
#include "net/tcp.h"
#include "lib/string.h"
#include "console/console.h"

static uint16_t next_id = 1;

void ipv4_handle_packet(const uint8_t *data, uint16_t len) {
    if (len < sizeof(ipv4_header_t)) return;

    ipv4_header_t *hdr = (ipv4_header_t *)data;

    // Check version (IPv4 = 4)
    if ((hdr->version_ihl >> 4) != 4) return;

    // Check header length (at least 20 bytes, i.e., IHL >= 5)
    uint8_t ihl = (hdr->version_ihl & 0x0F) * 4;
    if (len < ihl) return;

    // Verify Checksum
    uint16_t received_checksum = hdr->checksum;
    hdr->checksum = 0;
    if (calculate_checksum(hdr, ihl) != received_checksum) {
        return; // Invalid checksum
    }
    hdr->checksum = received_checksum;

    netif_t *nif = netif_get();
    uint32_t dst_ip = ntohl(hdr->dst_ip);

    // Filter packets not for us (and not broadcast)
    if (dst_ip != nif->ip && dst_ip != 0xFFFFFFFF) {
        return;
    }

    const uint8_t *payload = data + ihl;
    uint16_t payload_len = ntohs(hdr->length) - ihl;

    switch (hdr->protocol) {
        case PROTO_ICMP:
            icmp_handle_packet(payload, payload_len, ntohl(hdr->src_ip));
            break;
        case PROTO_UDP:
            udp_handle_packet(payload, payload_len, ntohl(hdr->src_ip), dst_ip);
            break;
        case PROTO_TCP:
            tcp_handle_packet(payload, payload_len, ntohl(hdr->src_ip), dst_ip);
            break;
        default:
            // Ignore protocols we don't handle
            break;
    }
}

int ipv4_send_packet(uint32_t dst_ip, uint8_t protocol, const void *data, uint16_t len) {
    netif_t *nif = netif_get();
    if (!nif->up) return -1;

    // Routing: if destination is off-subnet, send via gateway
    uint32_t next_hop = dst_ip;
    if (dst_ip != 0xFFFFFFFF && (dst_ip & nif->netmask) != (nif->ip & nif->netmask)) {
        next_hop = nif->gateway;
    }

    uint8_t dst_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

    if (dst_ip != 0xFFFFFFFF) {
        // Determine next-hop MAC via ARP
        const arp_entry_t *entry = arp_lookup(next_hop);
        if (!entry) {
            arp_send_request(next_hop);
            return -1;
        }
        memcpy(dst_mac, entry->mac, 6);
    }

    ipv4_header_t hdr;
    hdr.version_ihl = (4 << 4) | 5; // Ver 4, IHL 5 (20 bytes)
    hdr.tos = 0;
    hdr.length = htons(sizeof(ipv4_header_t) + len);
    hdr.id = htons(next_id++);
    hdr.flags_offset = 0;
    hdr.ttl = 64;
    hdr.protocol = protocol;
    hdr.src_ip = htonl(nif->ip);
    hdr.dst_ip = htonl(dst_ip);
    hdr.checksum = 0;
    hdr.checksum = calculate_checksum(&hdr, sizeof(ipv4_header_t));

    // Allocate buffer for frame
    uint8_t packet[sizeof(ipv4_header_t) + len];
    memcpy(packet, &hdr, sizeof(ipv4_header_t));
    if (len > 0 && data) {
        memcpy(packet + sizeof(ipv4_header_t), data, len);
    }

    return eth_send_frame(dst_mac, ETHERTYPE_IPV4, packet, sizeof(ipv4_header_t) + len);
}
