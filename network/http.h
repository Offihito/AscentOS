// http.h — AscentOS HTTP İstemcisi (Aşama 7)
// RFC 7230 — HTTP/1.1 (basitleştirilmiş GET/POST)
//
// Katman mimarisi:
//   http_get / http_post → tcp_connect → TCP → IPv4 → rtl8139
//
// Kullanım:
//   http_get("10.0.2.2", 8080, "/dosya.txt", &resp)  → GET isteği
//   http_post("10.0.2.2", 8080, "/api", body, len, &resp) → POST isteği
//   http_response_free(&resp)  → tamponu serbest bırak (statik tampon — no-op)
//
// Kısıtlamalar (hobi OS):
//   • Sadece IPv4 (domain adı çözümleme yok — önce DNS gerekir)
//   • HTTP/1.0 — Connection: close (keep-alive yok)
//   • Yanıt tampon: HTTP_RESP_BUF_SIZE byte (varsayılan 4096)
//   • Yönlendirme (3xx) yok
//   • HTTPS / TLS yok

#ifndef HTTP_H
#define HTTP_H

#include <stdint.h>
#include <stdbool.h>

// ============================================================================
// Sabitler
// ============================================================================
#define HTTP_RESP_BUF_SIZE   4096    // Yanıt tamponu (başlık + gövde)
#define HTTP_CONNECT_TIMEOUT 5000    // Bağlantı zaman aşımı (ms/tick)
#define HTTP_RECV_TIMEOUT    6000    // Yanıt alma zaman aşımı (ms/tick)
#define HTTP_MAX_HEADERS     256     // Başlık alanı maksimum boyutu
#define HTTP_USER_AGENT      "AscentOS/1.0 (hobi OS; x86_64)"

// ============================================================================
// HTTP durum kodları (sık kullanılanlar)
// ============================================================================
#define HTTP_STATUS_OK            200
#define HTTP_STATUS_CREATED       201
#define HTTP_STATUS_NO_CONTENT    204
#define HTTP_STATUS_MOVED         301
#define HTTP_STATUS_NOT_MODIFIED  304
#define HTTP_STATUS_BAD_REQUEST   400
#define HTTP_STATUS_FORBIDDEN     403
#define HTTP_STATUS_NOT_FOUND     404
#define HTTP_STATUS_SERVER_ERROR  500

// ============================================================================
// HTTP yanıt yapısı
// ============================================================================
typedef struct {
    int      status;                    // HTTP durum kodu (200, 404, vb.) — 0 = hata
    uint16_t header_len;                // Başlık uzunluğu (byte)
    uint16_t body_len;                  // Gövde uzunluğu (byte)
    uint16_t total_len;                 // Tampondaki toplam veri
    char     buf[HTTP_RESP_BUF_SIZE];  // Ham yanıt (başlık + "\r\n\r\n" + gövde)
    char*    body;                      // Gövde başlangıcı (buf içinde pointer)
    bool     complete;                  // Tüm yanıt alındı mı?
    bool     timed_out;                 // Zaman aşımı mı oldu?
} HTTPResponse;

// ============================================================================
// Hata kodları
// ============================================================================
typedef enum {
    HTTP_OK           = 0,
    HTTP_ERR_ARP      = 1,   // ARP çözümlenemedi
    HTTP_ERR_CONNECT  = 2,   // TCP bağlantısı kurulamadı
    HTTP_ERR_SEND     = 3,   // İstek gönderilemedi
    HTTP_ERR_TIMEOUT  = 4,   // Yanıt zaman aşımı
    HTTP_ERR_OVERFLOW = 5,   // Yanıt tamponu doldu
    HTTP_ERR_INIT     = 6,   // TCP/ARP başlatılmamış
} HTTPError;

// ============================================================================
// Public API
// ============================================================================

// HTTP GET isteği gönder
// host_ip  : 4-byte hedef IP (örn. {10,0,2,2})
// port     : hedef port (örn. 80, 8080)
// path     : istek yolu (örn. "/index.html", "/dosya.txt")
// out      : yanıtın yazılacağı yapı (çağıran sağlar)
// Dönüş   : HTTP_OK veya hata kodu
HTTPError http_get(const uint8_t host_ip[4], uint16_t port,
                   const char* path, HTTPResponse* out);

// HTTP POST isteği gönder
// body     : gönderilecek veri (metin veya binary)
// body_len : veri uzunluğu
HTTPError http_post(const uint8_t host_ip[4], uint16_t port,
                    const char* path,
                    const uint8_t* body, uint16_t body_len,
                    HTTPResponse* out);

// Hata kodunu string'e çevir
const char* http_err_str(HTTPError err);

// Durum kodunu string'e çevir (örn. 200 → "OK")
const char* http_status_str(int status);

// HTTPResponse'u sıfırla (tekrar kullanım için)
void http_response_reset(HTTPResponse* resp);

#endif // HTTP_H