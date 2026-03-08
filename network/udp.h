// udp.h — AscentOS UDP Katmanı (Aşama 4)
// RFC 768 — User Datagram Protocol
//
// Katman mimarisi:
//   rtl8139 → ipv4_handle_packet → udp_handle_packet → bağlı port handler'ı
//   Gönderim: udp_send → ipv4_send → rtl8139_send
//
// Kullanım:
//   udp_init()                              → UDP katmanını başlat (ipv4_init'ten sonra)
//   udp_bind(port, handler)                 → Porta gelen paketler için callback kaydet
//   udp_unbind(port)                        → Port bağını kaldır
//   udp_send(dst_ip, dst_port, src_port,    → UDP datagramı gönder
//            data, len)
//   udp_broadcast(dst_port, src_port,       → Ağa broadcast gönder
//                 data, len)

#ifndef UDP_H
#define UDP_H

#include <stdint.h>
#include <stdbool.h>

// ============================================================================
// UDP sabitleri
// ============================================================================
#define UDP_HDR_LEN         8       // UDP başlık uzunluğu (byte) — daima sabit
#define UDP_MAX_PAYLOAD     (1500 - 20 - 8)  // MTU - IPv4 hdr - UDP hdr = 1472
#define UDP_MAX_SOCKETS     16      // Aynı anda açık port sayısı

// Önceden tanımlı iyi bilinen portlar (referans için)
#define UDP_PORT_DHCP_SERVER    67
#define UDP_PORT_DHCP_CLIENT    68
#define UDP_PORT_DNS            53
#define UDP_PORT_NTP            123
#define UDP_PORT_SYSLOG         514

// ============================================================================
// UDP başlığı (8 byte, big-endian)
// RFC 768 §1
// ============================================================================
typedef struct {
    uint16_t src_port;      // Kaynak port (opsiyonel; 0 = belirtilmemiş)
    uint16_t dst_port;      // Hedef port
    uint16_t length;        // UDP başlık + veri uzunluğu (min 8)
    uint16_t checksum;      // Pseudo-başlık + UDP üzerinde (0 = devre dışı)
} __attribute__((packed)) UDPHeader;

// ============================================================================
// Gelen UDP paketi bilgisi — handler'a iletilir
// ============================================================================
typedef struct {
    uint8_t  src_ip[4];     // Gönderenin IP adresi
    uint16_t src_port;      // Gönderenin kaynak portu
    uint16_t dst_port;      // Bizim hedef portumuz
    const uint8_t* data;    // Veri başlangıcı (UDP başlığından sonrası)
    uint16_t len;           // Veri uzunluğu (byte)
} UDPPacket;

// ============================================================================
// Port handler callback tipi
// Gelen her UDP paketi için çağrılır.
// pkt: gelen paket bilgisi (sadece callback süresince geçerli — kopyala!)
// ctx: udp_bind() ile verilen kullanıcı bağlamı
// ============================================================================
typedef void (*udp_handler_t)(const UDPPacket* pkt, void* ctx);

// ============================================================================
// UDP socket kaydı (dahili — hobi OS için basit dizi)
// ============================================================================
typedef struct {
    uint16_t       port;        // Dinlenen port (0 = boş slot)
    udp_handler_t  handler;     // Callback
    void*          ctx;         // Kullanıcı bağlamı
    uint32_t       rx_count;    // Alınan paket sayacı
    uint32_t       tx_count;    // Gönderilen paket sayacı (bu porttan)
} UDPSocket;

// ============================================================================
// Checksum modu
// ============================================================================
typedef enum {
    UDP_CSUM_DISABLE = 0,   // Checksum hesaplama, daima 0 gönder
    UDP_CSUM_ENABLE  = 1,   // RFC 768 pseudo-başlık checksum'u hesapla
} UDPCsumMode;

// ============================================================================
// Public API
// ============================================================================

// UDP katmanını başlat. ipv4_init() + ipv4_register_handler çağrıldıktan sonra.
// csum_mode: UDP_CSUM_ENABLE önerilir; QEMU için DISABLE de çalışır.
void udp_init(UDPCsumMode csum_mode);

// Belirtilen porta gelen paketler için handler kaydet.
// Aynı port için tekrar çağrılırsa üzerine yazar.
// ctx: handler'a aynen iletilir (NULL olabilir).
// Dönüş: true = başarılı, false = tablo dolu
bool udp_bind(uint16_t port, udp_handler_t handler, void* ctx);

// Port bağını kaldır (handler'ı sil, gelen paketler sessizce atılır).
void udp_unbind(uint16_t port);

// UDP datagramı gönder.
// dst_ip    : 4-byte hedef IP
// dst_port  : hedef port (host byte order)
// src_port  : kaynak port (host byte order; 0 = otomatik efemeral seçim)
// data      : veri tamponu
// len       : veri uzunluğu (max UDP_MAX_PAYLOAD)
// Dönüş: true = gönderildi, false = hata (ARP PENDING, MTU aşımı, vb.)
bool udp_send(const uint8_t dst_ip[4], uint16_t dst_port, uint16_t src_port,
              const uint8_t* data, uint16_t len);

// Subnet broadcast (255.255.255.255) gönder.
bool udp_broadcast(uint16_t dst_port, uint16_t src_port,
                   const uint8_t* data, uint16_t len);

// IPv4 katmanından otomatik çağrılan handler (doğrudan çağırmaya gerek yok).
void udp_handle_packet(const uint8_t src_ip[4],
                       const uint8_t* payload, uint16_t len);

// Checksum modunu çalışma zamanında değiştir.
void udp_set_csum_mode(UDPCsumMode mode);

// Katman başlatıldı mı?
bool udp_is_initialized(void);

// Kayıtlı soketleri ve istatistikleri seri porta yaz (debug).
void udp_print_sockets(void);

// Toplam RX / TX paket sayıları
uint32_t udp_get_rx_count(void);
uint32_t udp_get_tx_count(void);

// Efemeral port aralığı (kaynak port otomatik seçimi için)
#define UDP_EPHEMERAL_BASE  49152u
#define UDP_EPHEMERAL_MAX   65535u

#endif // UDP_H