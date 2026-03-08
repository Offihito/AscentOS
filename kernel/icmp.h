// icmp.h — AscentOS ICMP Katmanı (Aşama 3)
// RFC 792 implementasyonu — Echo Request / Echo Reply (ping)
//
// Kullanım:
//   icmp_init()              → ICMP katmanını başlat (ipv4_init'ten sonra)
//   icmp_handle_packet(...)  → ipv4_register_handler ile otomatik bağlanır
//   icmp_ping(dst_ip)        → Echo Request gönder (ping)
//   icmp_last_rtt_ms()       → Son başarılı ping RTT'si (ms)
//   icmp_ping_pending()      → Yanıt bekleniyor mu?

#ifndef ICMP_H
#define ICMP_H

#include <stdint.h>
#include <stdbool.h>

// ============================================================================
// ICMP tip ve kod sabitleri
// ============================================================================
#define ICMP_TYPE_ECHO_REPLY    0    // Echo Reply   (ping yanıtı)
#define ICMP_TYPE_UNREACHABLE   3    // Destination Unreachable
#define ICMP_TYPE_ECHO_REQUEST  8    // Echo Request (ping isteği)
#define ICMP_TYPE_TIME_EXCEEDED 11   // TTL aşıldı
#define ICMP_CODE_NET_UNREACH   0    // Ağa ulaşılamıyor
#define ICMP_CODE_HOST_UNREACH  1    // Konağa ulaşılamıyor
#define ICMP_CODE_PORT_UNREACH  3    // Porta ulaşılamıyor
#define ICMP_CODE_TTL_TRANSIT   0    // TTL transit sırasında bitti

// ============================================================================
// ICMP başlık yapısı — tüm ICMP tipleri için ortak 8 byte
// (Echo için id+seq; diğer tipler için aynı alanlar farklı anlam taşır)
// ============================================================================
typedef struct {
    uint8_t  type;         // ICMP mesaj tipi
    uint8_t  code;         // Alt kod
    uint16_t checksum;     // Tüm ICMP mesajı üzerinde (başlık + veri)
    uint16_t id;           // Echo: tanımlayıcı (process ID benzeri)
    uint16_t seq;          // Echo: sıra numarası
    // Ardından isteğe bağlı veri alanı
} __attribute__((packed)) ICMPHeader;

#define ICMP_HDR_LEN        8    // Başlık uzunluğu (byte)
#define ICMP_PAYLOAD_LEN   32    // Echo veri alanı uzunluğu ("AscentOS PING…")
#define ICMP_ECHO_ID    0xA5CE   // Bize ait ping ID'si (AscentOS → 0xA5CE)

// ============================================================================
// Ping sonuç durumu
// ============================================================================
typedef enum {
    PING_IDLE    = 0,   // Bekleyen ping yok
    PING_PENDING,       // İstek gönderildi, yanıt bekleniyor
    PING_SUCCESS,       // Yanıt alındı (rtt_ms geçerli)
    PING_TIMEOUT,       // Yanıt gelmedi (timeout)
    PING_UNREACHABLE,   // ICMP Unreachable alındı
} PingState;

// ============================================================================
// Public API
// ============================================================================

// ICMP katmanını başlat ve IPv4 handler tablosuna kaydet.
// ipv4_init() çağrıldıktan sonra çağrılmalı.
void icmp_init(void);

// IPv4 katmanından çağrılan ICMP paket işleyici.
// Doğrudan çağırmaya gerek yok — ipv4_register_handler ile otomatik bağlanır.
void icmp_handle_packet(const uint8_t src_ip[4],
                        const uint8_t* payload, uint16_t len);

// Belirtilen IP'ye ICMP Echo Request (ping) gönder.
// Dönüş: true = paket gönderildi, false = ARP çözümlenmedi (kısa süre sonra tekrar dene)
bool icmp_ping(const uint8_t dst_ip[4]);

// Ping durum sorguları
PingState icmp_ping_state(void);          // Mevcut durum
uint32_t  icmp_last_rtt_ms(void);         // Son başarılı ping RTT'si (ms cinsinden tahmin)
uint16_t  icmp_last_seq(void);            // Son ping sıra numarası
void      icmp_get_last_src(uint8_t out[4]); // Son yanıtı gönderen IP

// Durumu sıfırla (yeni ping için)
void icmp_ping_reset(void);

// Geçen ticks → ms dönüşümü (timer tick = 1 ms varsayımı, QEMU varsayılan ~10ms)
uint32_t icmp_ticks_to_ms(uint32_t ticks);

#endif // ICMP_H