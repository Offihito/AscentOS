// dhcp.c — AscentOS DHCP İstemcisi (Temiz Yeniden Yazım)
// RFC 2131 — Dynamic Host Configuration Protocol
//
// ── NEDEN YENİDEN YAZILDI? ───────────────────────────────────────────────────
//
//  Önceki sürümün sorunu: UDP checksum hesabı tutarsızdı.
//  Pseudo-header big-endian (byte-by-byte) inşa edilirken UDP segment
//  uint16_t* cast ile little-endian okunuyordu. Bu karışım yanlış checksum
//  üretiyordu. QEMU SLiRP yanlış checksum'lu UDP paketini DROP ediyor,
//  DHCPOFFER hiç gelmiyor.
//
//  ÇÖZÜM: DHCP yayın paketleri UDP checksum = 0 ile gönderilir.
//  RFC 768 §1: "Checksum is optional for UDP. A value of zero means not computed."
//  RFC 2131: DHCP istemcileri checksum=0 kullanabilir ve tüm DHCP sunucuları
//  (QEMU SLiRP dahil) bunu kabul eder.
//
// ── MİMARİ ──────────────────────────────────────────────────────────────────
//
//  Gönderim:  dhcp_discover/request
//                → udp_set_csum_mode(DISABLE)
//                → udp_broadcast (checksum=0)
//                → ipv4_send → rtl8139_send
//
//  Alma:      rtl8139 IRQ → net_packet_callback
//                → ipv4_handle_packet → udp_handle_packet (port 68)
//                → dhcp_handle_packet → state machine
//
// ── STATE MACHINE ────────────────────────────────────────────────────────────
//
//  IDLE → [dhcp_discover()] → SELECTING
//       → [OFFER alındı]    → REQUESTING → [dhcp_send_request()]
//       → [ACK alındı]      → BOUND
//       → [NAK alındı]      → FAILED
//       → [timeout]         → FAILED

#include "dhcp.h"
#include "udp.h"
#include "ipv4.h"
#include "arp.h"
#include <stddef.h>

// ============================================================================
// Harici bağımlılıklar
// ============================================================================
extern void     serial_print(const char*);
extern void     serial_write(char);
extern uint64_t get_system_ticks(void);
extern void     rtl8139_get_mac(uint8_t out[6]);
extern bool     arp_is_initialized(void);

// ============================================================================
// Küçük yardımcılar — stdlib yok, freestanding kernel
// ============================================================================
static void _mc(void* d, const void* s, uint32_t n){
    uint8_t* dp=(uint8_t*)d; const uint8_t* sp=(const uint8_t*)s;
    while(n--) *dp++=*sp++;
}
static void _ms(void* d, uint8_t v, uint32_t n){
    uint8_t* dp=(uint8_t*)d; while(n--) *dp++=v;
}
static int _mcmp(const void* a, const void* b, uint32_t n){
    const uint8_t* ap=(const uint8_t*)a; const uint8_t* bp=(const uint8_t*)b;
    while(n--){ if(*ap!=*bp) return (int)*ap-(int)*bp; ap++; bp++; }
    return 0;
}

// Ağ byte sırası dönüşümleri (big-endian ↔ little-endian x86)
static inline uint16_t _be16(uint16_t v){ return (uint16_t)((v>>8)|(v<<8)); }
static inline uint32_t _be32(uint32_t v){
    return ((v&0xFF000000u)>>24)|((v&0x00FF0000u)>>8)|
           ((v&0x0000FF00u)<<8) |((v&0x000000FFu)<<24);
}

// Seri port yardımcıları
static void _sprint_ip(const uint8_t ip[4]){
    char b[16]; ip_to_str(ip, b); serial_print(b);
}
static void _sprint_u32(uint32_t v){
    if(!v){ serial_write('0'); return; }
    char b[12]; int i=0;
    while(v){ b[i++]='0'+(v%10); v/=10; }
    while(i--) serial_write(b[i]);
}
static void _sprint_hex32(uint32_t v){
    const char* h="0123456789ABCDEF";
    serial_write('0'); serial_write('x');
    for(int i=28;i>=0;i-=4) serial_write(h[(v>>i)&0xF]);
}

// ============================================================================
// Sabitler
// ============================================================================
static const uint8_t IP_ZERO[4]          = {0,0,0,0};
static const uint8_t IP_BROADCAST[4]     = {255,255,255,255};

#define DHCP_RETRY_COUNT    3       // En fazla kaç kez DISCOVER gönderilsin
#define DHCP_RETRY_TIMEOUT  2000    // Her deneme için bekleme (ms/tick)

// ============================================================================
// Modül durumu
// ============================================================================
static bool       g_init    = false;
static DHCPState  g_state   = DHCP_STATE_IDLE;
static DHCPConfig g_cfg;
static uint32_t   g_xid     = 0;
static uint8_t    g_mac[6]  = {0};
static uint8_t    g_offered_ip[4] = {0};
static uint8_t    g_server_ip[4]  = {0};
static uint8_t    g_csum_save;   // önceki csum modunu sakla

// ============================================================================
// XID: her deneme için farklı olacak şekilde basit bir sayaç + timer karışımı
// ============================================================================
static uint32_t _make_xid(void){
    uint64_t t = get_system_ticks();
    uint32_t x = (uint32_t)(t ^ 0xDEAD0000u)
                 ^ ((uint32_t)g_mac[3]<<16)
                 ^ ((uint32_t)g_mac[4]<< 8)
                 ^ ((uint32_t)g_mac[5]);
    x ^= (x << 13); x ^= (x >> 17); x ^= (x << 5);  // xorshift32
    return x ? x : 0x12345678u;
}

// ============================================================================
// DHCP paketi oluştur ve yayın olarak gönder
//
// msg_type      : DHCPDISCOVER veya DHCPREQUEST
// requested_ip  : DHCPREQUEST'te teklif edilen IP (DISCOVER'da NULL)
// server_id_ip  : DHCPREQUEST'te seçilen sunucu IP'si (DISCOVER'da NULL)
//
// Checksum politikası: UDP_CSUM_DISABLE ile gönderilir (checksum = 0).
//   RFC 768'e göre geçerli; tüm DHCP sunucuları kabul eder.
//   Bu sayede pseudo-header byte-order tutarsızlığından kaynaklanan
//   QEMU SLiRP DROP sorunu tamamen ortadan kalkar.
// ============================================================================
static bool _dhcp_send(uint8_t msg_type,
                       const uint8_t* requested_ip,
                       const uint8_t* server_id_ip)
{
    // ── DHCP paket tamponu (544 byte; RFC 2131 minimum 548'dir) ──────────
    uint8_t pkt[548];
    _ms(pkt, 0, sizeof(pkt));

    DHCPHeader* hdr = (DHCPHeader*)pkt;

    // Sabit BOOTP/DHCP alanları
    hdr->op    = DHCP_OP_REQUEST;    // 1 = istemci→sunucu
    hdr->htype = DHCP_HTYPE_ETH;     // 1 = Ethernet
    hdr->hlen  = DHCP_HLEN_ETH;      // 6
    hdr->hops  = 0;
    hdr->xid   = _be32(g_xid);       // işlem ID (big-endian)
    hdr->secs  = 0;
    hdr->flags = _be16(DHCP_FLAGS_BROADCAST); // 0x8000: broadcast yanıt iste

    // ciaddr: RENEWING'de kendi IP'mizi yazar; diğer durumlarda sıfır
    if(g_state == DHCP_STATE_RENEWING)
        _mc(hdr->ciaddr, g_cfg.ip, 4);

    // chaddr: MAC adresimiz (kalan 10 byte zaten sıfır)
    _mc(hdr->chaddr, g_mac, 6);

    // Magic cookie: RFC 2131 §3 — 99.130.83.99 = 0x63825363
    hdr->magic = _be32(DHCP_MAGIC_COOKIE);

    // ── Options alanı ────────────────────────────────────────────────────
    uint8_t* opt = pkt + DHCP_HDR_LEN;  // magic cookie'den hemen sonra
    int p = 0;                            // yazma pozisyonu

    // Opt 53 — DHCP Message Type (zorunlu, her zaman ilk)
    opt[p++] = DHCP_OPT_MSG_TYPE; opt[p++] = 1; opt[p++] = msg_type;

    // Opt 61 — Client Identifier (tip=1[Ethernet] + MAC)
    opt[p++] = DHCP_OPT_CLIENT_ID; opt[p++] = 7; opt[p++] = 0x01;
    _mc(opt+p, g_mac, 6); p += 6;

    if(msg_type == DHCPREQUEST){
        // Opt 50 — Requested IP Address (teklif edilen IP)
        if(requested_ip && _mcmp(requested_ip, IP_ZERO, 4) != 0){
            opt[p++] = DHCP_OPT_REQUESTED_IP; opt[p++] = 4;
            _mc(opt+p, requested_ip, 4); p += 4;
        }
        // Opt 54 — Server Identifier (hangi sunucuyu seçtiğimizi bildir)
        if(server_id_ip && _mcmp(server_id_ip, IP_ZERO, 4) != 0){
            opt[p++] = DHCP_OPT_SERVER_ID; opt[p++] = 4;
            _mc(opt+p, server_id_ip, 4); p += 4;
        }
    }

    // Opt 55 — Parameter Request List
    opt[p++] = DHCP_OPT_PARAM_REQUEST; opt[p++] = 4;
    opt[p++] = DHCP_OPT_SUBNET_MASK;
    opt[p++] = DHCP_OPT_ROUTER;
    opt[p++] = DHCP_OPT_DNS_SERVER;
    opt[p++] = DHCP_OPT_LEASE_TIME;

    // Opt 12 — Hostname
    const char* hn = "ascentos"; uint8_t hl = 8;
    opt[p++] = DHCP_OPT_HOSTNAME; opt[p++] = hl;
    for(int i=0;i<hl;i++) opt[p++]=(uint8_t)hn[i];

    // Opt 255 — End
    opt[p++] = DHCP_OPT_END;

    uint16_t total = (uint16_t)(DHCP_HDR_LEN + p);

    serial_print("[DHCP] TX ");
    serial_print(msg_type == DHCPDISCOVER ? "DISCOVER" :
                 msg_type == DHCPREQUEST  ? "REQUEST"  : "?");
    serial_print("  xid="); _sprint_hex32(g_xid); serial_write('\n');

    // ── UDP checksum DISABLE: yayın paketleri için checksum=0 geçerli ───
    // RFC 768: "An all zero transmitted checksum value means that the
    //  transmitter generated no checksum."
    // QEMU SLiRP ve tüm RFC-uyumlu DHCP sunucuları bunu kabul eder.
    udp_set_csum_mode(UDP_CSUM_DISABLE);
    bool ok = udp_broadcast(DHCP_SERVER_PORT, DHCP_CLIENT_PORT, pkt, total);
    udp_set_csum_mode(UDP_CSUM_ENABLE);   // geri aç (diğer UDP trafiği için)

    if(!ok) serial_print("[DHCP] TX BASARISIZ (udp_broadcast)\n");
    return ok;
}

// ============================================================================
// DHCP Options ayrıştırıcı
// Gereken alanları çıkarır; bilinmeyen option'lar atlanır.
// ============================================================================
static uint8_t _parse_opts(const uint8_t* opts, uint16_t len,
                            uint8_t  out_subnet[4],
                            uint8_t  out_gw[4],
                            uint8_t  out_dns[4],
                            uint8_t  out_server[4],
                            uint32_t* out_lease,
                            uint32_t* out_t1,
                            uint32_t* out_t2)
{
    uint8_t msg_type = 0;
    *out_lease = 86400; *out_t1 = 43200; *out_t2 = 75600;

    uint16_t i = 0;
    while(i < len){
        uint8_t code = opts[i++];
        if(code == 0)   continue;   // Pad byte
        if(code == 255) break;      // End

        if(i >= len) break;
        uint8_t olen = opts[i++];
        if(i + olen > len) break;

        switch(code){
        case DHCP_OPT_MSG_TYPE:
            if(olen >= 1) msg_type = opts[i];
            break;
        case DHCP_OPT_SUBNET_MASK:
            if(olen >= 4) _mc(out_subnet, opts+i, 4);
            break;
        case DHCP_OPT_ROUTER:
            if(olen >= 4) _mc(out_gw, opts+i, 4);
            break;
        case DHCP_OPT_DNS_SERVER:
            if(olen >= 4) _mc(out_dns, opts+i, 4);
            break;
        case DHCP_OPT_SERVER_ID:
            if(olen >= 4) _mc(out_server, opts+i, 4);
            break;
        case DHCP_OPT_LEASE_TIME:
            if(olen >= 4){ uint32_t v; _mc(&v, opts+i, 4); *out_lease=_be32(v); }
            break;
        case DHCP_OPT_RENEWAL_TIME:
            if(olen >= 4){ uint32_t v; _mc(&v, opts+i, 4); *out_t1=_be32(v); }
            break;
        case DHCP_OPT_REBINDING_TIME:
            if(olen >= 4){ uint32_t v; _mc(&v, opts+i, 4); *out_t2=_be32(v); }
            break;
        default: break;
        }
        i += olen;
    }
    return msg_type;
}

// ============================================================================
// Gelen UDP paketini işle — udp_bind(68, ...) callback'i
// ============================================================================
void dhcp_handle_packet(const void* raw, void* ctx){
    (void)ctx;
    if(!g_init) return;

    const UDPPacket* upkt = (const UDPPacket*)raw;

    // Minimum boyut kontrolü (DHCP başlığı = 240 byte)
    if(upkt->len < DHCP_HDR_LEN){
        serial_print("[DHCP] RX: cok kisa, atildi\n");
        return;
    }

    const DHCPHeader* hdr = (const DHCPHeader*)upkt->data;

    // BOOTREPLY (2) olmalı
    if(hdr->op != DHCP_OP_REPLY) return;

    // Magic cookie
    if(_be32(hdr->magic) != DHCP_MAGIC_COOKIE){
        serial_print("[DHCP] RX: gecersiz magic, atildi\n");
        return;
    }

    // XID: bizim isteğimize mı yanıt?
    if(_be32(hdr->xid) != g_xid){
        serial_print("[DHCP] RX: XID uyumsuz, atildi\n");
        return;
    }

    // Options
    uint8_t subnet[4]={255,255,255,0}, gw[4]={0}, dns[4]={0}, srv[4]={0};
    uint32_t lease=86400, t1=43200, t2=75600;

    const uint8_t* opts     = upkt->data + DHCP_HDR_LEN;
    uint16_t       opts_len = (uint16_t)(upkt->len - DHCP_HDR_LEN);
    uint8_t        mtype    = _parse_opts(opts, opts_len,
                                          subnet, gw, dns, srv,
                                          &lease, &t1, &t2);

    serial_print("[DHCP] RX ");
    switch(mtype){
    case DHCPOFFER: serial_print("OFFER"); break;
    case DHCPACK:   serial_print("ACK");   break;
    case DHCPNAK:   serial_print("NAK");   break;
    default:        serial_print("?");     break;
    }
    serial_print("  yiaddr="); _sprint_ip(hdr->yiaddr); serial_write('\n');

    // ── Durum makinesi ───────────────────────────────────────────────────

    switch(mtype){

    // OFFER → REQUEST gönder
    case DHCPOFFER:
        if(g_state != DHCP_STATE_SELECTING) return;

        _mc(g_offered_ip, hdr->yiaddr, 4);
        _mc(g_server_ip,  srv,         4);

        // Server ID option yoksa kaynak IP'yi kullan
        if(_mcmp(g_server_ip, IP_ZERO, 4) == 0)
            _mc(g_server_ip, upkt->src_ip, 4);

        serial_print("[DHCP] Teklif kabul edildi: ");
        _sprint_ip(g_offered_ip);
        serial_print("  sunucu: ");
        _sprint_ip(g_server_ip);
        serial_write('\n');

        g_state = DHCP_STATE_REQUESTING;
        _dhcp_send(DHCPREQUEST, g_offered_ip, g_server_ip);
        break;

    // ACK → BOUND: ağ yapılandırmasını uygula
    case DHCPACK:
        if(g_state != DHCP_STATE_REQUESTING &&
           g_state != DHCP_STATE_RENEWING   &&
           g_state != DHCP_STATE_REBINDING) return;

        // Yapılandırmayı kaydet
        _mc(g_cfg.ip,        hdr->yiaddr, 4);
        _mc(g_cfg.subnet,    subnet,      4);
        _mc(g_cfg.gateway,   gw,          4);
        _mc(g_cfg.dns,       dns,         4);
        _mc(g_cfg.server_ip, g_server_ip, 4);
        g_cfg.lease_time       = lease;
        g_cfg.renewal_time     = t1;
        g_cfg.rebinding_time   = t2;
        g_cfg.lease_start_tick = (uint32_t)get_system_ticks();
        g_cfg.valid            = true;
        g_state                = DHCP_STATE_BOUND;

        // ARP katmanını gerçek IP ile başlat (önceki 0.0.0.0 yerine)
        arp_init(g_cfg.ip, g_mac);
        ipv4_set_subnet(g_cfg.subnet);
        if(_mcmp(g_cfg.gateway, IP_ZERO, 4) != 0)
            ipv4_set_gateway(g_cfg.gateway);

        // Gratuitous ARP: IP çakışma bildirimi + ağa kendimizi tanıtalım
        arp_announce();

        serial_print("[DHCP] *** BOUND ***\n");
        serial_print("[DHCP]   IP      : "); _sprint_ip(g_cfg.ip);      serial_write('\n');
        serial_print("[DHCP]   Subnet  : "); _sprint_ip(g_cfg.subnet);  serial_write('\n');
        serial_print("[DHCP]   Gateway : "); _sprint_ip(g_cfg.gateway); serial_write('\n');
        serial_print("[DHCP]   DNS     : "); _sprint_ip(g_cfg.dns);     serial_write('\n');
        serial_print("[DHCP]   Lease   : "); _sprint_u32(lease); serial_print(" sn\n");
        break;

    // NAK → başarısız
    case DHCPNAK:
        serial_print("[DHCP] NAK alindi, IP reddedildi\n");
        g_state      = DHCP_STATE_FAILED;
        g_cfg.valid  = false;
        break;

    default:
        break;
    }
}

// ============================================================================
// Public API
// ============================================================================

void dhcp_init(void){
    _ms(&g_cfg, 0, sizeof(g_cfg));
    _ms(g_offered_ip, 0, 4);
    _ms(g_server_ip,  0, 4);
    g_state  = DHCP_STATE_IDLE;
    g_xid    = 0;
    g_init   = true;

    // MAC adresini RTL8139'dan al
    rtl8139_get_mac(g_mac);

    // UDP port 68'e dinleyici bağla
    udp_bind(DHCP_CLIENT_PORT, (udp_handler_t)dhcp_handle_packet, NULL);

    serial_print("[DHCP] Baslatildi, port 68 dinleniyor\n");
    serial_print("[DHCP] MAC: ");
    const char* hx="0123456789ABCDEF";
    for(int i=0;i<6;i++){
        serial_write(hx[(g_mac[i]>>4)&0xF]);
        serial_write(hx[g_mac[i]   &0xF]);
        if(i<5) serial_write(':');
    }
    serial_write('\n');
}

// ── dhcp_discover ────────────────────────────────────────────────────────────
// DHCPDISCOVER broadcast'i gönderir ve SELECTING moduna girer.
// ARP henüz başlatılmamışsa 0.0.0.0 ile geçici olarak başlatır.
bool dhcp_discover(void){
    if(!g_init){
        serial_print("[DHCP] Baslatilmamis!\n");
        return false;
    }
    if(!udp_is_initialized()){
        serial_print("[DHCP] UDP hazir degil!\n");
        return false;
    }

    // MAC'i taze oku (init'ten sonra kart başlamış olabilir)
    rtl8139_get_mac(g_mac);

    // Yeni XID üret ve eski yapılandırmayı sıfırla
    g_xid = _make_xid();
    _ms(&g_cfg,         0, sizeof(g_cfg));
    _ms(g_offered_ip,   0, 4);
    _ms(g_server_ip,    0, 4);

    // ARP/IPv4 katmanı henüz başlatılmamışsa 0.0.0.0 ile geçici başlat.
    // Broadcast paketleri için ARP çözümlemesi gerekmez (doğrudan FF:FF:FF gider).
    if(!arp_is_initialized()){
        uint8_t zero[4]={0,0,0,0};
        arp_init(zero, g_mac);
        serial_print("[DHCP] ARP 0.0.0.0 ile gecici baslatildi\n");
    }

    g_state = DHCP_STATE_SELECTING;

    bool ok = _dhcp_send(DHCPDISCOVER, NULL, NULL);
    if(!ok){
        serial_print("[DHCP] DISCOVER gonderilemedi\n");
        g_state = DHCP_STATE_FAILED;
    }
    return ok;
}

// ── dhcp_release ─────────────────────────────────────────────────────────────
void dhcp_release(void){
    if(!g_init || g_state != DHCP_STATE_BOUND) return;

    // RELEASE unicast gönderilir (RFC 2131 §4.4.4)
    uint8_t pkt[DHCP_HDR_LEN + 16];
    _ms(pkt, 0, sizeof(pkt));

    DHCPHeader* hdr = (DHCPHeader*)pkt;
    hdr->op    = DHCP_OP_REQUEST;
    hdr->htype = DHCP_HTYPE_ETH;
    hdr->hlen  = DHCP_HLEN_ETH;
    hdr->xid   = _be32(_make_xid());
    hdr->flags = 0;   // unicast
    _mc(hdr->ciaddr, g_cfg.ip, 4);
    _mc(hdr->chaddr, g_mac, 6);
    hdr->magic = _be32(DHCP_MAGIC_COOKIE);

    uint8_t* opt = pkt + DHCP_HDR_LEN; int p=0;
    opt[p++]=DHCP_OPT_MSG_TYPE;  opt[p++]=1; opt[p++]=DHCPRELEASE;
    opt[p++]=DHCP_OPT_SERVER_ID; opt[p++]=4;
    _mc(opt+p, g_cfg.server_ip, 4); p+=4;
    opt[p++]=DHCP_OPT_END;

    uint16_t plen = (uint16_t)(DHCP_HDR_LEN + p);
    udp_set_csum_mode(UDP_CSUM_DISABLE);
    udp_send(g_cfg.server_ip, DHCP_SERVER_PORT, DHCP_CLIENT_PORT, pkt, plen);
    udp_set_csum_mode(UDP_CSUM_ENABLE);

    serial_print("[DHCP] RELEASE gonderildi: "); _sprint_ip(g_cfg.ip); serial_write('\n');

    g_state     = DHCP_STATE_RELEASED;
    g_cfg.valid = false;
}

// ── Sorgulama fonksiyonları ──────────────────────────────────────────────────
DHCPState   dhcp_get_state(void)       { return g_state; }
bool        dhcp_is_initialized(void)  { return g_init; }
uint32_t    dhcp_get_xid(void)         { return g_xid; }

void dhcp_get_config(DHCPConfig* out){
    if(out) _mc(out, &g_cfg, sizeof(DHCPConfig));
}

const char* dhcp_state_str(void){
    switch(g_state){
    case DHCP_STATE_IDLE:       return "IDLE";
    case DHCP_STATE_SELECTING:  return "SELECTING";
    case DHCP_STATE_REQUESTING: return "REQUESTING";
    case DHCP_STATE_BOUND:      return "BOUND";
    case DHCP_STATE_RENEWING:   return "RENEWING";
    case DHCP_STATE_REBINDING:  return "REBINDING";
    case DHCP_STATE_RELEASED:   return "RELEASED";
    case DHCP_STATE_FAILED:     return "FAILED";
    default:                    return "?";
    }
}