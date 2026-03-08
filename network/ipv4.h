// ipv4.h — AscentOS IPv4 Katmanı (Aşama 3)
// RFC 791 implementasyonu
//
// Bağımlılıklar: arp.h, rtl8139.h
// Kullanım:
//   ipv4_init()                              → katmanı başlat
//   ipv4_handle_packet(frame, len)           → net_packet_callback'ten çağır (EtherType 0x0800)
//   ipv4_send(dst_ip, proto, payload, plen)  → IPv4 paketi gönder
//   ipv4_register_handler(proto, fn)         → üst katman protokolü kaydet (ICMP, UDP…)

#ifndef IPV4_H
#define IPV4_H

#include <stdint.h>
#include <stdbool.h>

// ============================================================================
// IPv4 sabitleri
// ============================================================================
#define IPV4_VERSION        4
#define IPV4_IHL_MIN        5          // 20 byte, opsiyonsuz
#define IPV4_HDR_LEN        20         // Minimum başlık uzunluğu (byte)
#define IPV4_TTL_DEFAULT    64         // Varsayılan yaşam süresi
#define IPV4_FLAG_DF        0x4000     // Don't Fragment (network byte order ham)
#define ETHERTYPE_IPV4      0x0800

// Protokol numaraları
#define IP_PROTO_ICMP       1
#define IP_PROTO_TCP        6
#define IP_PROTO_UDP        17

// Maksimum kayıtlı üst katman sayısı
#define IPV4_MAX_HANDLERS   8

// ============================================================================
// IPv4 başlığı (20 byte, big-endian alanlar)
// ============================================================================
typedef struct {
    uint8_t  ver_ihl;      // [7:4]=version(4), [3:0]=IHL (kelime sayısı, min 5)
    uint8_t  tos;          // Differentiated Services / ToS
    uint16_t total_len;    // Başlık + veri toplam uzunluğu (network byte order)
    uint16_t id;           // Parçalama kimliği
    uint16_t flags_frag;   // [15:13]=flags, [12:0]=fragment offset
    uint8_t  ttl;          // Time to Live
    uint8_t  protocol;     // Üst protokol (1=ICMP, 6=TCP, 17=UDP)
    uint16_t checksum;     // Başlık sağlama toplamı (RFC 791)
    uint8_t  src[4];       // Kaynak IP adresi
    uint8_t  dst[4];       // Hedef IP adresi
} __attribute__((packed)) IPv4Header;

// ============================================================================
// Üst katman protokol handler callback tipi
// src_ip  : gönderenin IP adresi (4 byte)
// payload : IPv4 veri alanının başı (ICMP/UDP/… başlığı dahil)
// len     : veri uzunluğu (byte)
// ============================================================================
typedef void (*ipv4_proto_handler_t)(const uint8_t src_ip[4],
                                     const uint8_t* payload,
                                     uint16_t len);

// ============================================================================
// Public API
// ============================================================================

// IPv4 katmanını başlat (ARP başlatıldıktan sonra çağrılmalı)
void ipv4_init(void);

// Gelen Ethernet çerçevesini işle
// net_packet_callback içinde EtherType 0x0800 ise bu fonksiyon çağrılır.
void ipv4_handle_packet(const uint8_t* frame, uint16_t len);

// IPv4 paketi gönder
// dst_ip    : 4-byte hedef IP (ARP ile MAC çözümlenir)
// protocol  : IP_PROTO_ICMP, IP_PROTO_UDP, …
// payload   : IPv4 veri alanı (ICMP / UDP gövdesi)
// plen      : veri uzunluğu (byte)
// Dönüş     : true = gönderildi, false = ARP henüz çözümlenmedi (PENDING)
bool ipv4_send(const uint8_t dst_ip[4], uint8_t protocol,
               const uint8_t* payload, uint16_t plen);

// Üst katman protokol handler'ı kaydet
// Aynı protokol numarası için tekrar çağrılırsa üzerine yazar.
void ipv4_register_handler(uint8_t protocol, ipv4_proto_handler_t handler);

// IPv4 başlık sağlama toplamı hesapla (RFC 791 one's complement)
// Gönderim sırasında checksum=0 ile çağrıl, sonucu başlığa yaz.
uint16_t ipv4_checksum(const void* data, uint16_t len);

// IPv4 katmanının başlatılıp başlatılmadığını sorgula
bool ipv4_is_initialized(void);

// Gateway ve subnet mask ayarla
void ipv4_set_gateway(const uint8_t gw[4]);
void ipv4_set_subnet(const uint8_t mask[4]);
void ipv4_get_gateway(uint8_t out[4]);

uint32_t ipv4_get_tx_count(void);
uint32_t ipv4_get_rx_count(void);

#endif // IPV4_H