// arp.c — AscentOS ARP Protokolü (Aşama 2)
// RFC 826 implementasyonu
//
// Derleme (Makefile'a ekle):
//   arp.o: kernel/arp.c kernel/arp.h kernel/rtl8139.h
//       $(CC) $(CFLAGS) -c kernel/arp.c -o arp.o
//   KERNEL_OBJS listesine arp.o ekle

#include "arp.h"
#include "../drivers/rtl8139.h"

// ============================================================================
// Kernel yardımcıları
// ============================================================================
extern void serial_print(const char*);
extern void serial_write(char);
extern uint64_t get_system_ticks(void);   // timer.c

// ============================================================================
// Yardımcı I/O
// ============================================================================
static inline void outb_arp(uint16_t p, uint8_t v){
    __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p));
}
static inline uint8_t inb_arp(uint16_t p){
    uint8_t v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p)); return v;
}

// ============================================================================
// Küçük yardımcılar
// ============================================================================
static void memcpy_a(void* d, const void* s, uint32_t n){
    uint8_t* dp=(uint8_t*)d; const uint8_t* sp=(const uint8_t*)s;
    while(n--) *dp++=*sp++;
}
static void memset_a(void* d, uint8_t v, uint32_t n){
    uint8_t* dp=(uint8_t*)d; while(n--) *dp++=v;
}
static int memcmp_a(const void* a, const void* b, uint32_t n){
    const uint8_t* ap=(const uint8_t*)a; const uint8_t* bp=(const uint8_t*)b;
    while(n--){ if(*ap!=*bp) return *ap-*bp; ap++; bp++; }
    return 0;
}

// big-endian 16-bit dönüşümleri (ağ byte sırası ↔ host)
static inline uint16_t htons_a(uint16_t v){ return (uint16_t)((v>>8)|(v<<8)); }
static inline uint16_t ntohs_a(uint16_t v){ return (uint16_t)((v>>8)|(v<<8)); }

// Serial hex yardımcıları
static void ser_hex8(uint8_t v){
    const char* h="0123456789ABCDEF";
    serial_write(h[(v>>4)&0xF]); serial_write(h[v&0xF]);
}
static void ser_dec(uint32_t v){
    if(!v){ serial_write('0'); return; }
    char b[12]; int i=0;
    while(v){ b[i++]='0'+(v%10); v/=10; }
    while(i--) serial_write(b[i]);
}

// ============================================================================
// Modül durumu
// ============================================================================
static uint8_t   g_my_ip[4];
static uint8_t   g_my_mac[6];
static ARPEntry  g_cache[ARP_CACHE_SIZE];
static bool      g_initialized = false;

// Broadcast MAC
static const uint8_t MAC_BROADCAST[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

// ============================================================================
// IP / string dönüşümleri
// ============================================================================
void ip_to_str(const uint8_t ip[4], char* out){
    // "aaa.bbb.ccc.ddd\0"  — maks 16 karakter
    int pos = 0;
    for(int octet = 0; octet < 4; octet++){
        uint8_t v = ip[octet];
        if(v >= 100){ out[pos++] = '0' + v/100;    v %= 100; }
        if(v >= 10 || ip[octet] >= 100){ out[pos++] = '0' + v/10; v %= 10; }
        out[pos++] = '0' + v;
        if(octet < 3) out[pos++] = '.';
    }
    out[pos] = '\0';
}

bool str_to_ip(const char* str, uint8_t out[4]){
    int octet = 0, val = 0, digits = 0;
    for(int i = 0; ; i++){
        char c = str[i];
        if(c >= '0' && c <= '9'){
            val = val * 10 + (c - '0');
            digits++;
            if(val > 255 || digits > 3) return false;
        } else if(c == '.' || c == '\0'){
            if(digits == 0 || octet > 3) return false;
            out[octet++] = (uint8_t)val;
            val = 0; digits = 0;
            if(c == '\0') break;
        } else return false;
    }
    return (octet == 4);
}

// ============================================================================
// ARP cache yönetimi
// ============================================================================

// Cache'te IP'yi ara, slot indeksi döner (-1 = bulunamadı)
static int cache_find(const uint8_t ip[4]){
    for(int i = 0; i < ARP_CACHE_SIZE; i++){
        if(g_cache[i].state != ARP_ENTRY_FREE &&
           memcmp_a(g_cache[i].ip, ip, 4) == 0)
            return i;
    }
    return -1;
}

// Boş slot bul, yoksa en eski RESOLVED girişi kurban et
static int cache_alloc(void){
    // Önce FREE slot
    for(int i = 0; i < ARP_CACHE_SIZE; i++)
        if(g_cache[i].state == ARP_ENTRY_FREE) return i;
    // PENDING olanı kurban et
    for(int i = 0; i < ARP_CACHE_SIZE; i++)
        if(g_cache[i].state == ARP_ENTRY_PENDING) return i;
    // En eski RESOLVED girişi kurban et
    uint32_t oldest_ts = 0xFFFFFFFF; int oldest_i = 0;
    for(int i = 0; i < ARP_CACHE_SIZE; i++){
        if(g_cache[i].state == ARP_ENTRY_RESOLVED &&
           g_cache[i].timestamp < oldest_ts){
            oldest_ts = g_cache[i].timestamp;
            oldest_i  = i;
        }
    }
    return oldest_i;
}

// ============================================================================
// ARP paket gönderimi
// ============================================================================
static void arp_send(uint16_t oper,
                     const uint8_t dst_eth[6],
                     const uint8_t sender_mac[6], const uint8_t sender_ip[4],
                     const uint8_t target_mac[6], const uint8_t target_ip[4])
{
    // Toplam çerçeve: EthHeader(14) + ARPPacket(28) = 42 byte
    // Ethernet min 60 → 18 byte padding ekle
    uint8_t frame[60];
    memset_a(frame, 0, 60);

    // Ethernet başlığı
    EthHeader* eth = (EthHeader*)frame;
    memcpy_a(eth->dst_mac, dst_eth,    6);
    memcpy_a(eth->src_mac, sender_mac, 6);
    eth->ethertype = htons_a(ETHERTYPE_ARP);

    // ARP gövdesi
    ARPPacket* arp = (ARPPacket*)(frame + ETH_HLEN);
    arp->htype = htons_a(ARP_HTYPE_ETH);
    arp->ptype = htons_a(ARP_PTYPE_IPV4);
    arp->hlen  = ARP_HLEN_ETH;
    arp->plen  = ARP_PLEN_IPV4;
    arp->oper  = htons_a(oper);
    memcpy_a(arp->sha, sender_mac, 6);
    memcpy_a(arp->spa, sender_ip,  4);
    memcpy_a(arp->tha, target_mac, 6);
    memcpy_a(arp->tpa, target_ip,  4);

    rtl8139_send(frame, 60);
}

// ============================================================================
// Public API
// ============================================================================
void arp_init(const uint8_t my_ip[4], const uint8_t my_mac[6]){
    memcpy_a(g_my_ip,  my_ip,  4);
    memcpy_a(g_my_mac, my_mac, 6);
    memset_a(g_cache, 0, sizeof(g_cache));
    g_initialized = true;

    serial_print("[ARP] Baslatildi — IP: ");
    {
        char ipbuf[16]; ip_to_str(my_ip, ipbuf);
        serial_print(ipbuf);
    }
    serial_print("  MAC: ");
    for(int i=0;i<6;i++){ ser_hex8(my_mac[i]); if(i<5) serial_write(':'); }
    serial_write('\n');
}

void arp_get_my_ip(uint8_t out[4])  { memcpy_a(out, g_my_ip,  4); }
void arp_get_my_mac(uint8_t out[6]) { memcpy_a(out, g_my_mac, 6); }

void arp_request(const uint8_t target_ip[4]){
    if(!g_initialized) return;
    static const uint8_t zero_mac[6] = {0};
    serial_print("[ARP] Request: who has ");
    { char b[16]; ip_to_str(target_ip, b); serial_print(b); }
    serial_write('\n');
    arp_send(ARP_OP_REQUEST,
             MAC_BROADCAST,
             g_my_mac, g_my_ip,
             zero_mac, target_ip);
}

void arp_announce(void){
    if(!g_initialized) return;
    serial_print("[ARP] Gratuitous ARP gonderiliyor\n");
    // Gratuitous ARP: TPA = SPA = kendi IP'miz
    arp_send(ARP_OP_REQUEST,
             MAC_BROADCAST,
             g_my_mac, g_my_ip,
             MAC_BROADCAST, g_my_ip);
}

void arp_handle_packet(const uint8_t* frame, uint16_t len){
    if(!g_initialized) return;
    if(len < ETH_HLEN + (uint16_t)sizeof(ARPPacket)) return;

    // EtherType kontrolü
    const EthHeader* eth = (const EthHeader*)frame;
    if(ntohs_a(eth->ethertype) != ETHERTYPE_ARP) return;

    const ARPPacket* arp = (const ARPPacket*)(frame + ETH_HLEN);

    // Sadece Ethernet/IPv4 ARP işle
    if(ntohs_a(arp->htype) != ARP_HTYPE_ETH)    return;
    if(ntohs_a(arp->ptype) != ARP_PTYPE_IPV4)   return;
    if(arp->hlen != ARP_HLEN_ETH) return;
    if(arp->plen != ARP_PLEN_IPV4) return;

    uint16_t oper = ntohs_a(arp->oper);

    serial_print("[ARP] ");
    serial_print(oper == ARP_OP_REQUEST ? "Request" : "Reply");
    serial_print("  ");
    { char b[16]; ip_to_str(arp->spa, b); serial_print(b); }
    serial_print(" -> ");
    { char b[16]; ip_to_str(arp->tpa, b); serial_print(b); }
    serial_write('\n');

    // Gönderenin IP/MAC çiftini her zaman cache'e al (öğrenme)
    // Sıfır IP'yi ekleme (başlatılmamış cihazlar)
    uint8_t zero_ip[4] = {0};
    if(memcmp_a(arp->spa, zero_ip, 4) != 0){
        int idx = cache_find(arp->spa);
        if(idx < 0){
            idx = cache_alloc();
            memcpy_a(g_cache[idx].ip, arp->spa, 4);
        }
        memcpy_a(g_cache[idx].mac, arp->sha, 6);
        g_cache[idx].state     = ARP_ENTRY_RESOLVED;
        g_cache[idx].timestamp = (uint32_t)get_system_ticks();
        serial_print("[ARP] Cache guncellendi: ");
        { char b[16]; ip_to_str(arp->spa, b); serial_print(b); }
        serial_print(" = ");
        for(int i=0;i<6;i++){ ser_hex8(arp->sha[i]); if(i<5) serial_write(':'); }
        serial_write('\n');
    }

    // ARP Request ve TPA bize ait → cevap gönder
    if(oper == ARP_OP_REQUEST &&
       memcmp_a(arp->tpa, g_my_ip, 4) == 0)
    {
        serial_print("[ARP] Bize yoneliktdi, reply gonderiyoruz\n");
        arp_send(ARP_OP_REPLY,
                 arp->sha,          // hedef: soran cihaz
                 g_my_mac, g_my_ip, // biz
                 arp->sha, arp->spa);// hedef MAC+IP
    }
}

bool arp_resolve(const uint8_t ip[4], uint8_t out_mac[6]){
    if(!g_initialized) return false;

    // Broadcast IP → broadcast MAC
    if(memcmp_a(ip, (uint8_t[]){255,255,255,255}, 4) == 0){
        memcpy_a(out_mac, MAC_BROADCAST, 6);
        return true;
    }

    int idx = cache_find(ip);
    if(idx >= 0 && g_cache[idx].state == ARP_ENTRY_RESOLVED){
        memcpy_a(out_mac, g_cache[idx].mac, 6);
        return true;
    }
    if(idx >= 0 && g_cache[idx].state == ARP_ENTRY_STATIC){
        memcpy_a(out_mac, g_cache[idx].mac, 6);
        return true;
    }

    // Bulunamadı — ilk kez PENDING yap ve request gönder.
    // Zaten PENDING ise tekrar request GÖNDERME (flood önleme).
    if(idx < 0){
        idx = cache_alloc();
        memcpy_a(g_cache[idx].ip, ip, 4);
        g_cache[idx].state     = ARP_ENTRY_PENDING;
        g_cache[idx].timestamp = (uint32_t)get_system_ticks();
        arp_request(ip);
    }
    return false;
}

void arp_add_static(const uint8_t ip[4], const uint8_t mac[6]){
    int idx = cache_find(ip);
    if(idx < 0) idx = cache_alloc();
    memcpy_a(g_cache[idx].ip,  ip,  4);
    memcpy_a(g_cache[idx].mac, mac, 6);
    g_cache[idx].state     = ARP_ENTRY_STATIC;
    g_cache[idx].timestamp = 0;
}

void arp_flush_cache(void){
    memset_a(g_cache, 0, sizeof(g_cache));
    serial_print("[ARP] Cache temizlendi\n");
}

void arp_print_cache(void){
    serial_print("[ARP] Cache:\n");
    for(int i = 0; i < ARP_CACHE_SIZE; i++){
        if(g_cache[i].state == ARP_ENTRY_FREE) continue;
        const char* states[] = {"FREE","PENDING","RESOLVED","STATIC"};
        serial_print("  [");
        ser_dec(i);
        serial_print("] ");
        { char b[16]; ip_to_str(g_cache[i].ip, b); serial_print(b); }
        serial_print(" = ");
        for(int j=0;j<6;j++){ ser_hex8(g_cache[i].mac[j]); if(j<5) serial_write(':'); }
        serial_print("  ");
        serial_print(states[g_cache[i].state]);
        serial_write('\n');
    }
}

// ============================================================================
// commands64.c için ekran çıktısı (serial değil, VGA'ya)
// Bu fonksiyon commands64.c'deki cmd_arp* tarafından çağrılır.
// Cache içeriğini çıktı tamponuna yazar.
// ============================================================================

// Her satır için callback tipi
typedef void (*arp_line_cb)(const char* line, uint8_t color, void* ctx);

void arp_cache_foreach(arp_line_cb cb, void* ctx){
    if(!g_initialized){
        cb("ARP katmani baslatilmadi. Once 'ipconfig' calistir.", 0x0C, ctx);
        return;
    }
    bool any = false;
    for(int i = 0; i < ARP_CACHE_SIZE; i++){
        if(g_cache[i].state == ARP_ENTRY_FREE) continue;
        any = true;

        // "10.0.2.2        52:54:00:12:34:56  RESOLVED"
        char line[64];
        int pos = 0;

        // IP (16 karakter, sola yaslanmış, boşluk doldur)
        char ipbuf[16]; ip_to_str(g_cache[i].ip, ipbuf);
        for(int k=0; ipbuf[k]; k++) line[pos++] = ipbuf[k];
        while(pos < 16) line[pos++] = ' ';

        // MAC
        for(int j=0;j<6;j++){
            const char* h="0123456789ABCDEF";
            line[pos++]=h[(g_cache[i].mac[j]>>4)&0xF];
            line[pos++]=h[g_cache[i].mac[j]&0xF];
            if(j<5) line[pos++]=':';
        }
        line[pos++] = ' ';
        line[pos++] = ' ';

        // Durum
        const char* st = "";
        uint8_t color = 0x07;
        switch(g_cache[i].state){
            case ARP_ENTRY_PENDING:  st="PENDING";  color=0x0E; break;
            case ARP_ENTRY_RESOLVED: st="RESOLVED"; color=0x0A; break;
            case ARP_ENTRY_STATIC:   st="STATIC";   color=0x0B; break;
            default: st="?"; break;
        }
        for(int k=0; st[k]; k++) line[pos++]=st[k];
        line[pos]='\0';

        cb(line, color, ctx);
    }
    if(!any) cb("  (Cache bos)", 0x08, ctx);
}

bool arp_is_initialized(void){ return g_initialized; }