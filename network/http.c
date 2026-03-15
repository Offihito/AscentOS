// http.c — AscentOS HTTP İstemcisi (Aşama 7)
// RFC 7230 tabanlı — GET ve POST, HTTP/1.0, Connection: close
//
// Akış:
//   http_get/post → ARP çözüm → tcp_connect → sti+hlt döngüsü →
//   ESTABLISHED → istek gönder → yanıt al → tcp_close → sonuç

#include "http.h"
#include "tcp.h"
#include "arp.h"
#include "ipv4.h"
#include "../drivers/rtl8139.h"

// ============================================================================
// Kernel yardımcıları
// ============================================================================
extern void     serial_print(const char*);
extern void     serial_write(char);
extern uint64_t get_system_ticks(void);

// ARP — arp.h döngüsel bağımlılık yaratmasın diye explicit extern
extern bool arp_is_initialized(void);
extern bool arp_resolve(const uint8_t ip[4], uint8_t out_mac[6]);
extern void arp_request(const uint8_t target_ip[4]);
extern void ip_to_str(const uint8_t ip[4], char* out);

// ============================================================================
// Freestanding yardımcılar
// ============================================================================
static void _hmc(void* d, const void* s, uint32_t n) {
    uint8_t* dp = (uint8_t*)d;
    const uint8_t* sp = (const uint8_t*)s;
    while (n--) *dp++ = *sp++;
}
static void _hms(void* d, uint8_t v, uint32_t n) {
    uint8_t* p = (uint8_t*)d;
    while (n--) *p++ = v;
}
static uint32_t _hsl(const char* s) {
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}
static void _hsc(char* d, const char* s) {
    while (*s) { *d++ = *s++; }
    *d = '\0';
}
static void _hscat(char* d, const char* s) {
    while (*d) { d++; }
    while (*s) { *d++ = *s++; }
    *d = '\0';
}
// uint16_t → decimal string
static void _u16str(uint16_t v, char* out) {
    if (!v) { out[0] = '0'; out[1] = '\0'; return; }
    char tmp[8]; int i = 0;
    while (v) { tmp[i++] = '0' + (v % 10); v /= 10; }
    int j = 0;
    while (i--) { out[j++] = tmp[i]; }
    out[j] = '\0';
}

// ============================================================================
// TCP callback bağlamı — _http_request'ten önce tanımlanmalı
// ============================================================================
typedef struct {
    volatile bool connected;
    volatile bool closed;
    volatile bool error;
    HTTPResponse* resp;
} HTTPCtx;

static void _http_tcp_cb(int conn_id, TCPEvent event,
                          const uint8_t* data, uint16_t len, void* ctx)
{
    (void)conn_id;
    HTTPCtx* c = (HTTPCtx*)ctx;
    if (!c) return;

    switch (event) {
    case TCP_EVENT_CONNECTED:
        c->connected = true;
        break;

    case TCP_EVENT_DATA: {
        HTTPResponse* r = c->resp;
        uint16_t space = (uint16_t)(HTTP_RESP_BUF_SIZE - r->total_len - 1);
        uint16_t copy  = (len < space) ? len : space;
        if (copy > 0) {
            _hmc(r->buf + r->total_len, data, copy);
            r->total_len += copy;
            r->buf[r->total_len] = '\0';
        }
        break;
    }

    case TCP_EVENT_CLOSED:
        c->closed = true;
        break;

    case TCP_EVENT_ERROR:
        c->error  = true;
        c->closed = true;
        break;

    default: break;
    }
}


// ============================================================================
// HTTP isteği oluşturucu
// GET veya POST isteğini buf'a yazar, uzunluğu döner.
// buf en az 512 + body_len byte olmalı.
// ============================================================================
static uint16_t build_request(char* buf, uint16_t buf_size,
                               const char* method,       // "GET" veya "POST"
                               const char* host_str,     // "10.0.2.2:8080"
                               const char* path,
                               const uint8_t* body, uint16_t body_len)
{
    char tmp[16];
    _hms((void*)buf, 0, buf_size);

    // Satır 1: METHOD /path HTTP/1.0
    _hsc(buf, method);
    _hscat(buf, " ");
    _hscat(buf, path[0] == '/' ? path : "/");
    if (path[0] != '/') _hscat(buf, path);
    _hscat(buf, " HTTP/1.0\r\n");

    // Host başlığı
    _hscat(buf, "Host: ");
    _hscat(buf, host_str);
    _hscat(buf, "\r\n");

    // User-Agent
    _hscat(buf, "User-Agent: " HTTP_USER_AGENT "\r\n");

    // Accept
    _hscat(buf, "Accept: */*\r\n");

    // Connection
    _hscat(buf, "Connection: close\r\n");

    // POST'a özel: Content-Length ve Content-Type
    if (body && body_len > 0) {
        _hscat(buf, "Content-Type: application/x-www-form-urlencoded\r\n");
        _hscat(buf, "Content-Length: ");
        _u16str(body_len, tmp);
        _hscat(buf, tmp);
        _hscat(buf, "\r\n");
    }

    // Boş satır → başlık sonu
    _hscat(buf, "\r\n");

    uint16_t hlen = (uint16_t)_hsl(buf);

    // POST gövdesini ekle
    if (body && body_len > 0) {
        if ((uint32_t)hlen + body_len < buf_size) {
            _hmc(buf + hlen, body, body_len);
            hlen += body_len;
        }
    }

    return hlen;
}

// ============================================================================
// HTTP yanıt ayrıştırıcı
// "HTTP/1.x NNN ..." formatından durum kodunu çeker.
// Başlık bitişini (\r\n\r\n) bulur, body pointer'ını ayarlar.
// ============================================================================
static void parse_response(HTTPResponse* r) {
    r->status = 0;
    r->body   = NULL;

    if (r->total_len < 12) return;

    // Durum kodunu oku: "HTTP/1.x " → sonraki 3 rakam
    const char* p = r->buf;
    // "HTTP/" atla
    if (p[0]=='H' && p[1]=='T' && p[2]=='T' && p[3]=='P' && p[4]=='/') {
        int i = 5;
        while (p[i] && p[i] != ' ') i++;  // "1.0" veya "1.1" atla
        while (p[i] == ' ') i++;
        int code = 0;
        for (int d = 0; d < 3 && p[i] >= '0' && p[i] <= '9'; d++, i++)
            code = code * 10 + (p[i] - '0');
        r->status = code;
    }

    // Başlık sonu: \r\n\r\n
    for (uint16_t i = 0; i + 3 < r->total_len; i++) {
        if (r->buf[i]=='\r' && r->buf[i+1]=='\n' &&
            r->buf[i+2]=='\r' && r->buf[i+3]=='\n') {
            r->header_len = (uint16_t)(i + 4);
            r->body       = r->buf + r->header_len;
            r->body_len   = (uint16_t)(r->total_len - r->header_len);
            return;
        }
    }
    // Başlık sonu bulunamadı — henüz tamamlanmamış
    r->header_len = 0;
    r->body       = NULL;
    r->body_len   = 0;
}

// ============================================================================
// ARP çözümle — ortak yardımcı
// ============================================================================
// Subnet kontrolü: ip, bizimle aynı /24'te mi?
static bool _same_subnet(const uint8_t ip[4]) {
    uint8_t my_ip[4];
    arp_get_my_ip(my_ip);
    // Basit /24 kontrolü (ipv4.c ile tutarlı)
    return (ip[0] == my_ip[0] && ip[1] == my_ip[1] && ip[2] == my_ip[2]);
}

// ARP çözümle — dış IP için gateway'e ARP at
// Dönüş: true = ARP hazır (paket gönderilebilir)
static bool _arp_wait(const uint8_t ip[4]) {
    uint8_t dummy[6];

    // Dış IP ise gateway üzerinden git
    uint8_t arp_target[4];
    if (_same_subnet(ip)) {
        arp_target[0]=ip[0]; arp_target[1]=ip[1];
        arp_target[2]=ip[2]; arp_target[3]=ip[3];
    } else {
        // Gateway'i al
        extern void ipv4_get_gateway(uint8_t out[4]);
        ipv4_get_gateway(arp_target);
        // Gateway tanımlı değilse başarısız
        uint8_t zero[4] = {0,0,0,0};
        bool gw_zero = (arp_target[0]==0 && arp_target[1]==0 &&
                        arp_target[2]==0 && arp_target[3]==0);
        if (gw_zero) {
            serial_print("[HTTP] Gateway tanimli degil, dis IP erisilemez\n");
            return false;
        }
        serial_print("[HTTP] Dis IP, gateway uzerinden: ");
        { char b[16]; ip_to_str(arp_target, b); serial_print(b); serial_write('\n'); }
    }

    if (arp_resolve(arp_target, dummy)) return true;

    arp_request(arp_target);
    __asm__ volatile("sti");
    uint64_t t = get_system_ticks();
    bool ok = false;
    while ((get_system_ticks() - t) < 4000) {
        rtl8139_poll();
        __asm__ volatile("" ::: "memory");
        if (arp_resolve(arp_target, dummy)) { ok = true; break; }
        __asm__ volatile("hlt");
    }
    __asm__ volatile("cli");
    return ok;
}

// ============================================================================
// Ortak istek gönderici (GET ve POST paylaşır)
// ============================================================================
static HTTPError _http_request(const uint8_t host_ip[4], uint16_t port,
                                const char* method, const char* path,
                                const uint8_t* body, uint16_t body_len,
                                HTTPResponse* out)
{
    if (!tcp_is_initialized() || !arp_is_initialized())
        return HTTP_ERR_INIT;

    // Yanıt yapısını sıfırla
    http_response_reset(out);

    // ARP çözüm
    serial_print("[HTTP] ARP cozumleniyor...\n");
    if (!_arp_wait(host_ip)) {
        serial_print("[HTTP] ARP zaman asimi\n");
        return HTTP_ERR_ARP;
    }

    // Host string oluştur: "10.0.2.2:8080" (Host başlığı için)
    char host_str[32];
    {
        ip_to_str(host_ip, host_str);
        if (port != 80) {
            char pstr[8];
            _u16str(port, pstr);
            uint32_t hl = _hsl(host_str);
            host_str[hl] = ':';
            _hsc(host_str + hl + 1, pstr);
        }
    }

    // HTTP isteği oluştur (statik tampon — yığında)
    char req_buf[768];
    uint16_t req_len = build_request(req_buf, sizeof(req_buf),
                                     method, host_str, path,
                                     body, body_len);

    serial_print("[HTTP] "); serial_print(method);
    serial_print(" "); serial_print(host_str);
    serial_print(path); serial_write('\n');

    // TCP callback bağlamı
    HTTPCtx hctx;
    hctx.connected = false;
    hctx.closed    = false;
    hctx.error     = false;
    hctx.resp      = out;

    // TCP bağlantısı kur
    int cid = tcp_connect(host_ip, port, _http_tcp_cb, &hctx);
    if (cid < 0) {
        serial_print("[HTTP] tcp_connect basarisiz\n");
        return HTTP_ERR_CONNECT;
    }

    // ESTABLISHED bekle
    __asm__ volatile("sti");
    uint64_t t0 = get_system_ticks();
    while ((get_system_ticks() - t0) < HTTP_CONNECT_TIMEOUT) {
        rtl8139_poll(); tcp_tick();
        __asm__ volatile("" ::: "memory");
        if (hctx.connected || hctx.error) break;
        __asm__ volatile("hlt");
    }
    __asm__ volatile("cli");

    if (!hctx.connected) {
        serial_print("[HTTP] Baglanti zaman asimi\n");
        tcp_abort(cid);
        return HTTP_ERR_CONNECT;
    }

    // Kısa gecikme: son ACK'in karşıya ulaşmasını bekle
    __asm__ volatile("sti");
    uint64_t tw = get_system_ticks();
    while ((get_system_ticks() - tw) < 50) {
        rtl8139_poll(); tcp_tick();
        __asm__ volatile("hlt");
    }
    __asm__ volatile("cli");

    // HTTP isteğini gönder
    int sent = tcp_send(cid, (const uint8_t*)req_buf, req_len);
    if (sent <= 0) {
        serial_print("[HTTP] Istek gonderilemedi\n");
        tcp_abort(cid);
        return HTTP_ERR_SEND;
    }
    serial_print("[HTTP] Istek gonderildi (");
    {
        char tmp[8]; _u16str((uint16_t)sent, tmp);
        serial_print(tmp);
    }
    serial_print(" byte)\n");

    // Yanıt bekle — bağlantı kapanana kadar al
    __asm__ volatile("sti");
    t0 = get_system_ticks();
    while ((get_system_ticks() - t0) < HTTP_RECV_TIMEOUT) {
        rtl8139_poll(); tcp_tick();
        __asm__ volatile("" ::: "memory");
        if (hctx.closed) break;
        __asm__ volatile("hlt");
    }
    __asm__ volatile("cli");

    if (!hctx.closed && out->total_len == 0) {
        out->timed_out = true;
        tcp_abort(cid);
        return HTTP_ERR_TIMEOUT;
    }

    // Temiz kapat
    if (!hctx.closed) {
        tcp_close(cid);
        __asm__ volatile("sti");
        uint64_t tc = get_system_ticks();
        while ((get_system_ticks() - tc) < 500) {
            rtl8139_poll(); tcp_tick();
            __asm__ volatile("" ::: "memory");
            if (hctx.closed) break;
            __asm__ volatile("hlt");
        }
        __asm__ volatile("cli");
    }

    // Yanıtı ayrıştır
    parse_response(out);
    out->complete = true;

    serial_print("[HTTP] Yanit alindi: ");
    { char tmp[8]; _u16str(out->total_len, tmp); serial_print(tmp); }
    serial_print(" byte  status=");
    { char tmp[8]; _u16str((uint16_t)out->status, tmp); serial_print(tmp); }
    serial_write('\n');

    return HTTP_OK;
}

// ============================================================================
// Public API
// ============================================================================
HTTPError http_get(const uint8_t host_ip[4], uint16_t port,
                   const char* path, HTTPResponse* out)
{
    return _http_request(host_ip, port, "GET", path, NULL, 0, out);
}

HTTPError http_post(const uint8_t host_ip[4], uint16_t port,
                    const char* path,
                    const uint8_t* body, uint16_t body_len,
                    HTTPResponse* out)
{
    return _http_request(host_ip, port, "POST", path, body, body_len, out);
}

void http_response_reset(HTTPResponse* r) {
    _hms((void*)r, 0, sizeof(HTTPResponse));
}

const char* http_err_str(HTTPError e) {
    switch (e) {
    case HTTP_OK:           return "OK";
    case HTTP_ERR_ARP:      return "ARP cozumlenemedi";
    case HTTP_ERR_CONNECT:  return "TCP baglantisi kurulamadi";
    case HTTP_ERR_SEND:     return "Istek gonderilemedi";
    case HTTP_ERR_TIMEOUT:  return "Yanit zaman asimi";
    case HTTP_ERR_OVERFLOW: return "Yanit tamponu dolu";
    case HTTP_ERR_INIT:     return "TCP/ARP baslatilmamis";
    default:                return "Bilinmeyen hata";
    }
}

const char* http_status_str(int s) {
    switch (s) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 301: return "Moved Permanently";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 500: return "Internal Server Error";
    default:  return "?";
    }
}