#include <stddef.h>
#include "commands64.h"
#include "../fs/files64.h"
#include "../kernel/vmm64.h"
#include "../kernel/heap.h"
#include "../kernel/pmm.h"
#include "../kernel/task.h"
#include "../kernel/scheduler.h"
#include "../kernel/ext2.h"
#include "../kernel/elf64.h"
#include "../kernel/syscall.h"
#include "../kernel/signal64.h"
#include "../drivers/pcspk.h"
#include "../kernel/cpu64.h"
#include "../kernel/spinlock64.h"   // spinlock_t, rwlock_t
#include <stdbool.h>

extern void spinlock_test(void);    // spinlock64.c

// ============================================================================
// RTL8139 Ağ Sürücüsü — extern bildirimleri
// ============================================================================
extern bool     rtl8139_init(void);
extern bool     rtl8139_send(const uint8_t* data, uint16_t len);
extern void     rtl8139_get_mac(uint8_t out[6]);
extern bool     rtl8139_link_is_up(void);
extern void     rtl8139_stats(void);
extern void     rtl8139_dump_regs(void);
extern void     rtl8139_set_packet_handler(void* handler);
extern void     rtl8139_dump_regs(void);
extern uint32_t rtl8139_get_rx_count(void);
extern uint32_t rtl8139_get_tx_count(void);
extern void     rtl8139_poll(void);

// ============================================================================
// ARP Katmanı — extern bildirimleri (arp.c)
// ============================================================================
extern void     arp_init(const uint8_t my_ip[4], const uint8_t my_mac[6]);
extern void     arp_handle_packet(const uint8_t* frame, uint16_t len);
extern bool     arp_resolve(const uint8_t ip[4], uint8_t out_mac[6]);
extern void     arp_request(const uint8_t target_ip[4]);
extern void     arp_announce(void);
extern void     arp_flush_cache(void);
extern void     arp_add_static(const uint8_t ip[4], const uint8_t mac[6]);
extern void     arp_get_my_ip(uint8_t out[4]);
extern void     arp_get_my_mac(uint8_t out[6]);
extern bool     arp_is_initialized(void);
extern void     ip_to_str(const uint8_t ip[4], char* out);
extern bool     str_to_ip(const char* str, uint8_t out[4]);

// arp_cache_foreach için callback tip tanımı
typedef void (*arp_line_cb)(const char* line, uint8_t color, void* ctx);
extern void arp_cache_foreach(arp_line_cb cb, void* ctx);

// ============================================================================
// IPv4 Katmanı — extern bildirimleri (ipv4.c)
// ============================================================================
extern void     ipv4_init(void);
extern void     ipv4_handle_packet(const uint8_t* frame, uint16_t len);
extern bool     ipv4_send(const uint8_t dst_ip[4], uint8_t protocol,
                          const uint8_t* payload, uint16_t plen);
extern bool     ipv4_is_initialized(void);
extern uint32_t ipv4_get_tx_count(void);
extern uint32_t ipv4_get_rx_count(void);
extern void     ipv4_set_gateway(const uint8_t gw[4]);
extern void     ipv4_set_subnet(const uint8_t mask[4]);
extern void     ipv4_get_gateway(uint8_t out[4]);

// ============================================================================
// ICMP Katmanı — extern bildirimleri (icmp.c)
// ============================================================================
extern void     icmp_init(void);
extern bool     icmp_ping(const uint8_t dst_ip[4]);
extern int      icmp_ping_state(void);   // PingState enum: 0=IDLE,1=PENDING,2=SUCCESS (hata:3/4)
extern uint32_t icmp_last_rtt_ms(void);
extern uint16_t icmp_last_seq(void);
extern void     icmp_get_last_src(uint8_t out[4]);
extern void     icmp_ping_reset(void);

// PING_* enum değerleri (icmp.h'daki PingState ile eşleşmeli)
#define PING_IDLE        0
#define PING_PENDING     1
#define PING_SUCCESS     2
#define PING_TIMEOUT     3
#define PING_UNREACHABLE 4

// ============================================================================
// UDP Katmanı — extern bildirimleri (udp.c)
// ============================================================================
typedef struct {
    uint8_t  src_ip[4];
    uint16_t src_port;
    uint16_t dst_port;
    const uint8_t* data;
    uint16_t len;
} UDPPacket;
typedef void (*udp_handler_t)(const UDPPacket* pkt, void* ctx);

extern void     udp_init_csum(int csum_enable);   // udp_init(mode) wrapper — aşağıda sarılır
extern bool     udp_bind(uint16_t port, udp_handler_t handler, void* ctx);
extern void     udp_unbind(uint16_t port);
extern bool     udp_send(const uint8_t dst_ip[4], uint16_t dst_port, uint16_t src_port,
                          const uint8_t* data, uint16_t len);
extern bool     udp_broadcast(uint16_t dst_port, uint16_t src_port,
                               const uint8_t* data, uint16_t len);
extern bool     udp_is_initialized(void);
extern uint32_t udp_get_rx_count(void);
extern uint32_t udp_get_tx_count(void);

// udp_sockets_foreach için callback tip tanımı
typedef void (*udp_line_cb)(const char* line, uint8_t color, void* ctx);
extern void udp_sockets_foreach(udp_line_cb cb, void* ctx);

// UDP_CSUM_ENABLE = 1 (udp.h'dan)
// udp.c içindeki udp_init(UDPCsumMode) fonksiyonunu doğrudan çağırıyoruz:
extern void udp_init(int csum_mode);

// ============================================================================
// DHCP İstemcisi — extern bildirimleri (dhcp.c)
// ============================================================================
typedef enum {
    DHCP_STATE_IDLE       = 0,
    DHCP_STATE_SELECTING,
    DHCP_STATE_REQUESTING,
    DHCP_STATE_BOUND,
    DHCP_STATE_RENEWING,
    DHCP_STATE_REBINDING,
    DHCP_STATE_RELEASED,
    DHCP_STATE_FAILED,
} DHCPState;

typedef struct {
    uint8_t  ip[4];
    uint8_t  subnet[4];
    uint8_t  gateway[4];
    uint8_t  dns[4];
    uint8_t  server_ip[4];
    uint32_t lease_time;
    uint32_t renewal_time;
    uint32_t rebinding_time;
    uint32_t lease_start_tick;
    bool     valid;
} DHCPConfig;

extern void        dhcp_init(void);
extern bool        dhcp_discover(void);
extern void        dhcp_release(void);
extern int         dhcp_get_state(void);      // DHCPState enum
extern void        dhcp_get_config(DHCPConfig* out);
extern const char* dhcp_state_str(void);
extern bool        dhcp_is_initialized(void);
extern uint32_t    dhcp_get_xid(void);

// ============================================================================
// TCP Katmanı — extern bildirimleri (tcp.c) — Aşama 6
// ============================================================================
typedef enum {
    TCP_STATE_CLOSED      = 0,
    TCP_STATE_LISTEN,
    TCP_STATE_SYN_SENT,
    TCP_STATE_SYN_RECEIVED,
    TCP_STATE_ESTABLISHED,
    TCP_STATE_FIN_WAIT_1,
    TCP_STATE_FIN_WAIT_2,
    TCP_STATE_CLOSE_WAIT,
    TCP_STATE_CLOSING,
    TCP_STATE_LAST_ACK,
    TCP_STATE_TIME_WAIT,
} TCPState_t;

typedef enum {
    TCP_EVENT_CONNECTED = 0,
    TCP_EVENT_DATA,
    TCP_EVENT_SENT,
    TCP_EVENT_CLOSED,
    TCP_EVENT_ERROR,
    TCP_EVENT_ACCEPT,
} TCPEvent_t;

typedef void (*tcp_event_cb_t)(int conn_id, TCPEvent_t event,
                               const uint8_t* data, uint16_t len, void* ctx);
typedef void (*tcp_accept_cb_t)(int new_conn_id, const uint8_t remote_ip[4],
                                uint16_t remote_port, void* ctx);

extern void         tcp_init(void);
extern int          tcp_connect(const uint8_t dst_ip[4], uint16_t dst_port,
                                tcp_event_cb_t event_cb, void* ctx);
extern int          tcp_listen(uint16_t port, tcp_accept_cb_t accept_cb, void* ctx);
extern int          tcp_send(int conn_id, const uint8_t* data, uint16_t len);
extern void         tcp_close(int conn_id);
extern void         tcp_abort(int conn_id);
extern int          tcp_get_state(int conn_id);   // TCPState_t
extern bool         tcp_is_connected(int conn_id);
extern uint16_t     tcp_read(int conn_id, uint8_t* out, uint16_t max_len);
extern void         tcp_tick(void);
extern bool         tcp_is_initialized(void);
extern void         tcp_print_connections(void);
extern uint32_t     tcp_get_tx_count(void);
extern uint32_t     tcp_get_rx_count(void);
extern int          tcp_get_conn_count(void);
extern const char*  tcp_state_str(int state);

// ── HTTP istemcisi (http.c) ──────────────────────────────────────────────────
typedef struct {
    int      status;
    uint16_t header_len;
    uint16_t body_len;
    uint16_t total_len;
    char     buf[4096];
    char*    body;
    bool     complete;
    bool     timed_out;
} HTTPResponse;

typedef enum {
    HTTP_OK           = 0,
    HTTP_ERR_ARP      = 1,
    HTTP_ERR_CONNECT  = 2,
    HTTP_ERR_SEND     = 3,
    HTTP_ERR_TIMEOUT  = 4,
    HTTP_ERR_OVERFLOW = 5,
    HTTP_ERR_INIT     = 6,
} HTTPError;

extern HTTPError    http_get(const uint8_t host_ip[4], uint16_t port,
                              const char* path, HTTPResponse* out);
extern HTTPError    http_post(const uint8_t host_ip[4], uint16_t port,
                               const char* path,
                               const uint8_t* body, uint16_t body_len,
                               HTTPResponse* out);
extern const char*  http_err_str(HTTPError err);
extern const char*  http_status_str(int status);
extern void         http_response_reset(HTTPResponse* resp);

// TCP test için global durum (cmd_tcpconnect / cmd_tcptest ortak kullanır)
static volatile int    g_tcp_conn_id      = -1;
static volatile bool   g_tcp_connected    = false;
static volatile bool   g_tcp_data_recvd   = false;
static volatile bool   g_tcp_closed       = false;
static volatile bool   g_tcp_error        = false;
// Alınan verinin ilk 128 baytını VGA'ya yazmak için
static char            g_tcp_recv_preview[128];
static volatile uint16_t g_tcp_recv_len   = 0;

// ============================================================================
// Sürücünün başlatılıp başlatılmadığını sorgulayan dahili değişken
// (rtl8139.c içinde static, doğrudan erişemeyiz — init sonucu burada saklarız)
// NOT: kernel64.c'den net_register_packet_handler() üzerinden güncellenir.
// ============================================================================
int g_net_initialized = 0;

// Son alınan paketin özet bilgisini TEXT modda göstermek için
static volatile uint32_t g_net_rx_display  = 0;  // gösterilecek paket sayısı
static volatile uint16_t g_net_last_etype  = 0;  // son paketin EtherType
static volatile uint8_t  g_net_last_src[6] = {0}; // son paketin kaynak MAC

// Forward declaration — tanım dosyanın sonundadır
void net_register_packet_handler(void);

// ============================================================================
// Paket alma callback: RTL8139'dan gelen her çerçeveyi
//   → ARP katmanına   (EtherType 0x0806)
//   → IPv4 katmanına  (EtherType 0x0800)
// ============================================================================
static void net_packet_callback(const uint8_t* buf, uint16_t len) {
    g_net_rx_display++;
    // EtherType (byte 12-13)
    if (len >= 14) {
        g_net_last_etype = (uint16_t)((buf[12] << 8) | buf[13]);
        // Kaynak MAC (byte 6-11)
        for (int i = 0; i < 6; i++) g_net_last_src[i] = buf[6 + i];

        // Serial debug: gelen her paketin EtherType + uzunluğunu yaz
        serial_print("[RX] etype=0x");
        {
            const char* h = "0123456789ABCDEF";
            uint16_t et = g_net_last_etype;
            serial_write(h[(et>>12)&0xF]); serial_write(h[(et>>8)&0xF]);
            serial_write(h[(et>>4)&0xF]);  serial_write(h[et&0xF]);
        }
        serial_print("  len=");
        {
            uint16_t v = len; char b[6]; int bi = 0;
            if (!v) { serial_write('0'); }
            else { while(v){ b[bi++]='0'+(v%10); v/=10; } while(bi--) serial_write(b[bi]); }
        }
        // IPv4 ise proto ve src IP yaz
        if (g_net_last_etype == 0x0800 && len >= 34) {
            serial_print("  proto=");
            uint8_t proto = buf[14+9];
            { char b[4]; int bi=0; uint8_t v=proto;
              if(!v){serial_write('0');}
              else{while(v){b[bi++]='0'+(v%10);v/=10;}while(bi--)serial_write(b[bi]);} }
            serial_print("  src=");
            serial_write('0'+buf[26]); serial_write('.');
            serial_write('0'+(buf[27]/100)); serial_write('0'+((buf[27]/10)%10)); serial_write('0'+(buf[27]%10));
            serial_write('.');
            serial_write('0'+(buf[28]/100)); serial_write('0'+((buf[28]/10)%10)); serial_write('0'+(buf[28]%10));
            serial_write('.');
            serial_write('0'+(buf[29]/100)); serial_write('0'+((buf[29]/10)%10)); serial_write('0'+(buf[29]%10));
        }
        serial_write('\n');
    }

    // ARP katmanına ilet (EtherType 0x0806)
    if (arp_is_initialized())
        arp_handle_packet(buf, len);

    // IPv4 katmanına ilet (EtherType 0x0800)
    // icmp_handle_packet ve udp_handle_packet, ipv4_register_handler
    // üzerinden otomatik çağrılır.
    if (ipv4_is_initialized() && len >= 14) {
        uint16_t etype = (uint16_t)((buf[12] << 8) | buf[13]);
        if (etype == 0x0800)
            ipv4_handle_packet(buf, len);
    }
}

extern void println64(const char* str, uint8_t color);
extern void print_str64(const char* str, uint8_t color);
extern uint64_t get_system_ticks(void);
extern void serial_print(const char* str);


// ===========================================
// CPU USAGE TRACKING
// ===========================================

static uint64_t last_total_ticks = 0;

uint64_t rdtsc64(void) {
    uint32_t low, high;
    __asm__ volatile ("rdtsc" : "=a"(low), "=d"(high));
    return ((uint64_t)high << 32) | low;
}

uint32_t get_cpu_usage_64(void) {
    uint64_t current_ticks = rdtsc64();
    uint64_t delta = current_ticks - last_total_ticks;
    
    if (delta == 0) return 0;
    
    uint32_t usage = (delta % 100);
    
    if (usage < 20) usage = 20 + (delta % 30);
    if (usage > 95) usage = 95;
    
    last_total_ticks = current_ticks;
    
    return usage;
}
static inline uint8_t inb64(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outb64(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
// ===========================================
// STRING UTILITIES
// ===========================================

int str_len(const char* str) {
    int len = 0;
    while (str[len]) len++;
    return len;
}

int str_cmp(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(uint8_t*)s1 - *(uint8_t*)s2;
}

void str_cpy(char* dest, const char* src) {
    while (*src) {
        *dest++ = *src++;
    }
    *dest = '\0';
}

void str_concat(char* dest, const char* src) {
    while (*dest) dest++;
    while (*src) {
        *dest++ = *src++;
    }
    *dest = '\0';
}

// ===========================================
// OUTPUT MANAGEMENT
// ===========================================

void output_init(CommandOutput* output) {
    output->line_count = 0;
    for (int i = 0; i < MAX_OUTPUT_LINES; i++) {
        output->lines[i][0] = '\0';
        output->colors[i] = VGA_WHITE;
    }
}

void output_add_line(CommandOutput* output, const char* line, uint8_t color) {
    if (output->line_count < MAX_OUTPUT_LINES) {
        int len = 0;
        while (line[len] && len < MAX_LINE_LENGTH - 1) {
            output->lines[output->line_count][len] = line[len];
            len++;
        }
        output->lines[output->line_count][len] = '\0';
        output->colors[output->line_count] = color;
        output->line_count++;
    }
}

void output_add_empty_line(CommandOutput* output) {
    output_add_line(output, "", VGA_WHITE);
}

// ===========================================
// NUMBER TO STRING CONVERSION
// ===========================================

void uint64_to_string(uint64_t num, char* str) {
    if (num == 0) {
        str[0] = '0';
        str[1] = '\0';
        return;
    }
    
    int i = 0;
    while (num > 0) {
        str[i++] = '0' + (num % 10);
        num /= 10;
    }
    str[i] = '\0';
    
    // Reverse
    for (int j = 0; j < i / 2; j++) {
        char temp = str[j];
        str[j] = str[i - j - 1];
        str[i - j - 1] = temp;
    }
}

void int_to_str(int num, char* str) {
    if (num == 0) {
        str[0] = '0';
        str[1] = '\0';
        return;
    }
    
    int is_negative = 0;
    if (num < 0) {
        is_negative = 1;
        num = -num;
    }
    
    char temp[20];
    int i = 0;
    
    while (num > 0) {
        temp[i++] = '0' + (num % 10);
        num /= 10;
    }
    
    int j = 0;
    if (is_negative) {
        str[j++] = '-';
    }
    
    while (i > 0) {
        str[j++] = temp[--i];
    }
    str[j] = '\0';
}

// ===========================================
// MEMORY INFO
// ===========================================

uint64_t get_memory_info() {
    extern uint8_t* heap_start;
    extern uint8_t* heap_current;
    return ((uint64_t)heap_current - (uint64_t)heap_start) / 1024; // KB
}

void format_memory_size(uint64_t kb, char* buffer) {
    if (kb >= 1024 * 1024) {
        uint64_t gb = kb / (1024 * 1024);
        uint64_t mb_remainder = (kb % (1024 * 1024)) / 1024;
        uint64_to_string(gb, buffer);
        str_concat(buffer, ".");
        char temp[10];
        uint64_to_string(mb_remainder / 102, temp);
        str_concat(buffer, temp);
        str_concat(buffer, " GB");
    } else if (kb >= 1024) {
        uint64_t mb = kb / 1024;
        uint64_to_string(mb, buffer);
        str_concat(buffer, " MB");
    } else {
        uint64_to_string(kb, buffer);
        str_concat(buffer, " KB");
    }
}

// ===========================================
// COMMAND HANDLERS - BASIC
// ===========================================

void cmd_hello(const char* args, CommandOutput* output) {
    (void)args;
    output_add_line(output, "Hello from AscentOS 64-bit! Why so serious? ;)", VGA_YELLOW);
}



void cmd_help(const char* args, CommandOutput* output) {
    (void)args;
    output_add_line(output, "Available commands:", VGA_CYAN);
    output_add_line(output, " hello     - Say hello", VGA_WHITE);
    output_add_line(output, " clear     - Clear screen", VGA_WHITE);
    output_add_line(output, " help      - Show this help", VGA_WHITE);
    output_add_line(output, " echo      - Echo text", VGA_WHITE);
    output_add_line(output, " about     - About AscentOS", VGA_WHITE);
    output_add_line(output, " neofetch  - Show system info", VGA_WHITE);
    output_add_line(output, " pmm       - Physical Memory Manager stats", VGA_WHITE);
    output_add_line(output, " vmm       - Virtual Memory Manager test", VGA_WHITE);
    output_add_empty_line(output);
    output_add_line(output, "ELF Loader Commands:", VGA_YELLOW);
    output_add_line(output, " exec      - Load ELF64 + Ring-3 task olustur", VGA_WHITE);
    output_add_line(output, " elfinfo   - Show ELF64 header (no load)", VGA_WHITE);
    output_add_line(output, " kilo      - Kilo editor'u calistir: kilo <dosya>", VGA_WHITE);
    output_add_line(output, " lua       - Lua interpreter'i calistir: lua <script>", VGA_WHITE);
    output_add_line(output, "  (kilo/lua -> exec KILO.ELF / LUA.ELF otomatik)", VGA_DARK_GRAY);
    output_add_empty_line(output);
    output_add_line(output, "Multitasking Commands:", VGA_YELLOW);
    output_add_line(output, " ps        - List all tasks", VGA_WHITE);
    output_add_line(output, " taskinfo  - Show task details", VGA_WHITE);
    output_add_line(output, " createtask- Create test tasks", VGA_WHITE);
    output_add_line(output, " schedinfo - Scheduler info", VGA_WHITE);
    output_add_empty_line(output);
    output_add_line(output, "File System Commands:", VGA_YELLOW);
    output_add_line(output, " ls        - List files and directories", VGA_WHITE);
    output_add_line(output, " cd        - Change directory", VGA_WHITE);
    output_add_line(output, " pwd       - Print working directory", VGA_WHITE);
    output_add_line(output, " mkdir     - Create directory", VGA_WHITE);
    output_add_line(output, " rmdir     - Remove directory", VGA_WHITE);
    output_add_line(output, " rmr       - Remove directory recursively", VGA_WHITE);
    output_add_line(output, " cat       - Show file content", VGA_WHITE);
    output_add_line(output, " touch     - Create new file", VGA_WHITE);
    output_add_line(output, " write     - Write to file", VGA_WHITE);
    output_add_line(output, " rm        - Delete file", VGA_WHITE);

    output_add_empty_line(output);
    output_add_line(output, "Advanced File System:", VGA_GREEN);
    output_add_line(output, " tree      - Show full directory tree", VGA_WHITE);
    output_add_line(output, " find      - Find files by pattern", VGA_WHITE);
    output_add_line(output, " du        - Show disk usage", VGA_WHITE);
    output_add_empty_line(output);
    output_add_line(output, "System Commands:", VGA_YELLOW);
    output_add_line(output, " sysinfo   - System information", VGA_WHITE);
    output_add_line(output, " cpuinfo   - CPU information", VGA_WHITE);
    output_add_line(output, " meminfo   - Memory information", VGA_WHITE);
    output_add_line(output, " reboot    - Reboot the system", VGA_WHITE);
    output_add_empty_line(output);
    output_add_line(output, "SYSCALL Commands:", VGA_YELLOW);
    output_add_line(output, " syscallinfo - SYSCALL/SYSRET MSR configuration", VGA_WHITE);
    output_add_line(output, " syscalltest - Run full test suite (116 tests)", VGA_WHITE);
    output_add_line(output, "              v10: signal/sigaction/sigprocmask", VGA_WHITE);
    output_add_empty_line(output);
    output_add_line(output, "Network Commands (RTL8139):", VGA_CYAN);
    output_add_line(output, " netinit   - RTL8139 ag surucusunu baslat", VGA_WHITE);
    output_add_line(output, " netstat   - Ag karti durumu + sayaclar", VGA_WHITE);
    output_add_line(output, " netsend   - Test paketi gonder [adet]", VGA_WHITE);
    output_add_line(output, " netmon    - Alinan paketleri izle", VGA_WHITE);
    output_add_empty_line(output);
    output_add_line(output, "Network Commands (ARP/IPv4/ICMP):", VGA_CYAN);
    output_add_line(output, " ipconfig  - IP ata / goster   ornek: ipconfig 10.0.2.15", VGA_WHITE);
    output_add_line(output, " arping    - ARP request gonder ornek: arping 10.0.2.2", VGA_WHITE);
    output_add_line(output, " arpcache  - ARP cache tablosunu goster", VGA_WHITE);
    output_add_line(output, " arpflush  - ARP cache temizle", VGA_WHITE);
    output_add_line(output, " arpstatic - Statik ARP: arpstatic <IP> <MAC>", VGA_WHITE);
    output_add_line(output, " ipv4info  - IPv4 katman durumu ve sayaclari", VGA_WHITE);
    output_add_line(output, " ping      - ICMP ping   ornek: ping 10.0.2.2", VGA_WHITE);
    output_add_line(output, " ping      - Birden fazla: ping 10.0.2.2 4", VGA_DARK_GRAY);
    output_add_empty_line(output);
    output_add_line(output, "Network Commands (UDP - Asama 4):", VGA_CYAN);
    output_add_line(output, " udpinit              - UDP katmanini baslat (ipconfig sonrasi)", VGA_WHITE);
    output_add_line(output, " udplisten <port>     - Porta UDP echo sunucusu bagla", VGA_WHITE);
    output_add_line(output, " udpsend <ip> <p> <m> - UDP mesaji gonder", VGA_WHITE);
    output_add_line(output, " udpclose <port>      - Port dinleyicisini kaldir", VGA_WHITE);
    output_add_line(output, " udpstat              - UDP soket tablosu ve sayaclar", VGA_WHITE);
    output_add_line(output, "", VGA_WHITE);
    output_add_line(output, "Network Commands (DHCP - Asama 5):", VGA_CYAN);
    output_add_line(output, " dhcp                 - DHCP ile otomatik IP al", VGA_WHITE);
    output_add_line(output, " dhcpstat             - DHCP durum + atanan IP/GW/DNS", VGA_WHITE);
    output_add_line(output, " dhcprel              - DHCP Release (IP iade et)", VGA_WHITE);
    output_add_empty_line(output);
    output_add_line(output, "Network Commands (TCP - Asama 6):", VGA_CYAN);
    output_add_line(output, " tcpstat              - Tum TCP baglantilarini goster", VGA_WHITE);
    output_add_line(output, " tcpconnect <ip> <p>  - TCP baglantisi kur", VGA_WHITE);
    output_add_line(output, " tcpsend <id> <msg>   - Baglantiya veri gonder", VGA_WHITE);
    output_add_line(output, " tcpclose <id>        - Baglantiyi kapat (FIN)", VGA_WHITE);
    output_add_line(output, " tcplisten <port>     - TCP sunucu baslatir", VGA_WHITE);
    output_add_line(output, " tcptest <ip> <port>  - HTTP GET testi (ornek: tcptest 10.0.2.2 80)", VGA_WHITE);
    output_add_empty_line(output);
    output_add_line(output, "Debug Commands:", VGA_RED);
    output_add_line(output, " panic       - Panic ekranini test et", VGA_WHITE);
    output_add_line(output, "   panic df    #DF Double Fault", VGA_DARK_GRAY);
    output_add_line(output, "   panic gp    #GP General Protection", VGA_DARK_GRAY);
    output_add_line(output, "   panic pf    #PF Page Fault (NULL deref)", VGA_DARK_GRAY);
    output_add_line(output, "   panic ud    #UD Invalid Opcode", VGA_DARK_GRAY);
    output_add_line(output, "   panic de    #DE Divide by Zero", VGA_DARK_GRAY);
    output_add_line(output, "   panic stack Stack overflow", VGA_DARK_GRAY);
    output_add_line(output, " perf        - RDTSC performans olcumu", VGA_WHITE);
    output_add_line(output, "   perf         Tum testler (dongu/memset/memcpy/overhead)", VGA_DARK_GRAY);
    output_add_line(output, " spinlock    - Spinlock / RWLock test suite", VGA_WHITE);
}

void cmd_clear(const char* args, CommandOutput* output) {
    (void)args;
    output_add_line(output, "__CLEAR_SCREEN__", VGA_WHITE);
}

void cmd_echo(const char* args, CommandOutput* output) {
    if (str_len(args) > 0) {
        output_add_line(output, args, VGA_WHITE);
    } else {
        output_add_empty_line(output);
    }
}

void cmd_about(const char* args, CommandOutput* output) {
    (void)args;
    output_add_line(output, "========================================", VGA_RED);
    output_add_line(output, "     ASCENTOS v0.1 - Why So Serious?", VGA_GREEN);
    output_add_line(output, "   A minimal x86_64 OS written in chaos", VGA_YELLOW);
    output_add_line(output, "      Built from scratch. No regrets.", VGA_RED);
    output_add_line(output, "========================================", VGA_RED);
    output_add_line(output, "", VGA_WHITE);
    output_add_line(output, "64-bit Edition - Now with MORE bits!", VGA_CYAN);
    output_add_line(output, "Featuring: Persistent File System!", VGA_GREEN);
}


// ===========================================
// FILE SYSTEM COMMANDS
// ===========================================

void cmd_ls(const char* args, CommandOutput* output) {
    // Ext2 üzerinden listele
    const char* path = (args && str_len(args) > 0) ? args : ext2_getcwd();

    static dirent64_t dents[256];
    int total = ext2_getdents(path, dents, (int)sizeof(dents));

    if (total < 0) {
        output_add_line(output, "Error: Cannot read directory (ext2 not mounted?)", VGA_RED);
        return;
    }

    // Başlık
    char hdr[MAX_LINE_LENGTH];
    str_cpy(hdr, "Directory: ");
    str_concat(hdr, path);
    output_add_line(output, hdr, VGA_CYAN);

    int count = 0;
    int off = 0;
    while (off < total) {
        dirent64_t* de = (dirent64_t*)((char*)dents + off);
        if (de->d_reclen == 0) break;

        // "." ve ".." gizle
        if (!(de->d_name[0] == '.' &&
              (de->d_name[1] == '\0' ||
              (de->d_name[1] == '.' && de->d_name[2] == '\0')))) {
            char line[MAX_LINE_LENGTH];
            if (de->d_type == DT_DIR) {
                str_cpy(line, "  [DIR]  ");
            } else {
                str_cpy(line, "  [FILE] ");
            }
            str_concat(line, de->d_name);

            // Dosya boyutunu ekle
            if (de->d_type == DT_REG) {
                // Tam yolu oluştur
                char fpath[256];
                str_cpy(fpath, path);
                int plen = str_len(fpath);
                if (plen > 1) { fpath[plen] = '/'; fpath[plen+1] = '\0'; }
                str_concat(fpath, de->d_name);
                uint32_t fsz = ext2_file_size(fpath);
                if (fsz > 0) {
                    str_concat(line, "  (");
                    char szb[16];
                    uint32_t v = fsz; int i = 0;
                    if (!v) { szb[i++] = '0'; }
                    else { char t[12]; int n=0; while(v){t[n++]='0'+(v%10);v/=10;}
                           for(int j=n-1;j>=0;j--) szb[i++]=t[j]; }
                    szb[i] = '\0';
                    str_concat(line, szb);
                    str_concat(line, " B)");
                }
            }
            output_add_line(output, line,
                de->d_type == DT_DIR ? VGA_YELLOW : VGA_WHITE);
            count++;
        }
        off += de->d_reclen;
    }

    if (count == 0) {
        output_add_line(output, "  (empty)", VGA_DARK_GRAY);
    } else {
        char footer[64];
        str_cpy(footer, "  ");
        char cb[12]; uint32_t v=(uint32_t)count,i=0;
        if(!v){cb[i++]='0';}else{char t[12];int n=0;while(v){t[n++]='0'+(v%10);v/=10;}for(int j=n-1;j>=0;j--)cb[i++]=t[j];}cb[i]='\0';
        str_concat(footer, cb);
        str_concat(footer, " item(s)");
        output_add_line(output, footer, VGA_DARK_GRAY);
    }
}

void cmd_cat(const char* args, CommandOutput* output) {
    if (str_len(args) == 0) {
        output_add_line(output, "Usage: cat <filename>", VGA_RED);
        return;
    }

    // Tam yol oluştur
    char path[256];
    if (args[0] == '/') {
        str_cpy(path, args);
    } else {
        str_cpy(path, ext2_getcwd());
        int plen = str_len(path);
        if (plen > 1) { path[plen] = '/'; path[plen+1] = '\0'; }
        str_concat(path, args);
    }

    uint32_t fsize = ext2_file_size(path);
    if (fsize == 0) {
        output_add_line(output, "File not found or empty: ", VGA_RED);
        output_add_line(output, path, VGA_RED);
        return;
    }

    // Max 64KB cat için yeterli
    static uint8_t cat_buf[65536];
    uint32_t to_read = (fsize < sizeof(cat_buf) - 1) ? fsize : sizeof(cat_buf) - 1;
    int n = ext2_read_file(path, cat_buf, to_read);
    if (n <= 0) {
        output_add_line(output, "Read error", VGA_RED);
        return;
    }
    cat_buf[n] = '\0';

    const char* p = (const char*)cat_buf;
    const char* start = p;
    while (*p) {
        if (*p == '\n') {
            char line[MAX_LINE_LENGTH];
            int len = (int)(p - start);
            if (len >= MAX_LINE_LENGTH) len = MAX_LINE_LENGTH - 1;
            for (int i = 0; i < len; i++) line[i] = start[i];
            line[len] = '\0';
            output_add_line(output, line, VGA_WHITE);
            start = p + 1;
        }
        p++;
    }
    if (p > start) {
        char line[MAX_LINE_LENGTH];
        int len = (int)(p - start);
        if (len >= MAX_LINE_LENGTH) len = MAX_LINE_LENGTH - 1;
        for (int i = 0; i < len; i++) line[i] = start[i];
        line[len] = '\0';
        output_add_line(output, line, VGA_WHITE);
    }
}

void cmd_touch(const char* args, CommandOutput* output) {
    if (str_len(args) == 0) {
        output_add_line(output, "Usage: touch <filename>", VGA_RED);
        return;
    }

    for (int i = 0; args[i]; i++) {
        if (args[i] == ' ') {
            output_add_line(output, "Error: Filename cannot contain spaces", VGA_RED);
            return;
        }
    }

    // Tam yol oluştur
    char tpath[256];
    if (args[0] == '/') { str_cpy(tpath, args); }
    else { str_cpy(tpath, ext2_getcwd()); int pl=str_len(tpath); if(pl>1){tpath[pl]='/';tpath[pl+1]='\0';} str_concat(tpath, args); }

    int rc = ext2_create_file(tpath);
    if (rc == 0) {
        output_add_line(output, "File created: ", VGA_GREEN);
        output_add_line(output, tpath, VGA_YELLOW);
    } else {
        output_add_line(output, "Error: Cannot create file", VGA_RED);
    }
}

void cmd_write(const char* args, CommandOutput* output) {
    if (str_len(args) == 0) {
        output_add_line(output, "Usage: write <filename> <content>", VGA_RED);
        output_add_line(output, "Example: write test.txt Hello World!", VGA_CYAN);
        return;
    }

    char filename[32];
    int i = 0;
    while (args[i] && args[i] != ' ' && i < 31) {
        filename[i] = args[i];
        i++;
    }
    filename[i] = '\0';

    if (str_len(filename) == 0) {
        output_add_line(output, "Error: No filename specified", VGA_RED);
        return;
    }

    while (args[i] == ' ') i++;
    const char* content = &args[i];

    if (str_len(content) == 0) {
        output_add_line(output, "Error: No content specified", VGA_RED);
        return;
    }

    // Tam yol oluştur
    char wpath[256];
    if (filename[0] == '/') { str_cpy(wpath, filename); }
    else { str_cpy(wpath, ext2_getcwd()); int pl=str_len(wpath); if(pl>1){wpath[pl]='/';wpath[pl+1]='\0';} str_concat(wpath, filename); }

    // Dosya yoksa oluştur
    if (!ext2_path_is_file(wpath)) ext2_create_file(wpath);

    int wlen = str_len(content);
    int wrc = ext2_write_file(wpath, 0, (const uint8_t*)content, (uint32_t)wlen);
    if (wrc >= 0) {
        char msg[MAX_LINE_LENGTH];
        str_cpy(msg, "Written to: "); str_concat(msg, wpath);
        output_add_line(output, msg, VGA_GREEN);
    } else {
        output_add_line(output, "Error: Write failed", VGA_RED);
    }
}

void cmd_rm(const char* args, CommandOutput* output) {
    if (str_len(args) == 0) {
        output_add_line(output, "Usage: rm <filename>", VGA_RED);
        output_add_line(output, "Example: rm test.txt", VGA_CYAN);
        return;
    }

    for (int i = 0; args[i]; i++) {
        if (args[i] == ' ') {
            output_add_line(output, "Error: Filename cannot contain spaces", VGA_RED);
            return;
        }
    }

    char rmpath[256];
    if (args[0] == '/') { str_cpy(rmpath, args); }
    else { str_cpy(rmpath, ext2_getcwd()); int pl=str_len(rmpath); if(pl>1){rmpath[pl]='/';rmpath[pl+1]='\0';} str_concat(rmpath, args); }

    int rmrc = ext2_unlink(rmpath);
    if (rmrc == 0) {
        char msg[MAX_LINE_LENGTH];
        str_cpy(msg, "Deleted: "); str_concat(msg, rmpath);
        output_add_line(output, msg, VGA_GREEN);
    } else {
        output_add_line(output, "Error: Cannot delete (not found or is a directory)", VGA_RED);
    }
}

void cmd_neofetch(const char* args, CommandOutput* output) {
    (void)args;

    const char* art_lines[18] = {
        "                                   ",
        "             .                     ",
        "           @@@@@@@@@@@@@           ",
        "       =@@@@@@@@@@@@@@@@@@@@@==    ",
        "     *#@@@@@@@@@@@@@@@@@@@@@   @=@ ",
        "     @@@@@@@@@@@@@@@@@@@@@@@@@  @= ",
        "    @@@@@@@@@@@@@@@@@@@@@@@@@@@ =@ ",
        "    @@@@@@@@@@@@@@@@@@@@@@@@@@@==  ",
        "   @@@@@@@@@@@@@@@@@@@@@@@@@@==@   ",
        "   @@@@@@@@@@@@@@@@@@@@@@@@=@=@@   ",
        "  %@@@@@@@@@@@@@@@@@@@@@@=@=@@@    ",
        " .%@@@@@@@@@@@@@@@@@@@@==%@@@@@    ",
        " =% :@@@@@@@@@@@@@=@@==@@@@@@@     ",
        " =%  +@@@@@@@@===#=@@@@@@@@@@      ",
        "  @@=@=@=@====#@@@@@@@@@@@@@       ",
        "         @@@@@@@@@@@@@@@@@         ",
        "            @@@@@@@@@@@            ",
        "                                   "
    };

    uint8_t art_colors[18] = {
        VGA_GREEN, VGA_GREEN, VGA_GREEN, VGA_GREEN, VGA_GREEN,
        VGA_GREEN, VGA_GREEN, VGA_GREEN, VGA_GREEN, VGA_GREEN,
        VGA_GREEN, VGA_GREEN, VGA_GREEN, VGA_GREEN, VGA_GREEN,
        VGA_GREEN, VGA_GREEN, VGA_GREEN
    };

    char info_lines[18][64];
    for (int i = 0; i < 18; i++) info_lines[i][0] = '\0';

    char cpu_brand[49];
    cpu_get_model_name(cpu_brand);
    uint64_t heap_kb = get_memory_info();
    char memory_str[64];
    format_memory_size(heap_kb, memory_str);
    // ext2 /bin dosya sayısı
    int file_count = 0;
    {
        static dirent64_t neo_dents[64];
        int tot = ext2_getdents("/bin", neo_dents, (int)sizeof(neo_dents));
        if (tot > 0) {
            int off = 0;
            while (off < tot) {
                dirent64_t* de = (dirent64_t*)((char*)neo_dents + off);
                if (de->d_reclen == 0) break;
                if (de->d_type == DT_REG) file_count++;
                off += de->d_reclen;
            }
        }
    }
    char count_str[16];
    int_to_str(file_count, count_str);

    // Calculate uptime
    uint64_t ticks = get_system_ticks();  // Ticks since boot (1000 Hz)
    uint64_t seconds = ticks / 1000;
    uint64_t minutes = seconds / 60;
    uint64_t hours = minutes / 60;
    uint64_t days = hours / 24;
    
    seconds %= 60;
    minutes %= 60;
    hours %= 24;
    
    char uptime_str[64];
    uptime_str[0] = '\0';
    str_cpy(uptime_str, "Uptime: ");
    
    if (days > 0) {
        char day_str[16];
        uint64_to_string(days, day_str);
        str_concat(uptime_str, day_str);
        str_concat(uptime_str, " day");
        if (days > 1) str_concat(uptime_str, "s");
        str_concat(uptime_str, ", ");
    }
    
    char hour_str[16], min_str[16], sec_str[16];
    uint64_to_string(hours, hour_str);
    uint64_to_string(minutes, min_str);
    uint64_to_string(seconds, sec_str);
    
    str_concat(uptime_str, hour_str);
    str_concat(uptime_str, "h ");
    str_concat(uptime_str, min_str);
    str_concat(uptime_str, "m ");
    str_concat(uptime_str, sec_str);
    str_concat(uptime_str, "s");

    // Sistem bilgileri
    str_cpy(info_lines[0],  "AscentOS v0.1 64-bit");
    str_cpy(info_lines[1],  "---------------------");
    str_cpy(info_lines[3],  "OS: AscentOS x86_64 - Why So Serious?");
    str_cpy(info_lines[4],  "Kernel: AscentOS Kernel 0.1");
    str_cpy(info_lines[5],  uptime_str);
    str_cpy(info_lines[6],  "Packages: 64 (get it?)");
    str_cpy(info_lines[7],  "Shell: AscentShell v0.1 64-bit");

    char temp[64];
    str_cpy(temp, "CPU: ");
    str_concat(temp, cpu_brand);
    str_cpy(info_lines[9], temp);

    str_cpy(info_lines[10], "GPU: VGA - colors of madness");

    str_cpy(temp, "Memory: ");
    str_concat(temp, memory_str);
    str_concat(temp, " (Heap)");
    str_cpy(info_lines[12], temp);

    str_cpy(temp, "Files: ");
    str_concat(temp, count_str);
    str_concat(temp, " files in filesystem");
    str_cpy(info_lines[14], temp);

    str_cpy(info_lines[16], "Type 'help' to see all commands");
    str_cpy(info_lines[17], "Why so serious? ;)");

    char full_line[MAX_LINE_LENGTH];
    for (int i = 0; i < 18; i++) {
        full_line[0] = '\0';
        str_cpy(full_line, art_lines[i]);
        str_concat(full_line, "   ");
        if (info_lines[i][0] != '\0') {
            str_concat(full_line, info_lines[i]);
        }
        output_add_line(output, full_line, art_colors[i]);
    }

    output_add_empty_line(output);
}

// ===========================================
// OLD STYLE COMMANDS (Direct VGA)
// ===========================================

void cmd_sysinfo(void) {
    println64("System Information:", VGA_CYAN);
    println64("", VGA_WHITE);
    
    char cpu_brand[49];
    cpu_get_model_name(cpu_brand);
    print_str64("CPU: ", VGA_WHITE);
    println64(cpu_brand, VGA_YELLOW);
    
    extern uint8_t* heap_start;
    extern uint8_t* heap_current;
    uint64_t heap_used = (uint64_t)heap_current - (uint64_t)heap_start;
    
    char mem_str[32];
    uint64_to_string(heap_used / 1024, mem_str);
    print_str64("Heap used: ", VGA_WHITE);
    print_str64(mem_str, VGA_GREEN);
    println64(" KB", VGA_WHITE);
    
    println64("Architecture: x86_64 (64-bit)", VGA_GREEN);
    
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    print_str64("Page Table (CR3): 0x", VGA_WHITE);
    
    char hex_str[20];
    const char hex_chars[] = "0123456789ABCDEF";
    for (int i = 0; i < 16; i++) {
        hex_str[i] = hex_chars[(cr3 >> (60 - i * 4)) & 0xF];
    }
    hex_str[16] = '\0';
    println64(hex_str, VGA_YELLOW);
    
    {
        int file_count = 0;
        static dirent64_t si_dents[64];
        int tot = ext2_getdents("/bin", si_dents, (int)sizeof(si_dents));
        if (tot > 0) {
            int off = 0;
            while (off < tot) {
                dirent64_t* de = (dirent64_t*)((char*)si_dents + off);
                if (de->d_reclen == 0) break;
                if (de->d_type == DT_REG) file_count++;
                off += de->d_reclen;
            }
        }
        print_str64("Files in /bin: ", VGA_WHITE);
        char count_str[16];
        int_to_str(file_count, count_str);
        println64(count_str, VGA_GREEN);
    }
}

void cmd_cpuinfo(void) {
    println64("CPU Information:", VGA_CYAN);
    println64("", VGA_WHITE);

    // ── Vendor ID ────────────────────────────────────────────────────────
    char vendor[13];
    get_cpu_info(vendor);
    print_str64("Vendor  : ", VGA_WHITE);
    println64(vendor, VGA_GREEN);

    // ── Tam model adı ────────────────────────────────────────────────────
    char model[49];
    cpu_get_model_name(model);
    if (model[0] != '\0') {
        print_str64("Model   : ", VGA_WHITE);
        println64(model, VGA_YELLOW);
    }

    // ── Family / Model / Stepping ─────────────────────────────────────────
    CPUStepping step;
    cpu_get_stepping(&step);
    {
        char buf[64]; char tmp[8];
        buf[0] = '\0';
        str_concat(buf, "Family:"); int_to_str((int)step.family,   tmp); str_concat(buf, tmp);
        str_concat(buf, "  Model:");    int_to_str((int)step.model,    tmp); str_concat(buf, tmp);
        str_concat(buf, "  Stepping:"); int_to_str((int)step.stepping, tmp); str_concat(buf, tmp);
        str_concat(buf, "  Type:");     int_to_str((int)step.cpu_type, tmp); str_concat(buf, tmp);
        print_str64("Ident   : ", VGA_WHITE);
        println64(buf, VGA_CYAN);
    }

    // ── Tahmini frekans ──────────────────────────────────────────────────
    {
        uint32_t mhz = cpu_get_freq_estimate();
        char buf[32];
        buf[0] = '\0';
        char tmp[12];
        uint64_to_string((uint64_t)mhz, tmp);
        str_concat(buf, tmp);
        str_concat(buf, " MHz");
        // GHz de göster
        if (mhz >= 1000) {
            uint32_t ghz_int  = mhz / 1000;
            uint32_t ghz_frac = (mhz % 1000) / 10;
            str_concat(buf, "  (~");
            uint64_to_string((uint64_t)ghz_int, tmp);  str_concat(buf, tmp);
            str_concat(buf, ".");
            if (ghz_frac < 10) str_concat(buf, "0");
            uint64_to_string((uint64_t)ghz_frac, tmp); str_concat(buf, tmp);
            str_concat(buf, " GHz)");
        }
        print_str64("Freq    : ", VGA_WHITE);
        println64(buf, VGA_YELLOW);
    }

    // ── Önbellek boyutları ───────────────────────────────────────────────
    {
        CacheInfo ci;
        cpu_get_cache_info(&ci);
        char buf[64]; char tmp[12];

        // L1
        buf[0] = '\0';
        str_concat(buf, "L1D:");
        uint64_to_string((uint64_t)ci.l1d_kb, tmp); str_concat(buf, tmp);
        str_concat(buf, "KB  L1I:");
        uint64_to_string((uint64_t)ci.l1i_kb, tmp); str_concat(buf, tmp);
        str_concat(buf, "KB");
        print_str64("Cache   : ", VGA_WHITE);
        println64(buf, VGA_GREEN);

        // L2 / L3
        buf[0] = '\0';
        str_concat(buf, "L2:");
        if (ci.l2_kb >= 1024) {
            uint64_to_string((uint64_t)(ci.l2_kb / 1024), tmp); str_concat(buf, tmp);
            str_concat(buf, "MB");
        } else {
            uint64_to_string((uint64_t)ci.l2_kb, tmp); str_concat(buf, tmp);
            str_concat(buf, "KB");
        }
        if (ci.l3_kb > 0) {
            str_concat(buf, "  L3:");
            if (ci.l3_kb >= 1024) {
                uint64_to_string((uint64_t)(ci.l3_kb / 1024), tmp); str_concat(buf, tmp);
                str_concat(buf, "MB");
            } else {
                uint64_to_string((uint64_t)ci.l3_kb, tmp); str_concat(buf, tmp);
                str_concat(buf, "KB");
            }
        }
        print_str64("          ", VGA_WHITE);
        println64(buf, VGA_GREEN);
    }

    // ── Özellikler ───────────────────────────────────────────────────────
    uint32_t feat = cpu_get_features();
    print_str64("Features: ", VGA_WHITE);
    if (feat & CPU_FEAT_FPU)    print_str64("FPU ",    VGA_YELLOW);
    if (feat & CPU_FEAT_TSC)    print_str64("TSC ",    VGA_YELLOW);
    if (feat & CPU_FEAT_PAE)    print_str64("PAE ",    VGA_YELLOW);
    if (feat & CPU_FEAT_MMX)    print_str64("MMX ",    VGA_YELLOW);
    if (feat & CPU_FEAT_SSE)    print_str64("SSE ",    VGA_YELLOW);
    if (feat & CPU_FEAT_SSE2)   print_str64("SSE2 ",   VGA_YELLOW);
    if (feat & CPU_FEAT_SSE3)   print_str64("SSE3 ",   VGA_YELLOW);
    if (feat & CPU_FEAT_SSSE3)  print_str64("SSSE3 ",  VGA_YELLOW);
    if (feat & CPU_FEAT_SSE41)  print_str64("SSE4.1 ", VGA_YELLOW);
    if (feat & CPU_FEAT_SSE42)  print_str64("SSE4.2 ", VGA_YELLOW);
    if (feat & CPU_FEAT_AVX)    print_str64("AVX ",    VGA_GREEN);
    if (feat & CPU_FEAT_AES)    print_str64("AES-NI ", VGA_GREEN);
    if (feat & CPU_FEAT_RDRAND) print_str64("RDRAND ", VGA_GREEN);
    println64("", VGA_WHITE);

    // ── Long Mode ────────────────────────────────────────────────────────
    if (feat & CPU_FEAT_LONG)
        println64("Long Mode: Supported (64-bit)", VGA_GREEN);
    else
        println64("Long Mode: NOT supported", VGA_RED);
}

void cmd_meminfo(void) {
    extern void show_memory_info(void);
    show_memory_info();
}

void cmd_reboot(const char* args, CommandOutput* output) {
    (void)args;
    
    output_add_line(output, "Rebooting...", VGA_YELLOW);
    
    __asm__ volatile ("cli");
    
    uint8_t good = 0x02;
    while (good & 0x02) {
        good = inb64(0x64);
    }
    
    outb64(0x64, 0xFE);
    __asm__ volatile ("hlt");
    for(;;);
}

void cmd_mkdir(const char* args, CommandOutput* output) {
    if (str_len(args) == 0) {
        output_add_line(output, "Usage: mkdir <dirname>", VGA_RED);
        output_add_line(output, "Example: mkdir documents", VGA_CYAN);
        return;
    }

    for (int i = 0; args[i]; i++) {
        if (args[i] == ' ') {
            output_add_line(output, "Error: Directory name cannot contain spaces", VGA_RED);
            return;
        }
    }

    char mdpath[256];
    if (args[0] == '/') { str_cpy(mdpath, args); }
    else { str_cpy(mdpath, ext2_getcwd()); int pl=str_len(mdpath); if(pl>1){mdpath[pl]='/';mdpath[pl+1]='\0';} str_concat(mdpath, args); }

    int mdrc = ext2_mkdir(mdpath);
    if (mdrc == 0) {
        char msg[MAX_LINE_LENGTH];
        str_cpy(msg, "Directory created: "); str_concat(msg, mdpath);
        output_add_line(output, msg, VGA_GREEN);
    } else {
        output_add_line(output, "Error: Cannot create directory", VGA_RED);
    }
}

void cmd_rmdir(const char* args, CommandOutput* output) {
    if (str_len(args) == 0) {
        output_add_line(output, "Usage: rmdir <dirname>", VGA_RED);
        output_add_line(output, "Example: rmdir documents", VGA_CYAN);
        return;
    }

    for (int i = 0; args[i]; i++) {
        if (args[i] == ' ') {
            output_add_line(output, "Error: Directory name cannot contain spaces", VGA_RED);
            return;
        }
    }

    char rdpath[256];
    if (args[0] == '/') { str_cpy(rdpath, args); }
    else { str_cpy(rdpath, ext2_getcwd()); int pl=str_len(rdpath); if(pl>1){rdpath[pl]='/';rdpath[pl+1]='\0';} str_concat(rdpath, args); }

    int rdrc = ext2_rmdir(rdpath);
    if (rdrc == 0) {
        char msg[MAX_LINE_LENGTH];
        str_cpy(msg, "Directory removed: "); str_concat(msg, rdpath);
        output_add_line(output, msg, VGA_GREEN);
    } else {
        output_add_line(output, "Error: Cannot remove (not found, not empty, or is a file)", VGA_RED);
    }
}

void cmd_cd(const char* args, CommandOutput* output) {
    const char* target = (str_len(args) == 0) ? "/" : args;

    int rc = ext2_chdir(target);
    if (rc == 0) {
        char msg[MAX_LINE_LENGTH];
        str_cpy(msg, "Changed directory to: ");
        str_concat(msg, ext2_getcwd());
        output_add_line(output, msg, VGA_GREEN);
    } else {
        output_add_line(output, "Error: Directory not found", VGA_RED);
    }
}

void cmd_pwd(const char* args, CommandOutput* output) {
    (void)args;
    const char* cwd = ext2_getcwd();
    if (!cwd || cwd[0] == '\0') cwd = fs_getcwd64();
    output_add_line(output, cwd, VGA_CYAN);
}

// ===========================================
// PMM COMMAND
// ===========================================

void cmd_pmm(const char* args, CommandOutput* output) {
    (void)args;
    
    extern void pmm_print_stats(void);
    
    output_add_line(output, "Physical Memory Manager (PMM)", VGA_CYAN);
    output_add_line(output, "=========================================", VGA_CYAN);
    output_add_empty_line(output);
    
    pmm_print_stats();
    
    output_add_empty_line(output);
    output_add_line(output, "PMM manages 4KB physical memory frames", VGA_WHITE);
    output_add_line(output, "Use 'meminfo' for heap statistics", VGA_DARK_GRAY);
}

// ===========================================
// VMM TEST COMMAND
// ===========================================

void cmd_vmm(const char* args, CommandOutput* output) {
    if (str_len(args) > 0 && str_cmp(args, "stats") == 0) {
        output_add_line(output, "VMM Statistics:", VGA_CYAN);
        output_add_line(output, "===============", VGA_CYAN);
        output_add_empty_line(output);
        
        extern uint64_t vmm_get_pages_mapped(void);
        extern uint64_t vmm_get_pages_unmapped(void);
        extern uint64_t vmm_get_page_faults(void);
        extern uint64_t vmm_get_tlb_flushes(void);
        extern uint64_t vmm_get_demand_allocations(void);
        extern uint64_t vmm_get_reserved_pages(void);
        
        char line[MAX_LINE_LENGTH];
        char num_str[32];
        
        str_cpy(line, "  Pages mapped: ");
        uint64_to_string(vmm_get_pages_mapped(), num_str);
        str_concat(line, num_str);
        output_add_line(output, line, VGA_WHITE);
        
        str_cpy(line, "  Pages unmapped: ");
        uint64_to_string(vmm_get_pages_unmapped(), num_str);
        str_concat(line, num_str);
        output_add_line(output, line, VGA_WHITE);
        
        str_cpy(line, "  Page faults: ");
        uint64_to_string(vmm_get_page_faults(), num_str);
        str_concat(line, num_str);
        output_add_line(output, line, VGA_YELLOW);
        
        str_cpy(line, "  TLB flushes: ");
        uint64_to_string(vmm_get_tlb_flushes(), num_str);
        str_concat(line, num_str);
        output_add_line(output, line, VGA_CYAN);
        
        output_add_empty_line(output);
        output_add_line(output, "Demand Paging:", VGA_CYAN);
        
        str_cpy(line, "  Demand allocations: ");
        uint64_to_string(vmm_get_demand_allocations(), num_str);
        str_concat(line, num_str);
        output_add_line(output, line, VGA_GREEN);
        
        str_cpy(line, "  Reserved pages: ");
        uint64_to_string(vmm_get_reserved_pages(), num_str);
        str_concat(line, num_str);
        output_add_line(output, line, VGA_MAGENTA);
        
        output_add_empty_line(output);
        output_add_line(output, "VMM manages 4-level page tables (PML4)", VGA_DARK_GRAY);
        output_add_line(output, "Supports 4KB and 2MB pages", VGA_DARK_GRAY);
        return;
    }
    
    if (str_len(args) > 0 && str_cmp(args, "demand") == 0) {
        extern int vmm_enable_demand_paging(void);
        extern int vmm_reserve_pages(uint64_t, uint64_t, uint64_t);
        extern int vmm_is_demand_paging_enabled(void);
        
        output_add_line(output, "VMM Demand Paging Test", VGA_CYAN);
        output_add_line(output, "======================", VGA_CYAN);
        output_add_empty_line(output);
        
        if (!vmm_is_demand_paging_enabled()) {
            vmm_enable_demand_paging();
            output_add_line(output, "[1] Demand paging enabled", VGA_GREEN);
        } else {
            output_add_line(output, "[1] Demand paging already enabled", VGA_YELLOW);
        }
        
        output_add_line(output, "[2] Reserving 10 pages at 0x700000...", VGA_YELLOW);
        int result = vmm_reserve_pages(0x700000, 10, PAGE_WRITE);
        if (result == 0) {
            output_add_line(output, "  OK Pages reserved (no physical memory yet)", VGA_GREEN);
        } else {
            output_add_line(output, "  ERROR Failed to reserve pages", VGA_RED);
            return;
        }
        
        output_add_empty_line(output);
        output_add_line(output, "[3] Accessing reserved page...", VGA_YELLOW);
        output_add_line(output, "  Writing to 0x700000 will trigger page fault", VGA_CYAN);
        output_add_line(output, "  Physical page will be allocated on demand", VGA_CYAN);
        
        volatile uint64_t* test_ptr = (volatile uint64_t*)0x700000;
        *test_ptr = 0xDEADBEEF;
        
        output_add_line(output, "  OK Page allocated on demand!", VGA_GREEN);
        
        if (*test_ptr == 0xDEADBEEF) {
            output_add_line(output, "  OK Value verified: 0xDEADBEEF", VGA_GREEN);
        }
        
        output_add_empty_line(output);
        output_add_line(output, "Demand Paging Test Complete!", VGA_GREEN);
        output_add_line(output, "Check 'vmm stats' to see demand allocations", VGA_CYAN);
        return;
    }
    
    output_add_line(output, "VMM (Virtual Memory Manager) Test", VGA_CYAN);
    output_add_line(output, "===================================", VGA_CYAN);
    output_add_empty_line(output);
    
    output_add_line(output, "[TEST 1] Mapping 4KB page...", VGA_YELLOW);
    uint64_t test_virt = 0x400000;
    uint64_t test_phys = 0x200000;
    
    int result = vmm_map_page(test_virt, test_phys, PAGE_WRITE | PAGE_PRESENT);
    if (result == 0) {
        output_add_line(output, "  OK Page mapped successfully", VGA_GREEN);
        uint64_t phys = vmm_get_physical_address(test_virt);
        if (phys == test_phys) {
            output_add_line(output, "  OK Address translation verified", VGA_GREEN);
        } else {
            output_add_line(output, "  ERROR Address translation failed", VGA_RED);
        }
    } else {
        output_add_line(output, "  ERROR Failed to map page", VGA_RED);
    }
    output_add_empty_line(output);
    
    output_add_line(output, "[TEST 2] Checking page presence...", VGA_YELLOW);
    if (vmm_is_page_present(test_virt)) {
        output_add_line(output, "  OK Page is present", VGA_GREEN);
    } else {
        output_add_line(output, "  ERROR Page not found", VGA_RED);
    }
    output_add_empty_line(output);
    
    output_add_line(output, "[TEST 3] Mapping 16KB range...", VGA_YELLOW);
    result = vmm_map_range(0x500000, 0x300000, 16384, PAGE_WRITE | PAGE_PRESENT);
    if (result == 0) {
        output_add_line(output, "  OK Range mapped (4 pages)", VGA_GREEN);
    } else {
        output_add_line(output, "  ERROR Range mapping failed", VGA_RED);
    }
    output_add_empty_line(output);
    
    output_add_line(output, "[TEST 4] Mapping 2MB large page...", VGA_YELLOW);
    result = vmm_map_page_2mb(0x800000, 0x800000, PAGE_WRITE | PAGE_PRESENT);
    if (result == 0) {
        output_add_line(output, "  OK 2MB page mapped", VGA_GREEN);
    } else {
        output_add_line(output, "  ERROR 2MB page mapping failed", VGA_RED);
    }
    output_add_empty_line(output);
    
    output_add_line(output, "[TEST 5] Identity mapping test...", VGA_YELLOW);
    result = vmm_identity_map(0x600000, PAGE_SIZE_4K, PAGE_WRITE | PAGE_PRESENT);
    if (result == 0) {
        output_add_line(output, "  OK Identity mapping created", VGA_GREEN);
        uint64_t phys = vmm_get_physical_address(0x600000);
        if (phys == 0x600000) {
            output_add_line(output, "  OK Identity verified (V==P)", VGA_GREEN);
        } else {
            output_add_line(output, "  ERROR Identity check failed", VGA_RED);
        }
    } else {
        output_add_line(output, "  ERROR Identity mapping failed", VGA_RED);
    }
    output_add_empty_line(output);
    
    output_add_line(output, "VMM Tests Complete!", VGA_GREEN);
    output_add_line(output, "", VGA_WHITE);
    output_add_line(output, "Available subcommands:", VGA_CYAN);
    output_add_line(output, "  vmm        - Run basic tests", VGA_WHITE);
    output_add_line(output, "  vmm stats  - Show statistics", VGA_WHITE);
    output_add_line(output, "  vmm demand - Test demand paging", VGA_WHITE);
}

void cmd_heap(const char* args, CommandOutput* output) {
    (void)args;
    
    output_add_line(output, "=== Heap Functionality Test ===", VGA_CYAN);
    output_add_empty_line(output);
    
    output_add_line(output, "Test 1: Basic Allocation", VGA_YELLOW);
    void* ptr1 = kmalloc(1024);
    if (ptr1) {
        output_add_line(output, "  [OK] Allocated 1KB", VGA_GREEN);
        char* test_str = (char*)ptr1;
        const char* msg = "Hello from heap!";
        int i = 0;
        while (msg[i]) { test_str[i] = msg[i]; i++; }
        test_str[i] = '\0';
        output_add_line(output, "  [OK] Wrote string to heap", VGA_GREEN);
        kfree(ptr1);
        output_add_line(output, "  [OK] Freed memory", VGA_GREEN);
    } else {
        output_add_line(output, "  [FAIL] Allocation failed", VGA_RED);
    }
    output_add_empty_line(output);
    
    output_add_line(output, "Test 2: Multiple Allocations", VGA_YELLOW);
    void* ptrs[5];
    int alloc_ok = 1;
    for (int i = 0; i < 5; i++) {
        ptrs[i] = kmalloc(256);
        if (!ptrs[i]) { alloc_ok = 0; break; }
    }
    if (alloc_ok) {
        output_add_line(output, "  [OK] Allocated 5 x 256B blocks", VGA_GREEN);
        for (int i = 0; i < 5; i++) kfree(ptrs[i]);
        output_add_line(output, "  [OK] Freed all blocks", VGA_GREEN);
    } else {
        output_add_line(output, "  [FAIL] Multiple allocation failed", VGA_RED);
    }
    output_add_empty_line(output);
    
    output_add_line(output, "Test 3: Realloc Test", VGA_YELLOW);
    void* ptr2 = kmalloc(512);
    if (ptr2) {
        output_add_line(output, "  [OK] Allocated 512B", VGA_GREEN);
        void* ptr3 = krealloc(ptr2, 2048);
        if (ptr3) {
            output_add_line(output, "  [OK] Reallocated to 2KB", VGA_GREEN);
            kfree(ptr3);
            output_add_line(output, "  [OK] Freed reallocated block", VGA_GREEN);
        } else {
            output_add_line(output, "  [FAIL] Realloc failed", VGA_RED);
            kfree(ptr2);
        }
    }
    output_add_empty_line(output);
    
    output_add_line(output, "Test 4: Calloc (Zero-init) Test", VGA_YELLOW);
    uint32_t* ptr4 = (uint32_t*)kcalloc(256, sizeof(uint32_t));
    if (ptr4) {
        output_add_line(output, "  [OK] Allocated 256 uint32s", VGA_GREEN);
        int all_zero = 1;
        for (int i = 0; i < 256; i++) {
            if (ptr4[i] != 0) { all_zero = 0; break; }
        }
        if (all_zero) {
            output_add_line(output, "  [OK] All values zeroed", VGA_GREEN);
        } else {
            output_add_line(output, "  [FAIL] Not all zeroed!", VGA_RED);
        }
        kfree(ptr4);
        output_add_line(output, "  [OK] Freed calloc block", VGA_GREEN);
    }
    output_add_empty_line(output);
    
    output_add_line(output, "Test 5: Fragmentation & Coalescing", VGA_YELLOW);
    void* frag1 = kmalloc(1024);
    void* frag2 = kmalloc(1024);
    void* frag3 = kmalloc(1024);
    if (frag1 && frag2 && frag3) {
        output_add_line(output, "  [OK] Allocated 3 x 1KB blocks", VGA_GREEN);
        kfree(frag2);
        output_add_line(output, "  [OK] Freed middle block", VGA_GREEN);
        kfree(frag1);
        output_add_line(output, "  [OK] Freed first block (coalesce)", VGA_GREEN);
        kfree(frag3);
        output_add_line(output, "  [OK] Freed last block", VGA_GREEN);
    }
    output_add_empty_line(output);
    
    output_add_line(output, "Test 6: Large Allocation (1MB)", VGA_YELLOW);
    void* large = kmalloc(1024 * 1024);
    if (large) {
        output_add_line(output, "  [OK] Allocated 1MB", VGA_GREEN);
        kfree(large);
        output_add_line(output, "  [OK] Freed 1MB", VGA_GREEN);
    } else {
        output_add_line(output, "  [WARN] 1MB allocation failed", VGA_YELLOW);
        output_add_line(output, "  (Heap may need expansion)", VGA_YELLOW);
    }
    
    output_add_empty_line(output);
    output_add_line(output, "All heap tests completed!", VGA_CYAN);
}

// ===========================================
// MULTITASKING COMMANDS
// ===========================================

void cmd_ps(const char* args, CommandOutput* output) {
    (void)args;
    
    output_add_line(output, "=== Process List ===", VGA_CYAN);
    output_add_empty_line(output);
    
    task_t* current = task_get_current();
    uint32_t task_count = task_get_count();
    
    char info[128];
    
    str_cpy(info, "Total tasks: ");
    char num[16];
    int_to_str(task_count + 1, num);
    str_concat(info, num);
    output_add_line(output, info, VGA_WHITE);
    output_add_empty_line(output);
    
    if (current) {
        str_cpy(info, "[*] PID ");
        int_to_str(current->pid, num);
        str_concat(info, num);
        str_concat(info, " - ");
        str_concat(info, current->name);
        str_concat(info, " (RUNNING)");
        output_add_line(output, info, VGA_GREEN);
    }
    
    output_add_empty_line(output);
    output_add_line(output, "Use 'taskinfo <pid>' for details", VGA_YELLOW);
}

void cmd_taskinfo(const char* args, CommandOutput* output) {
    if (!args || str_len(args) == 0) {
        output_add_line(output, "Usage: taskinfo <pid>", VGA_YELLOW);
        return;
    }
    
    int pid = 0;
    int i = 0;
    while (args[i] >= '0' && args[i] <= '9') {
        pid = pid * 10 + (args[i] - '0');
        i++;
    }
    
    task_t* task = task_find_by_pid(pid);
    if (!task) {
        output_add_line(output, "Task not found", VGA_RED);
        return;
    }
    
    output_add_line(output, "=== Task Information ===", VGA_CYAN);
    output_add_empty_line(output);
    
    char info[128];
    char num[32];
    
    str_cpy(info, "Name: ");
    str_concat(info, task->name);
    output_add_line(output, info, VGA_WHITE);
    
    str_cpy(info, "PID: ");
    int_to_str(task->pid, num);
    str_concat(info, num);
    output_add_line(output, info, VGA_WHITE);
    
    str_cpy(info, "State: ");
    switch (task->state) {
        case TASK_STATE_READY:      str_concat(info, "READY"); break;
        case TASK_STATE_RUNNING:    str_concat(info, "RUNNING"); break;
        case TASK_STATE_BLOCKED:    str_concat(info, "BLOCKED"); break;
        case TASK_STATE_TERMINATED: str_concat(info, "TERMINATED"); break;
        default:                    str_concat(info, "UNKNOWN"); break;
    }
    output_add_line(output, info, VGA_WHITE);
    
    str_cpy(info, "Priority: ");
    int_to_str(task->priority, num);
    str_concat(info, num);
    output_add_line(output, info, VGA_WHITE);
    
    str_cpy(info, "Context switches: ");
    uint64_to_string(task->context_switches, num);
    str_concat(info, num);
    output_add_line(output, info, VGA_WHITE);
}

void cmd_createtask(const char* args, CommandOutput* output) {
    (void)args;
    
    output_add_line(output, "Creating test tasks...", VGA_CYAN);
    
    task_t* task_a = task_create("TestA", test_task_a, 10);
    if (!task_a) {
        output_add_line(output, "Failed to create task A - task system may not be initialized", VGA_RED);
        return;
    }
    
    task_t* task_b = task_create("TestB", test_task_b, 10);
    if (!task_b) {
        output_add_line(output, "Failed to create task B - task system may not be initialized", VGA_RED);
        if (task_a) task_terminate(task_a);
        return;
    }
    
    if (task_start(task_a) != 0) {
        output_add_line(output, "Failed to start task A", VGA_RED);
        task_terminate(task_a);
        task_terminate(task_b);
        return;
    }
    
    if (task_start(task_b) != 0) {
        output_add_line(output, "Failed to start task B", VGA_RED);
        task_terminate(task_a);
        task_terminate(task_b);
        return;
    }
    
    output_add_line(output, "Created and started 2 test tasks", VGA_GREEN);
    output_add_line(output, "  - TestA (PID varies)", VGA_WHITE);
    output_add_line(output, "  - TestB (PID varies)", VGA_WHITE);
    output_add_line(output, "Check serial output for task messages", VGA_YELLOW);
}

void cmd_usertask(const char* args, CommandOutput* output) {
    output_add_line(output, "=== Ring-3 User Task Olusturuluyor ===", VGA_CYAN);
    output_add_empty_line(output);

    const char* task_name = "UserTest";
    void (*entry)(void)   = user_mode_test_task;

    if (args && str_len(args) > 0
        && str_cmp(args, "test")  != 0
        && str_cmp(args, "ring3") != 0) {
        task_name = args;
    }

    char info[128];
    char num[32];

    str_cpy(info, "Task adi : ");
    str_concat(info, task_name);
    output_add_line(output, info, VGA_WHITE);
    output_add_line(output, "Privilege: Ring-3 (DPL=3)", VGA_WHITE);
    output_add_line(output, "CS=0x23  SS=0x1B  Entry=user_mode_test_task", VGA_WHITE);
    output_add_empty_line(output);

    task_t* utask = task_create_user(task_name, entry, TASK_PRIORITY_NORMAL);
    if (!utask) {
        output_add_line(output, "[HATA] task_create_user() basarisiz!", VGA_RED);
        output_add_line(output, "  -> task_init() cagirildi mi?", VGA_YELLOW);
        return;
    }

    str_cpy(info, "Olusturuldu -> PID=");
    int_to_str(utask->pid, num);
    str_concat(info, num);
    output_add_line(output, info, VGA_GREEN);

    str_cpy(info, "  kernel_stack_top = 0x");
    uint64_to_string(utask->kernel_stack_top, num);
    str_concat(info, num);
    output_add_line(output, info, VGA_WHITE);

    str_cpy(info, "  user_stack_top   = 0x");
    uint64_to_string(utask->user_stack_top, num);
    str_concat(info, num);
    output_add_line(output, info, VGA_WHITE);

    if (task_start(utask) != 0) {
        output_add_line(output, "[HATA] task_start() basarisiz!", VGA_RED);
        task_terminate(utask);
        return;
    }

    output_add_empty_line(output);
    output_add_line(output, "Zamanlayici kuyruguna eklendi.", VGA_GREEN);
    output_add_line(output, "Sonraki timer interrupt -> IRETQ -> Ring-3 gecisi.", VGA_YELLOW);
    output_add_line(output, "Serial logda '[USER TASK] Hello from Ring-3' gormeli.", VGA_YELLOW);
    output_add_line(output, "Gorev SYS_EXIT(0) ile kendini sonlandiriyor.", VGA_WHITE);
}

void cmd_schedinfo(const char* args, CommandOutput* output) {
    (void)args;
    
    output_add_line(output, "=== Scheduler Information ===", VGA_CYAN);
    output_add_empty_line(output);
    
    char info[128];
    char num[32];
    
    str_cpy(info, "Total context switches: ");
    uint64_to_string(scheduler_get_context_switches(), num);
    str_concat(info, num);
    output_add_line(output, info, VGA_WHITE);
    
    str_cpy(info, "Total ticks: ");
    uint64_to_string(get_system_ticks(), num);
    str_concat(info, num);
    output_add_line(output, info, VGA_WHITE);
    
    str_cpy(info, "Ready queue size: ");
    int_to_str(task_get_count(), num);
    str_concat(info, num);
    output_add_line(output, info, VGA_WHITE);
    
    output_add_empty_line(output);
    output_add_line(output, "Multitasking is active!", VGA_GREEN);
}

void cmd_offihito(const char* args, CommandOutput* output) {
    (void)args;
    
    output_add_line(output, "Creating Offihito task...", VGA_CYAN);
    
    task_t* offihito = task_create("Offihito", offihito_task, 10);
    
    if (!offihito) {
        output_add_line(output, "Failed to create Offihito task", VGA_RED);
        output_add_line(output, "Task system may not be initialized", VGA_YELLOW);
        return;
    }
    
    if (task_start(offihito) != 0) {
        output_add_line(output, "Failed to start Offihito task", VGA_RED);
        task_terminate(offihito);
        return;
    }
    
    output_add_line(output, "Offihito task created and started!", VGA_GREEN);
    output_add_line(output, "It will print 'Offihito' every 2 seconds", VGA_YELLOW);
    output_add_line(output, "Check both VGA screen and serial output", VGA_YELLOW);
}

// ===========================================
// ELF LOADER COMMANDS
// ===========================================

void cmd_elfinfo(const char* args, CommandOutput* output) {
    if (!args || str_len(args) == 0) {
        output_add_line(output, "Usage: elfinfo <FILE.ELF>", VGA_YELLOW);
        output_add_line(output, "  Shows ELF header info without loading.", VGA_DARK_GRAY);
        output_add_line(output, "  Tam yol veya sadece isim verilebilir.", VGA_DARK_GRAY);
        output_add_line(output, "  Ornek: elfinfo hello.elf", VGA_DARK_GRAY);
        output_add_line(output, "  Ornek: elfinfo /bin/hello.elf", VGA_DARK_GRAY);
        return;
    }

    // Tam yol oluştur: eğer '/' ile başlamıyorsa /bin/ ekle
    char path[128];
    if (args[0] == '/') {
        str_cpy(path, args);
    } else {
        str_cpy(path, "/bin/");
        str_concat(path, args);
    }

    uint32_t fsize = ext2_file_size(path);
    if (fsize == 0) {
        char line[96];
        str_cpy(line, "File not found on ext2: ");
        str_concat(line, path);
        output_add_line(output, line, VGA_RED);
        return;
    }

    static uint8_t hdr_buf[512];
    int n = ext2_read_file(path, hdr_buf, 512);
    if (n < 64) {
        output_add_line(output, "Read failed or file too small for ELF header", VGA_RED);
        return;
    }

    char line[96];
    char tmp[24];
    uint64_to_string(fsize, tmp);
    str_cpy(line, "File: "); str_concat(line, path);
    str_concat(line, "  Size: "); str_concat(line, tmp); str_concat(line, " bytes");
    output_add_line(output, line, VGA_CYAN);

    elf64_dump_header(hdr_buf, output);

    int rc = elf64_validate(hdr_buf, (uint32_t)n);
    str_cpy(line, "Validation: ");
    str_concat(line, elf64_strerror(rc));
    output_add_line(output, line, rc == ELF_OK ? VGA_GREEN : VGA_RED);
}

void cmd_exec(const char* args, CommandOutput* output) {
    if (!args || str_len(args) == 0) {
        output_add_line(output, "Usage: exec <FILE.ELF> [base_hex]", VGA_YELLOW);
        output_add_line(output, "  ELF64 binary'yi ext2'den yukler, Ring-3 task olusturur.", VGA_DARK_GRAY);
        output_add_line(output, "  base_hex: PIE (ET_DYN) icin opsiyonel load tabanı.", VGA_DARK_GRAY);
        output_add_line(output, "  Ornek: exec hello.elf", VGA_DARK_GRAY);
        output_add_line(output, "  Ornek: exec /bin/hello.elf 0x500000", VGA_DARK_GRAY);
        return;
    }

    char filename[64];
    uint64_t load_base = 0x400000ULL;

    int i = 0;
    while (args[i] && args[i] != ' ' && i < 63) {
        filename[i] = args[i];
        i++;
    }
    filename[i] = '\0';

    if (args[i] == ' ') {
        i++;
        const char* base_str = &args[i];
        if (base_str[0] == '0' && (base_str[1] == 'x' || base_str[1] == 'X')) {
            base_str += 2;
            uint64_t parsed = 0;
            while (*base_str) {
                char c = *base_str++;
                uint64_t digit;
                if      (c >= '0' && c <= '9') digit = c - '0';
                else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
                else break;
                parsed = (parsed << 4) | digit;
            }
            if (parsed != 0) load_base = parsed;
        }
    }

    char line[96];
    char tmp[24];
    const char* hexc = "0123456789ABCDEF";

    #define FMT_HEX64(val) do { \
        tmp[0]='0'; tmp[1]='x'; \
        for(int _k=0;_k<16;_k++) tmp[2+_k]=hexc[((val)>>(60-_k*4))&0xF]; \
        tmp[18]='\0'; \
    } while(0)

    output_add_line(output, "=== exec: ELF Loader + Ring-3 Task ===", VGA_CYAN);

    // Tam yol oluştur: '/' ile başlamıyorsa /bin/ ekle
    char filepath[128];
    if (filename[0] == '/') {
        str_cpy(filepath, filename);
    } else {
        str_cpy(filepath, "/bin/");
        str_concat(filepath, filename);
    }

    str_cpy(line, "Dosya     : "); str_concat(line, filepath);
    output_add_line(output, line, VGA_WHITE);
    FMT_HEX64(load_base);
    str_cpy(line, "Load base : "); str_concat(line, tmp);
    output_add_line(output, line, VGA_WHITE);
    output_add_empty_line(output);

    if (!syscall_is_enabled()) {
        output_add_line(output, "[HATA] SYSCALL altyapisi baslatilmamis!", VGA_RED);
        output_add_line(output, "  kernel'de syscall_init() cagirildi mi?", VGA_YELLOW);
        return;
    }

    output_add_line(output, "[1/3] ELF ext2'den yukleniyor...", VGA_WHITE);
    ElfImage image;
    int rc = elf64_exec_from_ext2(filepath, load_base, &image, output);
    if (rc != ELF_OK) {
        str_cpy(line, "[HATA] ELF yuklenemedi: ");
        str_concat(line, elf64_strerror(rc));
        output_add_line(output, line, VGA_RED);
        return;
    }

    output_add_line(output, "[2/3] Ring-3 task olusturuluyor...", VGA_WHITE);

    static char argv_storage[8][128];
    const char* argv_ptrs[8];
    int exec_argc = 0;

    {
        int fn = 0;
        while (filepath[fn] && fn < 127) { argv_storage[0][fn] = filepath[fn]; fn++; }
        argv_storage[0][fn] = '\0';
        argv_ptrs[exec_argc++] = argv_storage[0];
    }

    {
        int skip = 0;
        while (args[skip] && args[skip] != ' ') skip++;
        if (args[skip] == ' ') {
            skip++;
            if (args[skip] == '0' && (args[skip+1] == 'x' || args[skip+1] == 'X')) {
                while (args[skip] && args[skip] != ' ') skip++;
                if (args[skip] == ' ') skip++;
            }
        }
        const char* rest = &args[skip];
        while (*rest && exec_argc < 7) {
            int ai = 0;
            while (*rest && *rest != ' ' && ai < 127) {
                argv_storage[exec_argc][ai++] = *rest++;
            }
            argv_storage[exec_argc][ai] = '\0';
            if (ai > 0) { argv_ptrs[exec_argc] = argv_storage[exec_argc]; exec_argc++; }
            while (*rest == ' ') rest++;
        }
        for (int ai = 1; ai < exec_argc; ai++)
            argv_ptrs[ai] = argv_storage[ai];
    }

    {
        char arginfo[64];
        str_cpy(arginfo, "  argc=");
        char tmp2[8]; int_to_str(exec_argc, tmp2);
        str_concat(arginfo, tmp2);
        output_add_line(output, arginfo, VGA_DARK_GRAY);
        for (int ai = 0; ai < exec_argc; ai++) {
            char aline[80];
            str_cpy(aline, "  argv["); char idx[4]; int_to_str(ai, idx);
            str_concat(aline, idx); str_concat(aline, "]=");
            str_concat(aline, argv_ptrs[ai]);
            output_add_line(output, aline, VGA_DARK_GRAY);
        }
    }

    task_t* utask = task_create_from_elf(filepath, &image, TASK_PRIORITY_NORMAL,
                                          exec_argc, argv_ptrs);
    if (!utask) {
        output_add_line(output, "[HATA] task_create_from_elf() basarisiz!", VGA_RED);
        output_add_line(output, "  task_init() cagirildi mi? Heap yeterli mi?", VGA_YELLOW);
        return;
    }

    FMT_HEX64(image.entry);
    str_cpy(line, "  Entry point     : "); str_concat(line, tmp);
    output_add_line(output, line, VGA_YELLOW);

    FMT_HEX64(image.load_min);
    str_cpy(line, "  Segment VA min  : "); str_concat(line, tmp);
    output_add_line(output, line, VGA_WHITE);

    FMT_HEX64(image.load_max);
    str_cpy(line, "  Segment VA max  : "); str_concat(line, tmp);
    output_add_line(output, line, VGA_WHITE);

    str_cpy(line, "  PID             : ");
    int_to_str((int)utask->pid, tmp);
    str_concat(line, tmp);
    output_add_line(output, line, VGA_WHITE);

    FMT_HEX64(utask->kernel_stack_top);
    str_cpy(line, "  Kernel RSP0     : "); str_concat(line, tmp);
    output_add_line(output, line, VGA_WHITE);

    FMT_HEX64(utask->user_stack_top);
    str_cpy(line, "  User stack top  : "); str_concat(line, tmp);
    output_add_line(output, line, VGA_WHITE);

    output_add_line(output, "[3/3] Zamanlayici kuyruguna ekleniyor...", VGA_WHITE);
    if (task_start(utask) != 0) {
        output_add_line(output, "[HATA] task_start() basarisiz!", VGA_RED);
        task_terminate(utask);
        return;
    }

    extern void kb_set_userland_mode(int on);
    extern void kb_set_enter_cr(int cr);
    kb_set_userland_mode(1);

    // kilo raw-mode: Enter → '\r' bekler.
    // Diğer tüm uygulamalar (shell, lua, calculator, vb.) → '\n' bekler.
    // filename'in sonundaki binary adına bakarak karar ver.
    {
        // filename zaten sadece binary adı (path ayrıştırılmış yukarıda)
        // yine de güvenli olsun: son '/' sonrasını bul
        const char* p = filename;
        const char* last = filename;
        while (*p) { if (*p == '/') last = p + 1; p++; }
        int cr_mode = (last[0]=='k' && last[1]=='i' && last[2]=='l' &&
                       last[3]=='o' &&
                       (last[4]=='\0' || last[4]=='.' || last[4]==' ' || last[4]=='-')) ? 1 : 0;
        kb_set_enter_cr(cr_mode);
    }

    extern volatile uint32_t foreground_pid;
    foreground_pid = utask->pid;

    output_add_empty_line(output);
    output_add_line(output, "================================================", VGA_GREEN);
    str_cpy(line, "  Task '"); str_concat(line, filename);
    str_concat(line, "' Ring-3'te basladi!");
    output_add_line(output, line, VGA_GREEN);
    output_add_line(output, "================================================", VGA_GREEN);
    output_add_empty_line(output);
    output_add_line(output, "Sonraki timer tick -> iretq -> Ring-3 (CS=0x23)", VGA_YELLOW);
    output_add_line(output, "Program syscall yaptiginda:", VGA_WHITE);
    output_add_line(output, "  Ring-3 syscall -> kernel_tss.rsp0 -> Ring-0", VGA_DARK_GRAY);
    output_add_line(output, "  syscall_dispatch() -> handler -> SYSRET", VGA_DARK_GRAY);
    output_add_line(output, "  SYSRET -> Ring-3 (program devam eder)", VGA_DARK_GRAY);
    output_add_line(output, "  SYS_EXIT(0) -> task_exit() -> TERMINATED", VGA_DARK_GRAY);
    output_add_empty_line(output);
    output_add_line(output, "Serial logda programin ciktisini izleyin.", VGA_CYAN);

    #undef FMT_HEX64
}

// ===========================================
// ADVANCED FILE SYSTEM COMMANDS
// ===========================================

// Yardımcı: ext2 üzerinde rekürsif tree
static void ext2_tree_recursive(const char* path, int depth, CommandOutput* output) {
    static dirent64_t dents[128];
    int total = ext2_getdents(path, dents, (int)sizeof(dents));
    if (total < 0) return;

    int off = 0;
    while (off < total) {
        dirent64_t* de = (dirent64_t*)((char*)dents + off);
        if (de->d_reclen == 0) break;

        if (!(de->d_name[0] == '.' &&
              (de->d_name[1] == '\0' ||
              (de->d_name[1] == '.' && de->d_name[2] == '\0')))) {
            char line[MAX_LINE_LENGTH];
            // Girinti
            for (int d = 0; d < depth && d < 8; d++) str_concat(line, "  ");
            line[depth * 2 < MAX_LINE_LENGTH ? depth * 2 : MAX_LINE_LENGTH - 1] = '\0';
            // Sıfırlama gerek: line init edilmedi
            int dsp = depth * 2; if (dsp >= MAX_LINE_LENGTH) dsp = MAX_LINE_LENGTH - 1;
            for (int k = 0; k < dsp; k++) line[k] = ' ';
            line[dsp] = '\0';

            if (de->d_type == DT_DIR) {
                str_concat(line, "[+] ");
                str_concat(line, de->d_name);
                output_add_line(output, line, VGA_YELLOW);
                // Rekürsif
                char subpath[256];
                str_cpy(subpath, path);
                int plen = str_len(subpath);
                if (plen > 1) { subpath[plen] = '/'; subpath[plen+1] = '\0'; }
                str_concat(subpath, de->d_name);
                if (depth < 4) ext2_tree_recursive(subpath, depth + 1, output);
            } else {
                str_concat(line, "    ");
                str_concat(line, de->d_name);
                output_add_line(output, line, VGA_WHITE);
            }
        }
        off += de->d_reclen;
    }
}

void cmd_tree(const char* args, CommandOutput* output) {
    (void)args;
    const char* root = "/";
    output_add_line(output, "/", VGA_CYAN);
    ext2_tree_recursive(root, 1, output);
}

void cmd_find(const char* args, CommandOutput* output) {
    if (!args || str_len(args) == 0) {
        output_add_line(output, "Usage: find <pattern>", VGA_YELLOW);
        output_add_line(output, "Example: find txt", VGA_DARK_GRAY);
        return;
    }
    // ext2 üzerinde rekürsif arama
    static dirent64_t find_dents[128];
    // Basit: sadece /bin, /usr, /etc, /home, /tmp içinde ara
    const char* search_dirs[] = {"/", "/bin", "/usr", "/etc", "/home", "/tmp", NULL};
    int found = 0;
    for (int di = 0; search_dirs[di]; di++) {
        int tot = ext2_getdents(search_dirs[di], find_dents, (int)sizeof(find_dents));
        if (tot < 0) continue;
        int off = 0;
        while (off < tot) {
            dirent64_t* de = (dirent64_t*)((char*)find_dents + off);
            if (de->d_reclen == 0) break;
            // args pattern içeriyor mu?
            const char* nm = de->d_name;
            const char* pat = args;
            int match = 0;
            // basit substring arama
            int nlen = str_len(nm), plen = str_len(pat);
            for (int si = 0; si <= nlen - plen && !match; si++) {
                match = 1;
                for (int pi = 0; pi < plen && match; pi++)
                    if (nm[si+pi] != pat[pi]) match = 0;
            }
            if (match && de->d_name[0] != '.') {
                char line[MAX_LINE_LENGTH];
                str_cpy(line, search_dirs[di]);
                if (str_len(search_dirs[di]) > 1) str_concat(line, "/");
                str_concat(line, de->d_name);
                output_add_line(output, line, VGA_GREEN);
                found++;
            }
            off += de->d_reclen;
        }
    }
    if (!found) output_add_line(output, "No matches found", VGA_DARK_GRAY);
}

void cmd_du(const char* args, CommandOutput* output) {
    const char* path = (args && str_len(args) > 0) ? args : NULL;
    const char* du_path = (args && str_len(args) > 0) ? args : ext2_getcwd();
    static dirent64_t du_dents[256];
    int tot = ext2_getdents(du_path, du_dents, (int)sizeof(du_dents));
    if (tot < 0) { output_add_line(output, "Cannot read directory", VGA_RED); return; }

    uint32_t total_bytes = 0;
    int off = 0;
    while (off < tot) {
        dirent64_t* de = (dirent64_t*)((char*)du_dents + off);
        if (de->d_reclen == 0) break;
        if (de->d_type == DT_REG && de->d_name[0] != '.') {
            char fp[256]; str_cpy(fp, du_path);
            int pl = str_len(fp); if(pl>1){fp[pl]='/';fp[pl+1]='\0';}
            str_concat(fp, de->d_name);
            uint32_t fsz = ext2_file_size(fp);
            total_bytes += fsz;
            char line[MAX_LINE_LENGTH];
            str_cpy(line, "  "); str_concat(line, de->d_name);
            str_concat(line, "\t");
            char szb[16]; uint32_t v=fsz; int i=0;
            if(!v){szb[i++]='0';}else{char t[12];int n=0;while(v){t[n++]='0'+(v%10);v/=10;}for(int j=n-1;j>=0;j--)szb[i++]=t[j];}szb[i]='\0';
            str_concat(line, szb); str_concat(line, " B");
            output_add_line(output, line, VGA_WHITE);
        }
        off += de->d_reclen;
    }
    char tot_line[MAX_LINE_LENGTH]; str_cpy(tot_line, "Total: ");
    char tb[16]; uint32_t v=total_bytes; int i=0;
    if(!v){tb[i++]='0';}else{char t[12];int n=0;while(v){t[n++]='0'+(v%10);v/=10;}for(int j=n-1;j>=0;j--)tb[i++]=t[j];}tb[i]='\0';
    str_concat(tot_line, tb); str_concat(tot_line, " B");
    output_add_line(output, tot_line, VGA_CYAN);
}

void cmd_rmr(const char* args, CommandOutput* output) {
    if (!args || str_len(args) == 0) {
        output_add_line(output, "Usage: rmr <directory>", VGA_YELLOW);
        output_add_line(output, "WARNING: Recursively removes directory and all contents!", VGA_RED);
        return;
    }
    char rmr_path[256];
    if (args[0] == '/') { str_cpy(rmr_path, args); }
    else { str_cpy(rmr_path, ext2_getcwd()); int pl=str_len(rmr_path); if(pl>1){rmr_path[pl]='/';rmr_path[pl+1]=' ';} str_concat(rmr_path, args); }
    // ext2_rmdir sadece boş dizini siler; recursive için önce içini temizle
    static dirent64_t rmr_dents[128];
    int tot = ext2_getdents(rmr_path, rmr_dents, (int)sizeof(rmr_dents));
    if (tot > 0) {
        int off = 0;
        while (off < tot) {
            dirent64_t* de = (dirent64_t*)((char*)rmr_dents + off);
            if (de->d_reclen == 0) break;
            if (de->d_name[0] != '.') {
                char child[256]; str_cpy(child, rmr_path);
                int pl=str_len(child); if(pl>1){child[pl]='/';child[pl+1]=' ';}
                str_concat(child, de->d_name);
                if (de->d_type == DT_REG) ext2_unlink(child);
                else if (de->d_type == DT_DIR) ext2_rmdir(child);
            }
            off += de->d_reclen;
        }
    }
    int rmr_rc = ext2_rmdir(rmr_path);
    if (rmr_rc == 0) {
        output_add_line(output, "Directory removed", VGA_GREEN);
    } else {
        output_add_line(output, "Failed: not empty or not found", VGA_RED);
    }
}

// ===========================================
// SYSCALL COMMANDS — syscalltest64.c icinde tanimli
// ===========================================
void cmd_syscalltest(const char* args, CommandOutput* output);

// ============================================================================
// SPINLOCK TEST KOMUTU
// ============================================================================
static void cmd_spinlock(const char* args, CommandOutput* output) {
    (void)args;
    output_add_line(output, "=== Spinlock / RWLock Testi ===", VGA_CYAN);
    output_add_empty_line(output);
    output_add_line(output, "Testler calistiriliyor...", VGA_WHITE);
    output_add_empty_line(output);
    spinlock_test();
    // spinlock_test() dogrudan println64 ile ekrana yaziyor
    output_add_empty_line(output);
    output_add_line(output, "Detayli log: serial porta yazildi (COM1).", VGA_DARK_GRAY);
}


// Kullanım: perf          → tüm testleri çalıştır
//           perf memcpy   → memcpy hız testi
//           perf memset   → memset hız testi
//           perf loop     → basit döngü testi
// ============================================================================
static void cmd_perf(const char* args, CommandOutput* output) {
    (void)args;

    output_add_line(output, "=== RDTSC Performans Olcumu ===", VGA_CYAN);
    output_add_empty_line(output);
    output_add_line(output, "Sonuclar ayni zamanda serial porta yaziliyor.", VGA_DARK_GRAY);
    output_add_empty_line(output);

    PerfCounter pc;
    char line[MAX_LINE_LENGTH];
    char tmp[24];

    // ── Test 1: Basit döngü (1M iterasyon) ───────────────────────────────
    output_add_line(output, "[1] Bos dongu  x1.000.000:", VGA_YELLOW);
    perf_start(&pc);
    for (volatile int i = 0; i < 1000000; i++);
    perf_stop(&pc);
    perf_print(&pc, "empty_loop_1M");

    str_cpy(line, "    Cycles : "); uint64_to_string(perf_cycles(&pc), tmp); str_concat(line, tmp);
    output_add_line(output, line, VGA_WHITE);
    str_cpy(line, "    Sure   : ~"); uint64_to_string(perf_us(&pc), tmp); str_concat(line, tmp);
    str_concat(line, " us  (~"); uint64_to_string(perf_ns(&pc) / 1000000, tmp); str_concat(line, tmp);
    str_concat(line, " ms)");
    output_add_line(output, line, VGA_GREEN);
    output_add_empty_line(output);

    // ── Test 2: memset — 64KB sıfırlama ──────────────────────────────────
    output_add_line(output, "[2] memset64  64KB:", VGA_YELLOW);
    static uint8_t perf_buf[65536];
    perf_start(&pc);
    extern void* memset64(void*, int, size_t);
    memset64(perf_buf, 0xAB, sizeof(perf_buf));
    perf_stop(&pc);
    perf_print(&pc, "memset64_64KB");

    str_cpy(line, "    Cycles : "); uint64_to_string(perf_cycles(&pc), tmp); str_concat(line, tmp);
    output_add_line(output, line, VGA_WHITE);
    str_cpy(line, "    Sure   : ~"); uint64_to_string(perf_us(&pc), tmp); str_concat(line, tmp);
    str_concat(line, " us");
    output_add_line(output, line, VGA_GREEN);

    // Bant genişliği: 64KB / us → MB/s
    uint32_t us = perf_us(&pc);
    if (us > 0) {
        uint64_t mbps = 65536ULL / (uint64_t)us;
        str_cpy(line, "    Bant   : ~"); uint64_to_string(mbps, tmp); str_concat(line, tmp);
        str_concat(line, " MB/s");
        output_add_line(output, line, VGA_CYAN);
    }
    output_add_empty_line(output);

    // ── Test 3: memcpy — 64KB kopyalama ──────────────────────────────────
    output_add_line(output, "[3] memcpy64  64KB:", VGA_YELLOW);
    static uint8_t perf_src[65536];
    extern void* memcpy64(void*, const void*, size_t);
    perf_start(&pc);
    memcpy64(perf_buf, perf_src, sizeof(perf_buf));
    perf_stop(&pc);
    perf_print(&pc, "memcpy64_64KB");

    str_cpy(line, "    Cycles : "); uint64_to_string(perf_cycles(&pc), tmp); str_concat(line, tmp);
    output_add_line(output, line, VGA_WHITE);
    str_cpy(line, "    Sure   : ~"); uint64_to_string(perf_us(&pc), tmp); str_concat(line, tmp);
    str_concat(line, " us");
    output_add_line(output, line, VGA_GREEN);

    us = perf_us(&pc);
    if (us > 0) {
        uint64_t mbps = 65536ULL / (uint64_t)us;
        str_cpy(line, "    Bant   : ~"); uint64_to_string(mbps, tmp); str_concat(line, tmp);
        str_concat(line, " MB/s");
        output_add_line(output, line, VGA_CYAN);
    }
    output_add_empty_line(output);

    // ── Test 4: RDTSC overhead (kendini ölçüyor) ─────────────────────────
    output_add_line(output, "[4] RDTSC overhead:", VGA_YELLOW);
    perf_start(&pc);
    perf_stop(&pc);
    perf_print(&pc, "rdtsc_overhead");

    str_cpy(line, "    Cycles : "); uint64_to_string(perf_cycles(&pc), tmp); str_concat(line, tmp);
    str_concat(line, "  (rdtscp cift okuma maliyeti)");
    output_add_line(output, line, VGA_WHITE);
    output_add_empty_line(output);

    // ── CPU frekansı ──────────────────────────────────────────────────────
    str_cpy(line, "CPU Frekans: ");
    uint64_to_string((uint64_t)pc.cpu_mhz, tmp); str_concat(line, tmp);
    str_concat(line, " MHz  (PIT olcumu)");
    output_add_line(output, line, VGA_CYAN);
    output_add_line(output, "Detayli log: serial porta yazildi (COM1).", VGA_DARK_GRAY);
}


static void panic_do_div0(void) {
    volatile int a = 42;
    volatile int b = 0;
    volatile int c = a / b;
    (void)c;
}

static void panic_do_stack_overflow(int depth) {
    volatile char buf[512];
    buf[0] = (char)depth;
    (void)buf;
    panic_do_stack_overflow(depth + 1);
}

static void cmd_panic(const char* args, CommandOutput* output) {
    if (!args || str_len(args) == 0) {
        output_add_line(output, "Kernel Panic Test - Mevcut exception tipleri:", VGA_CYAN);
        output_add_empty_line(output);
        output_add_line(output, "  panic df    - #DF Double Fault  (int 0x08)", VGA_RED);
        output_add_line(output, "  panic gp    - #GP Gen Protection (int 0x0D)", VGA_RED);
        output_add_line(output, "  panic pf    - #PF Page Fault (NULL deref)", VGA_YELLOW);
        output_add_line(output, "  panic ud    - #UD Invalid Opcode (ud2)", VGA_YELLOW);
        output_add_line(output, "  panic de    - #DE Divide by Zero", VGA_YELLOW);
        output_add_line(output, "  panic stack - Stack overflow (#SS / #DF)", VGA_YELLOW);
        output_add_empty_line(output);
        output_add_line(output, "UYARI: Sistem DONDURULUR. QEMU/serial logdan dump al.", VGA_RED);
        return;
    }

    if (str_cmp(args, "df") == 0) {
        output_add_line(output, "[PANIC TEST] #DF Double Fault tetikleniyor...", VGA_RED);
        output_add_line(output, "  int 0x08 -> exception_frame -> kernel_panic_handler", VGA_DARK_GRAY);
        for (volatile int i = 0; i < 5000000; i++);
        __asm__ volatile("int $0x08");

    } else if (str_cmp(args, "gp") == 0) {
        output_add_line(output, "[PANIC TEST] #GP General Protection Fault tetikleniyor...", VGA_RED);
        output_add_line(output, "  int 0x0D -> err_code -> kernel_panic_handler", VGA_DARK_GRAY);
        for (volatile int i = 0; i < 5000000; i++);
        __asm__ volatile("int $0x0D");

    } else if (str_cmp(args, "pf") == 0) {
        output_add_line(output, "[PANIC TEST] #PF Page Fault tetikleniyor...", VGA_RED);
        output_add_line(output, "  NULL pointer deref -> CR2=0x0 -> kernel_panic_handler", VGA_DARK_GRAY);
        for (volatile int i = 0; i < 5000000; i++);
        volatile uint64_t* null_ptr = (volatile uint64_t*)0x0;
        volatile uint64_t val = *null_ptr;
        (void)val;

    } else if (str_cmp(args, "ud") == 0) {
        output_add_line(output, "[PANIC TEST] #UD Invalid Opcode tetikleniyor...", VGA_RED);
        output_add_line(output, "  ud2 instruction -> kernel_panic_handler", VGA_DARK_GRAY);
        for (volatile int i = 0; i < 5000000; i++);
        __asm__ volatile("ud2");

    } else if (str_cmp(args, "de") == 0) {
        output_add_line(output, "[PANIC TEST] #DE Divide by Zero tetikleniyor...", VGA_RED);
        output_add_line(output, "  idiv 0 -> kernel_panic_handler", VGA_DARK_GRAY);
        for (volatile int i = 0; i < 5000000; i++);
        panic_do_div0();

    } else if (str_cmp(args, "stack") == 0) {
        output_add_line(output, "[PANIC TEST] Stack overflow tetikleniyor...", VGA_RED);
        output_add_line(output, "  Sonsuz rekursiyon -> #SS veya #DF", VGA_DARK_GRAY);
        for (volatile int i = 0; i < 5000000; i++);
        panic_do_stack_overflow(0);

    } else {
        output_add_line(output, "Bilinmeyen panic tipi. 'panic' yazarak listeleri gor.", VGA_YELLOW);
    }
}

// ============================================================================
// AĞ KOMUTLARI — Yardımcı fonksiyonlar
// ============================================================================

// Hex bayt yardımcısı (serial değil, ekrana)
static void byte_to_hex_str(uint8_t v, char* out) {
    const char* h = "0123456789ABCDEF";
    out[0] = h[(v >> 4) & 0xF];
    out[1] = h[v & 0xF];
    out[2] = '\0';
}

static void uint16_to_hex_str(uint16_t v, char* out) {
    const char* h = "0123456789ABCDEF";
    out[0] = '0'; out[1] = 'x';
    out[2] = h[(v >> 12) & 0xF];
    out[3] = h[(v >>  8) & 0xF];
    out[4] = h[(v >>  4) & 0xF];
    out[5] = h[v & 0xF];
    out[6] = '\0';
}

// ============================================================================
// AĞ KOMUTLARI — RTL8139
// ============================================================================

// ── netinit ──────────────────────────────────────────────────────────────────
static void cmd_netinit(const char* args, CommandOutput* output) {
    (void)args;
    if (g_net_initialized) {
        output_add_line(output, "Ag surucusu zaten baslatildi.", 0x0E);
        if (!ipv4_is_initialized()) {
            ipv4_init();
            icmp_init();
            output_add_line(output, "  [OK] IPv4 + ICMP katmanlari baslatildi.", 0x0A);
        }
        if (!udp_is_initialized()) {
            udp_init(1);
            output_add_line(output, "  [OK] UDP katmani baslatildi.", 0x0A);
        }
        if (!dhcp_is_initialized()) {
            dhcp_init();
            output_add_line(output, "  [OK] DHCP katmani baslatildi.", 0x0A);
        }
        if (!tcp_is_initialized()) {
            tcp_init();
            output_add_line(output, "  [OK] TCP katmani baslatildi.", 0x0A);
        }
        return;
    }
    output_add_line(output, "RTL8139 baslatiliyor...", 0x07);
    bool ok = rtl8139_init();
    if (ok) {
        net_register_packet_handler();
        ipv4_init();
        icmp_init();
        udp_init(1);   // 1 = UDP_CSUM_ENABLE
        dhcp_init();   // UDP port 68 dinlemeye alir
        tcp_init();    // IPv4'e proto=6 kaydet
        output_add_line(output, "  [OK] RTL8139 hazir!", 0x0A);
        output_add_line(output, "  [OK] IPv4 + ICMP + UDP + DHCP + TCP hazir.", 0x0A);
        output_add_line(output, "  'dhcp' komutu ile otomatik IP alabilirsin.", 0x07);
        output_add_line(output, "  Ya da 'ipconfig 10.0.2.15' ile elle ata.", 0x07);
    } else {
        output_add_line(output, "  [HATA] RTL8139 bulunamadi veya baslatilamadi.", 0x0C);
        output_add_line(output, "  QEMU'ya '-device rtl8139,netdev=net0' ekli mi?", 0x0E);
    }
}

// ── netstat ───────────────────────────────────────────────────────────────────
static void cmd_netstat(const char* args, CommandOutput* output) {
    (void)args;
    if (!g_net_initialized) {
        output_add_line(output, "Ag surucusu baslatilmadi. Once 'netinit' calistir.", 0x0C);
        return;
    }

    output_add_line(output, "=== RTL8139 Ag Durumu ===", 0x0B);

    uint8_t mac[6];
    rtl8139_get_mac(mac);
    char mac_str[32];
    str_cpy(mac_str, "  MAC : ");
    for (int i = 0; i < 6; i++) {
        char hx[3]; byte_to_hex_str(mac[i], hx);
        str_concat(mac_str, hx);
        if (i < 5) str_concat(mac_str, ":");
    }
    output_add_line(output, mac_str, 0x0F);

    bool up = rtl8139_link_is_up();
    char lnk[32]; str_cpy(lnk, "  Link: ");
    str_concat(lnk, up ? "UP  (bagli)" : "DOWN (kablo yok?)");
    output_add_line(output, lnk, up ? 0x0A : 0x0C);

    if (g_net_rx_display > 0) {
        char rxinfo[48];
        str_cpy(rxinfo, "  Son paket EtherType: ");
        char et[8]; uint16_to_hex_str(g_net_last_etype, et);
        str_concat(rxinfo, et);
        output_add_line(output, rxinfo, 0x07);

        char srcmac[32]; str_cpy(srcmac, "  Kaynak MAC       : ");
        for (int i = 0; i < 6; i++) {
            char hx[3]; byte_to_hex_str(g_net_last_src[i], hx);
            str_concat(srcmac, hx);
            if (i < 5) str_concat(srcmac, ":");
        }
        output_add_line(output, srcmac, 0x07);
    }

    // IPv4 sayaçları
    if (ipv4_is_initialized()) {
        char ipv4line[48];
        str_cpy(ipv4line, "  IPv4 TX: ");
        char cnt[12]; uint64_to_string(ipv4_get_tx_count(), cnt);
        str_concat(ipv4line, cnt); str_concat(ipv4line, "  RX: ");
        uint64_to_string(ipv4_get_rx_count(), cnt);
        str_concat(ipv4line, cnt); str_concat(ipv4line, " paket");
        output_add_line(output, ipv4line, 0x0B);
    }

    rtl8139_stats();
    output_add_line(output, "  (Detayli istatistik serial porta yazildi)", 0x08);
}

// ── netregs ───────────────────────────────────────────────────────────────────
static void cmd_netregs(const char* args, CommandOutput* output) {
    (void)args;
    if (!g_net_initialized) {
        output_add_line(output, "Ag surucusu baslatilmadi.", 0x0C);
        return;
    }
    rtl8139_dump_regs();
    output_add_line(output, "Yazimac dokumu serial porta yazildi.", 0x07);
    output_add_line(output, "  (minicom veya QEMU -serial stdio ile gorebilirsin)", 0x08);
}

// ── netsend ───────────────────────────────────────────────────────────────────
static void cmd_netsend(const char* args, CommandOutput* output) {
    if (!g_net_initialized) {
        output_add_line(output, "Ag surucusu baslatilmadi. Once 'netinit' calistir.", 0x0C);
        return;
    }

    int count = 1;
    if (args && args[0] >= '1' && args[0] <= '9') {
        count = 0;
        for (int i = 0; args[i] >= '0' && args[i] <= '9' && i < 3; i++)
            count = count * 10 + (args[i] - '0');
        if (count < 1) count = 1;
        if (count > 99) count = 99;
    }

    uint8_t mac[6];
    rtl8139_get_mac(mac);

    uint8_t frame[60];
    for (int i = 0; i < 6; i++) frame[i] = 0xFF;
    for (int i = 0; i < 6; i++) frame[6 + i] = mac[i];
    frame[12] = 0x88;
    frame[13] = 0xB5;

    const char* msg = "AscentOS NET TEST Asama-1";
    int mi = 0;
    for (int i = 14; i < 60; i++) {
        frame[i] = (msg[mi]) ? (uint8_t)msg[mi++] : 0x00;
    }

    int ok_count = 0;
    for (int n = 0; n < count; n++) {
        frame[38] = (uint8_t)(n + 1);
        if (rtl8139_send(frame, 60)) ok_count++;
    }

    char res[64];
    str_cpy(res, "  Gonderilen: ");
    char tmp[8]; int_to_str(ok_count, tmp); str_concat(res, tmp);
    str_concat(res, " / ");
    int_to_str(count, tmp); str_concat(res, tmp);
    str_concat(res, " paket (60 byte, broadcast)");
    output_add_line(output, res, ok_count == count ? 0x0A : 0x0E);

    if (ok_count > 0) {
        output_add_line(output, "  EtherType: 0x88B5 (test)", 0x07);
        output_add_line(output, "  Payload  : 'AscentOS NET TEST Asama-1'", 0x07);
        output_add_line(output, "  QEMU hosttan wireshark ile gorebilirsin.", 0x08);
    }
}

// ── netmon ────────────────────────────────────────────────────────────────────
static void cmd_netmon(const char* args, CommandOutput* output) {
    (void)args;
    if (!g_net_initialized) {
        output_add_line(output, "Ag surucusu baslatilmadi.", 0x0C);
        return;
    }
    output_add_line(output, "=== Paket Monitoru ===", 0x0B);

    char buf[48];
    str_cpy(buf, "  Toplam alinan: ");
    char tmp[12]; int_to_str((int)g_net_rx_display, tmp);
    str_concat(buf, tmp);
    str_concat(buf, " paket");
    output_add_line(output, buf, 0x0F);

    if (g_net_rx_display == 0) {
        output_add_line(output, "  Henuz paket alinmadi.", 0x07);
        output_add_line(output, "  QEMU host tarafindan ping atilabilir.", 0x08);
    } else {
        char et[48]; str_cpy(et, "  Son EtherType: ");
        char ets[8]; uint16_to_hex_str(g_net_last_etype, ets);
        str_concat(et, ets);

        const char* etype_name = "";
        if      (g_net_last_etype == 0x0800) etype_name = " (IPv4)";
        else if (g_net_last_etype == 0x0806) etype_name = " (ARP)";
        else if (g_net_last_etype == 0x86DD) etype_name = " (IPv6)";
        else if (g_net_last_etype == 0x88B5) etype_name = " (Test/Ozel)";
        str_concat(et, etype_name);
        output_add_line(output, et, 0x07);

        char sm[48]; str_cpy(sm, "  Son kaynak MAC: ");
        for (int i = 0; i < 6; i++) {
            char hx[3]; byte_to_hex_str(g_net_last_src[i], hx);
            str_concat(sm, hx);
            if (i < 5) str_concat(sm, ":");
        }
        output_add_line(output, sm, 0x07);
    }

    bool up = rtl8139_link_is_up();
    output_add_line(output, up ? "  Link: UP" : "  Link: DOWN", up ? 0x0A : 0x0C);
}

// ============================================================================
// ARP KOMUTLARI — Aşama 2
// ============================================================================

typedef struct { CommandOutput* out; } ARPCacheCtx;
static void arp_cache_line_cb(const char* line, uint8_t color, void* ctx){
    ARPCacheCtx* c = (ARPCacheCtx*)ctx;
    output_add_line(c->out, line, color);
}

// ── ipconfig ─────────────────────────────────────────────────────────────────
static void cmd_ipconfig(const char* args, CommandOutput* output) {
    if (!g_net_initialized) {
        output_add_line(output, "Ag surucusu hazir degil. 'netinit' calistir.", 0x0C);
        return;
    }

    // Argümansız → mevcut durumu göster
    if (!args || args[0] == '\0') {
        if (!arp_is_initialized()) {
            output_add_line(output, "IP henuz atanmadi.", 0x0E);
            output_add_line(output, "Kullanim: ipconfig <IP>   ornek: ipconfig 10.0.2.15", 0x07);
            return;
        }
        output_add_line(output, "=== IP Yapilandirmasi ===", 0x0B);
        uint8_t ip[4], mac[6];
        arp_get_my_ip(ip); arp_get_my_mac(mac);
        char buf[64]; char ipstr[16];

        str_cpy(buf, "  IP   : "); ip_to_str(ip, ipstr);
        str_concat(buf, ipstr);
        output_add_line(output, buf, 0x0F);

        str_cpy(buf, "  MAC  : ");
        for (int i = 0; i < 6; i++) {
            char hx[3]; byte_to_hex_str(mac[i], hx); str_concat(buf, hx);
            if (i < 5) str_concat(buf, ":");
        }
        output_add_line(output, buf, 0x07);

        uint8_t gw[4] = {ip[0], ip[1], ip[2], 2};
        str_cpy(buf, "  GW   : "); ip_to_str(gw, ipstr); str_concat(buf, ipstr);
        str_concat(buf, "  (QEMU SLiRP default)");
        output_add_line(output, buf, 0x08);

        // IPv4 / ICMP durumu
        if (ipv4_is_initialized()) {
            output_add_line(output, "  IPv4 : Aktif", 0x0A);
            output_add_line(output, "  ICMP : Aktif  ('ping <IP>' ile test et)", 0x0A);
        } else {
            output_add_line(output, "  IPv4 : Baslatilmamis", 0x0E);
        }
        return;
    }

    // IP adresi parse
    uint8_t new_ip[4];
    if (!str_to_ip(args, new_ip)) {
        output_add_line(output, "Gecersiz IP adresi.", 0x0C);
        output_add_line(output, "Ornek: ipconfig 10.0.2.15", 0x07);
        return;
    }

    uint8_t mac[6];
    rtl8139_get_mac(mac);
    arp_init(new_ip, mac);

    if (!ipv4_is_initialized()) {
        ipv4_init();
        icmp_init();
    }

    // Gateway: aynı /24'te .2 (QEMU SLiRP default: 10.x.x.2)
    uint8_t gw[4]   = {new_ip[0], new_ip[1], new_ip[2], 2};
    uint8_t mask[4] = {255, 255, 255, 0};
    ipv4_set_gateway(gw);
    ipv4_set_subnet(mask);

    // QEMU SLiRP gateway MAC'ini statik ARP cache'e ekle.
    // Bu sayede 'ping 1.1.1.1' gibi internet adresleri için ARP beklemeye gerek kalmaz.
    // QEMU SLiRP varsayılan gateway MAC: 52:54:00:12:34:02
    {
        static const uint8_t QEMU_GW_MAC[6] = {0x52,0x54,0x00,0x12,0x34,0x02};
        arp_add_static(gw, QEMU_GW_MAC);
    }

    char buf[64]; char ipstr[16]; ip_to_str(new_ip, ipstr);
    str_cpy(buf, "  IP atandi: "); str_concat(buf, ipstr);
    output_add_line(output, buf, 0x0A);
    char gwstr[16]; ip_to_str(gw, gwstr);
    str_cpy(buf, "  Gateway  : "); str_concat(buf, gwstr);
    output_add_line(output, buf, 0x07);
    output_add_line(output, "  IPv4 + ICMP katmanlari aktif.", 0x0A);
    output_add_line(output, "  Gateway MAC: 52:54:00:12:34:02 (QEMU SLiRP statik)", 0x08);
    output_add_line(output, "  Gratuitous ARP gonderiliyor...", 0x07);
    arp_announce();
    output_add_line(output, "  Hazir! Yerel: ping 10.0.2.2", 0x07);
    output_add_line(output, "         Internet: ping 1.1.1.1  ping 8.8.8.8", 0x07);
    output_add_line(output, "  NOT: Farkli gateway MAC icin: arpstatic <GW-IP> <MAC>", 0x08);
}

// ── arping ───────────────────────────────────────────────────────────────────
static void cmd_arping(const char* args, CommandOutput* output) {
    if (!arp_is_initialized()) {
        output_add_line(output, "Once 'ipconfig <IP>' ile IP ata.", 0x0C);
        return;
    }
    if (!args || args[0] == '\0') {
        output_add_line(output, "Kullanim: arping <hedef-IP>   ornek: arping 10.0.2.2", 0x0E);
        return;
    }
    uint8_t target[4];
    if (!str_to_ip(args, target)) {
        output_add_line(output, "Gecersiz IP.", 0x0C); return;
    }

    if (arp_is_initialized()) {
        uint8_t my_ip[4]; arp_get_my_ip(my_ip);
        if (target[0] != my_ip[0] || target[1] != my_ip[1] || target[2] != my_ip[2]) {
            char warn[80];
            str_cpy(warn, "  [UYARI] ARP yalnizca ayni /24 agda calisir.");
            output_add_line(output, warn, 0x0E);
            char gwbuf[32]; char gwip[16];
            uint8_t gw[4] = {my_ip[0], my_ip[1], my_ip[2], 2};
            ip_to_str(gw, gwip);
            str_cpy(gwbuf, "  Gateway deneyin: arping "); str_concat(gwbuf, gwip);
            output_add_line(output, gwbuf, 0x0B);
        }
    }

    char buf[48]; char ipstr[16]; ip_to_str(target, ipstr);
    str_cpy(buf, "ARP request -> "); str_concat(buf, ipstr);
    output_add_line(output, buf, 0x07);

    uint8_t found_mac[6];
    if (arp_resolve(target, found_mac)) {
        str_cpy(buf, "  Cache'ten: ");
        for (int i = 0; i < 6; i++) {
            char hx[3]; byte_to_hex_str(found_mac[i], hx); str_concat(buf, hx);
            if (i < 5) str_concat(buf, ":");
        }
        output_add_line(output, buf, 0x0A);
    } else {
        output_add_line(output, "  Request gonderildi. 'arpcache' ile sonucu gor.", 0x0E);
        output_add_line(output, "  (Cevap interrupt ile gelir, ~1sn bekle)", 0x08);
    }
}

// ── arpcache ─────────────────────────────────────────────────────────────────
static void cmd_arpcache(const char* args, CommandOutput* output) {
    (void)args;
    output_add_line(output, "=== ARP Cache ===", 0x0B);
    output_add_line(output, "  IP              MAC                DURUM", 0x08);
    output_add_line(output, "  --------------- -----------------  --------", 0x08);
    ARPCacheCtx ctx = { output };
    arp_cache_foreach(arp_cache_line_cb, &ctx);
}

// ── arpflush ─────────────────────────────────────────────────────────────────
static void cmd_arpflush(const char* args, CommandOutput* output) {
    (void)args;
    if (!arp_is_initialized()) {
        output_add_line(output, "ARP katmani baslatilmadi.", 0x0C); return;
    }
    arp_flush_cache();
    output_add_line(output, "ARP cache temizlendi.", 0x0A);
}

// ── arptest ──────────────────────────────────────────────────────────────────
static void cmd_arptest(const char* args, CommandOutput* output) {
    (void)args;
    output_add_line(output, "=== ARP / QEMU Ag Testi ===", 0x0B);
    output_add_line(output, "", 0x07);

    if (!g_net_initialized) {
        output_add_line(output, "[HATA] Ag surucusu baslatilmadi.", 0x0C); return;
    }
    if (!arp_is_initialized()) {
        output_add_line(output, "[HATA] Once 'ipconfig <IP>' calistir.", 0x0C); return;
    }

    output_add_line(output, "QEMU user-net (NAT) hakkinda:", 0x0E);
    output_add_line(output, "  - QEMU NAT bir L3 proxy'dir, L2 Ethernet frame'i ISLEMEZ.", 0x07);
    output_add_line(output, "  - ARP broadcast gonderilir ama HICBIR cihaz reply vermez.", 0x07);
    output_add_line(output, "  - Bu bir hata degil, QEMU'nun mimarisi boyledir.", 0x07);
    output_add_line(output, "  - Paket TX calisiyorsa surucunuz dogru demektir.", 0x07);
    output_add_line(output, "", 0x07);

    uint8_t my_ip[4]; arp_get_my_ip(my_ip);
    uint8_t test_ip[4] = {my_ip[0], my_ip[1], my_ip[2], 1};
    uint32_t rx_before = g_net_rx_display;
    arp_request(test_ip);
    output_add_line(output, "[OK] TX calisiyor — ARP request gonderildi.", 0x0A);
    if (g_net_rx_display == rx_before) {
        output_add_line(output, "[OK] RX: 0 reply — QEMU NAT'ta beklenen davranis.", 0x0A);
    } else {
        output_add_line(output, "[OK] RX: Paket alindi! (tap veya socket backend?)", 0x0A);
    }

    output_add_line(output, "", 0x07);
    output_add_line(output, "Pcap ile dogrulama (host terminalde):", 0x0B);
    output_add_line(output, "  make net-test  (QEMU'yu pcap dump ile ac)", 0x07);
    output_add_line(output, "  tcpdump -r /tmp/ascent_net.pcap arp", 0x07);
    output_add_line(output, "", 0x07);
    output_add_line(output, "Gercek ARP testi icin: make net-test", 0x0E);
}

// ── arpstatic ────────────────────────────────────────────────────────────────
static void cmd_arpstatic(const char* args, CommandOutput* output) {
    if (!arp_is_initialized()) {
        output_add_line(output, "Once 'ipconfig <IP>' ile IP ata.", 0x0C); return;
    }
    if (!args || args[0] == '\0') {
        output_add_line(output, "Kullanim: arpstatic <IP> <MAC>", 0x0E);
        output_add_line(output, "Ornek   : arpstatic 10.0.2.2 52:54:00:12:34:56", 0x08);
        return;
    }

    uint8_t ip[4];
    int ip_end = 0;
    while (args[ip_end] && args[ip_end] != ' ') ip_end++;
    char ip_str[16]; int k=0;
    while(k < ip_end && k < 15){ ip_str[k]=args[k]; k++; } ip_str[k]='\0';
    if (!str_to_ip(ip_str, ip)) {
        output_add_line(output, "Gecersiz IP.", 0x0C); return;
    }

    const char* mac_str = args + ip_end;
    while(*mac_str == ' ') mac_str++;
    uint8_t mac[6]; int mi = 0; int val = 0; int nibbles = 0;
    bool mac_ok = true;
    for(int ci = 0; mi < 6; ci++){
        char c = mac_str[ci];
        if((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F')){
            int nib = (c>='0'&&c<='9') ? c-'0' :
                      (c>='a'&&c<='f') ? c-'a'+10 : c-'A'+10;
            val = (val<<4)|nib; nibbles++;
            if(nibbles == 2){ mac[mi++]=val; val=0; nibbles=0; }
        } else if(c==':' || c=='\0'){
            if(c=='\0' && mi<6) break;
        } else { mac_ok=false; break; }
        if(c=='\0') break;
    }
    if(!mac_ok || mi != 6){
        output_add_line(output, "Gecersiz MAC. Ornek: 52:54:00:12:34:56", 0x0C); return;
    }

    arp_add_static(ip, mac);
    char buf[48]; char ipstr[16]; ip_to_str(ip, ipstr);
    str_cpy(buf, "  Statik ARP: "); str_concat(buf, ipstr); str_concat(buf, " eklendi.");
    output_add_line(output, buf, 0x0A);
}

// ============================================================================
// IPv4 + ICMP KOMUTLARI — Aşama 3
// ============================================================================

// ── ipv4info ─────────────────────────────────────────────────────────────────
// IPv4 ve ICMP katman durumu, sayaçlar
static void cmd_ipv4info(const char* args, CommandOutput* output) {
    (void)args;
    if (!ipv4_is_initialized()) {
        output_add_line(output, "IPv4 katmani baslatilmadi.", 0x0C);
        output_add_line(output, "  'ipconfig 10.0.2.15' komutu hem ARP hem IPv4+ICMP'yi baslatir.", 0x0E);
        return;
    }

    output_add_line(output, "=== IPv4 / ICMP Katman Durumu ===", 0x0B);

    uint8_t my_ip[4], my_mac[6];
    arp_get_my_ip(my_ip);
    arp_get_my_mac(my_mac);

    char line[64];

    str_cpy(line, "  IP     : "); char ipbuf[16]; ip_to_str(my_ip, ipbuf);
    str_concat(line, ipbuf);
    output_add_line(output, line, 0x0F);

    str_cpy(line, "  MAC    : ");
    for (int i = 0; i < 6; i++) {
        char hx[3]; byte_to_hex_str(my_mac[i], hx);
        str_concat(line, hx);
        if (i < 5) str_concat(line, ":");
    }
    output_add_line(output, line, 0x0F);

    char cnt[16];
    str_cpy(line, "  IPv4 TX: ");
    uint64_to_string(ipv4_get_tx_count(), cnt);
    str_concat(line, cnt); str_concat(line, " paket");
    output_add_line(output, line, 0x07);

    str_cpy(line, "  IPv4 RX: ");
    uint64_to_string(ipv4_get_rx_count(), cnt);
    str_concat(line, cnt); str_concat(line, " paket");
    output_add_line(output, line, 0x07);

    output_add_empty_line(output);
    output_add_line(output, "  ICMP (ping) hazir.", 0x0A);
    output_add_line(output, "  Ornek: ping 10.0.2.2", 0x07);
    output_add_line(output, "  Ornek: ping 10.0.2.2 4   (4 kez)", 0x07);
}

// ── ping ─────────────────────────────────────────────────────────────────────
// ICMP Echo Request gönder, yanıtı bekle, RTT göster.
// Kullanım: ping 10.0.2.2
//           ping 10.0.2.2 4      (4 kez)
static void cmd_ping(const char* args, CommandOutput* output) {

    // Ön koşul: ARP başlatılmış olmalı
    if (!arp_is_initialized()) {
        output_add_line(output, "ARP/IP katmani baslatilmadi.", 0x0C);
        output_add_line(output, "  Once: ipconfig 10.0.2.15", 0x0E);
        return;
    }
    if (!ipv4_is_initialized()) {
        output_add_line(output, "IPv4 katmani baslatilmadi.", 0x0C);
        output_add_line(output, "  'ipconfig <IP>' otomatik olarak baslatir.", 0x0E);
        return;
    }

    // Argüman yoksa yardım
    if (!args || args[0] == '\0') {
        output_add_line(output, "Kullanim: ping <IP> [adet]", 0x0E);
        output_add_line(output, "  Ornek : ping 10.0.2.2", 0x07);
        output_add_line(output, "  Ornek : ping 10.0.2.2 4", 0x07);
        return;
    }

    // IP'yi ayrıştır (boşluğa kadar)
    char ip_str[16];
    int si = 0;
    while (args[si] && args[si] != ' ' && si < 15) {
        ip_str[si] = args[si]; si++;
    }
    ip_str[si] = '\0';

    uint8_t dst_ip[4];
    if (!str_to_ip(ip_str, dst_ip)) {
        output_add_line(output, "Gecersiz IP adresi.", 0x0C);
        return;
    }

    // Tekrar sayısını ayrıştır (varsayılan 1, max 10)
    int count = 1;
    if (args[si] == ' ' && args[si+1] >= '1' && args[si+1] <= '9') {
        count = 0;
        int ci = si + 1;
        while (args[ci] >= '0' && args[ci] <= '9') {
            count = count * 10 + (args[ci] - '0');
            ci++;
        }
        if (count < 1)  count = 1;
        if (count > 10) count = 10;
    }

    // Başlık
    char hdr[56];
    str_cpy(hdr, "PING "); str_concat(hdr, ip_str);
    str_concat(hdr, " — ICMP Echo Request");
    output_add_line(output, hdr, 0x0B);

    // Subnet kontrolü: hedef aynı /24'te değilse gateway MAC'ine ARP yap
    uint8_t my_ip[4]; arp_get_my_ip(my_ip);
    uint8_t gw[4];    ipv4_get_gateway(gw);
    bool same_subnet = (dst_ip[0] == my_ip[0] &&
                        dst_ip[1] == my_ip[1] &&
                        dst_ip[2] == my_ip[2]);
    uint8_t arp_target[4];
    for(int k = 0; k < 4; k++) arp_target[k] = same_subnet ? dst_ip[k] : gw[k];

    // Gateway tanımlı mı?
    if (!same_subnet) {
        bool gw_zero = (gw[0]==0 && gw[1]==0 && gw[2]==0 && gw[3]==0);
        if (gw_zero) {
            output_add_line(output, "  [HATA] Gateway tanimli degil!", 0x0C);
            output_add_line(output, "  Once: ipconfig 10.0.2.15", 0x0E);
            return;
        }
    }

    // ARP — poll tabanlı bekleme
    {
        uint8_t dummy_mac[6];
        if (!arp_resolve(arp_target, dummy_mac)) {
            if (!same_subnet) {
                char arp_msg[64]; char gw_str[16]; ip_to_str(arp_target, gw_str);
                str_cpy(arp_msg, "  Gateway ARP: "); str_concat(arp_msg, gw_str);
                output_add_line(output, arp_msg, 0x07);
            }
            output_add_line(output, "  ARP istegi gonderiliyor...", 0x0E);
            arp_request(arp_target);
            __asm__ volatile("sti");
            uint64_t t_arp = get_system_ticks();
            bool resolved = false;
            while ((get_system_ticks() - t_arp) < 5000) {
                rtl8139_poll();
                if (arp_resolve(arp_target, dummy_mac)) { resolved = true; break; }
            }
            __asm__ volatile("cli");

            if (!resolved) {
                if (!same_subnet) {
                    // QEMU SLiRP gateway ARP reply vermeyebilir — bilinen MAC'i ekle
                    static const uint8_t QEMU_GW_MAC[6] = {0x52,0x54,0x00,0x12,0x34,0x02};
                    arp_add_static(arp_target, QEMU_GW_MAC);
                    output_add_line(output, "  ARP yanit yok — QEMU varsayilan MAC kullaniliyor", 0x0E);
                    output_add_line(output, "  (52:54:00:12:34:02)  Farkli MAC: arpstatic <GW> <MAC>", 0x08);
                } else {
                    output_add_line(output, "  ARP zaman asimi.", 0x0C);
                    return;
                }
            } else {
                output_add_line(output, "  ARP cozumlendi.", 0x0A);
            }
        }
    }

    int ok_count   = 0;
    int fail_count = 0;

    for (int i = 0; i < count; i++) {
        icmp_ping_reset();

        bool sent = icmp_ping(dst_ip);
        if (!sent) {
            char eline[48];
            str_cpy(eline, "  [");
            char ns[8]; int_to_str(i + 1, ns);
            str_concat(eline, ns); str_concat(eline, "] gonderilemedi");
            output_add_line(output, eline, 0x0C);
            fail_count++;
            continue;
        }

        // Poll tabanlı bekleme — sadece rtl8139_poll() kullan, hlt YOK.
        // hlt IRQ'yu bekler ama cli sonrası tick ilerlemez; pure poll daha güvenilir.
        // Internet ping'i için RTT 50-200ms olabilir, 8000 tick yeterli.
        {
            __asm__ volatile("sti");
            uint64_t t0 = get_system_ticks();
            while (icmp_ping_state() == PING_PENDING) {
                rtl8139_poll();
                uint64_t elapsed = get_system_ticks() - t0;
                if (elapsed >= 8000) break;  // 8 saniye timeout (internet için yeterli)
            }
            __asm__ volatile("cli");
        }

        int state = icmp_ping_state();
        char res_line[64];
        str_cpy(res_line, "  [");
        char ns[8]; int_to_str(i + 1, ns);
        str_concat(res_line, ns); str_concat(res_line, "] ");

        if (state == PING_SUCCESS) {
            uint32_t rtt = icmp_last_rtt_ms();
            uint8_t src[4]; icmp_get_last_src(src);
            char src_s[16]; ip_to_str(src, src_s);
            str_concat(res_line, src_s);
            str_concat(res_line, "  yanit verdi  RTT~");
            if (rtt == 0) {
                str_concat(res_line, "<1ms");
            } else {
                char rtt_s[12]; uint64_to_string((uint64_t)rtt, rtt_s);
                str_concat(res_line, rtt_s); str_concat(res_line, "ms");
            }
            output_add_line(output, res_line, 0x0A);
            ok_count++;
        } else if (state == PING_UNREACHABLE) {
            str_concat(res_line, "Destination Unreachable");
            output_add_line(output, res_line, 0x0C);
            fail_count++;
        } else {
            str_concat(res_line, "Zaman asimi (yanit yok)");
            output_add_line(output, res_line, 0x0C);
            fail_count++;
        }

        icmp_ping_reset();

        if (i < count - 1) {
            uint64_t tw = get_system_ticks();
            while ((get_system_ticks() - tw) < 500) rtl8139_poll();
        }
    }

    // Özet
    output_add_empty_line(output);
    char sum[64];
    str_cpy(sum, "  Sonuc: ");
    char ok_s[8]; int_to_str(ok_count, ok_s);
    str_concat(sum, ok_s); str_concat(sum, "/");
    char cnt_s[8]; int_to_str(count, cnt_s);
    str_concat(sum, cnt_s); str_concat(sum, " basarili");
    output_add_line(output, sum, ok_count == count ? 0x0A : (ok_count > 0 ? 0x0E : 0x0C));

    if (ok_count > 0) {
        output_add_line(output, "  NOT: RTT icmp.c icindeki get_system_ticks() delta'sidir.", 0x08);
        output_add_line(output, "       PIT 1kHz ise 1 tick = 1ms, aksi halde icmp_ticks_to_ms() ayarla.", 0x08);
    }
    if (fail_count > 0 && ok_count == 0) {
        output_add_line(output, "  Ipucu: 'arping 10.0.2.2' ile once ARP coz.", 0x0E);
        output_add_line(output, "  Sonra 'arpstatic 10.0.2.2 52:54:00:12:34:56' dene.", 0x0E);
    }
}

// ============================================================================
// UDP KOMUTLARI — Aşama 4
// ============================================================================

// UDP paket alındığında VGA'ya yazan ve aynı içeriği geri gönderen echo handler
static void _udp_echo_handler(const UDPPacket* pkt, void* ctx) {
    (void)ctx;

    // Kaynak IP:port + içerik → ekrana
    char line[80]; int pos = 0;
    const char* lbl = "UDP< ";
    for (int k = 0; lbl[k]; k++) line[pos++] = lbl[k];

    char ipbuf[16]; ip_to_str(pkt->src_ip, ipbuf);
    for (int k = 0; ipbuf[k]; k++) line[pos++] = ipbuf[k];
    line[pos++] = ':';

    // kaynak port → string
    {
        uint16_t pv = pkt->src_port;
        char rev[6]; int ri = 0;
        if (!pv) { rev[ri++] = '0'; }
        else { while (pv) { rev[ri++] = '0' + (pv % 10); pv /= 10; } }
        for (int x = ri - 1; x >= 0; x--) line[pos++] = rev[x];
    }

    line[pos++] = ' '; line[pos++] = '"';

    // Verinin yazdırılabilir kısmı (max 48 karakter)
    uint16_t show = pkt->len < 48 ? pkt->len : 48;
    for (uint16_t k = 0; k < show; k++) {
        char c = (char)pkt->data[k];
        line[pos++] = (c >= ' ' && c < 127) ? c : '.';
    }
    if (pkt->len > 48) { line[pos++] = '.'; line[pos++] = '.'; line[pos++] = '.'; }
    line[pos++] = '"'; line[pos] = '\0';

    println64(line, 0x0B);

    // Echo: gelen paketi aynen geri gönder
    udp_send(pkt->src_ip, pkt->src_port, pkt->dst_port, pkt->data, pkt->len);
}

// ── udpinit ──────────────────────────────────────────────────────────────────
static void cmd_udpinit(const char* args, CommandOutput* output) {
    (void)args;
    if (udp_is_initialized()) {
        output_add_line(output, "UDP katmani zaten baslatildi.", 0x0E);
        return;
    }
    if (!ipv4_is_initialized()) {
        output_add_line(output, "Hata: Once 'ipconfig <IP>' ile IPv4 katmanini baslat.", 0x0C);
        return;
    }
    udp_init(1);   // 1 = UDP_CSUM_ENABLE
    output_add_line(output, "UDP katmani baslatildi (checksum: etkin).", 0x0A);
    output_add_line(output, "  udplisten <port>     — echo sunucusu baslat", 0x07);
    output_add_line(output, "  udpsend <ip> <p> <m> — mesaj gonder", 0x07);
}

// ── udplisten ─────────────────────────────────────────────────────────────────
static void cmd_udplisten(const char* args, CommandOutput* output) {
    if (!udp_is_initialized()) {
        output_add_line(output, "Hata: Once 'udpinit' calistir.", 0x0C); return;
    }
    if (!args || args[0] == '\0') {
        output_add_line(output, "Kullanim: udplisten <port>   ornek: udplisten 5000", 0x0E); return;
    }
    uint16_t port = 0;
    for (int i = 0; args[i] >= '0' && args[i] <= '9'; i++)
        port = (uint16_t)(port * 10 + (args[i] - '0'));
    if (port == 0) {
        output_add_line(output, "Gecersiz port numarasi.", 0x0C); return;
    }
    if (udp_bind(port, _udp_echo_handler, (void*)(uint64_t)port)) {
        char buf[48]; str_cpy(buf, "Echo dinleyici baslatildi — port=");
        char ps[8]; int_to_str((int)port, ps); str_concat(buf, ps);
        output_add_line(output, buf, 0x0A);
        output_add_line(output, "  Host: nc -u <guest-ip> <port> ile test edebilirsin.", 0x08);
    } else {
        output_add_line(output, "Hata: Bind basarisiz (soket tablosu dolu?).", 0x0C);
    }
}

// ── udpsend ──────────────────────────────────────────────────────────────────
// Kullanim: udpsend 10.0.2.2 5000 merhaba dunya
static void cmd_udpsend(const char* args, CommandOutput* output) {
    if (!udp_is_initialized()) {
        output_add_line(output, "Hata: Once 'udpinit' calistir.", 0x0C); return;
    }
    if (!args || args[0] == '\0') {
        output_add_line(output, "Kullanim: udpsend <ip> <port> <mesaj>", 0x0E);
        output_add_line(output, "  Ornek : udpsend 10.0.2.2 5000 merhaba", 0x08);
        return;
    }

    int pos = 0;

    // IP
    char ip_str[20]; int ilen = 0;
    while (args[pos] && args[pos] != ' ' && ilen < 19) ip_str[ilen++] = args[pos++];
    ip_str[ilen] = '\0';
    if (ilen == 0) { output_add_line(output, "Gecersiz IP.", 0x0C); return; }

    while (args[pos] == ' ') pos++;

    // Port
    uint16_t dst_port = 0;
    while (args[pos] >= '0' && args[pos] <= '9')
        dst_port = (uint16_t)(dst_port * 10 + (args[pos++] - '0'));
    if (dst_port == 0) { output_add_line(output, "Gecersiz port.", 0x0C); return; }

    while (args[pos] == ' ') pos++;

    // Mesaj
    const char* msg = args + pos;
    uint16_t mlen = 0;
    while (msg[mlen]) mlen++;
    if (mlen == 0) { output_add_line(output, "Mesaj bos olamaz.", 0x0C); return; }

    // IP ayrıştır
    uint8_t dst_ip[4];
    if (!str_to_ip(ip_str, dst_ip)) {
        output_add_line(output, "Gecersiz IP adresi.", 0x0C); return;
    }

    // ARP çözümle (önce dene)
    {
        uint8_t dummy[6];
        if (!arp_resolve(dst_ip, dummy)) {
            output_add_line(output, "ARP isteği gönderiliyor...", 0x0E);
            arp_request(dst_ip);
            __asm__ volatile("sti");
            uint64_t t = get_system_ticks();
            bool ok = false;
            while ((get_system_ticks() - t) < 3000) {
                rtl8139_poll();
                if (arp_resolve(dst_ip, dummy)) { ok = true; break; }
                __asm__ volatile("hlt");
            }
            __asm__ volatile("cli");
            if (!ok) {
                output_add_line(output, "ARP zaman asimi — 'arping <ip>' ile once coz.", 0x0C);
                return;
            }
        }
    }

    bool sent = udp_send(dst_ip, dst_port, 0, (const uint8_t*)msg, mlen);

    if (sent) {
        char buf[64]; str_cpy(buf, "Gonderildi -> ");
        str_concat(buf, ip_str); str_concat(buf, ":");
        char ps[8]; int_to_str((int)dst_port, ps); str_concat(buf, ps);
        output_add_line(output, buf, 0x0A);
    } else {
        output_add_line(output, "Gonderim basarisiz.", 0x0C);
    }
}

// ── udpclose ─────────────────────────────────────────────────────────────────
static void cmd_udpclose(const char* args, CommandOutput* output) {
    if (!args || args[0] == '\0') {
        output_add_line(output, "Kullanim: udpclose <port>", 0x0E); return;
    }
    uint16_t port = 0;
    for (int i = 0; args[i] >= '0' && args[i] <= '9'; i++)
        port = (uint16_t)(port * 10 + (args[i] - '0'));
    if (port == 0) { output_add_line(output, "Gecersiz port.", 0x0C); return; }
    udp_unbind(port);
    char buf[40]; str_cpy(buf, "Port kapatildi: ");
    char ps[8]; int_to_str((int)port, ps); str_concat(buf, ps);
    output_add_line(output, buf, 0x07);
}

// ── udpstat ──────────────────────────────────────────────────────────────────
typedef struct { CommandOutput* out; } UDPLineCtx;
static void udp_line_vga_cb(const char* line, uint8_t color, void* ctx) {
    UDPLineCtx* c = (UDPLineCtx*)ctx;
    output_add_line(c->out, line, color);
}

static void cmd_udpstat(const char* args, CommandOutput* output) {
    (void)args;
    if (!udp_is_initialized()) {
        output_add_line(output, "UDP katmani baslatilmadi.", 0x08); return;
    }

    output_add_line(output, "=== UDP Soket Tablosu ===", 0x0B);
    output_add_line(output, "  PORT     RX              TX", 0x08);
    output_add_line(output, "  -------- --------------- ---------------", 0x08);

    UDPLineCtx ctx = { output };
    udp_sockets_foreach(udp_line_vga_cb, &ctx);

    // Toplam sayaçlar
    char buf[64];
    str_cpy(buf, "  Toplam RX: "); char tmp[12];
    uint64_to_string(udp_get_rx_count(), tmp); str_concat(buf, tmp);
    str_concat(buf, "  TX: ");
    uint64_to_string(udp_get_tx_count(), tmp); str_concat(buf, tmp);
    output_add_line(output, buf, 0x07);
}


// ============================================================================
// DHCP KOMUTLARI — Aşama 5
// ============================================================================

// ── dhcp ─────────────────────────────────────────────────────────────────────
// DHCPDISCOVER gönder → otomatik IP, GW, DNS al
static void cmd_dhcp(const char* args, CommandOutput* output) {
    (void)args;

    // Katman kontrolleri
    if (!g_net_initialized) {
        output_add_line(output, "Ag surucusu hazir degil. Once 'netinit' calistir.", 0x0C);
        return;
    }
    if (!ipv4_is_initialized()) {
        ipv4_init();
        icmp_init();
    }
    if (!udp_is_initialized()) {
        udp_init(1);
    }
    if (!dhcp_is_initialized()) {
        dhcp_init();
    }

    // Zaten BOUND ise mevcut yapılandırmayı göster
    if (dhcp_get_state() == (int)DHCP_STATE_BOUND) {
        output_add_line(output, "DHCP zaten BOUND. Mevcut yapilandirma:", 0x0E);
        DHCPConfig cfg;
        dhcp_get_config(&cfg);
        if (cfg.valid) {
            char buf[64]; char ipstr[16];
            str_cpy(buf, "  IP      : "); ip_to_str(cfg.ip,      ipstr); str_concat(buf, ipstr);
            output_add_line(output, buf, 0x0F);
            str_cpy(buf, "  Gateway : "); ip_to_str(cfg.gateway, ipstr); str_concat(buf, ipstr);
            output_add_line(output, buf, 0x07);
        }
        output_add_line(output, "  Yenilemek icin once 'dhcprel' calistir.", 0x08);
        return;
    }

    output_add_line(output, "DHCP DISCOVER gonderiliyor...", 0x0E);

    bool sent = dhcp_discover();
    if (!sent) {
        output_add_line(output, "  [HATA] DISCOVER gonderilemedi.", 0x0C);
        output_add_line(output, "  'netinit' calistirildi mi?", 0x0E);
        return;
    }

    // ── OFFER/ACK bekleme döngüsü ─────────────────────────────────────────
    // Klavye IRQ context'inde çalıştığımızdan IF=0. STI ile interrupt'ları
    // açıyoruz; ardından HLT döngüsü çalıştırıyoruz.
    //
    // STI + HLT neden gerekli:
    //   • HLT: CPU'yu beklet → QEMU ana döngüsü çalışır
    //   • QEMU SLiRP cevabı RTL8139 ring buffer'a yazar + IRQ11 çeker
    //   • IRQ11 → isr_net → rtl8139_irq_handler → rtl_process_rx
    //          → net_packet_callback → udp_handle_packet → dhcp_handle_packet
    //   • dhcp_handle_packet g_state'i REQUESTING/BOUND/FAILED'a günceller
    //   • Uyandıktan sonra rtl8139_poll() ek güvence sağlar (IRQ kaçırmaya karşı)
    //
    // pause ÇALIŞMAZ: sadece decoder ipucu, QEMU'ya kontrol vermiyor.
    output_add_line(output, "  OFFER bekleniyor (max 4 sn)...", 0x07);

    __asm__ volatile("sti");    // IRQ'ları aç (timer + RTL8139 IRQ11)

    uint64_t t0 = get_system_ticks();
    int reported_requesting = 0;

    while ((get_system_ticks() - t0) < 4000) {

        // Ring buffer'ı doğrudan kontrol et (IRQ'suz güvence)
        rtl8139_poll();

        int st = dhcp_get_state();
        if (st == (int)DHCP_STATE_REQUESTING && !reported_requesting) {
            reported_requesting = 1;
            output_add_line(output, "  OFFER alindi! REQUEST gonderildi...", 0x0E);
        }
        if (st == (int)DHCP_STATE_BOUND || st == (int)DHCP_STATE_FAILED) break;

        // HLT: CPU'yu bir sonraki IRQ'ya kadar beklet.
        // Bu sürede QEMU kendi event loop'unu çalıştırır ve
        // SLiRP cevabını RTL8139 DMA ring buffer'a yazabilir.
        __asm__ volatile("hlt");
    }

    __asm__ volatile("cli");    // IRQ'ları kapat (klavye ISR bağlamına geri dön)

    int final_state = dhcp_get_state();

    if (final_state == (int)DHCP_STATE_BOUND) {
        DHCPConfig cfg;
        dhcp_get_config(&cfg);
        output_add_line(output, "=== DHCP BOUND — Yapilandirma Tamam ===", 0x0A);
        if (cfg.valid) {
            char buf[64]; char ipstr[16];
            str_cpy(buf, "  IP      : "); ip_to_str(cfg.ip,      ipstr); str_concat(buf, ipstr);
            output_add_line(output, buf, 0x0F);
            str_cpy(buf, "  Subnet  : "); ip_to_str(cfg.subnet,  ipstr); str_concat(buf, ipstr);
            output_add_line(output, buf, 0x07);
            str_cpy(buf, "  Gateway : "); ip_to_str(cfg.gateway, ipstr); str_concat(buf, ipstr);
            output_add_line(output, buf, 0x07);
            str_cpy(buf, "  DNS     : "); ip_to_str(cfg.dns,     ipstr); str_concat(buf, ipstr);
            output_add_line(output, buf, 0x07);
            str_cpy(buf, "  Kira    : ");
            char ls[12]; uint64_to_string((uint64_t)cfg.lease_time, ls);
            str_concat(buf, ls); str_concat(buf, " sn");
            output_add_line(output, buf, 0x07);
        }
        output_add_empty_line(output);
        output_add_line(output, "  'ping 10.0.2.2' ile baglantıyı test et.", 0x0B);

    } else if (final_state == (int)DHCP_STATE_FAILED) {
        output_add_line(output, "  [HATA] DHCP basarisiz (DHCPNAK alindi).", 0x0C);
        output_add_line(output, "  Elle atamak icin: ipconfig 10.0.2.15", 0x07);

    } else {
        // Hala SELECTING veya REQUESTING — OFFER gelmiyor
        output_add_line(output, "  [ZAMAN ASIMI] DHCP: OFFER gelmedi.", 0x0C);
        // RTL8139 register dump — seri porta yazar; ISR_ROK=1 ise paket
        // geldi ama rtl_process_rx() onu işleyemedi demektir.
        rtl8139_dump_regs();
        output_add_line(output, "  Seri portu incele: ISR/CAPR/CBR degerleri yazildi.", 0x0E);
        output_add_line(output, "  QEMU SLiRP aktif mi? (-netdev user,id=net0)", 0x0E);
        output_add_line(output, "  Elle atamak icin: ipconfig 10.0.2.15", 0x07);
    }
}

// ── dhcpstat ─────────────────────────────────────────────────────────────────
static void cmd_dhcpstat(const char* args, CommandOutput* output) {
    (void)args;

    output_add_line(output, "=== DHCP Istemci Durumu ===", 0x0B);

    if (!dhcp_is_initialized()) {
        output_add_line(output, "  DHCP katmani baslatilmadi.", 0x0C);
        output_add_line(output, "  Once 'dhcp' komutunu calistir.", 0x07);
        return;
    }

    // BOUND ise tam yapılandırmayı doğrudan göster
    if (dhcp_get_state() == (int)DHCP_STATE_BOUND) {
        DHCPConfig cfg;
        dhcp_get_config(&cfg);
        output_add_line(output, "  Durum  : BOUND (IP atandi)", 0x0A);
        if (cfg.valid) {
            char buf[64]; char ipstr[16];
            str_cpy(buf, "  IP      : "); ip_to_str(cfg.ip,        ipstr); str_concat(buf, ipstr);
            output_add_line(output, buf, 0x0F);
            str_cpy(buf, "  Subnet  : "); ip_to_str(cfg.subnet,    ipstr); str_concat(buf, ipstr);
            output_add_line(output, buf, 0x07);
            str_cpy(buf, "  Gateway : "); ip_to_str(cfg.gateway,   ipstr); str_concat(buf, ipstr);
            output_add_line(output, buf, 0x07);
            str_cpy(buf, "  DNS     : "); ip_to_str(cfg.dns,       ipstr); str_concat(buf, ipstr);
            output_add_line(output, buf, 0x07);
            str_cpy(buf, "  DHCP Sv : "); ip_to_str(cfg.server_ip, ipstr); str_concat(buf, ipstr);
            output_add_line(output, buf, 0x08);
            char ls[12];
            str_cpy(buf, "  Kira    : ");
            uint64_to_string((uint64_t)cfg.lease_time, ls);
            str_concat(buf, ls); str_concat(buf, " sn");
            output_add_line(output, buf, 0x07);
        }
        output_add_empty_line(output);
        output_add_line(output, "  ping 10.0.2.2  komutu ile baglantıyı test edebilirsin.", 0x0B);
        return;
    }

    char buf[64];
    str_cpy(buf, "  Durum : "); str_concat(buf, dhcp_state_str());
    int st = dhcp_get_state();
    uint8_t color = 0x07;
    if      (st == (int)DHCP_STATE_BOUND)      color = 0x0A;
    else if (st == (int)DHCP_STATE_SELECTING ||
             st == (int)DHCP_STATE_REQUESTING) color = 0x0E;
    else if (st == (int)DHCP_STATE_FAILED)     color = 0x0C;
    output_add_line(output, buf, color);

    // XID
    {
        uint32_t xid = dhcp_get_xid();
        if (xid) {
            char xbuf[32]; str_cpy(xbuf, "  XID  : 0x");
            const char* hx = "0123456789ABCDEF";
            char xs[9]; int xi = 0;
            for (int s = 28; s >= 0; s -= 4) xs[xi++] = hx[(xid >> s) & 0xF];
            xs[8] = '\0';
            str_concat(xbuf, xs);
            output_add_line(output, xbuf, 0x08);
        }
    }

    DHCPConfig cfg;
    dhcp_get_config(&cfg);

    if (!cfg.valid) {
        output_add_line(output, "  Gecerli konfigürasyon yok.", 0x08);
        return;
    }

    output_add_empty_line(output);
    output_add_line(output, "  Atanan Yapilandirma:", 0x0B);

    char ipstr[16];
    str_cpy(buf, "    IP       : "); ip_to_str(cfg.ip,        ipstr); str_concat(buf, ipstr);
    output_add_line(output, buf, 0x0F);
    str_cpy(buf, "    Subnet   : "); ip_to_str(cfg.subnet,    ipstr); str_concat(buf, ipstr);
    output_add_line(output, buf, 0x07);
    str_cpy(buf, "    Gateway  : "); ip_to_str(cfg.gateway,   ipstr); str_concat(buf, ipstr);
    output_add_line(output, buf, 0x07);
    str_cpy(buf, "    DNS      : "); ip_to_str(cfg.dns,       ipstr); str_concat(buf, ipstr);
    output_add_line(output, buf, 0x07);
    str_cpy(buf, "    DHCP Srv : "); ip_to_str(cfg.server_ip, ipstr); str_concat(buf, ipstr);
    output_add_line(output, buf, 0x08);

    char ls[12];
    str_cpy(buf, "    Kira     : ");
    uint64_to_string((uint64_t)cfg.lease_time, ls); str_concat(buf, ls);
    str_concat(buf, " sn");
    output_add_line(output, buf, 0x07);

    str_cpy(buf, "    T1 (yen.): ");
    uint64_to_string((uint64_t)cfg.renewal_time, ls); str_concat(buf, ls);
    str_concat(buf, " sn");
    output_add_line(output, buf, 0x08);

    str_cpy(buf, "    T2 (yen.): ");
    uint64_to_string((uint64_t)cfg.rebinding_time, ls); str_concat(buf, ls);
    str_concat(buf, " sn");
    output_add_line(output, buf, 0x08);
}

// ── netrxtest ────────────────────────────────────────────────────────────────
// ARP request → 10.0.2.2 gönder, 3 saniye RX bekle.
// Herhangi bir paket gelirse RX path çalışıyor demektir.
// ─────────────────────────────────────────────────────────────────────────────
static void cmd_netrxtest(const char* args, CommandOutput* output) {
    (void)args;

    if (!g_net_initialized) {
        output_add_line(output, "netinit calistirilmadi.", 0x0C);
        return;
    }

    // ARP + IPv4 init gerekli
    if (!ipv4_is_initialized()) { ipv4_init(); icmp_init(); }
    if (!arp_is_initialized()) {
        // Geçici IP ile başlat — sadece ARP probe için
        uint8_t tmp_mac[6]; rtl8139_get_mac(tmp_mac);
        uint8_t tmp_ip[4] = {10,0,2,15};
        arp_init(tmp_ip, tmp_mac);
        ipv4_set_gateway((uint8_t[]){10,0,2,2});
    }

    output_add_line(output, "RX yolu testi: ARP probe 10.0.2.2 gonderiliyor...", 0x0E);
    // ARP resolve tetikler — probe gönderilir
    uint8_t dummy[6];
    arp_resolve((uint8_t[]){10,0,2,2}, dummy);

    output_add_line(output, "3 saniye RX bekleniyor...", 0x07);
    __asm__ volatile("sti");

    uint64_t t0 = get_system_ticks();
    uint32_t pkts_before = g_net_rx_display;
    while ((get_system_ticks() - t0) < 3000) {
        rtl8139_poll();
        if (g_net_rx_display > pkts_before) break;
        __asm__ volatile("hlt");
    }
    __asm__ volatile("cli");

    rtl8139_dump_regs();  // seri porta register durumu

    uint32_t received = g_net_rx_display - pkts_before;
    if (received > 0) {
        output_add_line(output, "  [OK] RX paket alindi! RX path calisıyor.", 0x0A);
        char buf[48]; str_cpy(buf, "  Alinan paket sayisi: ");
        char ns[12]; uint64_to_string((uint64_t)received, ns);
        str_concat(buf, ns);
        output_add_line(output, buf, 0x0F);
    } else {
        output_add_line(output, "  [HATA] Hic paket alinamadi.", 0x0C);
        output_add_line(output, "  Seri portta register degerlerini kontrol et.", 0x0E);
        output_add_line(output, "  ISR_ROK=1 ise rtl_process_rx bug'i var.", 0x0E);
        output_add_line(output, "  ISR=0 ise QEMU hic cevap vermedi.", 0x0E);
    }
}

// ── dhcprel ──────────────────────────────────────────────────────────────────
static void cmd_dhcprel(const char* args, CommandOutput* output) {
    (void)args;
    if (!dhcp_is_initialized()) {
        output_add_line(output, "DHCP katmani baslatilmadi.", 0x0C); return;
    }
    if (dhcp_get_state() != (int)DHCP_STATE_BOUND) {
        output_add_line(output, "DHCP BOUND durumunda degil, release gereksiz.", 0x0E);
        str_cpy((char[64]){0}, "  Mevcut durum: ");
        char buf[48]; str_cpy(buf, "  Mevcut durum: "); str_concat(buf, dhcp_state_str());
        output_add_line(output, buf, 0x07);
        return;
    }
    dhcp_release();
    output_add_line(output, "DHCP Release gonderildi.", 0x0A);
    output_add_line(output, "  IP birakildi. Yeniden almak icin 'dhcp' calistir.", 0x07);
}

// ============================================================================
// TCP KOMUTLARI — Aşama 6
// ============================================================================

// Bağlantı olayı callback'i (tcpconnect / tcptest komutları paylaşır)
static void _tcp_cmd_event_cb(int conn_id, TCPEvent_t event,
                               const uint8_t* data, uint16_t len, void* ctx)
{
    (void)ctx;
    g_tcp_conn_id = conn_id;

    switch(event) {
    case TCP_EVENT_CONNECTED:
        g_tcp_connected = true;
        println64("[TCP] Baglanti kuruldu!", 0x0A);
        break;

    case TCP_EVENT_DATA: {
        g_tcp_data_recvd = true;
        // İlk 127 baytı önizleme tampona kopyala
        uint16_t copy_len = (len < 127) ? len : 127;
        for(uint16_t i = 0; i < copy_len; i++) {
            char c = (char)data[i];
            g_tcp_recv_preview[i] = (c >= ' ' && c < 127) ? c : '.';
        }
        g_tcp_recv_preview[copy_len] = '\0';
        g_tcp_recv_len = len;

        // İlk satırı VGA'ya yaz
        char info[80];
        str_cpy(info, "[TCP] Veri alindi: ");
        char ns[12]; int_to_str((int)len, ns); str_concat(info, ns);
        str_concat(info, " byte");
        println64(info, 0x0B);

        // Önizleme (ilk 64 karakter)
        char preview[68]; int pi = 0;
        preview[pi++] = '['; preview[pi++] = 'T'; preview[pi++] = 'C';
        preview[pi++] = 'P'; preview[pi++] = ']'; preview[pi++] = ' ';
        for(int k = 0; k < 60 && g_tcp_recv_preview[k]; k++)
            preview[pi++] = g_tcp_recv_preview[k];
        if(len > 60) { preview[pi++]='.'; preview[pi++]='.'; preview[pi++]='.'; }
        preview[pi] = '\0';
        println64(preview, 0x0F);
        break;
    }

    case TCP_EVENT_CLOSED:
        g_tcp_closed = true;
        println64("[TCP] Baglanti kapandi.", 0x07);
        break;

    case TCP_EVENT_ERROR:
        g_tcp_error = true;
        println64("[TCP] Baglanti hatasi / zaman asimi.", 0x0C);
        break;

    default:
        break;
    }
}

// Sunucu accept callback'i (tcplisten komutu için)
static void _tcp_cmd_accept_cb(int new_conn_id, const uint8_t remote_ip[4],
                                uint16_t remote_port, void* ctx)
{
    (void)ctx;
    char line[72];
    str_cpy(line, "[TCP] Yeni baglanti kabul edildi! conn_id=");
    char ns[8]; int_to_str(new_conn_id, ns); str_concat(line, ns);
    str_concat(line, "  remote=");
    char ipbuf[16]; ip_to_str(remote_ip, ipbuf); str_concat(line, ipbuf);
    str_concat(line, ":"); int_to_str((int)remote_port, ns); str_concat(line, ns);
    println64(line, 0x0A);

    // Gelen bağlantıyı aktif conn_id olarak kaydet
    g_tcp_conn_id  = new_conn_id;
    g_tcp_connected = true;
}

// ── tcpstat ──────────────────────────────────────────────────────────────────
static void cmd_tcpstat(const char* args, CommandOutput* output) {
    (void)args;
    if (!tcp_is_initialized()) {
        output_add_line(output, "TCP katmani baslatilmadi.", 0x0C);
        output_add_line(output, "  Kernel otomatik baslatir. 'netinit' ile kontrol et.", 0x0E);
        return;
    }
    output_add_line(output, "=== TCP Baglanti Tablosu ===", 0x0B);
    tcp_print_connections();   // serial'a yazar

    char buf[64];
    str_cpy(buf, "  Aktif baglanti: ");
    char ns[8]; int_to_str(tcp_get_conn_count(), ns); str_concat(buf, ns);
    output_add_line(output, buf, 0x07);

    str_cpy(buf, "  TX: "); uint64_to_string(tcp_get_tx_count(), ns);
    str_concat(buf, ns); str_concat(buf, "  RX: ");
    uint64_to_string(tcp_get_rx_count(), ns); str_concat(buf, ns);
    str_concat(buf, " segment");
    output_add_line(output, buf, 0x07);

    // Mevcut aktif conn_id göster
    if (g_tcp_conn_id >= 0) {
        str_cpy(buf, "  Son baglanti ID: "); int_to_str(g_tcp_conn_id, ns);
        str_concat(buf, ns);
        str_concat(buf, "  Durum: ");
        str_concat(buf, tcp_state_str(tcp_get_state(g_tcp_conn_id)));
        output_add_line(output, buf, 0x0F);
    }
    output_add_line(output, "  (Detayli log serial porta yazildi)", 0x08);
}

// ── tcpconnect ───────────────────────────────────────────────────────────────
// Kullanim: tcpconnect 10.0.2.2 80
static void cmd_tcpconnect(const char* args, CommandOutput* output) {
    if (!tcp_is_initialized()) {
        output_add_line(output, "TCP katmani baslatilmadi.", 0x0C); return;
    }
    if (!arp_is_initialized()) {
        output_add_line(output, "Once 'dhcp' veya 'ipconfig' ile IP ata.", 0x0C); return;
    }
    if (!args || args[0] == '\0') {
        output_add_line(output, "Kullanim: tcpconnect <ip> <port>", 0x0E);
        output_add_line(output, "  Ornek : tcpconnect 10.0.2.2 80", 0x08);
        return;
    }

    // IP ayrıştır
    char ip_str[20]; int pos = 0, ilen = 0;
    while (args[pos] && args[pos] != ' ' && ilen < 19) ip_str[ilen++] = args[pos++];
    ip_str[ilen] = '\0';
    while (args[pos] == ' ') pos++;

    uint8_t dst_ip[4];
    if (!str_to_ip(ip_str, dst_ip)) {
        output_add_line(output, "Gecersiz IP adresi.", 0x0C); return;
    }

    // Port ayrıştır
    uint16_t port = 0;
    while (args[pos] >= '0' && args[pos] <= '9')
        port = (uint16_t)(port * 10 + (args[pos++] - '0'));
    if (port == 0) {
        output_add_line(output, "Gecersiz port numarasi.", 0x0C); return;
    }

    // ARP çözümle — önce mevcut cache kontrol et, yoksa istek gönder ve HLT ile QEMU'ya yield et
    {
        uint8_t dummy[6];
        if (!arp_resolve(dst_ip, dummy)) {
            output_add_line(output, "ARP isteği gönderiliyor...", 0x0E);
            arp_request(dst_ip);          // explicit ARP request gönder
            __asm__ volatile("sti");
            uint64_t t = get_system_ticks();
            bool ok = false;
            while ((get_system_ticks() - t) < 3000) {
                rtl8139_poll();
                if (arp_resolve(dst_ip, dummy)) { ok = true; break; }
                __asm__ volatile("hlt");  // QEMU'ya CPU ver → IRQ11 ile ARP reply gelir
            }
            __asm__ volatile("cli");
            if (!ok) {
                output_add_line(output, "ARP zaman asimi.", 0x0C); return;
            }
            output_add_line(output, "ARP cozumlendi.", 0x0A);
        }
    }

    // Durum sıfırla
    g_tcp_connected = false;
    g_tcp_data_recvd = false;
    g_tcp_closed = false;
    g_tcp_error = false;
    g_tcp_recv_len = 0;

    int cid = tcp_connect(dst_ip, port, _tcp_cmd_event_cb, (void*)0);
    if (cid < 0) {
        output_add_line(output, "tcp_connect() basarisiz (tablo dolu?).", 0x0C); return;
    }

    char buf[56]; str_cpy(buf, "SYN gonderildi: "); str_concat(buf, ip_str);
    str_concat(buf, ":"); char ps[8]; int_to_str((int)port, ps); str_concat(buf, ps);
    str_concat(buf, "  conn_id="); int_to_str(cid, ps); str_concat(buf, ps);
    output_add_line(output, buf, 0x0E);

    // SYN+ACK bekle (3 saniye)
    __asm__ volatile("sti");
    uint64_t t0 = get_system_ticks();
    while ((get_system_ticks() - t0) < 3000) {
        rtl8139_poll();
        tcp_tick();
        if (g_tcp_connected || g_tcp_error) break;
        __asm__ volatile("hlt");
    }
    __asm__ volatile("cli");

    if (g_tcp_connected) {
        str_cpy(buf, "  ESTABLISHED! conn_id="); int_to_str(cid, ps); str_concat(buf, ps);
        output_add_line(output, buf, 0x0A);
        output_add_line(output, "  'tcpsend <id> <mesaj>' ile veri gonder.", 0x07);
        output_add_line(output, "  'tcpclose <id>' ile baglantiyi kapat.", 0x07);
    } else if (g_tcp_error) {
        output_add_line(output, "  Baglanti basarisiz (RST veya zaman asimi).", 0x0C);
    } else {
        output_add_line(output, "  SYN_SENT: Yanit bekleniyor (server acik mi?).", 0x0E);
        output_add_line(output, "  'tcpstat' ile durumu izleyebilirsin.", 0x07);
    }
}

// ── tcpsend ──────────────────────────────────────────────────────────────────
// Kullanim: tcpsend 0 Merhaba dunya
static void cmd_tcpsend(const char* args, CommandOutput* output) {
    if (!tcp_is_initialized()) {
        output_add_line(output, "TCP katmani baslatilmadi.", 0x0C); return;
    }
    if (!args || args[0] == '\0') {
        output_add_line(output, "Kullanim: tcpsend <conn_id> <mesaj>", 0x0E);
        output_add_line(output, "  Ornek : tcpsend 0 GET / HTTP/1.0", 0x08);
        return;
    }

    // conn_id
    int cid = 0; int pos = 0;
    while (args[pos] >= '0' && args[pos] <= '9')
        cid = cid * 10 + (args[pos++] - '0');
    while (args[pos] == ' ') pos++;

    const char* msg = args + pos;
    uint16_t mlen = 0; while (msg[mlen]) mlen++;
    if (mlen == 0) {
        output_add_line(output, "Mesaj bos olamaz.", 0x0C); return;
    }

    if (!tcp_is_connected(cid)) {
        char buf[48]; str_cpy(buf, "conn_id="); char ns[8]; int_to_str(cid, ns);
        str_concat(buf, ns); str_concat(buf, " ESTABLISHED degil.");
        output_add_line(output, buf, 0x0C);
        output_add_line(output, "  'tcpstat' ile durumu kontrol et.", 0x07);
        return;
    }

    int sent = tcp_send(cid, (const uint8_t*)msg, mlen);
    if (sent > 0) {
        char buf[56]; str_cpy(buf, "Gonderildi: ");
        char ns[8]; int_to_str(sent, ns); str_concat(buf, ns);
        str_concat(buf, " byte  conn_id="); int_to_str(cid, ns); str_concat(buf, ns);
        output_add_line(output, buf, 0x0A);
    } else {
        output_add_line(output, "Gonderim basarisiz.", 0x0C);
    }
}

// ── tcpclose ─────────────────────────────────────────────────────────────────
static void cmd_tcpclose(const char* args, CommandOutput* output) {
    if (!tcp_is_initialized()) {
        output_add_line(output, "TCP katmani baslatilmadi.", 0x0C); return;
    }
    if (!args || args[0] == '\0') {
        output_add_line(output, "Kullanim: tcpclose <conn_id>", 0x0E); return;
    }
    int cid = 0;
    for (int i = 0; args[i] >= '0' && args[i] <= '9'; i++)
        cid = cid * 10 + (args[i] - '0');

    int st = tcp_get_state(cid);
    if (st == TCP_STATE_CLOSED) {
        char buf[48]; str_cpy(buf, "conn_id="); char ns[8]; int_to_str(cid, ns);
        str_concat(buf, ns); str_concat(buf, " zaten kapali.");
        output_add_line(output, buf, 0x0E); return;
    }

    tcp_close(cid);
    char buf[48]; str_cpy(buf, "FIN gonderildi: conn_id=");
    char ns[8]; int_to_str(cid, ns); str_concat(buf, ns);
    output_add_line(output, buf, 0x07);
    output_add_line(output, "  'tcpstat' ile kapanma durumunu izle.", 0x08);
}

// ── tcplisten ─────────────────────────────────────────────────────────────────
static void cmd_tcplisten(const char* args, CommandOutput* output) {
    if (!tcp_is_initialized()) {
        output_add_line(output, "TCP katmani baslatilmadi.", 0x0C); return;
    }
    if (!args || args[0] == '\0') {
        output_add_line(output, "Kullanim: tcplisten <port>", 0x0E);
        output_add_line(output, "  Ornek : tcplisten 8080", 0x08);
        output_add_line(output, "  Host  : nc 10.0.2.15 8080  (QEMU portfwd gerekli)", 0x08);
        return;
    }
    uint16_t port = 0;
    for (int i = 0; args[i] >= '0' && args[i] <= '9'; i++)
        port = (uint16_t)(port * 10 + (args[i] - '0'));
    if (port == 0) {
        output_add_line(output, "Gecersiz port.", 0x0C); return;
    }

    g_tcp_connected = false;
    g_tcp_data_recvd = false;
    g_tcp_closed = false;

    int lid = tcp_listen(port, _tcp_cmd_accept_cb, (void*)0);
    if (lid < 0) {
        output_add_line(output, "tcp_listen() basarisiz (tablo dolu?).", 0x0C); return;
    }

    char buf[64]; str_cpy(buf, "LISTEN baslatildi: port=");
    char ps[8]; int_to_str((int)port, ps); str_concat(buf, ps);
    str_concat(buf, "  conn_id="); int_to_str(lid, ps); str_concat(buf, ps);
    output_add_line(output, buf, 0x0A);
    output_add_line(output, "  Baglanti gelince VGA'ya yazilir.", 0x07);
    output_add_line(output, "  QEMU Makefile'da: hostfwd=tcp::<host_port>-:<guest_port>", 0x08);
    output_add_line(output, "  'tcpstat' ile durumu izle.", 0x08);
}

// ── wget ──────────────────────────────────────────────────────────────────────
// Kullanim: wget 10.0.2.2:8080/index.html
//           wget 10.0.2.2:8080/dosya.txt [hedef_adi]   (opsiyonel kayıt adı)
static void cmd_wget(const char* args, CommandOutput* output) {
    if (!tcp_is_initialized() || !arp_is_initialized()) {
        output_add_line(output, "Once 'dhcp' ile IP al.", 0x0C); return;
    }
    if (!args || args[0] == '\0') {
        output_add_line(output, "Kullanim: wget <ip[:port][/yol]> [dosyaadi]", 0x0E);
        output_add_line(output, "  Ornek : wget 10.0.2.2:9999/index.html", 0x08);
        output_add_line(output, "  Ornek : wget 10.0.2.2:9999/veri.txt benim.txt", 0x08);
        output_add_line(output, "  Host  : python3 -m http.server 9999", 0x08);
        return;
    }

    // ── URL ayrıştır: ip[:port][/path] [kayit_adi] ───────────────────────
    char ip_str[20] = {0};
    uint16_t port = 80;
    char path[128] = "/";
    char save_name[64] = {0};
    int pos = 0;

    int ilen = 0;
    while (args[pos] && args[pos] != ':' && args[pos] != '/' && args[pos] != ' ' && ilen < 19)
        ip_str[ilen++] = args[pos++];
    ip_str[ilen] = '\0';

    if (args[pos] == ':') {
        pos++; port = 0;
        while (args[pos] >= '0' && args[pos] <= '9')
            port = (uint16_t)(port * 10 + (args[pos++] - '0'));
        if (port == 0) port = 80;
    }
    if (args[pos] == '/') {
        int pi = 0;
        while (args[pos] && args[pos] != ' ' && pi < 127) path[pi++] = args[pos++];
        path[pi] = '\0';
    }
    while (args[pos] == ' ') pos++;

    // Opsiyonel kayıt adı
    if (args[pos]) {
        int si = 0;
        while (args[pos] && args[pos] != ' ' && si < 63) save_name[si++] = args[pos++];
        save_name[si] = '\0';
    }

    // Kayıt adı belirtilmemişse path'ten türet
    if (save_name[0] == '\0') {
        // /index.html → index.html, / → index.html
        const char* slash = path;
        const char* last = slash;
        for (int i = 0; path[i]; i++) if (path[i] == '/') last = path + i + 1;
        if (*last == '\0') {
            str_cpy(save_name, "index.html");
        } else {
            int si = 0;
            while (*last && si < 63) save_name[si++] = *last++;
            save_name[si] = '\0';
        }
    }

    uint8_t dst_ip[4];
    if (!str_to_ip(ip_str, dst_ip)) {
        output_add_line(output, "Gecersiz IP adresi.", 0x0C); return;
    }

    // Başlık
    char hdr[80];
    str_cpy(hdr, "wget  http://"); str_concat(hdr, ip_str);
    str_concat(hdr, ":"); char ps[8]; int_to_str((int)port, ps); str_concat(hdr, ps);
    str_concat(hdr, path);
    output_add_line(output, hdr, 0x0B);

    // ── HTTP GET ─────────────────────────────────────────────────────────
    static HTTPResponse resp;
    HTTPError err = http_get(dst_ip, port, path, &resp);

    if (err != HTTP_OK) {
        char emsg[64]; str_cpy(emsg, "  HATA: "); str_concat(emsg, http_err_str(err));
        output_add_line(output, emsg, 0x0C);
        return;
    }

    // ── Durum ─────────────────────────────────────────────────────────────
    {
        char sl[48]; str_cpy(sl, "  HTTP ");
        char sc[8]; int_to_str(resp.status, sc); str_concat(sl, sc);
        str_concat(sl, " "); str_concat(sl, http_status_str(resp.status));
        output_add_line(output, sl, resp.status == 200 ? 0x0A : 0x0E);
    }

    if (resp.status != 200 || !resp.body || resp.body_len == 0) {
        output_add_line(output, "  Govde bos veya hata kodu, kaydedilmedi.", 0x0E);
        return;
    }

    // ── Boyut ─────────────────────────────────────────────────────────────
    {
        char sz[64]; str_cpy(sz, "  Baslik: ");
        char tmp[8]; int_to_str((int)resp.header_len, tmp); str_concat(sz, tmp);
        str_concat(sz, " byte  Govde: ");
        int_to_str((int)resp.body_len, tmp); str_concat(sz, tmp);
        str_concat(sz, " byte");
        output_add_line(output, sz, 0x07);
    }

    // ── Önizleme (ilk 3 satır) ────────────────────────────────────────────
    output_add_line(output, "  ── Onizleme ──────────────────────────────", 0x08);
    int lines = 0, bi = 0;
    while (bi < (int)resp.body_len && lines < 3) {
        char line[76]; int li = 0;
        line[li++] = ' '; line[li++] = ' ';
        while (bi < (int)resp.body_len && resp.body[bi] != '\n' && li < 74) {
            char c = resp.body[bi++];
            if (c == '\r') continue;
            if (c < 32 && c != '\t') c = '.';
            line[li++] = c;
        }
        if (bi < (int)resp.body_len && resp.body[bi] == '\n') bi++;
        line[li] = '\0';
        output_add_line(output, line, 0x0F);
        lines++;
    }
    if (bi < (int)resp.body_len) {
        char more[32]; str_cpy(more, "  ... (");
        char tmp[8]; int_to_str((int)(resp.body_len - bi), tmp);
        str_concat(more, tmp); str_concat(more, " byte daha)");
        output_add_line(output, more, 0x08);
    }

    // ── Diske kaydet ──────────────────────────────────────────────────────
    // resp.body null-terminate edildi (http.c garantisi), doğrudan yazılabilir
    output_add_line(output, "  Diske kaydediliyor...", 0x0E);

    // Önce dosyayı oluştur (yoksa touch)
    // Dosya yoksa oluştur, sonra yaz
    if (!ext2_path_is_file(save_name)) ext2_create_file(save_name);
    int written = ext2_write_file(save_name, 0, (const uint8_t*)resp.body, (uint32_t)resp.body_len);

    if (written >= 0) {
        char smsg[64]; str_cpy(smsg, "  Kaydedildi: ");
        str_concat(smsg, save_name);
        str_concat(smsg, "  (");
        char tmp[8]; int_to_str((int)resp.body_len, tmp);
        str_concat(smsg, tmp); str_concat(smsg, " byte)");
        output_add_line(output, smsg, 0x0A);
        output_add_line(output, "  Ext2'ye yazildi.", 0x0A);
    } else {
        output_add_line(output, "  HATA: Diske yazma basarisiz!", 0x0C);
    }

    // ── Özet ──────────────────────────────────────────────────────────────
    {
        char sum[64]; str_cpy(sum, "  Toplam: ");
        char tmp[8]; int_to_str((int)resp.total_len, tmp); str_concat(sum, tmp);
        str_concat(sum, " byte alindi.");
        output_add_line(output, sum, written ? 0x0A : 0x0C);
    }
}

// ── httppost ───────────────────────────────────────────────────────────────
// Kullanim: httppost 10.0.2.2:8080/api key=value
static void cmd_httppost(const char* args, CommandOutput* output) {
    if (!tcp_is_initialized() || !arp_is_initialized()) {
        output_add_line(output, "Once 'dhcp' ile IP al.", 0x0C); return;
    }
    if (!args || args[0] == '\0') {
        output_add_line(output, "Kullanim: httppost <ip[:port][/yol]> <veri>", 0x0E);
        output_add_line(output, "  Ornek : httppost 10.0.2.2:9999/api key=merhaba", 0x08);
        return;
    }

    char ip_str[20] = {0};
    uint16_t port = 80;
    char path[128] = "/";
    int pos = 0;

    int ilen = 0;
    while (args[pos] && args[pos] != ':' && args[pos] != '/' && args[pos] != ' ' && ilen < 19)
        ip_str[ilen++] = args[pos++];
    ip_str[ilen] = '\0';

    if (args[pos] == ':') {
        pos++; port = 0;
        while (args[pos] >= '0' && args[pos] <= '9')
            port = (uint16_t)(port * 10 + (args[pos++] - '0'));
        if (port == 0) port = 80;
    }
    if (args[pos] == '/') {
        int pi = 0;
        while (args[pos] && args[pos] != ' ' && pi < 127) path[pi++] = args[pos++];
        path[pi] = '\0';
    }
    while (args[pos] == ' ') pos++;

    const char* body_str = args + pos;
    uint16_t body_len = 0;
    while (body_str[body_len]) body_len++;

    uint8_t dst_ip[4];
    if (!str_to_ip(ip_str, dst_ip)) {
        output_add_line(output, "Gecersiz IP.", 0x0C); return;
    }

    char hdr[80]; str_cpy(hdr, "POST  http://"); str_concat(hdr, ip_str);
    str_concat(hdr, ":"); char ps[8]; int_to_str((int)port, ps); str_concat(hdr, ps);
    str_concat(hdr, path);
    output_add_line(output, hdr, 0x0B);

    static HTTPResponse resp;
    HTTPError err = http_post(dst_ip, port, path,
                               (const uint8_t*)body_str, body_len, &resp);

    if (err != HTTP_OK) {
        char emsg[64]; str_cpy(emsg, "  HATA: "); str_concat(emsg, http_err_str(err));
        output_add_line(output, emsg, 0x0C);
        return;
    }

    {
        char sl[48]; str_cpy(sl, "  HTTP ");
        char sc[8]; int_to_str(resp.status, sc); str_concat(sl, sc);
        str_concat(sl, " "); str_concat(sl, http_status_str(resp.status));
        output_add_line(output, sl, resp.status < 400 ? 0x0A : 0x0C);
    }
    if (resp.body && resp.body_len > 0) {
        char preview[76]; int pi = 0;
        preview[pi++] = ' '; preview[pi++] = ' ';
        int bi = 0;
        while (bi < (int)resp.body_len && pi < 74) {
            char c = resp.body[bi++];
            if (c == '\r' || c == '\n') continue;
            if (c < 32) c = '.';
            preview[pi++] = c;
        }
        preview[pi] = '\0';
        output_add_line(output, preview, 0x0F);
    }
    {
        char sz[48]; str_cpy(sz, "  Toplam: ");
        char tmp[8]; int_to_str((int)resp.total_len, tmp);
        str_concat(sz, tmp); str_concat(sz, " byte");
        output_add_line(output, sz, 0x07);
    }
}

// ── tcptest ───────────────────────────────────────────────────────────────────
// Tam döngü TCP testi: SYN → ESTABLISHED → HTTP GET → yanıt bekle → FIN
// Kullanim: tcptest 10.0.2.2 80
//           tcptest 10.0.2.15 8080  (nc -l -p 8080 açık olmalı)
static void cmd_tcptest(const char* args, CommandOutput* output) {
    if (!tcp_is_initialized()) {
        output_add_line(output, "TCP katmani baslatilmadi.", 0x0C); return;
    }
    if (!arp_is_initialized()) {
        output_add_line(output, "Once 'dhcp' veya 'ipconfig' ile IP ata.", 0x0C); return;
    }
    if (!args || args[0] == '\0') {
        output_add_line(output, "Kullanim: tcptest <ip> <port>", 0x0E);
        output_add_line(output, "  Ornek : tcptest 10.0.2.2 80", 0x08);
        output_add_line(output, "  Host  : nc -l -p 8080   sonra: tcptest 10.0.2.15 8080", 0x08);
        return;
    }

    // IP ve port ayrıştır
    char ip_str[20]; int pos = 0, ilen = 0;
    while (args[pos] && args[pos] != ' ' && ilen < 19) ip_str[ilen++] = args[pos++];
    ip_str[ilen] = '\0';
    while (args[pos] == ' ') pos++;
    uint16_t port = 0;
    while (args[pos] >= '0' && args[pos] <= '9')
        port = (uint16_t)(port * 10 + (args[pos++] - '0'));
    if (port == 0) port = 80;

    uint8_t dst_ip[4];
    if (!str_to_ip(ip_str, dst_ip)) {
        output_add_line(output, "Gecersiz IP.", 0x0C); return;
    }

    char hdr[64]; str_cpy(hdr, "TCP Test: "); str_concat(hdr, ip_str);
    str_concat(hdr, ":"); char ps[8]; int_to_str((int)port, ps); str_concat(hdr, ps);
    output_add_line(output, hdr, 0x0B);

    // ARP çözümle
    {
        uint8_t dummy[6];
        uint8_t my_ip[4]; arp_get_my_ip(my_ip);
        uint8_t gw[4];    ipv4_get_gateway(gw);

        // Kendi IP'mize bağlanmaya çalışıyoruz → ARP çalışmaz
        bool dst_is_me = (dst_ip[0]==my_ip[0] && dst_ip[1]==my_ip[1] &&
                          dst_ip[2]==my_ip[2] && dst_ip[3]==my_ip[3]);
        if (dst_is_me) {
            output_add_line(output, "  [BILGI] Kendi IP'nize baglaniyor.", 0x0E);
            output_add_line(output, "  Loopback desteklenmez. Host'ta tcplisten'i test edin:", 0x07);
            output_add_line(output, "    QEMU'da: tcplisten 8080", 0x08);
            output_add_line(output, "    Host'ta: nc 127.0.0.1 8080  (Makefile: hostfwd=tcp::8080-:8080)", 0x08);
            return;
        }

        // Aynı /24 ise direkt hedef, değilse gateway üzerinden git
        bool same = (dst_ip[0]==my_ip[0] && dst_ip[1]==my_ip[1] && dst_ip[2]==my_ip[2]);
        uint8_t* arp_t = same ? dst_ip : gw;

        if (!arp_resolve(arp_t, dummy)) {
            char arp_msg[48]; str_cpy(arp_msg, "  ARP isteği gönderiliyor: ");
            char ipbuf2[16]; ip_to_str(arp_t, ipbuf2); str_concat(arp_msg, ipbuf2);
            output_add_line(output, arp_msg, 0x0E);
            arp_request(arp_t);          // explicit ARP request — cache boşsa tetikle
            __asm__ volatile("sti");
            uint64_t t = get_system_ticks(); bool ok = false;
            while ((get_system_ticks() - t) < 4000) {
                rtl8139_poll();
                if (arp_resolve(arp_t, dummy)) { ok = true; break; }
                __asm__ volatile("hlt");  // pause DEĞİL: QEMU'ya CPU ver → IRQ11 ARP reply
            }
            __asm__ volatile("cli");
            if (!ok) {
                output_add_line(output, "  ARP zaman asimi.", 0x0C);
                output_add_line(output, "  'arping' ile cache'i kontrol et.", 0x07);
                return;
            }
            output_add_line(output, "  ARP cozumlendi.", 0x0A);
        }
    }

    // Durum sıfırla
    g_tcp_connected = false; g_tcp_data_recvd = false;
    g_tcp_closed = false;    g_tcp_error = false;
    g_tcp_recv_len = 0;      g_tcp_recv_preview[0] = '\0';

    // [1] SYN gönder
    output_add_line(output, "[1/4] SYN gonderiliyor...", 0x07);
    int cid = tcp_connect(dst_ip, port, _tcp_cmd_event_cb, (void*)0);
    if (cid < 0) {
        output_add_line(output, "  HATA: tcp_connect() basarisiz.", 0x0C); return;
    }

    // [2] ESTABLISHED bekle
    output_add_line(output, "[2/4] SYN+ACK bekleniyor (3sn)...", 0x07);
    __asm__ volatile("sti");
    uint64_t t0 = get_system_ticks();
    while ((get_system_ticks() - t0) < 3000) {
        rtl8139_poll(); tcp_tick();
        if (g_tcp_connected || g_tcp_error) break;
        __asm__ volatile("hlt");
    }
    __asm__ volatile("cli");

    if (!g_tcp_connected) {
        output_add_line(output, "  HATA: ESTABLISHED kurulamadi.", 0x0C);
        output_add_line(output, "  Sunucu acik mi? QEMU hostfwd ayarli mi?", 0x0E);
        output_add_line(output, "  Ipucu: 'nc -l -p <port>' host'ta ac.", 0x08);
        tcp_abort(cid);
        return;
    }
    output_add_line(output, "  ESTABLISHED!", 0x0A);

    // [3] HTTP GET gönder
    output_add_line(output, "[3/4] HTTP GET gonderiliyor...", 0x07);
    const char* http_req =
        "GET / HTTP/1.0\r\n"
        "Host: ascentos\r\n"
        "User-Agent: AscentOS/1.0 (hobi OS; x86_64)\r\n"
        "Connection: close\r\n"
        "\r\n";
    uint16_t req_len = 0; while (http_req[req_len]) req_len++;
    int sent = tcp_send(cid, (const uint8_t*)http_req, req_len);
    if (sent > 0) {
        char buf[48]; str_cpy(buf, "  Gonderildi: ");
        char ns[8]; int_to_str(sent, ns); str_concat(buf, ns);
        str_concat(buf, " byte");
        output_add_line(output, buf, 0x07);
    } else {
        output_add_line(output, "  Gonderim basarisiz!", 0x0C);
    }

    // [4] Yanıt bekle
    output_add_line(output, "[4/4] Yanit bekleniyor (4sn)...", 0x07);
    __asm__ volatile("sti");
    t0 = get_system_ticks();
    while ((get_system_ticks() - t0) < 4000) {
        rtl8139_poll(); tcp_tick();
        if (g_tcp_data_recvd || g_tcp_closed || g_tcp_error) break;
        __asm__ volatile("hlt");
    }
    __asm__ volatile("cli");

    if (g_tcp_data_recvd) {
        char buf[64]; str_cpy(buf, "  Yanit alindi: ");
        char ns[8]; int_to_str((int)g_tcp_recv_len, ns); str_concat(buf, ns);
        str_concat(buf, " byte");
        output_add_line(output, buf, 0x0A);
        // İlk 60 karakter önizleme
        char preview[72]; str_cpy(preview, "  > ");
        for(int k = 0; k < 60 && g_tcp_recv_preview[k]; k++) {
            int pl = str_len(preview);
            if (pl < 70) { preview[pl] = g_tcp_recv_preview[k]; preview[pl+1] = '\0'; }
        }
        output_add_line(output, preview, 0x0F);
        output_add_empty_line(output);
        output_add_line(output, "  TCP katmani tam calisıyor!", 0x0A);
    } else if (g_tcp_error) {
        output_add_line(output, "  Baglanti hatasi / RST alindi.", 0x0C);
    } else {
        output_add_line(output, "  Yanit zaman asimi (4sn).", 0x0E);
        output_add_line(output, "  SYN+ACK gelmesi katmanin calistigini kanıtlar.", 0x07);
    }

    // Temiz kapat
    if (!g_tcp_closed) tcp_close(cid);

    char sum[64]; str_cpy(sum, "  Sonuc: ");
    if (g_tcp_connected) str_concat(sum, "3-way-handshake OK  ");
    if (g_tcp_data_recvd) str_concat(sum, "DATA OK  ");
    str_concat(sum, "conn_id="); char ns[8]; int_to_str(cid, ns); str_concat(sum, ns);
    output_add_line(output, sum, g_tcp_data_recvd ? 0x0A : (g_tcp_connected ? 0x0E : 0x0C));
}

// PC SPEAKER TEST COMMAND

void cmd_beep(const char* args, CommandOutput* output) {
    // Argümansız: sistem beep
    // "beep 440"      → 440 Hz, 300ms
    // "beep 440 500"  → 440 Hz, 500ms
    // "beep boot"     → boot melodisi
    // "beep stop"     → sustur

    if (!args || args[0] == '\0') {
        // Varsayılan sistem beep
        pcspk_system_beep();
        output_add_line(output, "Beep! (440 Hz, 100ms)", VGA_CYAN);
        return;
    }

    // "stop" komutu
    if (str_cmp(args, "stop") == 0) {
        pcspk_stop();
        output_add_line(output, "Speaker stopped.", VGA_CYAN);
        return;
    }

    // "boot" melodisi
    if (str_cmp(args, "boot") == 0) {
        pcspk_boot_melody();
        output_add_line(output, "Boot melody played!", VGA_CYAN);
        return;
    }

    // Sayısal argüman: frekans [ms]
    uint32_t freq = 0;
    uint32_t dur  = 300;   // varsayılan süre: 300ms

    const char* p = args;

    // Frekansı parse et
    while (*p >= '0' && *p <= '9') {
        freq = freq * 10 + (*p - '0');
        p++;
    }

    // İkinci argüman (süre) varsa parse et
    while (*p == ' ') p++;
    if (*p >= '0' && *p <= '9') {
        dur = 0;
        while (*p >= '0' && *p <= '9') {
            dur = dur * 10 + (*p - '0');
            p++;
        }
    }

    // Sınır kontrol
    if (freq < 20)    freq = 20;
    if (freq > 20000) freq = 20000;
    if (dur  < 10)    dur  = 10;
    if (dur  > 5000)  dur  = 5000;

    pcspk_beep(freq, dur);

    // Çıktı: "Beep: 440 Hz, 300 ms"
    char buf[64];
    // Basit sayı→string (kernel'deki int_to_str'i kullan)
    extern void int_to_str(int num, char* str);
    char fstr[16], dstr[16];
    int_to_str((int)freq, fstr);
    int_to_str((int)dur,  dstr);

    // buf = "Beep: " + freq + " Hz, " + dur + " ms"
    char* bp = buf;
    const char* prefix = "Beep: ";
    for (const char* s = prefix; *s; s++) *bp++ = *s;
    for (const char* s = fstr;   *s; s++) *bp++ = *s;
    const char* mid = " Hz, ";
    for (const char* s = mid; *s; s++) *bp++ = *s;
    for (const char* s = dstr; *s; s++) *bp++ = *s;
    const char* suf = " ms";
    for (const char* s = suf; *s; s++) *bp++ = *s;
    *bp = '\0';

    output_add_line(output, buf, VGA_CYAN);
}

// ============================================================================
// GFX KOMUTU — GUI moduna geç
// ============================================================================
extern volatile int request_gui_start;

static void cmd_gfx(const char* args, CommandOutput* output) {
    (void)args;
    output_add_line(output, "GUI moduna geciliyor...", 0x0E);
    output_add_line(output, "  Mouse: sol tik = pencere surukle/tikla", 0x0B);
    output_add_line(output, "  Klavye: N = yeni pencere ac", 0x0B);
    request_gui_start = 1;
}

// ============================================================================
// COMMAND TABLE
// ============================================================================
static Command command_table[] = {
    {"hello", "Say hello", cmd_hello},
    {"help", "Show available commands", cmd_help},
    {"clear", "Clear the screen", cmd_clear},
    {"echo", "Echo text back", cmd_echo},
    {"about", "About AscentOS", cmd_about},
    {"neofetch", "Show system information", cmd_neofetch},
    {"pmm", "Physical Memory Manager stats", cmd_pmm},
    {"vmm", "Virtual Memory Manager test", cmd_vmm},
    {"heap", "Heap memory test", cmd_heap},
    
    // Multitasking commands
    {"ps", "List all tasks", cmd_ps},
    {"taskinfo", "Show task information", cmd_taskinfo},
    {"createtask", "Create test tasks (Ring-0)", cmd_createtask},
    {"usertask", "Create Ring-3 user-mode task [isim]", cmd_usertask},
    {"schedinfo", "Scheduler information", cmd_schedinfo},
    {"offihito", "Start Offihito demo task", cmd_offihito},
    
    // File system commands
    {"ls", "List files and directories", cmd_ls},
    {"cd", "Change directory", cmd_cd},
    {"pwd", "Print working directory", cmd_pwd},
    {"mkdir", "Create directory", cmd_mkdir},
    {"rmdir", "Remove directory", cmd_rmdir},
    {"rmr", "Remove directory recursively", cmd_rmr},
    {"cat", "Show file content", cmd_cat},
    {"touch", "Create new file", cmd_touch},
    {"write", "Write to file", cmd_write},
    {"rm", "Delete file", cmd_rm},
    // Advanced file system commands
    {"tree", "Show directory tree", cmd_tree},
    {"find", "Find files by pattern", cmd_find},
    {"du", "Show disk usage", cmd_du},

    // ELF loader commands
    {"exec",    "Load and execute ELF64 binary from ext2", cmd_exec},
    {"elfinfo", "Show ELF64 header info (no load)",         cmd_elfinfo},

    // SYSCALL/SYSRET commands
    {"syscalltest", "Run SYSCALL test suite (116 tests)", cmd_syscalltest},

    // GUI modu
    {"gfx",  "GUI moduna gec (pencere yoneticisi + mouse)", cmd_gfx},

    // Ağ komutları — RTL8139 Aşama 1
    {"netinit",  "RTL8139 ag surucusunu baslat",            cmd_netinit},
    {"netstat",  "Ag karti durumu + paket sayaclari",       cmd_netstat},
    {"netregs",  "Kart donanim yazimaclarini serial doku",  cmd_netregs},
    {"netsend",  "Test paketi gonder [adet]",               cmd_netsend},
    {"netmon",   "Alinan paketleri izle",                   cmd_netmon},

    // ARP komutları — Aşama 2
    {"ipconfig",  "IP ata / goster  ornek: ipconfig 10.0.2.15",   cmd_ipconfig},
    {"arping",    "ARP request gonder  ornek: arping 10.0.2.2",   cmd_arping},
    {"arpcache",  "ARP cache tablosunu goster",                    cmd_arpcache},
    {"arpflush",  "ARP cache temizle",                             cmd_arpflush},
    {"arptest",   "QEMU NAT ARP sinirlamasini acikla + TX test",  cmd_arptest},
    {"arpstatic", "Statik ARP ekle  ornek: arpstatic <IP> <MAC>", cmd_arpstatic},

    // IPv4 + ICMP komutları — Aşama 3
    {"ipv4info", "IPv4 katman durumu ve sayaclari",               cmd_ipv4info},
    {"ping",     "ICMP Echo (ping) gonder  ornek: ping 10.0.2.2", cmd_ping},

    // UDP komutları — Aşama 4
    {"udpinit",   "UDP katmanini baslat (ipconfig sonrasi)",          cmd_udpinit},
    {"udplisten", "Porta UDP echo sunucusu bagla  ornek: udplisten 5000", cmd_udplisten},
    {"udpsend",   "UDP mesaji gonder  ornek: udpsend 10.0.2.2 5000 merhaba", cmd_udpsend},
    {"udpclose",  "Port dinleyicisini kaldir  ornek: udpclose 5000",  cmd_udpclose},
    {"udpstat",   "UDP soket tablosu ve sayaclar",                    cmd_udpstat},

    // DHCP komutları — Aşama 5
    {"dhcp",     "DHCP ile otomatik IP al",                          cmd_dhcp},
    {"dhcpstat", "DHCP durum + atanan IP/GW/DNS goster",             cmd_dhcpstat},
    {"dhcprel",  "DHCP Release — IP iade et",                        cmd_dhcprel},
    {"netrxtest","RX yolu testi (ARP probe 10.0.2.2)",                cmd_netrxtest},

    // TCP komutları — Aşama 6
    {"tcpstat",    "TCP baglanti tablosu ve sayaclar",                    cmd_tcpstat},
    {"tcpconnect", "TCP baglantisi kur  ornek: tcpconnect 10.0.2.2 80",  cmd_tcpconnect},
    {"tcpsend",    "TCP veri gonder     ornek: tcpsend 0 merhaba",        cmd_tcpsend},
    {"tcpclose",   "TCP baglantiyi kapat ornek: tcpclose 0",              cmd_tcpclose},
    {"tcplisten",  "TCP sunucu baslat   ornek: tcplisten 8080",           cmd_tcplisten},
    {"tcptest",    "TCP tam dongü testi  ornek: tcptest 10.0.2.2 80",     cmd_tcptest},
    {"wget",       "HTTP GET + diske kaydet: wget 10.0.2.2:9999/index.html", cmd_wget},
    {"httppost",   "HTTP POST gonder: httppost 10.0.2.2:9999/api key=val",   cmd_httppost},

    // Panic test
    {"panic", "Kernel panic ekranini test et [df|gp|pf|ud|de|stack]", cmd_panic},

    // Performans ölçümü
    {"perf",     "RDTSC performans olcumu [memcpy|memset|loop]", cmd_perf},

    // Spinlock testi
    {"spinlock", "Spinlock / RWLock test suite", cmd_spinlock},

    // PC Speaker test
    {"beep", "PC Speaker ile ses cikar  ornek: beep 440 300", cmd_beep},
};
static int command_count = sizeof(command_table) / sizeof(Command);

// ============================================================================
// net_register_packet_handler
// kernel64.c'den rtl8139_init() başarılı olduğunda çağrılır.
// ============================================================================
void net_register_packet_handler(void) {
    rtl8139_set_packet_handler(net_packet_callback);
    g_net_initialized = 1;
    serial_print("[NET] Paket handler kaydedildi.\n");
}

void init_commands64(void) {
    last_total_ticks = rdtsc64();
    init_filesystem64();
}

int execute_command64(const char* input, CommandOutput* output) {
    output_init(output);
    
    if (str_len(input) == 0) {
        return 1;
    }
    
    char command[MAX_COMMAND_LENGTH];
    const char* args = "";
    
    int i = 0;
    while (input[i] && input[i] != ' ' && i < MAX_COMMAND_LENGTH - 1) {
        command[i] = input[i];
        i++;
    }
    command[i] = '\0';
    
    if (input[i] == ' ') {
        i++;
        args = &input[i];
    }
    
    // Old style commands
    if (str_cmp(command, "sysinfo") == 0) {
        cmd_sysinfo();
        return 1;
    }
    if (str_cmp(command, "cpuinfo") == 0) {
        cmd_cpuinfo();
        return 1;
    }
    if (str_cmp(command, "meminfo") == 0) {
        cmd_meminfo();
        return 1;
    }
    
    // Normal commands
    for (int j = 0; j < command_count; j++) {
        if (str_cmp(command, command_table[j].name) == 0) {
            command_table[j].handler(args, output);
            return 1;
        }
    }
    
    // ── Evrensel ELF fallback ────────────────────────────────────────────────
    {
        int dot_pos = -1;
        for (int _k = 0; command[_k]; _k++) {
            if (command[_k] == '.') { dot_pos = _k; break; }
        }
        if (dot_pos > 0) {
            const char* ext = &command[dot_pos + 1];
            int is_elf = (ext[0]=='E'||ext[0]=='e') &&
                         (ext[1]=='L'||ext[1]=='l') &&
                         (ext[2]=='F'||ext[2]=='f') &&
                          ext[3]=='\0';
            if (is_elf) {
                char try_name[32];
                str_cpy(try_name, command);
                // ext2'de /bin/ altında ara
                char try_path[64];
                str_cpy(try_path, "/bin/");
                str_concat(try_path, try_name);
                if (ext2_file_size(try_path) == 0) {
                    // lowercase dene
                    for (int _k = 0; try_name[_k]; _k++) {
                        char c = try_name[_k];
                        if (c >= 'A' && c <= 'Z') try_name[_k] = c - 'A' + 'a';
                    }
                }
                char exec_args[MAX_COMMAND_LENGTH];
                str_cpy(exec_args, try_name);
                if (args && str_len(args) > 0) {
                    str_concat(exec_args, " ");
                    str_concat(exec_args, args);
                }
                cmd_exec(exec_args, output);
                return 1;
            }
        }

        char elf_filename[32];
        int cmd_len = str_len(command);
        if (cmd_len == 0 || cmd_len > 8) {
            output_add_line(output, "Komut bulunamadi.", VGA_RED);
            return 0;
        }

        for (int _k = 0; _k < cmd_len; _k++) {
            char c = command[_k];
            if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
            elf_filename[_k] = c;
        }
        elf_filename[cmd_len] = '\0';
        str_concat(elf_filename, ".ELF");

        // ELF'i /bin/ altında ara (HELLO → /bin/HELLO.ELF → /bin/hello.elf)
        char elf_path[64];
        str_cpy(elf_path, "/bin/");
        str_concat(elf_path, elf_filename);

        uint32_t fsize = ext2_file_size(elf_path);
        if (fsize == 0) {
            char elf_lower[32];
            for (int _k = 0; _k < cmd_len; _k++) {
                char c = command[_k];
                if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
                elf_lower[_k] = c;
            }
            elf_lower[cmd_len] = '\0';
            str_concat(elf_lower, ".elf");

            char elf_lower_path[64];
            str_cpy(elf_lower_path, "/bin/");
            str_concat(elf_lower_path, elf_lower);

            fsize = ext2_file_size(elf_lower_path);
            if (fsize > 0) {
                str_cpy(elf_filename, elf_lower);
            } else {
                char msg[MAX_LINE_LENGTH];
                str_cpy(msg, "Bilinmeyen komut: ");
                str_concat(msg, command);
                output_add_line(output, msg, VGA_RED);
                return 0;
            }
        }

        char exec_args[MAX_COMMAND_LENGTH];
        str_cpy(exec_args, elf_filename);
        if (args && str_len(args) > 0) {
            str_concat(exec_args, " ");
            str_concat(exec_args, args);
        }
        cmd_exec(exec_args, output);
        return 1;
    }
}

const Command* get_all_commands64(int* count) {
    *count = command_count;
    return command_table;
}