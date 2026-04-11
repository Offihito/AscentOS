#include "net/icmp.h"
#include "net/ipv4.h"
#include "net/checksum.h"
#include "net/byteorder.h"
#include "lib/string.h"
#include "console/console.h"

uint64_t icmp_reply_count = 0;

void icmp_handle_packet(const uint8_t *data, uint16_t len, uint32_t src_ip) {
    if (len < sizeof(icmp_header_t)) return;

    icmp_header_t *hdr = (icmp_header_t *)data;

    // Verify Checksum
    uint16_t received_checksum = hdr->checksum;
    hdr->checksum = 0;
    if (calculate_checksum(hdr, len) != received_checksum) {
        return; // Invalid checksum
    }
    hdr->checksum = received_checksum;

    if (hdr->type == ICMP_TYPE_ECHO_REQUEST) {
        // ... (Echo Request handling unchanged) ...
        // (I'll provide the full function below for clarity)
        icmp_header_t reply_hdr;
        reply_hdr.type = ICMP_TYPE_ECHO_REPLY;
        reply_hdr.code = 0;
        reply_hdr.id = hdr->id;
        reply_hdr.sequence = hdr->sequence;
        reply_hdr.checksum = 0;

        uint16_t data_len = len - sizeof(icmp_header_t);
        uint8_t packet[sizeof(icmp_header_t) + data_len];
        memcpy(packet, &reply_hdr, sizeof(icmp_header_t));
        if (data_len > 0) {
            memcpy(packet + sizeof(icmp_header_t), data + sizeof(icmp_header_t), data_len);
        }

        ((icmp_header_t*)packet)->checksum = calculate_checksum(packet, sizeof(icmp_header_t) + data_len);
        ipv4_send_packet(src_ip, PROTO_ICMP, packet, sizeof(icmp_header_t) + data_len);

    } else if (hdr->type == ICMP_TYPE_ECHO_REPLY) {
        icmp_reply_count++;
        console_puts("Received ICMP Echo Reply from ");
        // (Optional: Print IP here if desired, but shell loop will detect counter change)
    }
}

int icmp_send_echo(uint32_t dst_ip, uint16_t id, uint16_t sequence, const void *data, uint16_t len) {
    icmp_header_t hdr;
    hdr.type = ICMP_TYPE_ECHO_REQUEST;
    hdr.code = 0;
    hdr.id = htons(id);
    hdr.sequence = htons(sequence);
    hdr.checksum = 0;

    uint8_t packet[sizeof(icmp_header_t) + len];
    memcpy(packet, &hdr, sizeof(icmp_header_t));
    if (len > 0) {
        memcpy(packet + sizeof(icmp_header_t), data, len);
    }

    ((icmp_header_t*)packet)->checksum = calculate_checksum(packet, sizeof(icmp_header_t) + len);

    return ipv4_send_packet(dst_ip, PROTO_ICMP, packet, sizeof(icmp_header_t) + len);
}
