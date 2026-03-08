// dhcp.h — AscentOS DHCP İstemcisi (Aşama 5)
// RFC 2131 — Dynamic Host Configuration Protocol
//
// Katman mimarisi:
//   rtl8139 → ipv4 → udp_handle_packet(port 68) → dhcp_handle_packet
//   Gönderim: dhcp_discover/request → udp_broadcast → ipv4_send → rtl8139
//
// Kullanım:
//   dhcp_init()        → DHCP katmanını başlat (udp_init'ten sonra)
//   dhcp_discover()    → DHCPDISCOVER gönder (IP almayı başlat)
//   dhcp_state()       → Mevcut DHCP durumunu sorgula
//   dhcp_get_config()  → Atanan IP/GW/DNS bilgilerini al

#ifndef DHCP_H
#define DHCP_H

#include <stdint.h>
#include <stdbool.h>

// ============================================================================
// DHCP Sabit Değerleri (RFC 2131 / RFC 2132)
// ============================================================================
#define DHCP_SERVER_PORT    67      // DHCP sunucusu dinleme portu
#define DHCP_CLIENT_PORT    68      // DHCP istemcisi dinleme portu

#define DHCP_OP_REQUEST     1       // İstemci → Sunucu
#define DHCP_OP_REPLY       2       // Sunucu → İstemci

#define DHCP_HTYPE_ETH      1       // Donanım tipi: Ethernet
#define DHCP_HLEN_ETH       6       // Ethernet MAC uzunluğu

#define DHCP_MAGIC_COOKIE   0x63825363UL  // RFC 2131 §3 (big-endian: 99.130.83.99)

#define DHCP_SECS           0       // Geçen saniye (basit istemci için 0)
#define DHCP_FLAGS_BROADCAST 0x8000 // Sunucuya broadcast yanıt iste

// ============================================================================
// DHCP Mesaj Tipleri (Option 53 değerleri — RFC 2132 §9.6)
// ============================================================================
#define DHCPDISCOVER    1
#define DHCPOFFER       2
#define DHCPREQUEST     3
#define DHCPDECLINE     4
#define DHCPACK         5
#define DHCPNAK         6
#define DHCPRELEASE     7
#define DHCPINFORM      8

// ============================================================================
// DHCP Option Kodları (RFC 2132)
// ============================================================================
#define DHCP_OPT_SUBNET_MASK        1
#define DHCP_OPT_ROUTER             3    // Gateway
#define DHCP_OPT_DNS_SERVER         6
#define DHCP_OPT_HOSTNAME           12
#define DHCP_OPT_REQUESTED_IP       50
#define DHCP_OPT_LEASE_TIME         51
#define DHCP_OPT_MSG_TYPE           53   // DHCP mesaj tipi (zorunlu)
#define DHCP_OPT_SERVER_ID          54   // Sunucu tanımlayıcısı
#define DHCP_OPT_PARAM_REQUEST      55   // İstenen option listesi
#define DHCP_OPT_RENEWAL_TIME       58   // T1 (yenileme zamanı)
#define DHCP_OPT_REBINDING_TIME     59   // T2 (yeniden bağlama zamanı)
#define DHCP_OPT_CLIENT_ID          61
#define DHCP_OPT_END                255  // Option listesi sonu

// ============================================================================
// DHCP Başlık Yapısı (RFC 2131 §2 — 236 byte sabit alan)
// Ardından 4-byte magic cookie + options alanı gelir.
// ============================================================================
typedef struct {
    uint8_t  op;            // Mesaj tipi: 1=BOOTREQUEST, 2=BOOTREPLY
    uint8_t  htype;         // Donanım adresi tipi (1=Ethernet)
    uint8_t  hlen;          // Donanım adresi uzunluğu (6)
    uint8_t  hops;          // Relay agent hop sayısı (istemci: 0)
    uint32_t xid;           // İşlem ID'si (istemci tarafından seçilir)
    uint16_t secs;          // Başlangıçtan bu yana geçen saniye
    uint16_t flags;         // 0x8000 = broadcast iste
    uint8_t  ciaddr[4];     // İstemci IP'si (REBINDING/RENEWING'de dolu)
    uint8_t  yiaddr[4];     // "Your" IP: sunucunun önerdiği istemci IP'si
    uint8_t  siaddr[4];     // Next server IP (TFTP için)
    uint8_t  giaddr[4];     // Relay agent IP
    uint8_t  chaddr[16];    // İstemci donanım adresi (MAC + padding)
    uint8_t  sname[64];     // Sunucu adı (opsiyonel, null-terminate)
    uint8_t  file[128];     // Boot dosyası adı (opsiyonel)
    uint32_t magic;         // Magic cookie: 0x63825363
    // Ardından değişken uzunluklu options alanı (max ~308 byte)
} __attribute__((packed)) DHCPHeader;

#define DHCP_HDR_LEN        240     // DHCPHeader büyüklüğü (magic dahil)
#define DHCP_OPTIONS_LEN    308     // Maksimum options alanı (toplam paket ≤548)
#define DHCP_PKT_MAX        548     // RFC 2131 minimum DHCP paket boyutu

// ============================================================================
// DHCP İstemci Durumu (State Machine)
// ============================================================================
typedef enum {
    DHCP_STATE_IDLE       = 0,  // Başlatılmamış / devre dışı
    DHCP_STATE_SELECTING,       // DISCOVER gönderildi, OFFER bekleniyor
    DHCP_STATE_REQUESTING,      // REQUEST gönderildi, ACK bekleniyor
    DHCP_STATE_BOUND,           // IP atandı, kira geçerli
    DHCP_STATE_RENEWING,        // Kira yenileme isteği gönderildi
    DHCP_STATE_REBINDING,       // T2 süresi doldu, herhangi sunucuya başvur
    DHCP_STATE_RELEASED,        // RELEASE gönderildi, IP bırakıldı
    DHCP_STATE_FAILED,          // DHCPNAK veya zaman aşımı
} DHCPState;

// ============================================================================
// Atanan Ağ Yapılandırması
// ============================================================================
typedef struct {
    uint8_t  ip[4];             // Atanan IP adresi
    uint8_t  subnet[4];         // Alt ağ maskesi
    uint8_t  gateway[4];        // Varsayılan ağ geçidi
    uint8_t  dns[4];            // Birincil DNS sunucusu
    uint8_t  server_ip[4];      // DHCP sunucusunun IP adresi
    uint32_t lease_time;        // Kira süresi (saniye)
    uint32_t renewal_time;      // T1 — yenileme zamanı (saniye)
    uint32_t rebinding_time;    // T2 — yeniden bağlama zamanı (saniye)
    uint32_t lease_start_tick;  // Kiranın başladığı sistem ticki
    bool     valid;             // Yapılandırma geçerli mi?
} DHCPConfig;

// ============================================================================
// Public API
// ============================================================================

// DHCP katmanını başlat. udp_init() çağrıldıktan sonra çağrılmalı.
// UDP port 68'i bağlar, her şeyi sıfırlar.
void dhcp_init(void);

// DHCPDISCOVER gönder — IP edinme sürecini başlatır.
// Dönüş: true = paket gönderildi, false = katman hazır değil
bool dhcp_discover(void);

// DHCPRELEASE gönder — mevcut IP'yi sunucuya iade et.
// Ardından arp/ipv4 yapılandırması sıfırlanır.
void dhcp_release(void);

// Gelen UDP paketini işle (udp_bind callback'i — doğrudan çağırmaya gerek yok)
void dhcp_handle_packet(const void* pkt, void* ctx);

// Mevcut DHCP durumunu döndür
DHCPState dhcp_get_state(void);

// Atanan yapılandırmayı döndür (geçerliyse valid=true)
void dhcp_get_config(DHCPConfig* out);

// commands64.c için durum string'i
const char* dhcp_state_str(void);

// Katman başlatıldı mı?
bool dhcp_is_initialized(void);

// İşlem ID'sini döndür (debug)
uint32_t dhcp_get_xid(void);

#endif // DHCP_H