// arp.h — AscentOS ARP Protokolü (Aşama 2)
// RFC 826 — Address Resolution Protocol
//
// Görevler:
//   1. Gelen ARP paketlerini işle (istek → cevap, cevap → tablo güncelle)
//   2. IP → MAC çözümleme tablosu (ARP cache) tut
//   3. ARP request gönder (IP'nin MAC'ini sor)
//   4. Gratuitous ARP gönder (kendi IP'mizi duyur)
//
// Kullanım:
//   arp_init(my_ip, my_mac)         → ARP katmanını başlat
//   arp_handle_packet(buf, len)     → rtl8139 callback'inden çağır
//   arp_resolve(ip, out_mac)        → IP'ye karşılık MAC bul (cache'ten)
//   arp_request(target_ip)          → ARP who-has gönder
//   arp_announce()                  → Gratuitous ARP (ağa kendini tanıt)

#ifndef ARP_H
#define ARP_H

#include <stdint.h>
#include <stdbool.h>

// ============================================================================
// Ethernet + ARP sabitleri
// ============================================================================
#define ETH_ALEN        6
#define ETH_HLEN        14
#define ETHERTYPE_ARP   0x0806
#define ETHERTYPE_IPV4  0x0800

// ARP sabit değerleri
#define ARP_HTYPE_ETH   0x0001   // Hardware type: Ethernet
#define ARP_PTYPE_IPV4  0x0800   // Protocol type: IPv4
#define ARP_HLEN_ETH    6        // Ethernet MAC uzunluğu
#define ARP_PLEN_IPV4   4        // IPv4 adresi uzunluğu
#define ARP_OP_REQUEST  0x0001   // ARP isteği (who has?)
#define ARP_OP_REPLY    0x0002   // ARP cevabı (is at!)

// ============================================================================
// Ethernet çerçeve başlığı
// ============================================================================
typedef struct {
    uint8_t  dst_mac[6];    // Hedef MAC
    uint8_t  src_mac[6];    // Kaynak MAC
    uint16_t ethertype;     // Protokol tipi (big-endian)
} __attribute__((packed)) EthHeader;

// ============================================================================
// ARP paket yapısı (Ethernet üzeri IPv4 için)
// ============================================================================
typedef struct {
    uint16_t htype;         // Hardware type (0x0001 = Ethernet)
    uint16_t ptype;         // Protocol type (0x0800 = IPv4)
    uint8_t  hlen;          // Hardware addr uzunluğu (6)
    uint8_t  plen;          // Protocol addr uzunluğu (4)
    uint16_t oper;          // Operasyon: 1=request, 2=reply
    uint8_t  sha[6];        // Sender Hardware Address (MAC)
    uint8_t  spa[4];        // Sender Protocol Address (IP)
    uint8_t  tha[6];        // Target Hardware Address (MAC)
    uint8_t  tpa[4];        // Target Protocol Address (IP)
} __attribute__((packed)) ARPPacket;

// ============================================================================
// ARP cache girişi
// ============================================================================
#define ARP_CACHE_SIZE  16      // Maksimum eş sayısı (hobi OS için yeterli)
#define ARP_ENTRY_TTL   300     // Saniye cinsinden geçerlilik süresi (ticks'e çevrilir)

typedef enum {
    ARP_ENTRY_FREE = 0,     // Boş slot
    ARP_ENTRY_PENDING,      // İstek gönderildi, cevap bekleniyor
    ARP_ENTRY_RESOLVED,     // MAC çözümlendi, geçerli
    ARP_ENTRY_STATIC,       // Elle eklendi, süre dolmaz
} ARPEntryState;

typedef struct {
    uint8_t        ip[4];
    uint8_t        mac[6];
    ARPEntryState  state;
    uint32_t       timestamp;   // get_system_ticks() değeri
} ARPEntry;

// ============================================================================
// ARP modülü public API
// ============================================================================

// ARP katmanını başlat
// my_ip: 4-byte IP (örn. {10,0,2,15})
// my_mac: 6-byte MAC (rtl8139_get_mac() çıktısı)
void arp_init(const uint8_t my_ip[4], const uint8_t my_mac[6]);

// Gelen Ethernet çerçevesini işle
// EtherType 0x0806 olan paketleri ARP olarak işler
// rtl8139'un packet_handler callback'inden çağrılır
void arp_handle_packet(const uint8_t* frame, uint16_t len);

// IP adresine karşılık MAC adresini bul (cache'ten)
// Bulunursa out_mac'e kopyalar ve true döner
// Bulunamazsa false döner ve otomatik ARP request gönderir
bool arp_resolve(const uint8_t ip[4], uint8_t out_mac[6]);

// Belirli bir IP için ARP who-has request gönder
void arp_request(const uint8_t target_ip[4]);

// Gratuitous ARP: kendi IP+MAC'ini ağa duyur
// boot sonrası ve IP değişiminde çağır
void arp_announce(void);

// ARP cache tablosunu ekrana yaz (komut için)
void arp_print_cache(void);

// ARP cache'e statik giriş ekle
void arp_add_static(const uint8_t ip[4], const uint8_t mac[6]);

// ARP cache'i temizle
void arp_flush_cache(void);

// Kendi IP adresini döndür
void arp_get_my_ip(uint8_t out[4]);

// Kendi MAC adresini döndür
void arp_get_my_mac(uint8_t out[6]);

// IP'yi string'e çevir (örn. "10.0.2.15") — null-terminate eder
void ip_to_str(const uint8_t ip[4], char* out);

// String'i IP'ye çevir (örn. "10.0.2.15" → {10,0,2,15})
// Başarılıysa true döner
bool str_to_ip(const char* str, uint8_t out[4]);

#endif // ARP_H