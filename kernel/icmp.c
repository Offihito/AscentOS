// icmp.c — AscentOS ICMP Katmanı (Aşama 3)
// RFC 792 — Internet Control Message Protocol
//
// Desteklenen işlevler:
//   • Echo Request (ping) gönderme
//   • Echo Reply (pong) alma + RTT hesaplama
//   • Gelen Echo Request'lere otomatik Reply gönderme
//   • Destination Unreachable / Time Exceeded loglama

#include "icmp.h"
#include "ipv4.h"
#include "arp.h"

// ============================================================================
// Kernel yardımcıları
// ============================================================================
extern void     serial_print(const char*);
extern void     serial_write(char);
extern uint64_t get_system_ticks(void);  // timer.c — her tick ~1 ms (QEMU varsayılan)

// ============================================================================
// Yerel yardımcılar
// ============================================================================
static void _memcpy(void* d, const void* s, uint32_t n){
    uint8_t* dp=(uint8_t*)d; const uint8_t* sp=(const uint8_t*)s;
    while(n--) *dp++=*sp++;
}
static void _memset(void* d, uint8_t v, uint32_t n){
    uint8_t* dp=(uint8_t*)d; while(n--) *dp++=v;
}
static inline uint16_t _htons(uint16_t v){ return (uint16_t)((v>>8)|(v<<8)); }
static inline uint16_t _ntohs(uint16_t v){ return (uint16_t)((v>>8)|(v<<8)); }

static void _ser_dec(uint32_t v){
    if(!v){ serial_write('0'); return; }
    char b[12]; int i=0;
    while(v){ b[i++]='0'+(v%10); v/=10; }
    while(i--) serial_write(b[i]);
}
static void _ser_hex8(uint8_t v){
    const char* h="0123456789ABCDEF";
    serial_write(h[(v>>4)&0xF]); serial_write(h[v&0xF]);
}

// ============================================================================
// ICMP checksum (RFC 792: başlık + veri üzerinde ones-complement toplamı)
// ipv4_checksum ile aynı algoritma — ICMP için ayrı tutuyoruz (daha net)
// ============================================================================
static uint16_t icmp_checksum(const void* data, uint16_t len){
    const uint16_t* ptr = (const uint16_t*)data;
    uint32_t sum = 0;
    while(len > 1){ sum += *ptr++; len -= 2; }
    if(len) sum += *(const uint8_t*)ptr;
    while(sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum);
}

// ============================================================================
// Modül durumu
// ============================================================================
static bool               g_initialized    = false;
static volatile PingState g_ping_state     = PING_IDLE;  // interrupt'tan yazılır
static volatile uint16_t  g_ping_seq       = 0;
static volatile uint64_t  g_ping_sent_tick = 0;
static volatile uint32_t  g_last_rtt_ms    = 0;
static volatile uint64_t  g_last_rtt_ticks = 0;
static uint8_t            g_last_src[4]    = {0};
static uint8_t            g_ping_dst[4]    = {0};

// Echo veri alanı sabiti (32 byte, Wireshark'ta tanınır)
static const char PING_DATA[ICMP_PAYLOAD_LEN] =
    "AscentOS-PING-ASAMA3-HOBI-OS!!";  // tam 32 karakter

// ============================================================================
// ICMP Echo Reply gönder (birine ping geldiğinde)
// ============================================================================
static void send_echo_reply(const uint8_t dst_ip[4],
                            uint16_t id, uint16_t seq,
                            const uint8_t* echo_data, uint16_t data_len)
{
    // ICMP paketi: başlık (8 byte) + veri
    uint16_t icmp_len = (uint16_t)(ICMP_HDR_LEN + data_len);
    uint8_t  buf[ICMP_HDR_LEN + 256];   // max 256 byte veri
    if(data_len > 256) data_len = 256;
    _memset(buf, 0, icmp_len);

    ICMPHeader* hdr = (ICMPHeader*)buf;
    hdr->type     = ICMP_TYPE_ECHO_REPLY;
    hdr->code     = 0;
    hdr->checksum = 0;
    hdr->id       = id;   // network byte order olduğu gibi yansıt
    hdr->seq      = seq;
    _memcpy(buf + ICMP_HDR_LEN, echo_data, data_len);
    hdr->checksum = icmp_checksum(buf, icmp_len);

    ipv4_send(dst_ip, IP_PROTO_ICMP, buf, icmp_len);

    serial_print("[ICMP] Echo Reply gonderildi -> ");
    char ipbuf[16]; ip_to_str(dst_ip, ipbuf); serial_print(ipbuf);
    serial_write('\n');
}

// ============================================================================
// Gelen ICMP paketini işle (ipv4_register_handler üzerinden çağrılır)
// ============================================================================
void icmp_handle_packet(const uint8_t src_ip[4],
                        const uint8_t* payload, uint16_t len)
{
    if(!g_initialized) return;
    if(len < ICMP_HDR_LEN) return;

    const ICMPHeader* hdr = (const ICMPHeader*)payload;

    // Checksum doğrula
    if(icmp_checksum(payload, len) != 0){
        serial_print("[ICMP] Checksum hatasi, atildi\n");
        return;
    }

    uint8_t  type = hdr->type;
    uint8_t  code = hdr->code;
    uint16_t id   = hdr->id;
    uint16_t seq  = hdr->seq;

    switch(type){

    // ── Echo Reply ────────────────────────────────────────────────────────
    case ICMP_TYPE_ECHO_REPLY:
        serial_print("[ICMP] Echo Reply  id=");
        _ser_dec(_ntohs(id));
        serial_print("  seq=");
        _ser_dec(_ntohs(seq));
        serial_print("  from=");
        { char b[16]; ip_to_str(src_ip, b); serial_print(b); }
        serial_write('\n');

        // Bizim ping'imiz mi? (id + seq eşleşmesi yeterli — state'e güvenmiyoruz)
        if(_ntohs(id)  == ICMP_ECHO_ID &&
           _ntohs(seq) == (uint16_t)g_ping_seq)
        {
            uint64_t now = get_system_ticks();
            uint64_t dt  = (now > g_ping_sent_tick) ? (now - g_ping_sent_tick) : 0;
            g_last_rtt_ticks = dt;
            g_last_rtt_ms    = icmp_ticks_to_ms((uint32_t)dt);
            _memcpy(g_last_src, src_ip, 4);
            g_ping_state   = PING_SUCCESS;

            serial_print("[ICMP] Ping basarili! RTT~");
            if(g_last_rtt_ms == 0){
                // Tick farkı var ama ms'e yuvarlandı — microsecond göster
                _ser_dec((uint32_t)dt);
                serial_print(" tick (<1ms)\n");
            } else {
                _ser_dec(g_last_rtt_ms);
                serial_print("ms\n");
            }
        }
        break;

    // ── Echo Request (başkası bizi ping'liyor) ────────────────────────────
    case ICMP_TYPE_ECHO_REQUEST: {
        serial_print("[ICMP] Echo Request alindi, Reply gonderiyoruz\n");
        const uint8_t* echo_data = payload + ICMP_HDR_LEN;
        uint16_t       data_len  = (uint16_t)(len - ICMP_HDR_LEN);
        send_echo_reply(src_ip, id, seq, echo_data, data_len);
        break;
    }

    // ── Destination Unreachable ───────────────────────────────────────────
    case ICMP_TYPE_UNREACHABLE:
        serial_print("[ICMP] Destination Unreachable  code=");
        _ser_dec(code);
        serial_print("  from=");
        { char b[16]; ip_to_str(src_ip, b); serial_print(b); }
        serial_write('\n');
        if(g_ping_state == PING_PENDING)
            g_ping_state = PING_UNREACHABLE;
        break;

    // ── Time Exceeded (TTL bitti) ─────────────────────────────────────────
    case ICMP_TYPE_TIME_EXCEEDED:
        serial_print("[ICMP] Time Exceeded (TTL=0)  from=");
        { char b[16]; ip_to_str(src_ip, b); serial_print(b); }
        serial_write('\n');
        break;

    // ── Bilinmeyen tip ────────────────────────────────────────────────────
    default:
        serial_print("[ICMP] Bilinmeyen tip=");
        _ser_dec(type);
        serial_write('\n');
        break;
    }
}

// ============================================================================
// Public API — başlatma
// ============================================================================
void icmp_init(void){
    g_initialized  = true;
    g_ping_state   = PING_IDLE;
    g_ping_seq     = 0;
    g_last_rtt_ms  = 0;
    _memset(g_last_src, 0, 4);
    _memset(g_ping_dst, 0, 4);

    // IPv4 katmanına ICMP handler'ını kaydet
    ipv4_register_handler(IP_PROTO_ICMP, icmp_handle_packet);

    serial_print("[ICMP] Baslatildi, IPv4'e kayit yapildi (proto=1)\n");
}

// ============================================================================
// Ping gönder
// ============================================================================
bool icmp_ping(const uint8_t dst_ip[4]){
    if(!g_initialized || !ipv4_is_initialized()) return false;

    g_ping_seq++;
    g_ping_state     = PING_PENDING;
    g_ping_sent_tick = get_system_ticks();
    _memcpy(g_ping_dst, dst_ip, 4);

    // ICMP Echo Request paketi oluştur
    uint8_t buf[ICMP_HDR_LEN + ICMP_PAYLOAD_LEN];
    _memset(buf, 0, sizeof(buf));

    ICMPHeader* hdr = (ICMPHeader*)buf;
    hdr->type     = ICMP_TYPE_ECHO_REQUEST;
    hdr->code     = 0;
    hdr->checksum = 0;
    hdr->id       = _htons(ICMP_ECHO_ID);
    hdr->seq      = _htons(g_ping_seq);
    _memcpy(buf + ICMP_HDR_LEN, PING_DATA, ICMP_PAYLOAD_LEN);
    hdr->checksum = icmp_checksum(buf, sizeof(buf));

    serial_print("[ICMP] Ping gonderiliyor -> ");
    { char b[16]; ip_to_str(dst_ip, b); serial_print(b); }
    serial_print("  seq=");
    _ser_dec(g_ping_seq);
    serial_write('\n');

    bool sent = ipv4_send(dst_ip, IP_PROTO_ICMP, buf, sizeof(buf));
    if(!sent){
        // ARP henüz çözümlenmedi, durumu IDLE'a al (çağıran retry yapabilir)
        g_ping_state = PING_IDLE;
    }
    return sent;
}

// ============================================================================
// Durum sorgulama fonksiyonları
// ============================================================================
PingState icmp_ping_state(void)              { return (PingState)g_ping_state; }
uint32_t  icmp_last_rtt_ms(void)             { return (uint32_t)g_last_rtt_ms; }
uint16_t  icmp_last_seq(void)                { return (uint16_t)g_ping_seq; }
void      icmp_get_last_src(uint8_t out[4])  { _memcpy(out, g_last_src, 4); }
void      icmp_ping_reset(void)              { g_ping_state = PING_IDLE; }

// ============================================================================
// Ticks → ms dönüşümü
// QEMU varsayılan timer: IRQ0 PIT ~18.2 Hz (eski) ya da 1000 Hz (HPET/LAPIC)
// AscentOS'ta her tick ~1 ms ise çarpan = 1.
// Sistemin gerçek tick hızına göre bu sabiti ayarla.
// ============================================================================
uint32_t icmp_ticks_to_ms(uint32_t ticks){
    // Varsayım: 1 tick = 1 ms
    // Eğer PIT 18.2 Hz ise: return (ticks * 1000) / 18;
    return ticks;
}