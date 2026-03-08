// tcp.h — AscentOS TCP Katmanı (Aşama 6)
// RFC 793 — Transmission Control Protocol
//
// Katman mimarisi:
//   rtl8139 → ipv4_handle_packet → tcp_handle_packet → bağlantı state machine
//   Gönderim: tcp_send → tcp_output → ipv4_send → rtl8139_send
//
// Kullanım:
//   tcp_init()                         → TCP katmanını başlat (ipv4_init'ten sonra)
//   tcp_connect(ip, port, cb, ctx)     → Aktif bağlantı kur (SYN gönder)
//   tcp_listen(port, accept_cb, ctx)   → Pasif dinleme başlat
//   tcp_send(conn_id, data, len)       → Veri gönder (ESTABLISHED olmalı)
//   tcp_close(conn_id)                 → Bağlantıyı düzgünce kapat (FIN gönder)
//   tcp_abort(conn_id)                 → Bağlantıyı zorla kes (RST gönder)
//
// Kısıtlamalar (hobi OS için kabul edilebilir):
//   • Maksimum TCP_MAX_CONN eş zamanlı bağlantı
//   • TCP_SEND_BUF_SIZE / TCP_RECV_BUF_SIZE byte halka tamponu
//   • Parçalanmış segment yeniden sıralama YOK (sırasız segmentler atılır)
//   • Nagle algoritması YOK (her tcp_send() hemen gönderilir)
//   • Pencere ölçekleme (RFC 7323) YOK

#ifndef TCP_H
#define TCP_H

#include <stdint.h>
#include <stdbool.h>

// ============================================================================
// TCP sabitleri
// ============================================================================
#define TCP_HDR_LEN_MIN     20          // Minimum TCP başlık uzunluğu (opsiyonsuz)
#define TCP_MSS_DEFAULT     1460        // Maximum Segment Size (ETH MTU − IP − TCP)
#define TCP_WINDOW_DEFAULT  8192        // Varsayılan alıcı penceresi (byte)
#define TCP_TTL_DEFAULT     64          // IPv4 TTL
#define TCP_MAX_CONN        8           // Eş zamanlı maksimum bağlantı sayısı
#define TCP_SEND_BUF_SIZE   4096        // Her bağlantı için gönderme tamponu (byte)
#define TCP_RECV_BUF_SIZE   4096        // Her bağlantı için alma tamponu (byte)
#define TCP_RETX_TIMEOUT    3000        // Yeniden gönderim zaman aşımı (ms / tick)
#define TCP_MAX_RETX        5           // Maksimum yeniden gönderim denemesi
#define TCP_TIME_WAIT_MS    4000        // TIME_WAIT bekleme süresi (ms)
#define TCP_SYN_TIMEOUT     3000        // SYN cevap bekleme süresi (ms)

// TCP bayrakları (Flags alanı)
#define TCP_FLAG_FIN        (1 << 0)
#define TCP_FLAG_SYN        (1 << 1)
#define TCP_FLAG_RST        (1 << 2)
#define TCP_FLAG_PSH        (1 << 3)
#define TCP_FLAG_ACK        (1 << 4)
#define TCP_FLAG_URG        (1 << 5)
#define TCP_FLAG_ECE        (1 << 6)
#define TCP_FLAG_CWR        (1 << 7)

// TCP seçenek türleri
#define TCP_OPT_END         0           // Liste sonu
#define TCP_OPT_NOP         1           // Dolgu
#define TCP_OPT_MSS         2           // Maximum Segment Size
#define TCP_OPT_MSS_LEN     4           // MSS seçenek uzunluğu

// Efemeral port aralığı (istemci tarafı port seçimi)
#define TCP_EPHEMERAL_BASE  49152u
#define TCP_EPHEMERAL_MAX   65535u

// ============================================================================
// TCP başlığı — 20 byte sabit alan (RFC 793 §3.1)
// ============================================================================
typedef struct {
    uint16_t src_port;      // Kaynak port
    uint16_t dst_port;      // Hedef port
    uint32_t seq;           // Sıra numarası
    uint32_t ack;           // Onay numarası (ACK bayrağı etkinse geçerli)
    uint8_t  data_off;      // [7:4]=veri offseti (kelime sayısı), [3:0]=rezerve
    uint8_t  flags;         // Kontrol bitleri (TCP_FLAG_* makroları)
    uint16_t window;        // Alıcı pencere boyutu
    uint16_t checksum;      // Pseudo-başlık + segment checksum'u
    uint16_t urg_ptr;       // Acil veri göstericisi (URG bayrağı etkinse)
    // Ardından opsiyonel alanlar (data_off*4 - 20 byte)
} __attribute__((packed)) TCPHeader;

// ============================================================================
// TCP bağlantı durumları — RFC 793 §3.2 durum diyagramı
// ============================================================================
typedef enum {
    TCP_STATE_CLOSED      = 0,  // Bağlantı yok
    TCP_STATE_LISTEN,           // Pasif açık: SYN bekleniyor (sunucu)
    TCP_STATE_SYN_SENT,         // SYN gönderildi, SYN+ACK bekleniyor
    TCP_STATE_SYN_RECEIVED,     // SYN+ACK gönderildi, ACK bekleniyor
    TCP_STATE_ESTABLISHED,      // Bağlantı kuruldu, veri aktarımı aktif
    TCP_STATE_FIN_WAIT_1,       // FIN gönderildi, FIN veya ACK bekleniyor
    TCP_STATE_FIN_WAIT_2,       // ACK alındı, karşı FIN bekleniyor
    TCP_STATE_CLOSE_WAIT,       // Karşı FIN alındı, uygulama kapatmayı bekliyor
    TCP_STATE_CLOSING,          // Eş zamanlı FIN: FIN+ACK bekleniyor
    TCP_STATE_LAST_ACK,         // Son ACK bekleniyor (CLOSE_WAIT sonrası FIN)
    TCP_STATE_TIME_WAIT,        // 2×MSL bekleniyor (eski segmentlerin tükenmesi)
} TCPState;

// ============================================================================
// Bağlantı olayları — callback'e iletilen olay türü
// ============================================================================
typedef enum {
    TCP_EVENT_CONNECTED   = 0,  // Bağlantı kuruldu (ESTABLISHED)
    TCP_EVENT_DATA,             // Veri alındı
    TCP_EVENT_SENT,             // Veriler karşı tarafça onaylandı
    TCP_EVENT_CLOSED,           // Bağlantı kapandı (FIN veya RST)
    TCP_EVENT_ERROR,            // Hata oluştu (zaman aşımı, RST vb.)
    TCP_EVENT_ACCEPT,           // Sunucu: yeni bağlantı kabul edildi
} TCPEvent;

// ============================================================================
// Callback türleri
// ============================================================================

// Bağlantı olayı callback'i
// conn_id : tcp_connect() / tcp_listen()'den dönen bağlantı ID'si
// event   : ne olduğunu belirtir
// data    : TCP_EVENT_DATA için alınan veri tamponu (sadece callback süresince geçerli)
// len     : veri uzunluğu (TCP_EVENT_DATA için)
// ctx     : tcp_connect() / tcp_listen()'e verilen kullanıcı bağlamı
typedef void (*tcp_event_cb_t)(int conn_id, TCPEvent event,
                               const uint8_t* data, uint16_t len, void* ctx);

// Sunucu accept callback'i — yeni bağlantı geldiğinde çağrılır
// Geri dönen conn_id ile tcp_send/close çağrılabilir.
// new_conn_id : yeni bağlantının ID'si
// remote_ip   : istemcinin IP adresi
// remote_port : istemcinin kaynak portu
// ctx         : tcp_listen()'e verilen bağlam
typedef void (*tcp_accept_cb_t)(int new_conn_id, const uint8_t remote_ip[4],
                                uint16_t remote_port, void* ctx);

// ============================================================================
// TCP bağlantı kaydı (dahili, erişim için ID kullan)
// ============================================================================
typedef struct {
    TCPState    state;
    uint8_t     remote_ip[4];
    uint16_t    local_port;
    uint16_t    remote_port;

    // Sıra numarası takibi
    uint32_t    snd_isn;    // Gönderici başlangıç sıra numarası (Initial Sequence Number)
    uint32_t    snd_nxt;    // Gönderilecek bir sonraki seq
    uint32_t    snd_una;    // En eski onaylanmamış seq (unacknowledged)
    uint32_t    rcv_isn;    // Alıcı başlangıç sıra numarası
    uint32_t    rcv_nxt;    // Beklenen bir sonraki seq (gelen)

    // Pencere yönetimi
    uint16_t    snd_wnd;    // Karşı tarafın penceresi
    uint16_t    rcv_wnd;    // Bizim penceremiz

    // Halka tamponları
    uint8_t     send_buf[TCP_SEND_BUF_SIZE];
    uint16_t    send_head;  // Yazma pozisyonu
    uint16_t    send_tail;  // Okuma pozisyonu
    uint16_t    send_len;   // Tampondaki byte sayısı

    uint8_t     recv_buf[TCP_RECV_BUF_SIZE];
    uint16_t    recv_head;
    uint16_t    recv_tail;
    uint16_t    recv_len;

    // Yeniden gönderim
    uint64_t    retx_timer;     // Son gönderimin sistem ticki
    uint8_t     retx_count;     // Kaç kez yeniden gönderildi

    // TIME_WAIT zamanlayıcı
    uint64_t    timewait_start; // TIME_WAIT başladığı tick

    // Callback'ler
    tcp_event_cb_t  event_cb;
    tcp_accept_cb_t accept_cb;
    void*           cb_ctx;

    bool        used;       // Slot kullanımda mı?
    bool        is_server;  // Sunucu (LISTEN) bağlantısı mı?
} TCPConn;

// ============================================================================
// Public API
// ============================================================================

// TCP katmanını başlat. ipv4_init() çağrıldıktan sonra çağrılmalı.
// IPv4'e protokol numarası 6 ile handler kaydeder.
void tcp_init(void);

// Uzak sunucuya TCP bağlantısı başlat (istemci modu — aktif açık).
// dst_ip    : hedef IP adresi (4 byte)
// dst_port  : hedef port (örn. 80)
// event_cb  : bağlantı olaylarını alan callback
// ctx       : callback'e iletilen kullanıcı bağlamı
// Dönüş     : ≥0 = bağlantı ID'si, <0 = hata (tablo dolu vb.)
int tcp_connect(const uint8_t dst_ip[4], uint16_t dst_port,
                tcp_event_cb_t event_cb, void* ctx);

// Belirtilen porta gelen bağlantıları dinle (sunucu modu — pasif açık).
// port      : dinlenecek yerel port
// accept_cb : yeni bağlantı kabul callback'i
// ctx       : callback bağlamı
// Dönüş     : ≥0 = dinleyici ID'si, <0 = hata
int tcp_listen(uint16_t port, tcp_accept_cb_t accept_cb, void* ctx);

// Veri gönder (bağlantı ESTABLISHED durumunda olmalı).
// conn_id   : tcp_connect() veya accept callback'inden gelen ID
// data      : gönderilecek veri
// len       : byte sayısı (max TCP_SEND_BUF_SIZE)
// Dönüş     : gönderme tamponuna yazılan byte sayısı (<0 = hata)
int tcp_send(int conn_id, const uint8_t* data, uint16_t len);

// Bağlantıyı düzgünce kapat — FIN gönderir, FIN+ACK bekler.
// Aktif olarak gönderilen tüm veriler önce tamamlanır.
void tcp_close(int conn_id);

// Bağlantıyı zorla sonlandır — RST gönderir, tamponları temizler.
void tcp_abort(int conn_id);

// Bağlantı durumunu sorgula
TCPState tcp_get_state(int conn_id);

// Bağlantının ESTABLISHED olup olmadığını kontrol et
bool tcp_is_connected(int conn_id);

// Alınan veriyi recv_buf'tan oku (veri callback'i dışından kullanmak için)
// Dönüş: okunan byte sayısı
uint16_t tcp_read(int conn_id, uint8_t* out, uint16_t max_len);

// Durum stringini döndür (debug)
const char* tcp_state_str(TCPState state);

// IPv4 katmanından otomatik çağrılan handler (doğrudan çağırmaya gerek yok).
void tcp_handle_packet(const uint8_t src_ip[4],
                       const uint8_t* payload, uint16_t len);

// Zamanlayıcı güncellemesi — her ~100ms'de bir ana döngüden çağır.
// Zaman aşımları (retransmit, TIME_WAIT) bu fonksiyon ile işlenir.
void tcp_tick(void);

// Katman başlatıldı mı?
bool tcp_is_initialized(void);

// Tüm bağlantıları seri porta yaz (debug)
void tcp_print_connections(void);

// İstatistikler
uint32_t tcp_get_tx_count(void);
uint32_t tcp_get_rx_count(void);

// Bağlantı sayısı
int tcp_get_conn_count(void);

#endif // TCP_H