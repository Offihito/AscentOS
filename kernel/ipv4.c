// ipv4.c — AscentOS IPv4 Katmanı (Aşama 3)
// RFC 791 implementasyonu
//
// Gelen çerçeve işleme akışı:
//   rtl8139 IRQ → net_packet_callback → ipv4_handle_packet
//                                     → kayıtlı proto handler (örn. icmp_handle_packet)
//
// Gönderim akışı:
//   icmp_ping / üst katman → ipv4_send → ARP çözümle → Ethernet çerçevesi → rtl8139_send

#include "ipv4.h"
#include "rtl8139.h"

// ============================================================================
// Kernel yardımcıları
// ============================================================================
extern void serial_print(const char*);
extern void serial_write(char);

// ============================================================================
// ARP katmanı — sadece ihtiyaç duyulan fonksiyonlar
// (arp.h include etmek yerine explicit extern: döngüsel bağımlılık yok)
// ============================================================================
extern bool    arp_is_initialized(void);
extern bool    arp_resolve(const uint8_t ip[4], uint8_t out_mac[6]);
extern void    arp_get_my_ip(uint8_t out[4]);
extern void    arp_get_my_mac(uint8_t out[6]);
extern void    ip_to_str(const uint8_t ip[4], char* out);

// ============================================================================
// Yerel yardımcılar (arp.c'deki statik fonksiyonların kopyası, bağımsız olsun)
// ============================================================================
static void  _memcpy(void* d, const void* s, uint32_t n){
    uint8_t* dp = (uint8_t*)d; const uint8_t* sp = (const uint8_t*)s;
    while(n--) *dp++ = *sp++;
}
static void  _memset(void* d, uint8_t v, uint32_t n){
    uint8_t* dp = (uint8_t*)d; while(n--) *dp++ = v;
}
static int   _memcmp(const void* a, const void* b, uint32_t n){
    const uint8_t* ap=(const uint8_t*)a; const uint8_t* bp=(const uint8_t*)b;
    while(n--){ if(*ap!=*bp) return *ap-*bp; ap++; bp++; }
    return 0;
}

// Network ↔ host byte order (big-endian platform için no-op, x86 için swap)
static inline uint16_t _htons(uint16_t v){ return (uint16_t)((v>>8)|(v<<8)); }
static inline uint16_t _ntohs(uint16_t v){ return (uint16_t)((v>>8)|(v<<8)); }

// Serial decimal yardımcısı
static void _ser_dec(uint32_t v){
    if(!v){ serial_write('0'); return; }
    char b[12]; int i=0;
    while(v){ b[i++]='0'+(v%10); v/=10; }
    while(i--) serial_write(b[i]);
}

// ============================================================================
// Ethernet başlığı — arp.h'daki EthHeader ile aynı yapı, tekrar tanımlamayız;
// sadece ETH_HLEN sabitini kullanıyoruz (arp.h'dan gelir).
// ============================================================================
#ifndef ETH_HLEN
#define ETH_HLEN 14
#endif

// ============================================================================
// Modül durumu
// ============================================================================
typedef struct {
    uint8_t              protocol;
    ipv4_proto_handler_t handler;
} ProtoEntry;

static ProtoEntry  g_handlers[IPV4_MAX_HANDLERS];
static uint8_t     g_handler_count = 0;
static bool        g_initialized   = false;
static uint32_t    g_tx_count      = 0;
static uint32_t    g_rx_count      = 0;
static uint16_t    g_ip_id         = 0x1234;  // parçalama ID sayacı
static uint8_t     g_gateway[4]    = {0,0,0,0};
static uint8_t     g_subnet[4]     = {255,255,255,0};  // /24 varsayılan

// Broadcast IP sabiti
static const uint8_t IP_BROADCAST[4] = {255,255,255,255};
static const uint8_t MAC_BROADCAST[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

// ============================================================================
// IPv4 başlık sağlama toplamı (RFC 791)
// data: ham başlık (checksum alanı = 0 ile gönderilmeli)
// len : başlık uzunluğu (byte, tipik 20)
// ============================================================================
uint16_t ipv4_checksum(const void* data, uint16_t len){
    const uint16_t* ptr = (const uint16_t*)data;
    uint32_t sum = 0;
    while(len > 1){ sum += *ptr++; len -= 2; }
    if(len) sum += *(const uint8_t*)ptr;          // tek kalan byte
    while(sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16); // taşmaları katlama
    return (uint16_t)(~sum);
}

// ============================================================================
// Public API
// ============================================================================
void ipv4_init(void){
    _memset(g_handlers, 0, sizeof(g_handlers));
    g_handler_count = 0;
    g_tx_count      = 0;
    g_rx_count      = 0;
    g_initialized   = true;
    serial_print("[IPv4] Baslatildi\n");
}

void ipv4_register_handler(uint8_t protocol, ipv4_proto_handler_t handler){
    // Mevcut kaydı güncelle
    for(int i = 0; i < g_handler_count; i++){
        if(g_handlers[i].protocol == protocol){
            g_handlers[i].handler = handler;
            return;
        }
    }
    // Yeni kayıt
    if(g_handler_count < IPV4_MAX_HANDLERS){
        g_handlers[g_handler_count].protocol = protocol;
        g_handlers[g_handler_count].handler  = handler;
        g_handler_count++;
    }
}

// ============================================================================
// Gelen Ethernet çerçevesini işle
// ============================================================================
void ipv4_handle_packet(const uint8_t* frame, uint16_t len){
    if(!g_initialized) return;

    // Minimum uzunluk: ETH(14) + IPv4(20)
    if(len < (uint16_t)(ETH_HLEN + IPV4_HDR_LEN)) return;

    // EtherType kontrolü
    uint16_t etype = (uint16_t)((frame[12] << 8) | frame[13]);
    if(etype != ETHERTYPE_IPV4) return;

    const IPv4Header* ip = (const IPv4Header*)(frame + ETH_HLEN);

    // Versiyon ve IHL kontrolü
    uint8_t version = (ip->ver_ihl >> 4) & 0xF;
    uint8_t ihl     = (ip->ver_ihl & 0xF) * 4;   // byte cinsinden başlık uzunluğu
    if(version != 4 || ihl < IPV4_HDR_LEN) return;

    // Başlık sağlama toplamı doğrula (0 çıkmalı)
    if(ipv4_checksum(ip, ihl) != 0){
        serial_print("[IPv4] Checksum hatasi, paket atildi\n");
        return;
    }

    uint16_t total = _ntohs(ip->total_len);
    if(total < ihl || (uint16_t)(ETH_HLEN + total) > len) return;

    // Parçalanmış paketleri şimdilik atla (MF veya offset > 0)
    uint16_t flags_frag = _ntohs(ip->flags_frag);
    bool mf     = (flags_frag >> 13) & 1;
    uint16_t fo = flags_frag & 0x1FFF;
    if(mf || fo){
        serial_print("[IPv4] Parcalanmis paket, atiliyor (desteklenmiyor)\n");
        return;
    }

    g_rx_count++;

    // Debug: kimden geldi
    serial_print("[IPv4] RX proto=");
    _ser_dec(ip->protocol);
    serial_print("  src=");
    char ipbuf[16]; ip_to_str(ip->src, ipbuf); serial_print(ipbuf);
    serial_write('\n');

    // Veri alanı = başlık sonrası
    const uint8_t* payload = (const uint8_t*)ip + ihl;
    uint16_t       plen    = total - ihl;

    // Kayıtlı handler'ı bul ve çağır
    for(int i = 0; i < g_handler_count; i++){
        if(g_handlers[i].protocol == ip->protocol){
            g_handlers[i].handler(ip->src, payload, plen);
            return;
        }
    }
    // Bilinmeyen protokol — sessizce bırak
}

// ============================================================================
// IPv4 paketi gönder
// ============================================================================
bool ipv4_send(const uint8_t dst_ip[4], uint8_t protocol,
               const uint8_t* payload, uint16_t plen)
{
    if(!g_initialized || !arp_is_initialized()) return false;

    // Toplam IPv4 uzunluğu
    uint16_t ip_total = (uint16_t)(IPV4_HDR_LEN + plen);

    // Toplam Ethernet çerçevesi uzunluğu
    uint16_t frame_len = (uint16_t)(ETH_HLEN + ip_total);
    if(frame_len < 60) frame_len = 60;  // Ethernet minimum

    // Çerçeve tamponu (yığında, hobı OS için yeterli — MTU aşılmaz)
    uint8_t frame[1514];
    _memset(frame, 0, frame_len);

    // ── Ethernet başlığı ──────────────────────────────────────────────────
    uint8_t dst_mac[6];
    bool resolved;

    // Broadcast → doğrudan broadcast MAC
    if(_memcmp(dst_ip, IP_BROADCAST, 4) == 0 || dst_ip[3] == 255){
        _memcpy(dst_mac, MAC_BROADCAST, 6);
        resolved = true;
    } else {
        // Subnet kontrolü: hedef aynı /24'te değilse gateway MAC'ine yönlendir
        uint8_t my_ip[4]; arp_get_my_ip(my_ip);
        bool same = true;
        for(int i = 0; i < 4; i++){
            if((dst_ip[i] & g_subnet[i]) != (my_ip[i] & g_subnet[i])){ same = false; break; }
        }
        const uint8_t* arp_target = same ? dst_ip : g_gateway;

        // Gateway tanımlı değilse paketi düşür
        if(!same){
            uint8_t zero[4] = {0,0,0,0};
            if(_memcmp(g_gateway, zero, 4) == 0){
                serial_print("[IPv4] Gateway tanimli degil, paket atildi\n");
                return false;
            }
        }
        resolved = arp_resolve(arp_target, dst_mac);
    }

    if(!resolved){
        // ARP PENDING → paket düşürüldü, üst katman retry yapmalı
        serial_print("[IPv4] ARP PENDING, paket atildi (retry gerekli)\n");
        return false;
    }

    uint8_t my_mac[6];
    arp_get_my_mac(my_mac);

    _memcpy(frame + 0, dst_mac, 6);    // hedef MAC
    _memcpy(frame + 6, my_mac,  6);    // kaynak MAC
    frame[12] = 0x08; frame[13] = 0x00; // EtherType IPv4

    // ── IPv4 başlığı ──────────────────────────────────────────────────────
    IPv4Header* ip = (IPv4Header*)(frame + ETH_HLEN);
    ip->ver_ihl   = (4 << 4) | 5;          // IPv4, IHL=5 (20 byte)
    ip->tos       = 0;
    ip->total_len = _htons(ip_total);
    ip->id        = _htons(g_ip_id++);
    ip->flags_frag= _htons(0x4000);         // DF biti, offset=0
    ip->ttl       = IPV4_TTL_DEFAULT;
    ip->protocol  = protocol;
    ip->checksum  = 0;                       // önce sıfırla

    uint8_t my_ip[4];
    arp_get_my_ip(my_ip);
    _memcpy(ip->src, my_ip,  4);
    _memcpy(ip->dst, dst_ip, 4);

    // Sağlama toplamı (başlık üzerinde, payload dahil değil)
    ip->checksum = ipv4_checksum(ip, IPV4_HDR_LEN);

    // ── Payload ───────────────────────────────────────────────────────────
    _memcpy(frame + ETH_HLEN + IPV4_HDR_LEN, payload, plen);

    // ── Gönder ────────────────────────────────────────────────────────────
    bool ok = rtl8139_send(frame, frame_len);
    if(ok){
        g_tx_count++;
        serial_print("[IPv4] TX proto=");
        _ser_dec(protocol);
        serial_print("  dst=");
        char ipbuf[16]; ip_to_str(dst_ip, ipbuf); serial_print(ipbuf);
        serial_write('\n');
    } else {
        serial_print("[IPv4] TX HATA (rtl8139_send basarisiz)\n");
    }
    return ok;
}

bool     ipv4_is_initialized(void){ return g_initialized; }
uint32_t ipv4_get_tx_count(void)  { return g_tx_count; }
uint32_t ipv4_get_rx_count(void)  { return g_rx_count; }

void ipv4_set_gateway(const uint8_t gw[4]){
    _memcpy(g_gateway, gw, 4);
    serial_print("[IPv4] Gateway: ");
    char b[16]; ip_to_str(gw, b); serial_print(b); serial_write('\n');
}
void ipv4_set_subnet(const uint8_t mask[4]){ _memcpy(g_subnet, mask, 4); }
void ipv4_get_gateway(uint8_t out[4])      { _memcpy(out, g_gateway, 4); }