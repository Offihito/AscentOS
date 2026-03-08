// tcp.c — AscentOS TCP Katmanı (Aşama 6) — Temiz Yeniden Yazım
// RFC 793 — Transmission Control Protocol
//
// Checksum notu:
//   GÖNDERME  : tcp_cksum_calc()   → 0 gelirse 0xFFFF döndür (RFC kuralı)
//   DOĞRULAMA : tcp_cksum_verify() → ham ~sum; 0 ise paket geçerli
//
// Önceki hatanın özeti:
//   Eski kod: return r ? r : 0xFFFF
//   Doğrulama: tcp_checksum(...) != 0  → geçerli paketin checksumu 0 gelir
//   ama fonksiyon 0 yerine 0xFFFF döndürüyordu → HER PAKET atılıyordu.
//
// Gelen akış:
//   rtl8139 IRQ → net_packet_callback → ipv4_handle_packet
//              → tcp_handle_packet → state machine → event_cb
//
// Gönderim akışı:
//   tcp_send → send_buf → tcp_flush → send_seg → tcp_cksum_calc → ipv4_send

#include "tcp.h"
#include "ipv4.h"
#include "arp.h"
#include <stddef.h>

// ============================================================================
// Kernel yardımcıları
// ============================================================================
extern void     serial_print(const char*);
extern void     serial_write(char);
extern uint64_t get_system_ticks(void);

// ============================================================================
// Freestanding yardımcılar
// ============================================================================
static void _mc(void* d, const void* s, uint32_t n) {
    uint8_t* dp = (uint8_t*)d;
    const uint8_t* sp = (const uint8_t*)s;
    while (n--) *dp++ = *sp++;
}
static void _ms(void* d, uint8_t v, uint32_t n) {
    uint8_t* p = (uint8_t*)d; while (n--) *p++ = v;
}
static int _meq(const void* a, const void* b, uint32_t n) {
    const uint8_t* ap = (const uint8_t*)a;
    const uint8_t* bp = (const uint8_t*)b;
    while (n--) if (*ap++ != *bp++) return 0;
    return 1;
}

// Big-endian ↔ little-endian (x86 host)
static inline uint16_t be16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
static inline uint32_t be32(uint32_t v) {
    return ((v & 0xFF000000u) >> 24) | ((v & 0x00FF0000u) >> 8)
         | ((v & 0x0000FF00u) << 8)  | ((v & 0x000000FFu) << 24);
}

// Seri log yardımcıları
static void _sdec(uint32_t v) {
    if (!v) { serial_write('0'); return; }
    char b[12]; int i = 0;
    while (v) { b[i++] = '0' + (v % 10); v /= 10; }
    while (i--) serial_write(b[i]);
}
static void _ship(const uint8_t ip[4]) {
    char buf[16]; ip_to_str(ip, buf); serial_print(buf);
}

// RFC 793 §3.3 — sıra numarası karşılaştırması (wraparound güvenli)
static inline bool seq_lt(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }
static inline bool seq_le(uint32_t a, uint32_t b) { return (int32_t)(a - b) <= 0; }

// ============================================================================
// Checksum — iki ayrı fonksiyon
//
//  IPv4 pseudo-header: src_ip(4) | dst_ip(4) | 0x00 | proto(0x06) | tcp_len(2)
//
//  _cksum_sum  : raw fold — hem gönderme hem doğrulama için iç kullanım
//  tcp_cksum_calc   : gönderme — ~sum, 0 gelince 0xFFFF (RFC 793)
//  tcp_cksum_verify : doğrulama — ~sum olduğu gibi; 0 = geçerli
// ============================================================================
static uint16_t _cksum_sum(const uint8_t src[4], const uint8_t dst[4],
                            const uint8_t* seg, uint16_t tcp_len)
{
    uint32_t s = 0;
    // Pseudo-header (byte-by-byte — alignment güvenli, endian güvenli)
    s += ((uint32_t)src[0] << 8) | src[1];
    s += ((uint32_t)src[2] << 8) | src[3];
    s += ((uint32_t)dst[0] << 8) | dst[1];
    s += ((uint32_t)dst[2] << 8) | dst[3];
    s += (uint32_t)IP_PROTO_TCP;   // = 6
    s += (uint32_t)tcp_len;

    // TCP segment (big-endian 16-bit çiftler)
    const uint8_t* p = seg;
    uint16_t rem = tcp_len;
    while (rem > 1) { s += ((uint32_t)p[0] << 8) | p[1]; p += 2; rem -= 2; }
    if (rem)         s += (uint32_t)p[0] << 8;  // son tek bayt → high byte

    // Carry fold
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return (uint16_t)(~s & 0xFFFF);
}

// Gönderme: 0 → 0xFFFF (RFC: 0 "checksum yok" anlamına gelir)
static uint16_t tcp_cksum_calc(const uint8_t src[4], const uint8_t dst[4],
                                const uint8_t* seg, uint16_t tcp_len)
{
    uint16_t r = _cksum_sum(src, dst, seg, tcp_len);
    return r ? r : 0xFFFF;
}

// Doğrulama: geçerli paket → 0 döner  (0xFFFF case özel işlem YOK)
static uint16_t tcp_cksum_verify(const uint8_t src[4], const uint8_t dst[4],
                                  const uint8_t* seg, uint16_t tcp_len)
{
    return _cksum_sum(src, dst, seg, tcp_len);
}

// ============================================================================
// ISN üreteci (kriptografik DEĞİL — hobi OS için yeterli)
// ============================================================================
static uint32_t make_isn(uint16_t lp, uint16_t rp) {
    uint64_t t = get_system_ticks();
    uint32_t x = (uint32_t)(t ^ ((uint32_t)lp << 16) ^ rp ^ (t >> 32));
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;  // xorshift32
    return x ? x : 0xBEEF0001u;
}

// ============================================================================
// Efemeral port sayacı
// ============================================================================
static uint16_t g_next_eport = TCP_EPHEMERAL_BASE;
static uint16_t alloc_eport(void) {
    uint16_t p = g_next_eport++;
    if (g_next_eport > TCP_EPHEMERAL_MAX) g_next_eport = TCP_EPHEMERAL_BASE;
    return p;
}

// ============================================================================
// Modül durumu
// ============================================================================
static bool     g_init   = false;
static TCPConn  g_conns[TCP_MAX_CONN];
static uint32_t g_tx_cnt = 0;
static uint32_t g_rx_cnt = 0;

// ============================================================================
// Bağlantı yönetimi
// ============================================================================
static TCPConn* conn_alloc(void) {
    for (int i = 0; i < TCP_MAX_CONN; i++) {
        if (!g_conns[i].used) {
            _ms(&g_conns[i], 0, sizeof(TCPConn));
            g_conns[i].used  = true;
            g_conns[i].state = TCP_STATE_CLOSED;
            return &g_conns[i];
        }
    }
    return NULL;
}
static int conn_idx(const TCPConn* c) { return (int)(c - g_conns); }
static TCPConn* conn_by_id(int id) {
    if (id < 0 || id >= TCP_MAX_CONN) return NULL;
    return g_conns[id].used ? &g_conns[id] : NULL;
}

// Gelen segmenti eşleştir: aktif bağlantı önce, yoksa LISTEN slotu
static TCPConn* conn_match(const uint8_t rip[4], uint16_t rport, uint16_t lport) {
    TCPConn* listener = NULL;
    for (int i = 0; i < TCP_MAX_CONN; i++) {
        TCPConn* c = &g_conns[i];
        if (!c->used) continue;
        if (c->local_port != lport) continue;
        if (c->state == TCP_STATE_LISTEN) { listener = c; continue; }
        if (c->remote_port == rport && _meq(c->remote_ip, rip, 4)) return c;
    }
    return listener;
}

// ============================================================================
// Ham segment gönder
// ============================================================================
static bool send_seg(TCPConn* c,
                     uint32_t seq, uint32_t ack,
                     uint8_t  flags,
                     const uint8_t* data, uint16_t dlen,
                     const uint8_t* opts, uint8_t  olen)
{
    uint8_t my_ip[4];
    arp_get_my_ip(my_ip);

    uint8_t  hlen    = (uint8_t)(TCP_HDR_LEN_MIN + olen);
    uint16_t seg_len = (uint16_t)(hlen + dlen);

    if (seg_len > 1460 + 60) {
        serial_print("[TCP] send_seg: oversized\n"); return false;
    }

    uint8_t buf[1500];
    _ms(buf, 0, seg_len);

    TCPHeader* h = (TCPHeader*)buf;
    h->src_port = be16(c->local_port);
    h->dst_port = be16(c->remote_port);
    h->seq      = be32(seq);
    h->ack      = be32(ack);
    h->data_off = (uint8_t)((hlen / 4) << 4);
    h->flags    = flags;
    h->window   = be16(TCP_WINDOW_DEFAULT);
    h->checksum = 0;        // önce 0, sonra hesapla
    h->urg_ptr  = 0;

    if (opts && olen) _mc(buf + TCP_HDR_LEN_MIN, opts, olen);
    if (data && dlen) _mc(buf + hlen, data, dlen);

    // Checksum: my_ip(src) → c->remote_ip(dst)
    h->checksum = be16(tcp_cksum_calc(my_ip, c->remote_ip, buf, seg_len));

    bool ok = ipv4_send(c->remote_ip, IP_PROTO_TCP, buf, seg_len);
    if (ok) { g_tx_cnt++; c->retx_timer = get_system_ticks(); }
    return ok;
}

// ── Kısa gönderim yardımcıları ───────────────────────────────────────────────

static bool send_syn(TCPConn* c, bool synack) {
    uint8_t opt[4] = { TCP_OPT_MSS, TCP_OPT_MSS_LEN,
                       (uint8_t)(TCP_MSS_DEFAULT >> 8),
                       (uint8_t)(TCP_MSS_DEFAULT & 0xFF) };
    uint8_t fl = TCP_FLAG_SYN | (synack ? TCP_FLAG_ACK : 0);
    return send_seg(c, c->snd_isn, c->rcv_nxt, fl, NULL, 0, opt, 4);
}
static bool send_ack(TCPConn* c) {
    return send_seg(c, c->snd_nxt, c->rcv_nxt, TCP_FLAG_ACK, NULL, 0, NULL, 0);
}
static bool send_fin(TCPConn* c) {
    return send_seg(c, c->snd_nxt, c->rcv_nxt,
                    TCP_FLAG_FIN | TCP_FLAG_ACK, NULL, 0, NULL, 0);
}
static void send_rst(const uint8_t dst[4], uint16_t lp, uint16_t rp, uint32_t seq) {
    TCPConn tmp; _ms(&tmp, 0, sizeof(tmp));
    _mc(tmp.remote_ip, dst, 4);
    tmp.local_port = lp; tmp.remote_port = rp; tmp.used = true;
    send_seg(&tmp, seq, 0, TCP_FLAG_RST, NULL, 0, NULL, 0);
}

// ============================================================================
// Halka tamponu
// ============================================================================
static uint16_t rbuf_push(uint8_t* buf, uint16_t* head, uint16_t* len,
                           uint16_t cap, const uint8_t* src, uint16_t n) {
    uint16_t free = (uint16_t)(cap - *len);
    if (n > free) n = free;
    for (uint16_t i = 0; i < n; i++) {
        buf[*head] = src[i]; *head = (uint16_t)((*head + 1) % cap);
    }
    *len += n; return n;
}
static uint16_t rbuf_pop(uint8_t* buf, uint16_t* tail, uint16_t* len,
                          uint16_t cap, uint8_t* dst, uint16_t n) {
    if (n > *len) n = *len;
    for (uint16_t i = 0; i < n; i++) {
        if (dst) dst[i] = buf[*tail];
        *tail = (uint16_t)((*tail + 1) % cap);
    }
    *len -= n; return n;
}

// ============================================================================
// Tampon flush — bekleyen veriyi gönder
// ============================================================================
static void tcp_flush(TCPConn* c) {
    if (c->state != TCP_STATE_ESTABLISHED &&
        c->state != TCP_STATE_CLOSE_WAIT) return;
    if (c->send_len == 0) return;
    // snd_wnd kontrolü — karşı taraf penceresi kapalıysa dur
    if (c->snd_wnd == 0) return;

    // Gönderilecek miktar: tampondaki veri, pencere ve MSS ile sınırlı
    // Retransmit durumunda: snd_una'dan itibaren yeniden gönder
    uint32_t in_flight = c->snd_nxt - c->snd_una;  // gönderilmiş ama ACK'lenmemiş
    if (in_flight >= c->snd_wnd) return;            // pencere doldu

    uint16_t tosend = c->send_len;
    uint16_t wnd_free = (uint16_t)(c->snd_wnd - in_flight);
    if (tosend > wnd_free)       tosend = wnd_free;
    if (tosend > TCP_MSS_DEFAULT) tosend = TCP_MSS_DEFAULT;
    if (tosend == 0) return;

    // send_tail'den oku (ACK'lenmemiş kısım hariç — halka tamponu)
    uint8_t tmp[TCP_MSS_DEFAULT];
    uint16_t tail = c->send_tail;
    // in_flight kadar ileri atla (bu kısım gönderildi ama ACK'lenmedi)
    for (uint32_t i = 0; i < in_flight && i < TCP_SEND_BUF_SIZE; i++)
        tail = (uint16_t)((tail + 1) % TCP_SEND_BUF_SIZE);
    for (uint16_t i = 0; i < tosend; i++) {
        tmp[i] = c->send_buf[tail];
        tail = (uint16_t)((tail + 1) % TCP_SEND_BUF_SIZE);
    }

    if (send_seg(c, c->snd_nxt, c->rcv_nxt,
                 TCP_FLAG_ACK | TCP_FLAG_PSH, tmp, tosend, NULL, 0)) {
        c->snd_nxt += tosend;
        serial_print("[TCP] Data TX: "); _sdec(tosend); serial_print(" B\n");
    }
}

// ============================================================================
// Bağlantı kapat + callback
// ============================================================================
static void conn_die(TCPConn* c, TCPEvent ev) {
    c->state = TCP_STATE_CLOSED;
    if (c->event_cb) c->event_cb(conn_idx(c), ev, NULL, 0, c->cb_ctx);
    c->used = false;
}

// ============================================================================
// IPv4'ten gelen TCP segmenti — ana işleyici
// ============================================================================
void tcp_handle_packet(const uint8_t src_ip[4],
                       const uint8_t* payload, uint16_t len)
{
    if (!g_init)               return;
    if (len < TCP_HDR_LEN_MIN) return;

    // ── Checksum doğrula ─────────────────────────────────────────────────────
    // DÜZELTME (eski hata):
    //   tcp_checksum() "return r ? r : 0xFFFF" yapıyordu.
    //   Doğrulamada != 0 kontrolü → geçerli paket 0 vermesi gerekirken
    //   0→0xFFFF dönüşümü yüzünden HER paket reddediliyordu.
    //
    // QEMU offloading:
    //   checksum alanı 0 ise doğrulamayı atla (HW offload simülasyonu).
    uint8_t my_ip[4];
    arp_get_my_ip(my_ip);
    {
        // checksum alanı network byte order'da — 0 ise offload demek
        uint16_t stored = ((const TCPHeader*)payload)->checksum;
        if (stored != 0) {
            if (tcp_cksum_verify(src_ip, my_ip, payload, len) != 0) {
                serial_print("[TCP] Checksum hatasi, atildi\n");
                return;
            }
        }
    }

    const TCPHeader* h  = (const TCPHeader*)payload;
    uint16_t lport  = be16(h->dst_port);
    uint16_t rport  = be16(h->src_port);
    uint32_t seq    = be32(h->seq);
    uint32_t ack    = be32(h->ack);
    uint8_t  flags  = h->flags;
    uint16_t wnd    = be16(h->window);
    uint8_t  doff   = (uint8_t)((h->data_off >> 4) * 4);

    if (doff < TCP_HDR_LEN_MIN || doff > len) return;

    const uint8_t* data = payload + doff;
    uint16_t       dlen = (uint16_t)(len - doff);
    g_rx_cnt++;

    // ── Bağlantı bul ─────────────────────────────────────────────────────────
    TCPConn* c = conn_match(src_ip, rport, lport);

    // RST
    if (flags & TCP_FLAG_RST) {
        if (c && c->state != TCP_STATE_LISTEN) {
            serial_print("[TCP] RST rx\n");
            conn_die(c, TCP_EVENT_ERROR);
        }
        return;
    }

    // Bilinmeyen hedef → RST
    if (!c) {
        send_rst(src_ip, lport, rport,
                 (flags & TCP_FLAG_ACK) ? ack : seq + (uint32_t)dlen + 1u);
        return;
    }

    // ── State machine ─────────────────────────────────────────────────────────

    switch (c->state) {

    // LISTEN ──────────────────────────────────────────────────────────────────
    case TCP_STATE_LISTEN: {
        if (!(flags & TCP_FLAG_SYN)) return;
        if (flags & TCP_FLAG_ACK) { send_rst(src_ip, lport, rport, ack); return; }

        TCPConn* nc = conn_alloc();
        if (!nc) {
            send_rst(src_ip, lport, rport, seq + 1);
            serial_print("[TCP] Tablo dolu, SYN reddedildi\n");
            return;
        }
        _mc(nc->remote_ip, src_ip, 4);
        nc->local_port  = lport;
        nc->remote_port = rport;
        nc->rcv_isn     = seq;
        nc->rcv_nxt     = seq + 1;
        nc->snd_isn     = make_isn(lport, rport);
        nc->snd_nxt     = nc->snd_isn + 1;
        nc->snd_una     = nc->snd_isn;
        nc->snd_wnd     = wnd;
        nc->rcv_wnd     = TCP_WINDOW_DEFAULT;
        nc->state       = TCP_STATE_SYN_RECEIVED;
        nc->is_server   = true;
        nc->event_cb    = c->event_cb;
        nc->accept_cb   = c->accept_cb;
        nc->cb_ctx      = c->cb_ctx;

        send_syn(nc, true);
        serial_print("[TCP] SYN rx -> SYN+ACK  lport="); _sdec(lport);
        serial_print(" from "); _ship(src_ip); serial_write('\n');
        break;
    }

    // SYN_SENT ────────────────────────────────────────────────────────────────
    case TCP_STATE_SYN_SENT:
        if ((flags & TCP_FLAG_SYN) && (flags & TCP_FLAG_ACK)) {
            if (!seq_le(c->snd_isn, ack - 1) || !seq_le(ack, c->snd_nxt)) {
                send_rst(src_ip, lport, rport, ack);
                conn_die(c, TCP_EVENT_ERROR);
                return;
            }
            c->rcv_isn = seq;
            c->rcv_nxt = seq + 1;
            c->snd_una = ack;
            c->snd_wnd = wnd;
            c->state   = TCP_STATE_ESTABLISHED;
            send_ack(c);

            serial_print("[TCP] ESTABLISHED  lport="); _sdec(c->local_port);
            serial_print(" rport="); _sdec(c->remote_port); serial_write('\n');

            if (c->event_cb)
                c->event_cb(conn_idx(c), TCP_EVENT_CONNECTED, NULL, 0, c->cb_ctx);
            tcp_flush(c);
        } else if (flags & TCP_FLAG_SYN) {
            // Eş zamanlı açılma
            c->rcv_isn = seq; c->rcv_nxt = seq + 1;
            c->state   = TCP_STATE_SYN_RECEIVED;
            send_syn(c, true);
        }
        break;

    // SYN_RECEIVED ────────────────────────────────────────────────────────────
    case TCP_STATE_SYN_RECEIVED:
        if ((flags & TCP_FLAG_ACK) && ack == c->snd_nxt) {
            c->snd_una = ack; c->snd_wnd = wnd;
            c->state   = TCP_STATE_ESTABLISHED;
            serial_print("[TCP] ESTABLISHED (server)  rport=");
            _sdec(c->remote_port); serial_write('\n');
            if (c->accept_cb)
                c->accept_cb(conn_idx(c), src_ip, rport, c->cb_ctx);
            else if (c->event_cb)
                c->event_cb(conn_idx(c), TCP_EVENT_CONNECTED, NULL, 0, c->cb_ctx);
        }
        break;

    // ESTABLISHED ─────────────────────────────────────────────────────────────
    case TCP_STATE_ESTABLISHED:
        if (flags & TCP_FLAG_ACK) {
            if (seq_le(c->snd_una, ack) && seq_le(ack, c->snd_nxt)) {
                uint32_t acked = ack - c->snd_una;
                c->snd_una = ack; c->snd_wnd = wnd; c->retx_count = 0;
                if (acked > 0 && c->send_len > 0) {
                    uint16_t consume = (acked < c->send_len)
                                     ? (uint16_t)acked : c->send_len;
                    rbuf_pop(c->send_buf, &c->send_tail, &c->send_len,
                             TCP_SEND_BUF_SIZE, NULL, consume);
                    if (c->event_cb)
                        c->event_cb(conn_idx(c), TCP_EVENT_SENT,
                                    NULL, consume, c->cb_ctx);
                }
                tcp_flush(c);
            }
        }
        if (dlen > 0) {
            if (seq == c->rcv_nxt) {
                uint16_t stored = rbuf_push(c->recv_buf, &c->recv_head,
                                            &c->recv_len, TCP_RECV_BUF_SIZE,
                                            data, dlen);
                c->rcv_nxt += stored;
                send_ack(c);
                serial_print("[TCP] Data RX: "); _sdec(stored);
                serial_print(" B\n");
                if (c->event_cb)
                    c->event_cb(conn_idx(c), TCP_EVENT_DATA,
                                data, stored, c->cb_ctx);
            } else {
                send_ack(c);  // out-of-order: tekrar ACK yolla
            }
        }
        if (flags & TCP_FLAG_FIN) {
            c->rcv_nxt++;
            send_ack(c);
            c->state = TCP_STATE_CLOSE_WAIT;
            serial_print("[TCP] FIN rx -> CLOSE_WAIT\n");
            if (c->event_cb)
                c->event_cb(conn_idx(c), TCP_EVENT_CLOSED, NULL, 0, c->cb_ctx);
        }
        break;

    case TCP_STATE_CLOSE_WAIT: break;  // tcp_close() bekle

    // FIN_WAIT_1 ──────────────────────────────────────────────────────────────
    case TCP_STATE_FIN_WAIT_1:
        if (flags & TCP_FLAG_ACK) {
            if (ack == c->snd_nxt) {
                c->snd_una = ack;
                if (flags & TCP_FLAG_FIN) {
                    c->rcv_nxt++; send_ack(c);
                    c->state = TCP_STATE_CLOSING;
                } else {
                    c->state = TCP_STATE_FIN_WAIT_2;
                }
            }
        } else if (flags & TCP_FLAG_FIN) {
            c->rcv_nxt++; send_ack(c);
            c->state = TCP_STATE_CLOSING;
        }
        break;

    // FIN_WAIT_2 ──────────────────────────────────────────────────────────────
    case TCP_STATE_FIN_WAIT_2:
        if (dlen > 0 && seq == c->rcv_nxt) {
            rbuf_push(c->recv_buf, &c->recv_head, &c->recv_len,
                      TCP_RECV_BUF_SIZE, data, dlen);
            c->rcv_nxt += dlen;
            if (c->event_cb)
                c->event_cb(conn_idx(c), TCP_EVENT_DATA, data, dlen, c->cb_ctx);
        }
        if (flags & TCP_FLAG_FIN) {
            c->rcv_nxt++; send_ack(c);
            c->state = TCP_STATE_TIME_WAIT;
            c->timewait_start = get_system_ticks();
            serial_print("[TCP] TIME_WAIT\n");
        }
        break;

    // CLOSING ─────────────────────────────────────────────────────────────────
    case TCP_STATE_CLOSING:
        if ((flags & TCP_FLAG_ACK) && ack == c->snd_nxt) {
            c->state = TCP_STATE_TIME_WAIT;
            c->timewait_start = get_system_ticks();
        }
        break;

    // LAST_ACK ────────────────────────────────────────────────────────────────
    case TCP_STATE_LAST_ACK:
        if ((flags & TCP_FLAG_ACK) && ack == c->snd_nxt) {
            serial_print("[TCP] LAST_ACK done\n");
            conn_die(c, TCP_EVENT_CLOSED);
        }
        break;

    // TIME_WAIT ───────────────────────────────────────────────────────────────
    case TCP_STATE_TIME_WAIT:
        if (flags & TCP_FLAG_FIN) send_ack(c);
        break;

    default: break;
    }
}

// ============================================================================
// Public API
// ============================================================================

void tcp_init(void) {
    _ms(g_conns, 0, sizeof(g_conns));
    g_tx_cnt     = 0;
    g_rx_cnt     = 0;
    g_next_eport = TCP_EPHEMERAL_BASE;
    g_init       = true;
    ipv4_register_handler(IP_PROTO_TCP, tcp_handle_packet);
    serial_print("[TCP] Baslatildi, IPv4'e kayit yapildi (proto=6)\n");
}

bool tcp_is_initialized(void) { return g_init; }

int tcp_connect(const uint8_t dst_ip[4], uint16_t dst_port,
                tcp_event_cb_t event_cb, void* ctx)
{
    if (!g_init || !ipv4_is_initialized()) return -1;
    TCPConn* c = conn_alloc();
    if (!c) { serial_print("[TCP] Tablo dolu\n"); return -1; }

    _mc(c->remote_ip, dst_ip, 4);
    c->local_port  = alloc_eport();
    c->remote_port = dst_port;
    c->snd_isn     = make_isn(c->local_port, dst_port);
    c->snd_nxt     = c->snd_isn + 1;
    c->snd_una     = c->snd_isn;
    c->rcv_wnd     = TCP_WINDOW_DEFAULT;
    c->state       = TCP_STATE_SYN_SENT;
    c->event_cb    = event_cb;
    c->cb_ctx      = ctx;
    c->retx_timer  = get_system_ticks();

    serial_print("[TCP] SYN -> ");
    _ship(dst_ip); serial_write(':'); _sdec(dst_port);
    serial_print(" lport="); _sdec(c->local_port); serial_write('\n');

    send_syn(c, false);
    return conn_idx(c);
}

int tcp_listen(uint16_t port, tcp_accept_cb_t accept_cb, void* ctx)
{
    if (!g_init) return -1;
    for (int i = 0; i < TCP_MAX_CONN; i++)
        if (g_conns[i].used && g_conns[i].state == TCP_STATE_LISTEN
            && g_conns[i].local_port == port) return i;

    TCPConn* c = conn_alloc();
    if (!c) { serial_print("[TCP] Tablo dolu\n"); return -1; }
    c->local_port = port; c->remote_port = 0;
    c->state      = TCP_STATE_LISTEN;
    c->accept_cb  = accept_cb; c->cb_ctx = ctx;
    serial_print("[TCP] LISTEN port="); _sdec(port); serial_write('\n');
    return conn_idx(c);
}

int tcp_send(int id, const uint8_t* data, uint16_t len)
{
    TCPConn* c = conn_by_id(id);
    if (!c) return -1;
    if (c->state != TCP_STATE_ESTABLISHED &&
        c->state != TCP_STATE_CLOSE_WAIT) return -1;
    uint16_t w = rbuf_push(c->send_buf, &c->send_head, &c->send_len,
                            TCP_SEND_BUF_SIZE, data, len);
    tcp_flush(c);
    return (int)w;
}

void tcp_close(int id) {
    TCPConn* c = conn_by_id(id);
    if (!c) return;
    switch (c->state) {
    case TCP_STATE_ESTABLISHED:
        send_fin(c); c->snd_nxt++;
        c->state = TCP_STATE_FIN_WAIT_1;
        serial_print("[TCP] FIN -> FIN_WAIT_1\n"); break;
    case TCP_STATE_CLOSE_WAIT:
        send_fin(c); c->snd_nxt++;
        c->state = TCP_STATE_LAST_ACK;
        serial_print("[TCP] FIN -> LAST_ACK\n"); break;
    case TCP_STATE_LISTEN:
    case TCP_STATE_SYN_SENT:
        conn_die(c, TCP_EVENT_CLOSED); break;
    default: break;
    }
}

void tcp_abort(int id) {
    TCPConn* c = conn_by_id(id);
    if (!c) return;
    if (c->state != TCP_STATE_CLOSED && c->state != TCP_STATE_LISTEN)
        send_rst(c->remote_ip, c->local_port, c->remote_port, c->snd_nxt);
    conn_die(c, TCP_EVENT_ERROR);
}

uint16_t tcp_read(int id, uint8_t* out, uint16_t maxlen) {
    TCPConn* c = conn_by_id(id);
    if (!c || !c->recv_len) return 0;
    return rbuf_pop(c->recv_buf, &c->recv_tail, &c->recv_len,
                    TCP_RECV_BUF_SIZE, out, maxlen);
}

void tcp_tick(void) {
    if (!g_init) return;
    uint64_t now = get_system_ticks();
    for (int i = 0; i < TCP_MAX_CONN; i++) {
        TCPConn* c = &g_conns[i];
        if (!c->used) continue;
        switch (c->state) {
        case TCP_STATE_SYN_SENT:
            if (now - c->retx_timer > TCP_SYN_TIMEOUT) {
                if (c->retx_count < TCP_MAX_RETX) {
                    c->retx_count++;
                    serial_print("[TCP] SYN retry #"); _sdec(c->retx_count);
                    serial_write('\n');
                    send_syn(c, false); c->retx_timer = now;
                } else {
                    serial_print("[TCP] SYN timeout\n");
                    conn_die(c, TCP_EVENT_ERROR);
                }
            }
            break;
        case TCP_STATE_ESTABLISHED:
        case TCP_STATE_FIN_WAIT_1:
        case TCP_STATE_LAST_ACK:
            if (c->snd_una != c->snd_nxt &&
                (now - c->retx_timer) > TCP_RETX_TIMEOUT) {
                if (c->retx_count < TCP_MAX_RETX) {
                    c->retx_count++;
                    serial_print("[TCP] Retx #"); _sdec(c->retx_count);
                    serial_write('\n');
                    if (c->state == TCP_STATE_ESTABLISHED) {
                        // snd_nxt'i snd_una'ya geri al: doğru seq'ten yeniden gönder
                        c->snd_nxt = c->snd_una;
                        tcp_flush(c);
                    } else {
                        send_fin(c);
                    }
                    c->retx_timer = now;
                } else {
                    serial_print("[TCP] Retx max, abort\n");
                    conn_die(c, TCP_EVENT_ERROR);
                }
            }
            break;
        case TCP_STATE_TIME_WAIT:
            if (now - c->timewait_start > TCP_TIME_WAIT_MS) {
                serial_print("[TCP] TIME_WAIT done\n");
                c->state = TCP_STATE_CLOSED; c->used = false;
            }
            break;
        default: break;
        }
    }
}

TCPState tcp_get_state(int id) {
    TCPConn* c = conn_by_id(id);
    return c ? c->state : TCP_STATE_CLOSED;
}
bool     tcp_is_connected(int id) {
    TCPConn* c = conn_by_id(id);
    return c && c->state == TCP_STATE_ESTABLISHED;
}
uint32_t tcp_get_tx_count(void)  { return g_tx_cnt; }
uint32_t tcp_get_rx_count(void)  { return g_rx_cnt; }
int      tcp_get_conn_count(void){
    int n = 0;
    for (int i = 0; i < TCP_MAX_CONN; i++) if (g_conns[i].used) n++;
    return n;
}
const char* tcp_state_str(TCPState s) {
    switch (s) {
    case TCP_STATE_CLOSED:       return "CLOSED";
    case TCP_STATE_LISTEN:       return "LISTEN";
    case TCP_STATE_SYN_SENT:     return "SYN_SENT";
    case TCP_STATE_SYN_RECEIVED: return "SYN_RECEIVED";
    case TCP_STATE_ESTABLISHED:  return "ESTABLISHED";
    case TCP_STATE_FIN_WAIT_1:   return "FIN_WAIT_1";
    case TCP_STATE_FIN_WAIT_2:   return "FIN_WAIT_2";
    case TCP_STATE_CLOSE_WAIT:   return "CLOSE_WAIT";
    case TCP_STATE_CLOSING:      return "CLOSING";
    case TCP_STATE_LAST_ACK:     return "LAST_ACK";
    case TCP_STATE_TIME_WAIT:    return "TIME_WAIT";
    default:                     return "?";
    }
}
void tcp_print_connections(void) {
    serial_print("[TCP] Connections:\n");
    bool any = false;
    for (int i = 0; i < TCP_MAX_CONN; i++) {
        TCPConn* c = &g_conns[i];
        if (!c->used) continue; any = true;
        serial_print("  ["); _sdec(i); serial_print("] ");
        serial_print(tcp_state_str(c->state));
        serial_print(" lport="); _sdec(c->local_port);
        serial_print(" -> "); _ship(c->remote_ip);
        serial_write(':'); _sdec(c->remote_port); serial_write('\n');
    }
    if (!any) serial_print("  (none)\n");
    serial_print("  TX="); _sdec(g_tx_cnt);
    serial_print("  RX="); _sdec(g_rx_cnt); serial_write('\n');
}