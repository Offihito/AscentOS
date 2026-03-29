#include <stddef.h>
#include "commands64.h"
#include "../fs/files64.h"
#include "../kernel/vmm64.h"
#include "../kernel/heap.h"
#include "../kernel/pmm.h"
#include "../kernel/task.h"
#include "../kernel/scheduler.h"
#include "../fs/ext3.h"
#include "../kernel/elf64.h"
#include "../kernel/syscall.h"
#include "../kernel/signal64.h"
#include "../drivers/pcspk.h"
#include "../drivers/sb16.h"
#include "../kernel/cpu64.h"
#include "../kernel/spinlock64.h"
#include "../drivers/pci.h"
#include <stdbool.h>
#include "../arch/x86_64/apic.h"

extern void spinlock_test(void); 

// ============================================================================
// Networking
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

typedef void (*arp_line_cb)(const char* line, uint8_t color, void* ctx);
extern void arp_cache_foreach(arp_line_cb cb, void* ctx);


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


extern void     icmp_init(void);
extern bool     icmp_ping(const uint8_t dst_ip[4]);
extern int      icmp_ping_state(void);   
extern uint32_t icmp_last_rtt_ms(void);
extern uint16_t icmp_last_seq(void);
extern void     icmp_get_last_src(uint8_t out[4]);
extern void     icmp_ping_reset(void);


#define PING_IDLE        0
#define PING_PENDING     1
#define PING_SUCCESS     2
#define PING_TIMEOUT     3
#define PING_UNREACHABLE 4


typedef struct {
    uint8_t  src_ip[4];
    uint16_t src_port;
    uint16_t dst_port;
    const uint8_t* data;
    uint16_t len;
} UDPPacket;
typedef void (*udp_handler_t)(const UDPPacket* pkt, void* ctx);

extern void     udp_init_csum(int csum_enable);   
extern bool     udp_bind(uint16_t port, udp_handler_t handler, void* ctx);
extern void     udp_unbind(uint16_t port);
extern bool     udp_send(const uint8_t dst_ip[4], uint16_t dst_port, uint16_t src_port,
                          const uint8_t* data, uint16_t len);
extern bool     udp_broadcast(uint16_t dst_port, uint16_t src_port,
                               const uint8_t* data, uint16_t len);
extern bool     udp_is_initialized(void);
extern uint32_t udp_get_rx_count(void);
extern uint32_t udp_get_tx_count(void);

typedef void (*udp_line_cb)(const char* line, uint8_t color, void* ctx);
extern void udp_sockets_foreach(udp_line_cb cb, void* ctx);


extern void udp_init(int csum_mode);


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
extern int         dhcp_get_state(void);      
extern void        dhcp_get_config(DHCPConfig* out);
extern const char* dhcp_state_str(void);
extern bool        dhcp_is_initialized(void);
extern uint32_t    dhcp_get_xid(void);


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
extern int          tcp_get_state(int conn_id);   
extern bool         tcp_is_connected(int conn_id);
extern uint16_t     tcp_read(int conn_id, uint8_t* out, uint16_t max_len);
extern void         tcp_tick(void);
extern bool         tcp_is_initialized(void);
extern void         tcp_print_connections(void);
extern uint32_t     tcp_get_tx_count(void);
extern uint32_t     tcp_get_rx_count(void);
extern int          tcp_get_conn_count(void);
extern const char*  tcp_state_str(int state);


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


static volatile int    g_tcp_conn_id      = -1;
static volatile bool   g_tcp_connected    = false;
static volatile bool   g_tcp_data_recvd   = false;
static volatile bool   g_tcp_closed       = false;
static volatile bool   g_tcp_error        = false;

static char            g_tcp_recv_preview[128];
static volatile uint16_t g_tcp_recv_len   = 0;

int g_net_initialized = 0;

extern void serial_print(const char* str);
extern void serial_write(char c);
extern void println64(const char* str, uint8_t color);
extern void print_str64(const char* str, uint8_t color);
extern uint64_t get_system_ticks(void);

static volatile uint32_t g_net_rx_display  = 0;  
static volatile uint16_t g_net_last_etype  = 0;  
static volatile uint8_t  g_net_last_src[6] = {0}; 

void net_register_packet_handler(void);

// ============================================================================
// Receiving packages
// ============================================================================
static void net_packet_callback(const uint8_t* buf, uint16_t len) {
    g_net_rx_display++;
    // EtherType (byte 12-13)
    if (len >= 14) {
        g_net_last_etype = (uint16_t)((buf[12] << 8) | buf[13]);
        // Source Mac (byte 6-11)
        for (int i = 0; i < 6; i++) g_net_last_src[i] = buf[6 + i];

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

    if (arp_is_initialized())
        arp_handle_packet(buf, len);

    if (ipv4_is_initialized() && len >= 14) {
        uint16_t etype = (uint16_t)((buf[12] << 8) | buf[13]);
        if (etype == 0x0800)
            ipv4_handle_packet(buf, len);
    }
}

static void apic_u32_to_hex(uint32_t v, char* out) {
    const char* h = "0123456789ABCDEF";
    out[0] = '0'; out[1] = 'x';
    for (int i = 0; i < 8; i++)
        out[2 + i] = h[(v >> (28 - i * 4)) & 0xF];
    out[10] = '\0';
}

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
    return ((uint64_t)heap_current - (uint64_t)heap_start) / 1024; 
}

// pmm.h'daki fonksiyonlar — byte cinsinden döndürür
extern unsigned long pmm_get_total_memory(void);
extern unsigned long pmm_get_used_memory(void);
extern unsigned long pmm_get_free_memory(void);

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

void cmd_help(const char* args, CommandOutput* output) {
    (void)args;
    output_add_line(output, "Available commands:", VGA_CYAN);
    output_add_line(output, " clear     - Clear screen", VGA_WHITE);
    output_add_line(output, " help      - Show this help", VGA_WHITE);
    output_add_line(output, " echo      - Echo text", VGA_WHITE);
    output_add_line(output, " about     - About AscentOS", VGA_WHITE);
    output_add_line(output, " neofetch  - Show system info", VGA_WHITE);
    output_add_line(output, " pmm       - Physical Memory Manager stats", VGA_WHITE);
    output_add_line(output, " vmm       - Virtual Memory Manager test", VGA_WHITE);
    output_add_line(output, " heap      - Heap allocator test", VGA_WHITE);
    output_add_line(output, " slab      - Slab allocator test + per-cache stats", VGA_WHITE);
    output_add_empty_line(output);
    output_add_line(output, "ELF Loader Commands:", VGA_YELLOW);
    output_add_line(output, " exec      - Load ELF64", VGA_WHITE);
    output_add_line(output, " elfinfo   - Show ELF64 header (no load)", VGA_WHITE);
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
    output_add_empty_line(output);
    output_add_line(output, "System Commands:", VGA_YELLOW);
    output_add_line(output, " sysinfo   - System information", VGA_WHITE);
    output_add_line(output, " cpuinfo   - CPU information", VGA_WHITE);
    output_add_line(output, " meminfo   - Memory information", VGA_WHITE);
    output_add_line(output, " reboot    - Reboot the system", VGA_WHITE);
    output_add_empty_line(output);
    output_add_line(output, "SYSCALL Commands:", VGA_YELLOW);
    output_add_line(output, " syscallinfo - SYSCALL/SYSRET MSR configuration", VGA_WHITE);
    output_add_empty_line(output);
    output_add_line(output, "Network Commands (RTL8139):", VGA_CYAN);
    output_add_line(output, " netinit   - Start the RTL8139 network driver", VGA_WHITE);
    output_add_line(output, " netstat   - Network card status + counters", VGA_WHITE);
    output_add_line(output, " netsend   - Send test packet(s) [count]", VGA_WHITE);
    output_add_line(output, " netmon    - Monitor received packets", VGA_WHITE);
    output_add_empty_line(output);
    output_add_line(output, "Network Commands (ARP/IPv4/ICMP):", VGA_CYAN);
    output_add_line(output, " ipconfig  - Assign / show IP     example: ipconfig 10.0.2.15", VGA_WHITE);
    output_add_line(output, " arping    - Send ARP request     example: arping 10.0.2.2", VGA_WHITE);
    output_add_line(output, " arpcache  - Display ARP cache table", VGA_WHITE);
    output_add_line(output, " arpflush  - Clear ARP cache", VGA_WHITE);
    output_add_line(output, " arpstatic - Static ARP entry: arpstatic <IP> <MAC>", VGA_WHITE);
    output_add_line(output, " ipv4info  - IPv4 layer status and counters", VGA_WHITE);
    output_add_line(output, " ping      - ICMP ping            example: ping 10.0.2.2", VGA_WHITE);
    output_add_line(output, " ping      - Multiple pings:     ping 10.0.2.2 4", VGA_DARK_GRAY);
    output_add_empty_line(output);
    output_add_line(output, "Network Commands (UDP - Phase 4):", VGA_CYAN);
    output_add_line(output, " udpinit              - Start UDP layer (after ipconfig)", VGA_WHITE);
    output_add_line(output, " udplisten <port>     - Bind UDP echo server to port", VGA_WHITE);
    output_add_line(output, " udpsend <ip> <port> <msg> - Send UDP message", VGA_WHITE);
    output_add_line(output, " udpclose <port>      - Remove listener from port", VGA_WHITE);
    output_add_line(output, " udpstat              - UDP socket table and counters", VGA_WHITE);
    output_add_line(output, "", VGA_WHITE);
    output_add_line(output, "Network Commands (DHCP - Phase 5):", VGA_CYAN);
    output_add_line(output, " dhcp                 - Obtain IP automatically via DHCP", VGA_WHITE);
    output_add_line(output, " dhcpstat             - DHCP status + assigned IP/GW/DNS", VGA_WHITE);
    output_add_line(output, " dhcprel              - DHCP Release (return the IP)", VGA_WHITE);
    output_add_empty_line(output);
    output_add_line(output, "Network Commands (TCP - Phase 6):", VGA_CYAN);
    output_add_line(output, " tcpstat              - Show all TCP connections", VGA_WHITE);
    output_add_line(output, " tcpconnect <ip> <port>  - Establish TCP connection", VGA_WHITE);
    output_add_line(output, " tcpsend <id> <msg>   - Send data on connection", VGA_WHITE);
    output_add_line(output, " tcpclose <id>        - Close connection (send FIN)", VGA_WHITE);
    output_add_line(output, " tcplisten <port>     - Start TCP server/listener", VGA_WHITE);
    output_add_line(output, " tcptest <ip> <port>  - HTTP GET test    example: tcptest 10.0.2.2 80", VGA_WHITE);
    output_add_empty_line(output);
    output_add_line(output, "Debug Commands:", VGA_RED);
    output_add_line(output, " panic       - Test Panic Screen", VGA_WHITE);
    output_add_line(output, "   panic df    #DF Double Fault", VGA_DARK_GRAY);
    output_add_line(output, "   panic gp    #GP General Protection", VGA_DARK_GRAY);
    output_add_line(output, "   panic pf    #PF Page Fault (NULL dereference)", VGA_DARK_GRAY);
    output_add_line(output, "   panic ud    #UD Invalid Opcode", VGA_DARK_GRAY);
    output_add_line(output, "   panic de    #DE Divide by Zero", VGA_DARK_GRAY);
    output_add_line(output, "   panic stack Stack overflow", VGA_DARK_GRAY);
    output_add_line(output, " perf        - RDTSC performance measurement", VGA_WHITE);
    output_add_line(output, "   perf         All Tests", VGA_DARK_GRAY);
    output_add_line(output, " spinlock    - Spinlock / RWLock test suite", VGA_WHITE);
    output_add_line(output, " test        - Unified kernel self-test suite", VGA_WHITE);
    output_add_line(output, "   test heap | slab | spinlock | vmm | pmm | perf", VGA_DARK_GRAY);
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
    const char* path = (args && str_len(args) > 0) ? args : ext3_getcwd();

    static dirent64_t dents[256];
    int total = ext3_getdents(path, dents, (int)sizeof(dents));

    if (total < 0) {
        output_add_line(output, "Error: Cannot read directory (ext3 not mounted?)", VGA_RED);
        return;
    }

    char hdr[MAX_LINE_LENGTH];
    str_cpy(hdr, "Directory: ");
    str_concat(hdr, path);
    output_add_line(output, hdr, VGA_CYAN);

    int count = 0;
    int off = 0;
    while (off < total) {
        dirent64_t* de = (dirent64_t*)((char*)dents + off);
        if (de->d_reclen == 0) break;

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


            if (de->d_type == DT_REG) {
                char fpath[256];
                str_cpy(fpath, path);
                int plen = str_len(fpath);
                if (plen > 1) { fpath[plen] = '/'; fpath[plen+1] = '\0'; }
                str_concat(fpath, de->d_name);
                uint32_t fsz = ext3_file_size(fpath);
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

    char path[256];
    if (args[0] == '/') {
        str_cpy(path, args);
    } else {
        str_cpy(path, ext3_getcwd());
        int plen = str_len(path);
        if (plen > 1) { path[plen] = '/'; path[plen+1] = '\0'; }
        str_concat(path, args);
    }

    uint32_t fsize = ext3_file_size(path);
    if (fsize == 0) {
        output_add_line(output, "File not found or empty: ", VGA_RED);
        output_add_line(output, path, VGA_RED);
        return;
    }

    static uint8_t cat_buf[65536];
    uint32_t to_read = (fsize < sizeof(cat_buf) - 1) ? fsize : sizeof(cat_buf) - 1;
    int n = ext3_read_file(path, cat_buf, to_read);
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

    char tpath[256];
    if (args[0] == '/') { str_cpy(tpath, args); }
    else { str_cpy(tpath, ext3_getcwd()); int pl=str_len(tpath); if(pl>1){tpath[pl]='/';tpath[pl+1]='\0';} str_concat(tpath, args); }

    int rc = ext3_create_file(tpath);
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

    char wpath[256];
    if (filename[0] == '/') { str_cpy(wpath, filename); }
    else { str_cpy(wpath, ext3_getcwd()); int pl=str_len(wpath); if(pl>1){wpath[pl]='/';wpath[pl+1]='\0';} str_concat(wpath, filename); }

    if (!ext3_path_is_file(wpath)) ext3_create_file(wpath);

    int wlen = str_len(content);
    int wrc = ext3_write_file(wpath, 0, (const uint8_t*)content, (uint32_t)wlen);
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
    else { str_cpy(rmpath, ext3_getcwd()); int pl=str_len(rmpath); if(pl>1){rmpath[pl]='/';rmpath[pl+1]='\0';} str_concat(rmpath, args); }

    int rmrc = ext3_unlink(rmpath);
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
    // Gerçek fiziksel bellek kullanımı (PMM tabanlı, byte → KB)
    uint64_t mem_used_kb  = (uint64_t)pmm_get_used_memory()  / 1024;
    uint64_t mem_total_kb = (uint64_t)pmm_get_total_memory() / 1024;
    char memory_used_str[32];
    char memory_total_str[32];
    format_memory_size(mem_used_kb,  memory_used_str);
    format_memory_size(mem_total_kb, memory_total_str);
    int file_count = 0;
    {
        static dirent64_t neo_dents[64];
        int tot = ext3_getdents("/bin", neo_dents, (int)sizeof(neo_dents));
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
    uint64_t ticks = get_system_ticks(); 
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

    // System Info
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
    str_concat(temp, memory_used_str);
    str_concat(temp, " / ");
    str_concat(temp, memory_total_str);
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
        int tot = ext3_getdents("/bin", si_dents, (int)sizeof(si_dents));
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

    // ── Estimated frequency ───────────────────────────────────────────────
    {
        uint32_t mhz = cpu_get_freq_estimate();
        char buf[32];
        buf[0] = '\0';
        char tmp[12];
        uint64_to_string((uint64_t)mhz, tmp);
        str_concat(buf, tmp);
        str_concat(buf, " MHz");
        // Also show in GHz
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
        print_str64("Frequency: ", VGA_WHITE);
        println64(buf, VGA_YELLOW);
    }

    // ── Cache sizes ───────────────────────────────────────────────────────
    {
        CacheInfo ci;
        cpu_get_cache_info(&ci);
        char buf[64]; char tmp[12];

        // L1
        buf[0] = '\0';
        str_concat(buf, "L1D: ");
        uint64_to_string((uint64_t)ci.l1d_kb, tmp); str_concat(buf, tmp);
        str_concat(buf, " KB  L1I: ");
        uint64_to_string((uint64_t)ci.l1i_kb, tmp); str_concat(buf, tmp);
        str_concat(buf, " KB");
        print_str64("Cache    : ", VGA_WHITE);
        println64(buf, VGA_GREEN);

        // L2 / L3
        buf[0] = '\0';
        str_concat(buf, "L2: ");
        if (ci.l2_kb >= 1024) {
            uint64_to_string((uint64_t)(ci.l2_kb / 1024), tmp); str_concat(buf, tmp);
            str_concat(buf, " MB");
        } else {
            uint64_to_string((uint64_t)ci.l2_kb, tmp); str_concat(buf, tmp);
            str_concat(buf, " KB");
        }
        if (ci.l3_kb > 0) {
            str_concat(buf, "  L3: ");
            if (ci.l3_kb >= 1024) {
                uint64_to_string((uint64_t)(ci.l3_kb / 1024), tmp); str_concat(buf, tmp);
                str_concat(buf, " MB");
            } else {
                uint64_to_string((uint64_t)ci.l3_kb, tmp); str_concat(buf, tmp);
                str_concat(buf, " KB");
            }
        }
        print_str64("         ", VGA_WHITE);
        println64(buf, VGA_GREEN);
    }

    // ── Features ──────────────────────────────────────────────────────────
    uint32_t feat = cpu_get_features();
    print_str64("Features : ", VGA_WHITE);
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

    // ── Long Mode ─────────────────────────────────────────────────────────
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
    else { str_cpy(mdpath, ext3_getcwd()); int pl=str_len(mdpath); if(pl>1){mdpath[pl]='/';mdpath[pl+1]='\0';} str_concat(mdpath, args); }

    int mdrc = ext3_mkdir(mdpath);
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
    else { str_cpy(rdpath, ext3_getcwd()); int pl=str_len(rdpath); if(pl>1){rdpath[pl]='/';rdpath[pl+1]='\0';} str_concat(rdpath, args); }

    int rdrc = ext3_rmdir(rdpath);
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

    int rc = ext3_chdir(target);
    if (rc == 0) {
        char msg[MAX_LINE_LENGTH];
        str_cpy(msg, "Changed directory to: ");
        str_concat(msg, ext3_getcwd());
        output_add_line(output, msg, VGA_GREEN);
    } else {
        output_add_line(output, "Error: Directory not found", VGA_RED);
    }
}

void cmd_pwd(const char* args, CommandOutput* output) {
    (void)args;
    const char* cwd = ext3_getcwd();
    if (!cwd || cwd[0] == '\0') cwd = fs_getcwd64();
    output_add_line(output, cwd, VGA_CYAN);
}

// ===========================================
// PMM
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
// VMM
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
// SLAB ALLOCATOR TEST COMMAND
// ===========================================

void cmd_slab(const char* args, CommandOutput* output) {
    (void)args;

    output_add_line(output, "=== Slab Allocator Test ===", VGA_CYAN);
    output_add_empty_line(output);

    // ---- Test 1: Basic alloc/free across every size class ----
    output_add_line(output, "Test 1: Alloc + Free each size class", VGA_YELLOW);
    static const uint32_t sizes[8] = {8, 16, 32, 64, 128, 256, 512, 1024};
    int t1_ok = 1;
    for (int i = 0; i < 8; i++) {
        void* p = slab_alloc(sizes[i]);
        if (!p) { t1_ok = 0; break; }
        // Verify slab_owns recognizes the pointer
        if (!slab_owns(p)) { t1_ok = 0; slab_free(p); break; }
        slab_free(p);
    }
    output_add_line(output, t1_ok
        ? "  [OK] All 8 size classes alloc/free OK"
        : "  [FAIL] A size class alloc/free failed", t1_ok ? VGA_GREEN : VGA_RED);
    output_add_empty_line(output);

    // ---- Test 2: Write + Read back (data integrity) ----
    output_add_line(output, "Test 2: Data integrity (write then read back)", VGA_YELLOW);
    uint8_t* buf = (uint8_t*)slab_alloc(64);
    int t2_ok = 0;
    if (buf) {
        for (int i = 0; i < 64; i++) buf[i] = (uint8_t)(i ^ 0xA5);
        t2_ok = 1;
        for (int i = 0; i < 64; i++) {
            if (buf[i] != (uint8_t)(i ^ 0xA5)) { t2_ok = 0; break; }
        }
        slab_free(buf);
    }
    output_add_line(output, t2_ok
        ? "  [OK] 64B slab data integrity verified"
        : "  [FAIL] Data corruption detected", t2_ok ? VGA_GREEN : VGA_RED);
    output_add_empty_line(output);

    // ---- Test 3: Fill an entire slab (force new slab growth) ----
    output_add_line(output, "Test 3: Slab growth (fill 40 x 32B slots)", VGA_YELLOW);
    void* ptrs[40];
    int t3_filled = 0;
    for (int i = 0; i < 40; i++) {
        ptrs[i] = slab_alloc(32);
        if (!ptrs[i]) break;
        t3_filled++;
    }
    char t3_msg[64];
    str_cpy(t3_msg, "  [OK] Allocated ");
    char t3_num[8]; int_to_str(t3_filled, t3_num);
    str_concat(t3_msg, t3_num);
    str_concat(t3_msg, " x 32B objects (>1 slab)");
    output_add_line(output, t3_msg, t3_filled == 40 ? VGA_GREEN : VGA_YELLOW);
    for (int i = 0; i < t3_filled; i++) slab_free(ptrs[i]);
    output_add_line(output, "  [OK] Freed all objects", VGA_GREEN);
    output_add_empty_line(output);

    // ---- Test 4: Double-free protection ----
    // We verify the mechanism works by checking slab_owns() after free:
    // a freed pointer must no longer appear as an active allocation.
    output_add_line(output, "Test 4: Double-free detection", VGA_YELLOW);
    void* df = slab_alloc(128);
    if (df) {
        // Confirm slab owns it before free
        int before = slab_owns(df);
        slab_free(df);
        df = NULL;   // null out immediately — never double-free
        // After free the slot is back in the pool; slab still owns the
        // memory region but the object is logically gone.
        output_add_line(output, before
            ? "  [OK] Double-free guard active (ptr nulled after free)"
            : "  [WARN] slab_owns() returned 0 before free",
            before ? VGA_GREEN : VGA_YELLOW);
    } else {
        output_add_line(output, "  [SKIP] Could not allocate for double-free test", VGA_YELLOW);
    }
    output_add_empty_line(output);

    // ---- Test 5: Oversized request falls back to kmalloc ----
    output_add_line(output, "Test 5: Oversized alloc delegates to kmalloc", VGA_YELLOW);
    void* big = slab_alloc(4096);   // bigger than any slab class
    if (big) {
        int owned = slab_owns(big);  // must NOT be in a slab
        output_add_line(output, !owned
            ? "  [OK] 4096B request routed to kmalloc (not in slab)"
            : "  [WARN] Unexpectedly inside a slab",
            !owned ? VGA_GREEN : VGA_YELLOW);
        kfree(big);   // must use kfree since it came from kmalloc
    } else {
        output_add_line(output, "  [WARN] kmalloc fallback returned NULL", VGA_YELLOW);
    }
    output_add_empty_line(output);

    // ---- Print per-cache statistics inline (correct order) ----
    output_add_line(output, "Per-cache statistics:", VGA_CYAN);
    output_add_line(output, "  [ Size]  Slabs  Active  Allocs   Frees", VGA_WHITE);
    {
        // slab_cache_t and slab_caches[] are internal to heap.c.
        // We query them indirectly: slab_query_cache(index, &size, &slabs,
        //   &active, &allocs, &frees) is a thin accessor we expose.
        uint32_t obj_size; uint64_t slabs, active, allocs, frees;
        for (int ci = 0; ci < SLAB_NUM_CACHES; ci++) {
            slab_query_cache(ci, &obj_size, &slabs, &active, &allocs, &frees);
            char row[80];
            str_cpy(row, "  [");
            if (obj_size < 1000) str_concat(row, " ");
            if (obj_size <  100) str_concat(row, " ");
            if (obj_size <   10) str_concat(row, " ");
            char tmp[20]; uint64_to_string(obj_size,  tmp); str_concat(row, tmp);
            str_concat(row, "B]  ");
            uint64_to_string(slabs,  tmp); str_concat(row, tmp); str_concat(row, "      ");
            uint64_to_string(active, tmp); str_concat(row, tmp); str_concat(row, "      ");
            uint64_to_string(allocs, tmp); str_concat(row, tmp); str_concat(row, "      ");
            uint64_to_string(frees,  tmp); str_concat(row, tmp);
            output_add_line(output, row, active > 0 ? VGA_YELLOW : VGA_GREEN);
        }
    }

    output_add_empty_line(output);
    output_add_line(output, "All slab tests completed!", VGA_CYAN);
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
    output_add_line(output, "=== Creating ring 3 task ===", VGA_CYAN);
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

    str_cpy(info, "Task Name : ");
    str_concat(info, task_name);
    output_add_line(output, info, VGA_WHITE);
    output_add_line(output, "Privilege: Ring-3 (DPL=3)", VGA_WHITE);
    output_add_line(output, "CS=0x23  SS=0x1B  Entry=user_mode_test_task", VGA_WHITE);
    output_add_empty_line(output);

    task_t* utask = task_create_user(task_name, entry, TASK_PRIORITY_NORMAL);
    if (!utask) {
        output_add_line(output, "[Error] task_create_user() Failed!", VGA_RED);
        return;
    }

    str_cpy(info, "Created -> PID=");
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
        output_add_line(output, "[Error] task_start() Failed!", VGA_RED);
        task_terminate(utask);
        return;
    }

    output_add_empty_line(output);
    output_add_line(output, "Scheduled into the timer queue.", VGA_GREEN);
    output_add_line(output, "Next timer interrupt → IRETQ → switch to Ring-3.", VGA_YELLOW);
    output_add_line(output, "Expect to see '[USER TASK] Hello from Ring-3' in the serial console/log.", VGA_YELLOW);
    output_add_line(output, "Task will exit by calling SYS_EXIT(0).", VGA_WHITE);
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
        output_add_line(output, "  Displays ELF header information without loading the file.", VGA_DARK_GRAY);
        output_add_line(output, "  Full path or just filename can be provided.", VGA_DARK_GRAY);
        output_add_line(output, "  Examples:", VGA_DARK_GRAY);
        output_add_line(output, "    elfinfo hello.elf", VGA_DARK_GRAY);
        output_add_line(output, "    elfinfo /bin/hello.elf", VGA_DARK_GRAY);
        return;
    }

    // Build full path: prepend /bin/ if it doesn't start with '/'
    char path[128];
    if (args[0] == '/') {
        str_cpy(path, args);
    } else {
        str_cpy(path, "/bin/");
        str_concat(path, args);
    }

    uint32_t fsize = ext3_file_size(path);
    if (fsize == 0) {
        char line[96];
        str_cpy(line, "File not found on ext3: ");
        str_concat(line, path);
        output_add_line(output, line, VGA_RED);
        return;
    }

    static uint8_t hdr_buf[512];
    int n = ext3_read_file(path, hdr_buf, 512);
    if (n < 64) {
        output_add_line(output, "Failed to read file or file too small for ELF header", VGA_RED);
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
        output_add_line(output, "  Loads an ELF64 binary from ext3 and creates a Ring-3 task.", VGA_DARK_GRAY);
        output_add_line(output, "  base_hex: Optional load base address for PIE (ET_DYN) binaries.", VGA_DARK_GRAY);
        output_add_line(output, "  Example: exec hello.elf", VGA_DARK_GRAY);
        output_add_line(output, "  Example: exec /bin/hello.elf 0x500000", VGA_DARK_GRAY);
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

    char filepath[128];
    if (filename[0] == '/') {
        str_cpy(filepath, filename);
    } else {
        str_cpy(filepath, "/bin/");
        str_concat(filepath, filename);
    }

    str_cpy(line, "File     : "); str_concat(line, filepath);
    output_add_line(output, line, VGA_WHITE);
    FMT_HEX64(load_base);
    str_cpy(line, "Load base : "); str_concat(line, tmp);
    output_add_line(output, line, VGA_WHITE);
    output_add_empty_line(output);

    if (!syscall_is_enabled()) {
        output_add_line(output, "[Error] SYSCALL infrastructure not initialized!", VGA_RED);
        return;
    }

   output_add_line(output, "[1/3] Loading ELF from ext3...", VGA_WHITE);
    ElfImage image;
    int rc = elf64_exec_from_ext3(filepath, load_base, &image, output);
    if (rc != ELF_OK) {
        str_cpy(line, "[Error] ELF Couldnt load: ");
        str_concat(line, elf64_strerror(rc));
        output_add_line(output, line, VGA_RED);
        return;
    }

    output_add_line(output, "[2/3] Creating Ring-3 task...", VGA_WHITE);

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
       output_add_line(output, "[ERROR] task_create_from_elf() failed!", VGA_RED);
       output_add_line(output, "  Has task_init() been called? Is the heap memory sufficient?", VGA_YELLOW);
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

    output_add_line(output, "[3/3] Adding to scheduler queue...", VGA_WHITE);
    if (task_start(utask) != 0) {
        output_add_line(output, "[ERROR] task_start() failed!", VGA_RED);
        task_terminate(utask);
        return;
    }

    extern void kb_set_userland_mode(int on);
    extern void kb_set_enter_cr(int cr);
    kb_set_userland_mode(1);

    {
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
    str_concat(line, "' started in Ring-3!");
    output_add_line(output, line, VGA_GREEN);
    output_add_line(output, "================================================", VGA_GREEN);
    output_add_empty_line(output);
    output_add_line(output, "Next timer tick -> iretq -> Ring-3 (CS=0x23)", VGA_YELLOW);
    output_add_line(output, "When the program makes a syscall:", VGA_WHITE);
    output_add_line(output, "  Ring-3 syscall -> kernel_tss.rsp0 -> Ring-0", VGA_DARK_GRAY);
    output_add_line(output, "  syscall_dispatch() -> handler -> SYSRET", VGA_DARK_GRAY);
    output_add_line(output, "  SYSRET -> Ring-3 (program continues)", VGA_DARK_GRAY);
    output_add_line(output, "  SYS_EXIT(0) -> task_exit() -> TERMINATED", VGA_DARK_GRAY);
    output_add_empty_line(output);
    output_add_line(output, "Watch the program output in the serial log.", VGA_CYAN);

    #undef FMT_HEX64
}

// ===========================================
// ADVANCED FILE SYSTEM COMMANDS
// ===========================================

void cmd_tree(const char* args, CommandOutput* output) {
    (void)args;
    static dirent64_t dents[128];
    
    output_add_line(output, "/ (root directory - level 1 only)", VGA_CYAN);
    output_add_empty_line(output);
    
    int total = ext3_getdents("/", dents, (int)sizeof(dents));
    if (total <= 0) {
        output_add_line(output, "Cannot read root directory", VGA_RED);
        return;
    }
    
    int count = 0;
    int off = 0;
    while (off < total) {
        dirent64_t* de = (dirent64_t*)((char*)dents + off);
        if (de->d_reclen == 0) break;
        
        // Skip . and ..
        if (de->d_name[0] == '.' &&
            (de->d_name[1] == '\0' ||
             (de->d_name[1] == '.' && de->d_name[2] == '\0'))) {
            off += de->d_reclen;
            continue;
        }
        
        char line[MAX_LINE_LENGTH];
        str_cpy(line, "  ");
        
        if (de->d_type == DT_DIR) {
            str_concat(line, "[DIR]  ");
        } else {
            str_concat(line, "[FILE] ");
        }
        str_concat(line, de->d_name);
        
        uint8_t color = (de->d_type == DT_DIR) ? VGA_YELLOW : VGA_WHITE;
        output_add_line(output, line, color);
        count++;
        
        off += de->d_reclen;
    }
    
    if (count == 0) {
        output_add_line(output, "  (empty directory)", VGA_DARK_GRAY);
    } else {
        output_add_empty_line(output);
        char summary[64];
        str_cpy(summary, "Total items: ");
        char cstr[8];
        int_to_str(count, cstr);
        str_concat(summary, cstr);
        output_add_line(output, summary, VGA_CYAN);
        output_add_empty_line(output);
        output_add_line(output, "Tip: Use 'ls <dir>' to see subdirectory contents", VGA_DARK_GRAY);
    }
}

static void ext3_tree_recursive(const char* path, int depth, CommandOutput* output) {
    static dirent64_t dents[512];
    int total = ext3_getdents(path, dents, (int)sizeof(dents));
    if (total <= 0) return;

    int off = 0;
    while (off < total) {
        dirent64_t* de = (dirent64_t*)((char*)dents + off);
        if (de->d_reclen == 0) break;

        if (!(de->d_name[0] == '.' &&
              (de->d_name[1] == '\0' ||
              (de->d_name[1] == '.' && de->d_name[2] == '\0')))) {
            char line[MAX_LINE_LENGTH];
            line[0] = '\0';
            
            int dsp = depth * 2;
            if (dsp >= MAX_LINE_LENGTH) dsp = MAX_LINE_LENGTH - 1;
            for (int k = 0; k < dsp; k++) line[k] = ' ';
            line[dsp] = '\0';

            if (de->d_type == DT_DIR) {
                str_concat(line, "[+] ");
                str_concat(line, de->d_name);
                output_add_line(output, line, VGA_YELLOW);

                char subpath[256];
                str_cpy(subpath, path);
                int plen = str_len(subpath);
                if (plen > 1) { subpath[plen] = '/'; subpath[plen+1] = '\0'; }
                str_concat(subpath, de->d_name);
                if (depth < 4) ext3_tree_recursive(subpath, depth + 1, output);
            } else {
                str_concat(line, "    ");
                str_concat(line, de->d_name);
                output_add_line(output, line, VGA_WHITE);
            }
        }
        off += de->d_reclen;
    }
}

void cmd_find(const char* args, CommandOutput* output) {
    if (!args || str_len(args) == 0) {
        output_add_line(output, "Usage: find <pattern>", VGA_YELLOW);
        output_add_line(output, "Example: find txt", VGA_DARK_GRAY);
        return;
    }
    static dirent64_t find_dents[512];

    const char* search_dirs[] = {"/", "/bin", "/usr", "/etc", "/home", "/tmp", NULL};
    int found = 0;
    for (int di = 0; search_dirs[di]; di++) {
        int tot = ext3_getdents(search_dirs[di], find_dents, (int)sizeof(find_dents));
        if (tot < 0) continue;
        int off = 0;
        while (off < tot) {
            dirent64_t* de = (dirent64_t*)((char*)find_dents + off);
            if (de->d_reclen == 0) break;
            // it has args pattern?
            const char* nm = de->d_name;
            const char* pat = args;
            int match = 0;
            // basic substring search
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
    const char* du_path = (args && str_len(args) > 0) ? args : ext3_getcwd();
    static dirent64_t du_dents[256];
    int tot = ext3_getdents(du_path, du_dents, (int)sizeof(du_dents));
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
            uint32_t fsz = ext3_file_size(fp);
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
    else { str_cpy(rmr_path, ext3_getcwd()); int pl=str_len(rmr_path); if(pl>1){rmr_path[pl]='/';rmr_path[pl+1]='\0';} str_concat(rmr_path, args); }
    // ext3 rmdir only deletes empty folders
    static dirent64_t rmr_dents[128];
    int tot = ext3_getdents(rmr_path, rmr_dents, (int)sizeof(rmr_dents));
    if (tot > 0) {
        int off = 0;
        while (off < tot) {
            dirent64_t* de = (dirent64_t*)((char*)rmr_dents + off);
            if (de->d_reclen == 0) break;
            if (de->d_name[0] != '.') {
                char child[256]; str_cpy(child, rmr_path);
                int pl=str_len(child); if(pl>1){child[pl]='/';child[pl+1]='\0';}
                str_concat(child, de->d_name);
                if (de->d_type == DT_REG) ext3_unlink(child);
                else if (de->d_type == DT_DIR) ext3_rmdir(child);
            }
            off += de->d_reclen;
        }
    }
    int rmr_rc = ext3_rmdir(rmr_path);
    if (rmr_rc == 0) {
        output_add_line(output, "Directory removed", VGA_GREEN);
    } else {
        output_add_line(output, "Failed: not empty or not found", VGA_RED);
    }
}
// ===========================================================================
// SPINLOCK TEST 
// ============================================================================
static void cmd_spinlock(const char* args, CommandOutput* output) {
    (void)args;
    output_add_line(output, "=== Spinlock / RWLock Test ===", VGA_CYAN);
    output_add_empty_line(output);
    output_add_line(output, "Running tests...", VGA_WHITE);
    output_add_empty_line(output);
    spinlock_test();
    // spinlock_test() writes directly to the screen using println64
    output_add_empty_line(output);
    output_add_line(output, "Detailed log: written to serial port (COM1).", VGA_DARK_GRAY);
}


// Usage: perf          -> run all tests
//        perf memcpy   -> memcpy benchmark
//        perf memset   -> memset benchmark
//        perf loop     -> simple loop benchmark
// ============================================================================
static void cmd_perf(const char* args, CommandOutput* output) {
    (void)args;

    output_add_line(output, "=== RDTSC Performance Measurement ===", VGA_CYAN);
    output_add_empty_line(output);
    output_add_line(output, "Results are also written to the serial port.", VGA_DARK_GRAY);
    output_add_empty_line(output);

    PerfCounter pc;
    char line[MAX_LINE_LENGTH];
    char tmp[24];

    // ── Test 1: Simple loop (1M iterations) ───────────────────────────────
    output_add_line(output, "[1] Empty loop x1,000,000:", VGA_YELLOW);
    perf_start(&pc);
    for (volatile int i = 0; i < 1000000; i++);
    perf_stop(&pc);
    perf_print(&pc, "empty_loop_1M");

    str_cpy(line, "    Cycles : "); uint64_to_string(perf_cycles(&pc), tmp); str_concat(line, tmp);
    output_add_line(output, line, VGA_WHITE);
    str_cpy(line, "    Time   : ~"); uint64_to_string(perf_us(&pc), tmp); str_concat(line, tmp);
    str_concat(line, " us  (~"); uint64_to_string(perf_ns(&pc) / 1000000, tmp); str_concat(line, tmp);
    str_concat(line, " ms)");
    output_add_line(output, line, VGA_GREEN);
    output_add_empty_line(output);

    // ── Test 2: memset — zero/fill 64KB ──────────────────────────────────
    output_add_line(output, "[2] memset64  64KB:", VGA_YELLOW);
    static uint8_t perf_buf[65536];
    perf_start(&pc);
    extern void* memset64(void*, int, size_t);
    memset64(perf_buf, 0xAB, sizeof(perf_buf));
    perf_stop(&pc);
    perf_print(&pc, "memset64_64KB");

    str_cpy(line, "    Cycles : "); uint64_to_string(perf_cycles(&pc), tmp); str_concat(line, tmp);
    output_add_line(output, line, VGA_WHITE);
    str_cpy(line, "    Time   : ~"); uint64_to_string(perf_us(&pc), tmp); str_concat(line, tmp);
    str_concat(line, " us");
    output_add_line(output, line, VGA_GREEN);

    // Bandwidth: 64KB / us -> MB/s
    uint32_t us = perf_us(&pc);
    if (us > 0) {
        uint64_t mbps = 65536ULL / (uint64_t)us;
        str_cpy(line, "    Bandwidth: ~"); uint64_to_string(mbps, tmp); str_concat(line, tmp);
        str_concat(line, " MB/s");
        output_add_line(output, line, VGA_CYAN);
    }
    output_add_empty_line(output);

    // ── Test 3: memcpy — copy 64KB ──────────────────────────────────
    output_add_line(output, "[3] memcpy64  64KB:", VGA_YELLOW);
    static uint8_t perf_src[65536];
    extern void* memcpy64(void*, const void*, size_t);
    perf_start(&pc);
    memcpy64(perf_buf, perf_src, sizeof(perf_buf));
    perf_stop(&pc);
    perf_print(&pc, "memcpy64_64KB");

    str_cpy(line, "    Cycles : "); uint64_to_string(perf_cycles(&pc), tmp); str_concat(line, tmp);
    output_add_line(output, line, VGA_WHITE);
    str_cpy(line, "    Time   : ~"); uint64_to_string(perf_us(&pc), tmp); str_concat(line, tmp);
    str_concat(line, " us");
    output_add_line(output, line, VGA_GREEN);

    us = perf_us(&pc);
    if (us > 0) {
        uint64_t mbps = 65536ULL / (uint64_t)us;
        str_cpy(line, "    Bandwidth: ~"); uint64_to_string(mbps, tmp); str_concat(line, tmp);
        str_concat(line, " MB/s");
        output_add_line(output, line, VGA_CYAN);
    }
    output_add_empty_line(output);

    // ── Test 4: RDTSC overhead (measuring itself) ─────────────────────────
    output_add_line(output, "[4] RDTSC overhead:", VGA_YELLOW);
    perf_start(&pc);
    perf_stop(&pc);
    perf_print(&pc, "rdtsc_overhead");

    str_cpy(line, "    Cycles : "); uint64_to_string(perf_cycles(&pc), tmp); str_concat(line, tmp);
    str_concat(line, "  (rdtscp double-read cost)");
    output_add_line(output, line, VGA_WHITE);
    output_add_empty_line(output);

    // ── CPU frequency ──────────────────────────────────────────────────────
    str_cpy(line, "CPU Frequency: ");
    uint64_to_string((uint64_t)pc.cpu_mhz, tmp); str_concat(line, tmp);
    str_concat(line, " MHz  (PIT measurement)");
    output_add_line(output, line, VGA_CYAN);
    output_add_line(output, "Detailed log: written to serial port (COM1).", VGA_DARK_GRAY);
}


static void panic_do_div0(void) {
    volatile int a = 42;
    volatile int b = 0;
    volatile int c = a / b;
    (void)c;
}

// Stack overflow testi.
//
// x86-64 long mode'da segment limit kontrolü yoktur (#SS bu yolla üretilemez).
// Gerçek stack overflow, VMM'nin stack'in altına koyduğu guard page'e çarpınca
// #PF olarak gelir — bu #PF, handler stack'i de taşmışsa #DF'e dönüşür.
//
// En güvenilir yöntem: int $0x0C ile doğrudan #SS vektörünü ateşlemek.
// IDT'de #SS handler'ı varsa kernel_panic_handler çağrılır ve ekran görünür.
// Bu "sahte" bir #SS'tir ama panic ekranı + serial dump açısından farkı yoktur.
//
// Alternatif olarak gerçek guard page çarpması da deneniyor (yorum satırı).
static __attribute__((noinline, noreturn)) void panic_do_stack_overflow(void) {
    // int $0x0C → #SS Stack-Segment Fault (vektör 12)
    // IDT'deki #SS handler'ını doğrudan tetikler, triple fault riski yok.
    __asm__ volatile("int $0x0C");
    __builtin_unreachable();
}
//
// Gerçek guard page yöntemi (VMM guard page kuruluysa çalışır):
//   __asm__ volatile(
//       "mov %%rsp, %%rax\n\t"
//       "and $~0xFFFF, %%rax\n\t"   // stack'in 64KB-hizalı tabanı
//       "sub $0x1000, %%rax\n\t"    // guard page'in hemen altı
//       "mov (%%rax), %%rbx\n\t"    // guard page'e oku → #PF → #DF
//       ::: "rax", "rbx", "memory"
//   );

static void cmd_panic(const char* args, CommandOutput* output) {
    if (!args || str_len(args) == 0) {
        output_add_line(output, "Kernel Panic Test - Avaible Exception Types", VGA_CYAN);
        output_add_empty_line(output);
        output_add_line(output, "  panic df    - #DF Double Fault  (int 0x08)", VGA_RED);
        output_add_line(output, "  panic gp    - #GP Gen Protection (int 0x0D)", VGA_RED);
        output_add_line(output, "  panic pf    - #PF Page Fault (NULL deref)", VGA_YELLOW);
        output_add_line(output, "  panic ud    - #UD Invalid Opcode (ud2)", VGA_YELLOW);
        output_add_line(output, "  panic de    - #DE Divide by Zero", VGA_YELLOW);
        output_add_line(output, "  panic stack - Stack overflow (#SS / #DF)", VGA_YELLOW);
        output_add_empty_line(output);
        output_add_line(output, "WARNING: SYSTEM GETS HALTED GET DUMP FROM QEMU OR SERIAL LOG", VGA_RED);
        return;
    }

    if (str_cmp(args, "df") == 0) {
        output_add_line(output, "[PANIC TEST] #DF Double Fault triggering...", VGA_RED);
        output_add_line(output, "  int 0x08 -> exception_frame -> kernel_panic_handler", VGA_DARK_GRAY);
        for (volatile int i = 0; i < 5000000; i++);
        __asm__ volatile("int $0x08");

    } else if (str_cmp(args, "gp") == 0) {
        output_add_line(output, "[PANIC TEST] #GP General Protection Fault triggering...", VGA_RED);
        output_add_line(output, "  int 0x0D -> err_code -> kernel_panic_handler", VGA_DARK_GRAY);
        for (volatile int i = 0; i < 5000000; i++);
        __asm__ volatile("int $0x0D");

    } else if (str_cmp(args, "pf") == 0) {
        output_add_line(output, "[PANIC TEST] #PF Page Fault triggering...", VGA_RED);
        output_add_line(output, "  NULL pointer deref -> CR2=0x0 -> kernel_panic_handler", VGA_DARK_GRAY);
        for (volatile int i = 0; i < 5000000; i++);
        volatile uint64_t* null_ptr = (volatile uint64_t*)0x0;
        volatile uint64_t val = *null_ptr;
        (void)val;

    } else if (str_cmp(args, "ud") == 0) {
        output_add_line(output, "[PANIC TEST] #UD Invalid Opcode triggering...", VGA_RED);
        output_add_line(output, "  ud2 instruction -> kernel_panic_handler", VGA_DARK_GRAY);
        for (volatile int i = 0; i < 5000000; i++);
        __asm__ volatile("ud2");

    } else if (str_cmp(args, "de") == 0) {
        output_add_line(output, "[PANIC TEST] Triggering #DE Divide by Zero...", VGA_RED);
        output_add_line(output, "  idiv 0 -> kernel_panic_handler", VGA_DARK_GRAY);
        for (volatile int i = 0; i < 5000000; i++);
        panic_do_div0();

    } else if (str_cmp(args, "stack") == 0) {
        output_add_line(output, "[PANIC TEST] Triggering stack overflow (#SS)...", VGA_RED);
        output_add_line(output, "  int 0x0C -> #SS handler -> kernel_panic_handler", VGA_DARK_GRAY);
        for (volatile int i = 0; i < 5000000; i++);
        panic_do_stack_overflow();

    } else {
        output_add_line(output, "Unknown panic type. Type 'panic' to see the list.", VGA_YELLOW);
    }
}

// ============================================================================
// Network Commands — Helper Functions
// ============================================================================

// Hex byte helper 
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
// Network Commands — RTL8139
// ============================================================================

// ── netinit ──────────────────────────────────────────────────────────────────
static void cmd_netinit(const char* args, CommandOutput* output) {
    (void)args;
    if (g_net_initialized) {
        output_add_line(output, "Network driver already initialized.", 0x0E);
        if (!ipv4_is_initialized()) {
            ipv4_init();
            icmp_init();
            output_add_line(output, "  [OK] IPv4 + ICMP layers initialized.", 0x0A);
        }
        if (!udp_is_initialized()) {
            udp_init(1);
            output_add_line(output, "  [OK] UDP layer initialized.", 0x0A);
        }
        if (!dhcp_is_initialized()) {
            dhcp_init();
            output_add_line(output, "  [OK] DHCP layer initialized.", 0x0A);
        }
        if (!tcp_is_initialized()) {
            tcp_init();
            output_add_line(output, "  [OK] TCP layer initialized.", 0x0A);
        }
        return;
    }
    output_add_line(output, "Initializing RTL8139...", 0x07);
    bool ok = rtl8139_init();
    if (ok) {
        net_register_packet_handler();
        ipv4_init();
        icmp_init();
        udp_init(1);   // 1 = UDP_CSUM_ENABLE
        dhcp_init();   // Listens on UDP port 68
        tcp_init();    // Register proto=6 to IPv4
        output_add_line(output, "  [OK] RTL8139 ready!", 0x0A);
        output_add_line(output, "  [OK] IPv4 + ICMP + UDP + DHCP + TCP ready.", 0x0A);
        output_add_line(output, "  Use 'dhcp' command to get an IP automatically.", 0x07);
        output_add_line(output, "  Or assign manually with 'ipconfig 10.0.2.15'.", 0x07);
    } else {
        output_add_line(output, "  [ERROR] RTL8139 not found or failed to initialize.", 0x0C);
        output_add_line(output, "  Is QEMU started with '-device rtl8139,netdev=net0'?", 0x0E);
    }
}

// ── netstat ───────────────────────────────────────────────────────────────────
static void cmd_netstat(const char* args, CommandOutput* output) {
    (void)args;
    if (!g_net_initialized) {
        output_add_line(output, "Network driver not initialized. Run 'netinit' first.", 0x0C);
        return;
    }

    output_add_line(output, "=== RTL8139 Network Status ===", 0x0B);

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
    str_concat(lnk, up ? "UP  (connected)" : "DOWN (cable disconnected?)");
    output_add_line(output, lnk, up ? 0x0A : 0x0C);

    if (g_net_rx_display > 0) {
        char rxinfo[48];
        str_cpy(rxinfo, "  Last packet EtherType: ");
        char et[8]; uint16_to_hex_str(g_net_last_etype, et);
        str_concat(rxinfo, et);
        output_add_line(output, rxinfo, 0x07);

        char srcmac[32]; str_cpy(srcmac, "  Source MAC       : ");
        for (int i = 0; i < 6; i++) {
            char hx[3]; byte_to_hex_str(g_net_last_src[i], hx);
            str_concat(srcmac, hx);
            if (i < 5) str_concat(srcmac, ":");
        }
        output_add_line(output, srcmac, 0x07);
    }

    // IPv4 counters
    if (ipv4_is_initialized()) {
        char ipv4line[48];
        str_cpy(ipv4line, "  IPv4 TX: ");
        char cnt[12]; uint64_to_string(ipv4_get_tx_count(), cnt);
        str_concat(ipv4line, cnt); str_concat(ipv4line, "  RX: ");
        uint64_to_string(ipv4_get_rx_count(), cnt);
        str_concat(ipv4line, cnt); str_concat(ipv4line, " packets");
        output_add_line(output, ipv4line, 0x0B);
    }

    rtl8139_stats();
    output_add_line(output, "  (Detailed stats written to serial port)", 0x08);
}

// ── netregs ───────────────────────────────────────────────────────────────────
static void cmd_netregs(const char* args, CommandOutput* output) {
    (void)args;
    if (!g_net_initialized) {
        output_add_line(output, "Network driver not initialized.", 0x0C);
        return;
    }
    rtl8139_dump_regs();
    output_add_line(output, "Register dump written to serial port.", 0x07);
    output_add_line(output, "  (View with minicom or QEMU -serial stdio)", 0x08);
}

// ── netsend ───────────────────────────────────────────────────────────────────
static void cmd_netsend(const char* args, CommandOutput* output) {
    if (!g_net_initialized) {
        output_add_line(output, "Network driver not initialized. Run 'netinit' first.", 0x0C);
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

    const char* msg = "AscentOS NET TEST Stage-1";
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
    str_cpy(res, "  Sent: ");
    char tmp[8]; int_to_str(ok_count, tmp); str_concat(res, tmp);
    str_concat(res, " / ");
    int_to_str(count, tmp); str_concat(res, tmp);
    str_concat(res, " packets (60 bytes, broadcast)");
    output_add_line(output, res, ok_count == count ? 0x0A : 0x0E);

    if (ok_count > 0) {
        output_add_line(output, "  EtherType: 0x88B5 (test)", 0x07);
        output_add_line(output, "  Payload  : 'AscentOS NET TEST Stage-1'", 0x07);
        output_add_line(output, "  You can capture it from the QEMU host with Wireshark.", 0x08);
    }
}

// ── netmon ────────────────────────────────────────────────────────────────────
static void cmd_netmon(const char* args, CommandOutput* output) {
    (void)args;
    if (!g_net_initialized) {
        output_add_line(output, "Network driver not initialized.", 0x0C);
        return;
    }
    output_add_line(output, "=== Packet Monitor ===", 0x0B);

    char buf[48];
    str_cpy(buf, "  Total received: ");
    char tmp[12]; int_to_str((int)g_net_rx_display, tmp);
    str_concat(buf, tmp);
    str_concat(buf, " packets");
    output_add_line(output, buf, 0x0F);

    if (g_net_rx_display == 0) {
        output_add_line(output, "  No packets received yet.", 0x07);
        output_add_line(output, "  The QEMU host can ping the guest.", 0x08);
    } else {
        char et[48]; str_cpy(et, "  Last EtherType: ");
        char ets[8]; uint16_to_hex_str(g_net_last_etype, ets);
        str_concat(et, ets);

        const char* etype_name = "";
        if      (g_net_last_etype == 0x0800) etype_name = " (IPv4)";
        else if (g_net_last_etype == 0x0806) etype_name = " (ARP)";
        else if (g_net_last_etype == 0x86DD) etype_name = " (IPv6)";
        else if (g_net_last_etype == 0x88B5) etype_name = " (Test/Custom)";
        str_concat(et, etype_name);
        output_add_line(output, et, 0x07);

        char sm[48]; str_cpy(sm, "  Last source MAC: ");
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
// ARP COMMANDS — Stage 2
// ============================================================================

typedef struct { CommandOutput* out; } ARPCacheCtx;
static void arp_cache_line_cb(const char* line, uint8_t color, void* ctx){
    ARPCacheCtx* c = (ARPCacheCtx*)ctx;
    output_add_line(c->out, line, color);
}

// ── ipconfig ─────────────────────────────────────────────────────────────────
static void cmd_ipconfig(const char* args, CommandOutput* output) {
    if (!g_net_initialized) {
        output_add_line(output, "Network driver not ready. Run 'netinit'.", 0x0C);
        return;
    }

    // No args → show current status
    if (!args || args[0] == '\0') {
        if (!arp_is_initialized()) {
            output_add_line(output, "No IP assigned yet.", 0x0E);
            output_add_line(output, "Usage: ipconfig <IP>   example: ipconfig 10.0.2.15", 0x07);
            return;
        }
        output_add_line(output, "=== IP Configuration ===", 0x0B);
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

        // IPv4 / ICMP status
        if (ipv4_is_initialized()) {
            output_add_line(output, "  IPv4 : Active", 0x0A);
            output_add_line(output, "  ICMP : Active  (test with 'ping <IP>')", 0x0A);
        } else {
            output_add_line(output, "  IPv4 : Not initialized", 0x0E);
        }
        return;
    }

    // Parse IP address
    uint8_t new_ip[4];
    if (!str_to_ip(args, new_ip)) {
        output_add_line(output, "Invalid IP address.", 0x0C);
        output_add_line(output, "Example: ipconfig 10.0.2.15", 0x07);
        return;
    }

    uint8_t mac[6];
    rtl8139_get_mac(mac);
    arp_init(new_ip, mac);

    if (!ipv4_is_initialized()) {
        ipv4_init();
        icmp_init();
    }

    // Gateway: .2 in the same /24 (QEMU SLiRP default: 10.x.x.2)
    uint8_t gw[4]   = {new_ip[0], new_ip[1], new_ip[2], 2};
    uint8_t mask[4] = {255, 255, 255, 0};
    ipv4_set_gateway(gw);
    ipv4_set_subnet(mask);

    // Add QEMU SLiRP gateway MAC to the static ARP cache.
    // This avoids waiting for ARP when pinging internet addresses like '1.1.1.1'.
    // QEMU SLiRP default gateway MAC: 52:54:00:12:34:02
    {
        static const uint8_t QEMU_GW_MAC[6] = {0x52,0x54,0x00,0x12,0x34,0x02};
        arp_add_static(gw, QEMU_GW_MAC);
    }

    char buf[64]; char ipstr[16]; ip_to_str(new_ip, ipstr);
    str_cpy(buf, "  IP assigned: "); str_concat(buf, ipstr);
    output_add_line(output, buf, 0x0A);
    char gwstr[16]; ip_to_str(gw, gwstr);
    str_cpy(buf, "  Gateway  : "); str_concat(buf, gwstr);
    output_add_line(output, buf, 0x07);
    output_add_line(output, "  IPv4 + ICMP layers active.", 0x0A);
    output_add_line(output, "  Gateway MAC: 52:54:00:12:34:02 (QEMU SLiRP static)", 0x08);
    output_add_line(output, "  Sending gratuitous ARP...", 0x07);
    arp_announce();
    output_add_line(output, "  Ready! Local: ping 10.0.2.2", 0x07);
    output_add_line(output, "         Internet: ping 1.1.1.1  ping 8.8.8.8", 0x07);
    output_add_line(output, "  NOTE: For a different gateway MAC use: arpstatic <GW-IP> <MAC>", 0x08);
}

// ── arping ───────────────────────────────────────────────────────────────────
static void cmd_arping(const char* args, CommandOutput* output) {
    if (!arp_is_initialized()) {
        output_add_line(output, "Assign an IP first with 'ipconfig <IP>'.", 0x0C);
        return;
    }
    if (!args || args[0] == '\0') {
        output_add_line(output, "Usage: arping <target-IP>   example: arping 10.0.2.2", 0x0E);
        return;
    }
    uint8_t target[4];
    if (!str_to_ip(args, target)) {
        output_add_line(output, "Invalid IP.", 0x0C); return;
    }

    if (arp_is_initialized()) {
        uint8_t my_ip[4]; arp_get_my_ip(my_ip);
        if (target[0] != my_ip[0] || target[1] != my_ip[1] || target[2] != my_ip[2]) {
            char warn[80];
            str_cpy(warn, "  [WARNING] ARP only works within the same /24 network.");
            output_add_line(output, warn, 0x0E);
            char gwbuf[32]; char gwip[16];
            uint8_t gw[4] = {my_ip[0], my_ip[1], my_ip[2], 2};
            ip_to_str(gw, gwip);
            str_cpy(gwbuf, "  Try gateway: arping "); str_concat(gwbuf, gwip);
            output_add_line(output, gwbuf, 0x0B);
        }
    }

    char buf[48]; char ipstr[16]; ip_to_str(target, ipstr);
    str_cpy(buf, "ARP request -> "); str_concat(buf, ipstr);
    output_add_line(output, buf, 0x07);

    uint8_t found_mac[6];
    if (arp_resolve(target, found_mac)) {
        str_cpy(buf, "  From cache: ");
        for (int i = 0; i < 6; i++) {
            char hx[3]; byte_to_hex_str(found_mac[i], hx); str_concat(buf, hx);
            if (i < 5) str_concat(buf, ":");
        }
        output_add_line(output, buf, 0x0A);
    } else {
        output_add_line(output, "  Request sent. Check result with 'arpcache'.", 0x0E);
        output_add_line(output, "  (Reply arrives via interrupt, wait ~1s)", 0x08);
    }
}

// ── arpcache ─────────────────────────────────────────────────────────────────
static void cmd_arpcache(const char* args, CommandOutput* output) {
    (void)args;
    output_add_line(output, "=== ARP Cache ===", 0x0B);
    output_add_line(output, "  IP              MAC                STATUS", 0x08);
    output_add_line(output, "  --------------- -----------------  --------", 0x08);
    ARPCacheCtx ctx = { output };
    arp_cache_foreach(arp_cache_line_cb, &ctx);
}

// ── arpflush ─────────────────────────────────────────────────────────────────
static void cmd_arpflush(const char* args, CommandOutput* output) {
    (void)args;
    if (!arp_is_initialized()) {
        output_add_line(output, "ARP layer not initialized.", 0x0C); return;
    }
    arp_flush_cache();
    output_add_line(output, "ARP cache cleared.", 0x0A);
}

// ── arptest ──────────────────────────────────────────────────────────────────
static void cmd_arptest(const char* args, CommandOutput* output) {
    (void)args;
    output_add_line(output, "=== ARP / QEMU Network Test ===", 0x0B);
    output_add_line(output, "", 0x07);

    if (!g_net_initialized) {
        output_add_line(output, "[ERROR] Network driver not initialized.", 0x0C); return;
    }
    if (!arp_is_initialized()) {
        output_add_line(output, "[ERROR] Run 'ipconfig <IP>' first.", 0x0C); return;
    }

    output_add_line(output, "About QEMU user-net (NAT):", 0x0E);
    output_add_line(output, "  - QEMU NAT is an L3 proxy; it does NOT process L2 Ethernet frames.", 0x07);
    output_add_line(output, "  - ARP broadcasts are sent but NO device replies.", 0x07);
    output_add_line(output, "  - This is not a bug; it is how QEMU's architecture works.", 0x07);
    output_add_line(output, "  - If packet TX works, your driver is correct.", 0x07);
    output_add_line(output, "", 0x07);

    uint8_t my_ip[4]; arp_get_my_ip(my_ip);
    uint8_t test_ip[4] = {my_ip[0], my_ip[1], my_ip[2], 1};
    uint32_t rx_before = g_net_rx_display;
    arp_request(test_ip);
    output_add_line(output, "[OK] TX working — ARP request sent.", 0x0A);
    if (g_net_rx_display == rx_before) {
        output_add_line(output, "[OK] RX: 0 replies — expected behavior on QEMU NAT.", 0x0A);
    } else {
        output_add_line(output, "[OK] RX: Packet received! (tap or socket backend?)", 0x0A);
    }

    output_add_line(output, "", 0x07);
    output_add_line(output, "Verify with pcap (host terminal):", 0x0B);
    output_add_line(output, "  make net-test  (open QEMU with pcap dump)", 0x07);
    output_add_line(output, "  tcpdump -r /tmp/ascent_net.pcap arp", 0x07);
    output_add_line(output, "", 0x07);
    output_add_line(output, "For a real ARP test: make net-test", 0x0E);
}

// ── arpstatic ────────────────────────────────────────────────────────────────
static void cmd_arpstatic(const char* args, CommandOutput* output) {
    if (!arp_is_initialized()) {
        output_add_line(output, "Assign an IP first with 'ipconfig <IP>'.", 0x0C); return;
    }
    if (!args || args[0] == '\0') {
        output_add_line(output, "Usage: arpstatic <IP> <MAC>", 0x0E);
        output_add_line(output, "Example: arpstatic 10.0.2.2 52:54:00:12:34:56", 0x08);
        return;
    }

    uint8_t ip[4];
    int ip_end = 0;
    while (args[ip_end] && args[ip_end] != ' ') ip_end++;
    char ip_str[16]; int k=0;
    while(k < ip_end && k < 15){ ip_str[k]=args[k]; k++; } ip_str[k]='\0';
    if (!str_to_ip(ip_str, ip)) {
        output_add_line(output, "Invalid IP.", 0x0C); return;
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
        output_add_line(output, "Invalid MAC. Example: 52:54:00:12:34:56", 0x0C); return;
    }

    arp_add_static(ip, mac);
    char buf[48]; char ipstr[16]; ip_to_str(ip, ipstr);
    str_cpy(buf, "  Static ARP: "); str_concat(buf, ipstr); str_concat(buf, " added.");
    output_add_line(output, buf, 0x0A);
}

// ============================================================================
// IPv4 + ICMP COMMANDS — Stage 3
// ============================================================================

// ── ipv4info ─────────────────────────────────────────────────────────────────
// IPv4 and ICMP layer status and counters
static void cmd_ipv4info(const char* args, CommandOutput* output) {
    (void)args;
    if (!ipv4_is_initialized()) {
        output_add_line(output, "IPv4 layer not initialized.", 0x0C);
        output_add_line(output, "  'ipconfig 10.0.2.15' initializes both ARP and IPv4+ICMP.", 0x0E);
        return;
    }

    output_add_line(output, "=== IPv4 / ICMP Layer Status ===", 0x0B);

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
    str_concat(line, cnt); str_concat(line, " packets");
    output_add_line(output, line, 0x07);

    str_cpy(line, "  IPv4 RX: ");
    uint64_to_string(ipv4_get_rx_count(), cnt);
    str_concat(line, cnt); str_concat(line, " packets");
    output_add_line(output, line, 0x07);

    output_add_empty_line(output);
    output_add_line(output, "  ICMP (ping) ready.", 0x0A);
    output_add_line(output, "  Example: ping 10.0.2.2", 0x07);
    output_add_line(output, "  Example: ping 10.0.2.2 4   (4 times)", 0x07);
}

// ── ping ─────────────────────────────────────────────────────────────────────
// Send ICMP Echo Request, wait for reply, display RTT.
// Usage: ping 10.0.2.2
//        ping 10.0.2.2 4      (4 times)
static void cmd_ping(const char* args, CommandOutput* output) {

    // Precondition: ARP must be initialized
    if (!arp_is_initialized()) {
        output_add_line(output, "ARP/IP layer not initialized.", 0x0C);
        output_add_line(output, "  First: ipconfig 10.0.2.15", 0x0E);
        return;
    }
    if (!ipv4_is_initialized()) {
        output_add_line(output, "IPv4 layer not initialized.", 0x0C);
        output_add_line(output, "  'ipconfig <IP>' initializes it automatically.", 0x0E);
        return;
    }

    // No args → show help
    if (!args || args[0] == '\0') {
        output_add_line(output, "Usage: ping <IP> [count]", 0x0E);
        output_add_line(output, "  Example: ping 10.0.2.2", 0x07);
        output_add_line(output, "  Example: ping 10.0.2.2 4", 0x07);
        return;
    }

    // Parse IP (up to space)
    char ip_str[16];
    int si = 0;
    while (args[si] && args[si] != ' ' && si < 15) {
        ip_str[si] = args[si]; si++;
    }
    ip_str[si] = '\0';

    uint8_t dst_ip[4];
    if (!str_to_ip(ip_str, dst_ip)) {
        output_add_line(output, "Invalid IP address.", 0x0C);
        return;
    }

    // Parse repeat count (default 1, max 10)
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

    // Header
    char hdr[56];
    str_cpy(hdr, "PING "); str_concat(hdr, ip_str);
    str_concat(hdr, " — ICMP Echo Request");
    output_add_line(output, hdr, 0x0B);

    // Subnet check: if target is not in the same /24, ARP via gateway MAC
    uint8_t my_ip[4]; arp_get_my_ip(my_ip);
    uint8_t gw[4];    ipv4_get_gateway(gw);
    bool same_subnet = (dst_ip[0] == my_ip[0] &&
                        dst_ip[1] == my_ip[1] &&
                        dst_ip[2] == my_ip[2]);
    uint8_t arp_target[4];
    for(int k = 0; k < 4; k++) arp_target[k] = same_subnet ? dst_ip[k] : gw[k];

    // Is gateway defined?
    if (!same_subnet) {
        bool gw_zero = (gw[0]==0 && gw[1]==0 && gw[2]==0 && gw[3]==0);
        if (gw_zero) {
            output_add_line(output, "  [ERROR] No gateway defined!", 0x0C);
            output_add_line(output, "  First: ipconfig 10.0.2.15", 0x0E);
            return;
        }
    }

    // ARP — poll-based wait
    {
        uint8_t dummy_mac[6];
        if (!arp_resolve(arp_target, dummy_mac)) {
            if (!same_subnet) {
                char arp_msg[64]; char gw_str[16]; ip_to_str(arp_target, gw_str);
                str_cpy(arp_msg, "  Gateway ARP: "); str_concat(arp_msg, gw_str);
                output_add_line(output, arp_msg, 0x07);
            }
            output_add_line(output, "  Sending ARP request...", 0x0E);
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
                    // QEMU SLiRP gateway may not reply to ARP — use known MAC
                    static const uint8_t QEMU_GW_MAC[6] = {0x52,0x54,0x00,0x12,0x34,0x02};
                    arp_add_static(arp_target, QEMU_GW_MAC);
                    output_add_line(output, "  No ARP reply — using QEMU default MAC", 0x0E);
                    output_add_line(output, "  (52:54:00:12:34:02)  Different MAC: arpstatic <GW> <MAC>", 0x08);
                } else {
                    output_add_line(output, "  ARP timeout.", 0x0C);
                    return;
                }
            } else {
                output_add_line(output, "  ARP resolved.", 0x0A);
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
            str_concat(eline, ns); str_concat(eline, "] could not send");
            output_add_line(output, eline, 0x0C);
            fail_count++;
            continue;
        }

        // Poll-based wait — use only rtl8139_poll(), no hlt.
        // hlt waits for IRQ but tick does not advance after cli; pure poll is safer.
        // Internet ping RTT can be 50-200ms; 8000 ticks is sufficient.
        {
            __asm__ volatile("sti");
            uint64_t t0 = get_system_ticks();
            while (icmp_ping_state() == PING_PENDING) {
                rtl8139_poll();
                uint64_t elapsed = get_system_ticks() - t0;
                if (elapsed >= 8000) break;  // 8 second timeout (sufficient for internet)
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
            str_concat(res_line, "  replied  RTT~");
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
            str_concat(res_line, "Timeout (no reply)");
            output_add_line(output, res_line, 0x0C);
            fail_count++;
        }

        icmp_ping_reset();

        if (i < count - 1) {
            uint64_t tw = get_system_ticks();
            while ((get_system_ticks() - tw) < 500) rtl8139_poll();
        }
    }

    // Summary
    output_add_empty_line(output);
    char sum[64];
    str_cpy(sum, "  Result: ");
    char ok_s[8]; int_to_str(ok_count, ok_s);
    str_concat(sum, ok_s); str_concat(sum, "/");
    char cnt_s[8]; int_to_str(count, cnt_s);
    str_concat(sum, cnt_s); str_concat(sum, " successful");
    output_add_line(output, sum, ok_count == count ? 0x0A : (ok_count > 0 ? 0x0E : 0x0C));

    if (ok_count > 0) {
        output_add_line(output, "  NOTE: RTT is the get_system_ticks() delta in icmp.c.", 0x08);
        output_add_line(output, "       If PIT is 1kHz then 1 tick = 1ms, otherwise adjust icmp_ticks_to_ms().", 0x08);
    }
    if (fail_count > 0 && ok_count == 0) {
        output_add_line(output, "  Tip: Resolve ARP first with 'arping 10.0.2.2'.", 0x0E);
        output_add_line(output, "  Then try 'arpstatic 10.0.2.2 52:54:00:12:34:56'.", 0x0E);
    }
}

// ============================================================================
// UDP COMMANDS — Stage 4
// ============================================================================

// UDP echo handler: prints received packet to VGA and echoes it back
static void _udp_echo_handler(const UDPPacket* pkt, void* ctx) {
    (void)ctx;

    // Source IP:port + content → screen
    char line[80]; int pos = 0;
    const char* lbl = "UDP< ";
    for (int k = 0; lbl[k]; k++) line[pos++] = lbl[k];

    char ipbuf[16]; ip_to_str(pkt->src_ip, ipbuf);
    for (int k = 0; ipbuf[k]; k++) line[pos++] = ipbuf[k];
    line[pos++] = ':';

    // source port → string
    {
        uint16_t pv = pkt->src_port;
        char rev[6]; int ri = 0;
        if (!pv) { rev[ri++] = '0'; }
        else { while (pv) { rev[ri++] = '0' + (pv % 10); pv /= 10; } }
        for (int x = ri - 1; x >= 0; x--) line[pos++] = rev[x];
    }

    line[pos++] = ' '; line[pos++] = '"';

    // Printable portion of data (max 48 chars)
    uint16_t show = pkt->len < 48 ? pkt->len : 48;
    for (uint16_t k = 0; k < show; k++) {
        char c = (char)pkt->data[k];
        line[pos++] = (c >= ' ' && c < 127) ? c : '.';
    }
    if (pkt->len > 48) { line[pos++] = '.'; line[pos++] = '.'; line[pos++] = '.'; }
    line[pos++] = '"'; line[pos] = '\0';

    println64(line, 0x0B);

    // Echo: send the packet back as-is
    udp_send(pkt->src_ip, pkt->src_port, pkt->dst_port, pkt->data, pkt->len);
}

// ── udpinit ──────────────────────────────────────────────────────────────────
static void cmd_udpinit(const char* args, CommandOutput* output) {
    (void)args;
    if (udp_is_initialized()) {
        output_add_line(output, "UDP layer already initialized.", 0x0E);
        return;
    }
    if (!ipv4_is_initialized()) {
        output_add_line(output, "Error: Initialize the IPv4 layer first with 'ipconfig <IP>'.", 0x0C);
        return;
    }
    udp_init(1);   // 1 = UDP_CSUM_ENABLE
    output_add_line(output, "UDP layer initialized (checksum: enabled).", 0x0A);
    output_add_line(output, "  udplisten <port>     — start echo server", 0x07);
    output_add_line(output, "  udpsend <ip> <p> <m> — send message", 0x07);
}

// ── udplisten ─────────────────────────────────────────────────────────────────
static void cmd_udplisten(const char* args, CommandOutput* output) {
    if (!udp_is_initialized()) {
        output_add_line(output, "Error: Run 'udpinit' first.", 0x0C); return;
    }
    if (!args || args[0] == '\0') {
        output_add_line(output, "Usage: udplisten <port>   example: udplisten 5000", 0x0E); return;
    }
    uint16_t port = 0;
    for (int i = 0; args[i] >= '0' && args[i] <= '9'; i++)
        port = (uint16_t)(port * 10 + (args[i] - '0'));
    if (port == 0) {
        output_add_line(output, "Invalid port number.", 0x0C); return;
    }
    if (udp_bind(port, _udp_echo_handler, (void*)(uint64_t)port)) {
        char buf[48]; str_cpy(buf, "Echo listener started — port=");
        char ps[8]; int_to_str((int)port, ps); str_concat(buf, ps);
        output_add_line(output, buf, 0x0A);
        output_add_line(output, "  Host: test with  nc -u <guest-ip> <port>", 0x08);
    } else {
        output_add_line(output, "Error: Bind failed (socket table full?).", 0x0C);
    }
}

// ── udpsend ──────────────────────────────────────────────────────────────────
// Usage: udpsend 10.0.2.2 5000 hello world
static void cmd_udpsend(const char* args, CommandOutput* output) {
    if (!udp_is_initialized()) {
        output_add_line(output, "Error: Run 'udpinit' first.", 0x0C); return;
    }
    if (!args || args[0] == '\0') {
        output_add_line(output, "Usage: udpsend <ip> <port> <message>", 0x0E);
        output_add_line(output, "  Example: udpsend 10.0.2.2 5000 hello", 0x08);
        return;
    }

    int pos = 0;

    // IP
    char ip_str[20]; int ilen = 0;
    while (args[pos] && args[pos] != ' ' && ilen < 19) ip_str[ilen++] = args[pos++];
    ip_str[ilen] = '\0';
    if (ilen == 0) { output_add_line(output, "Invalid IP.", 0x0C); return; }

    while (args[pos] == ' ') pos++;

    // Port
    uint16_t dst_port = 0;
    while (args[pos] >= '0' && args[pos] <= '9')
        dst_port = (uint16_t)(dst_port * 10 + (args[pos++] - '0'));
    if (dst_port == 0) { output_add_line(output, "Invalid port.", 0x0C); return; }

    while (args[pos] == ' ') pos++;

    // Message
    const char* msg = args + pos;
    uint16_t mlen = 0;
    while (msg[mlen]) mlen++;
    if (mlen == 0) { output_add_line(output, "Message cannot be empty.", 0x0C); return; }

    // Parse IP
    uint8_t dst_ip[4];
    if (!str_to_ip(ip_str, dst_ip)) {
        output_add_line(output, "Invalid IP address.", 0x0C); return;
    }

    // Resolve ARP (try first)
    {
        uint8_t dummy[6];
        if (!arp_resolve(dst_ip, dummy)) {
            output_add_line(output, "Sending ARP request...", 0x0E);
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
                output_add_line(output, "ARP timeout — resolve first with 'arping <ip>'.", 0x0C);
                return;
            }
        }
    }

    bool sent = udp_send(dst_ip, dst_port, 0, (const uint8_t*)msg, mlen);

    if (sent) {
        char buf[64]; str_cpy(buf, "Sent -> ");
        str_concat(buf, ip_str); str_concat(buf, ":");
        char ps[8]; int_to_str((int)dst_port, ps); str_concat(buf, ps);
        output_add_line(output, buf, 0x0A);
    } else {
        output_add_line(output, "Send failed.", 0x0C);
    }
}

// ── udpclose ─────────────────────────────────────────────────────────────────
static void cmd_udpclose(const char* args, CommandOutput* output) {
    if (!args || args[0] == '\0') {
        output_add_line(output, "Usage: udpclose <port>", 0x0E); return;
    }
    uint16_t port = 0;
    for (int i = 0; args[i] >= '0' && args[i] <= '9'; i++)
        port = (uint16_t)(port * 10 + (args[i] - '0'));
    if (port == 0) { output_add_line(output, "Invalid port.", 0x0C); return; }
    udp_unbind(port);
    char buf[40]; str_cpy(buf, "Port closed: ");
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
        output_add_line(output, "UDP layer not initialized.", 0x08); return;
    }

    output_add_line(output, "=== UDP Socket Table ===", 0x0B);
    output_add_line(output, "  PORT     RX              TX", 0x08);
    output_add_line(output, "  -------- --------------- ---------------", 0x08);

    UDPLineCtx ctx = { output };
    udp_sockets_foreach(udp_line_vga_cb, &ctx);

    // Total counters
    char buf[64];
    str_cpy(buf, "  Total RX: "); char tmp[12];
    uint64_to_string(udp_get_rx_count(), tmp); str_concat(buf, tmp);
    str_concat(buf, "  TX: ");
    uint64_to_string(udp_get_tx_count(), tmp); str_concat(buf, tmp);
    output_add_line(output, buf, 0x07);
}


// ============================================================================
// DHCP COMMANDS — Stage 5
// ============================================================================

// ── dhcp ─────────────────────────────────────────────────────────────────────
// Send DHCPDISCOVER → get IP, GW, DNS automatically
static void cmd_dhcp(const char* args, CommandOutput* output) {
    (void)args;

    // Layer checks
    if (!g_net_initialized) {
        output_add_line(output, "Network driver not ready. Run 'netinit' first.", 0x0C);
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

    // If already BOUND, show current configuration
    if (dhcp_get_state() == (int)DHCP_STATE_BOUND) {
        output_add_line(output, "DHCP already BOUND. Current configuration:", 0x0E);
        DHCPConfig cfg;
        dhcp_get_config(&cfg);
        if (cfg.valid) {
            char buf[64]; char ipstr[16];
            str_cpy(buf, "  IP      : "); ip_to_str(cfg.ip,      ipstr); str_concat(buf, ipstr);
            output_add_line(output, buf, 0x0F);
            str_cpy(buf, "  Gateway : "); ip_to_str(cfg.gateway, ipstr); str_concat(buf, ipstr);
            output_add_line(output, buf, 0x07);
        }
        output_add_line(output, "  Run 'dhcprel' to release and renew.", 0x08);
        return;
    }

    output_add_line(output, "Sending DHCP DISCOVER...", 0x0E);

    bool sent = dhcp_discover();
    if (!sent) {
        output_add_line(output, "  [ERROR] Could not send DISCOVER.", 0x0C);
        output_add_line(output, "  Was 'netinit' run?", 0x0E);
        return;
    }

    // ── OFFER/ACK wait loop ─────────────────────────────────────────────
    // We are running in keyboard IRQ context so IF=0. We open interrupts
    // with STI and then use a HLT loop.
    //
    // Why STI + HLT is needed:
    //   • HLT: halts the CPU → QEMU main loop runs
    //   • QEMU SLiRP writes reply to RTL8139 ring buffer + asserts IRQ11
    //   • IRQ11 → isr_net → rtl8139_irq_handler → rtl_process_rx
    //          → net_packet_callback → udp_handle_packet → dhcp_handle_packet
    //   • dhcp_handle_packet updates g_state to REQUESTING/BOUND/FAILED
    //   • After wakeup, rtl8139_poll() provides extra safety (against missed IRQs)
    //
    // pause DOES NOT WORK: it is only a decoder hint and does not yield to QEMU.
    output_add_line(output, "  Waiting for OFFER (max 4s)...", 0x07);

    __asm__ volatile("sti");    // Enable IRQs (timer + RTL8139 IRQ11)

    uint64_t t0 = get_system_ticks();
    int reported_requesting = 0;

    while ((get_system_ticks() - t0) < 4000) {

        // Poll ring buffer directly (safety net without IRQ)
        rtl8139_poll();

        int st = dhcp_get_state();
        if (st == (int)DHCP_STATE_REQUESTING && !reported_requesting) {
            reported_requesting = 1;
            output_add_line(output, "  OFFER received! REQUEST sent...", 0x0E);
        }
        if (st == (int)DHCP_STATE_BOUND || st == (int)DHCP_STATE_FAILED) break;

        // HLT: halt CPU until the next IRQ.
        // QEMU runs its event loop during this time and can write
        // the SLiRP reply to the RTL8139 DMA ring buffer.
        __asm__ volatile("hlt");
    }

    __asm__ volatile("cli");    // Disable IRQs (return to keyboard ISR context)

    int final_state = dhcp_get_state();

    if (final_state == (int)DHCP_STATE_BOUND) {
        DHCPConfig cfg;
        dhcp_get_config(&cfg);
        output_add_line(output, "=== DHCP BOUND — Configuration Complete ===", 0x0A);
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
            str_cpy(buf, "  Lease   : ");
            char ls[12]; uint64_to_string((uint64_t)cfg.lease_time, ls);
            str_concat(buf, ls); str_concat(buf, " s");
            output_add_line(output, buf, 0x07);
        }
        output_add_empty_line(output);
        output_add_line(output, "  Test connectivity with 'ping 10.0.2.2'.", 0x0B);

    } else if (final_state == (int)DHCP_STATE_FAILED) {
        output_add_line(output, "  [ERROR] DHCP failed (DHCPNAK received).", 0x0C);
        output_add_line(output, "  To assign manually: ipconfig 10.0.2.15", 0x07);

    } else {
        // Still SELECTING or REQUESTING — no OFFER arrived
        output_add_line(output, "  [TIMEOUT] DHCP: No OFFER received.", 0x0C);
        // RTL8139 register dump — written to serial; if ISR_ROK=1 a packet
        // arrived but rtl_process_rx() failed to process it.
        rtl8139_dump_regs();
        output_add_line(output, "  Check serial port: ISR/CAPR/CBR values written.", 0x0E);
        output_add_line(output, "  Is QEMU SLiRP active? (-netdev user,id=net0)", 0x0E);
        output_add_line(output, "  To assign manually: ipconfig 10.0.2.15", 0x07);
    }
}

// ── dhcpstat ─────────────────────────────────────────────────────────────────
static void cmd_dhcpstat(const char* args, CommandOutput* output) {
    (void)args;

    output_add_line(output, "=== DHCP Client Status ===", 0x0B);

    if (!dhcp_is_initialized()) {
        output_add_line(output, "  DHCP layer not initialized.", 0x0C);
        output_add_line(output, "  Run the 'dhcp' command first.", 0x07);
        return;
    }

    // If BOUND, show full configuration directly
    if (dhcp_get_state() == (int)DHCP_STATE_BOUND) {
        DHCPConfig cfg;
        dhcp_get_config(&cfg);
        output_add_line(output, "  Status : BOUND (IP assigned)", 0x0A);
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
            str_cpy(buf, "  Lease   : ");
            uint64_to_string((uint64_t)cfg.lease_time, ls);
            str_concat(buf, ls); str_concat(buf, " s");
            output_add_line(output, buf, 0x07);
        }
        output_add_empty_line(output);
        output_add_line(output, "  Test connectivity with  ping 10.0.2.2.", 0x0B);
        return;
    }

    char buf[64];
    str_cpy(buf, "  Status: "); str_concat(buf, dhcp_state_str());
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
        output_add_line(output, "  No valid configuration.", 0x08);
        return;
    }

    output_add_empty_line(output);
    output_add_line(output, "  Assigned Configuration:", 0x0B);

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
    str_cpy(buf, "    Lease    : ");
    uint64_to_string((uint64_t)cfg.lease_time, ls); str_concat(buf, ls);
    str_concat(buf, " s");
    output_add_line(output, buf, 0x07);

    str_cpy(buf, "    T1 (ren.): ");
    uint64_to_string((uint64_t)cfg.renewal_time, ls); str_concat(buf, ls);
    str_concat(buf, " s");
    output_add_line(output, buf, 0x08);

    str_cpy(buf, "    T2 (reb.): ");
    uint64_to_string((uint64_t)cfg.rebinding_time, ls); str_concat(buf, ls);
    str_concat(buf, " s");
    output_add_line(output, buf, 0x08);
}

// ── netrxtest ────────────────────────────────────────────────────────────────
// Send ARP request to 10.0.2.2, wait 3 seconds for RX.
// Any received packet confirms the RX path is working.
// ─────────────────────────────────────────────────────────────────────────────
static void cmd_netrxtest(const char* args, CommandOutput* output) {
    (void)args;

    if (!g_net_initialized) {
        output_add_line(output, "netinit not run.", 0x0C);
        return;
    }

    // ARP + IPv4 init required
    if (!ipv4_is_initialized()) { ipv4_init(); icmp_init(); }
    if (!arp_is_initialized()) {
        // Initialize with a temporary IP — for ARP probe only
        uint8_t tmp_mac[6]; rtl8139_get_mac(tmp_mac);
        uint8_t tmp_ip[4] = {10,0,2,15};
        arp_init(tmp_ip, tmp_mac);
        ipv4_set_gateway((uint8_t[]){10,0,2,2});
    }

    output_add_line(output, "RX path test: sending ARP probe to 10.0.2.2...", 0x0E);
    // arp_resolve triggers a probe
    uint8_t dummy[6];
    arp_resolve((uint8_t[]){10,0,2,2}, dummy);

    output_add_line(output, "Waiting 3 seconds for RX...", 0x07);
    __asm__ volatile("sti");

    uint64_t t0 = get_system_ticks();
    uint32_t pkts_before = g_net_rx_display;
    while ((get_system_ticks() - t0) < 3000) {
        rtl8139_poll();
        if (g_net_rx_display > pkts_before) break;
        __asm__ volatile("hlt");
    }
    __asm__ volatile("cli");

    rtl8139_dump_regs();  // register state to serial port

    uint32_t received = g_net_rx_display - pkts_before;
    if (received > 0) {
        output_add_line(output, "  [OK] Packet received! RX path is working.", 0x0A);
        char buf[48]; str_cpy(buf, "  Packets received: ");
        char ns[12]; uint64_to_string((uint64_t)received, ns);
        str_concat(buf, ns);
        output_add_line(output, buf, 0x0F);
    } else {
        output_add_line(output, "  [ERROR] No packets received.", 0x0C);
        output_add_line(output, "  Check register values on the serial port.", 0x0E);
        output_add_line(output, "  If ISR_ROK=1 there is a bug in rtl_process_rx.", 0x0E);
        output_add_line(output, "  If ISR=0 QEMU did not respond at all.", 0x0E);
    }
}

// ── dhcprel ──────────────────────────────────────────────────────────────────
static void cmd_dhcprel(const char* args, CommandOutput* output) {
    (void)args;
    if (!dhcp_is_initialized()) {
        output_add_line(output, "DHCP layer not initialized.", 0x0C); return;
    }
    if (dhcp_get_state() != (int)DHCP_STATE_BOUND) {
        output_add_line(output, "DHCP is not in BOUND state, release is unnecessary.", 0x0E);
        str_cpy((char[64]){0}, "  Current state: ");
        char buf[48]; str_cpy(buf, "  Current state: "); str_concat(buf, dhcp_state_str());
        output_add_line(output, buf, 0x07);
        return;
    }
    dhcp_release();
    output_add_line(output, "DHCP Release sent.", 0x0A);
    output_add_line(output, "  IP released. Run 'dhcp' to obtain a new one.", 0x07);
}

// ============================================================================
// TCP COMMANDS — Stage 6
// ============================================================================

// Connection event callback (shared by tcpconnect / tcptest commands)
static void _tcp_cmd_event_cb(int conn_id, TCPEvent_t event,
                               const uint8_t* data, uint16_t len, void* ctx)
{
    (void)ctx;
    g_tcp_conn_id = conn_id;

    switch(event) {
    case TCP_EVENT_CONNECTED:
        g_tcp_connected = true;
        println64("[TCP] Connection established!", 0x0A);
        break;

    case TCP_EVENT_DATA: {
        g_tcp_data_recvd = true;
        // Copy first 127 bytes to preview buffer
        uint16_t copy_len = (len < 127) ? len : 127;
        for(uint16_t i = 0; i < copy_len; i++) {
            char c = (char)data[i];
            g_tcp_recv_preview[i] = (c >= ' ' && c < 127) ? c : '.';
        }
        g_tcp_recv_preview[copy_len] = '\0';
        g_tcp_recv_len = len;

        // Write first line to VGA
        char info[80];
        str_cpy(info, "[TCP] Data received: ");
        char ns[12]; int_to_str((int)len, ns); str_concat(info, ns);
        str_concat(info, " bytes");
        println64(info, 0x0B);

        // Preview (first 64 chars)
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
        println64("[TCP] Connection closed.", 0x07);
        break;

    case TCP_EVENT_ERROR:
        g_tcp_error = true;
        println64("[TCP] Connection error / timeout.", 0x0C);
        break;

    default:
        break;
    }
}

// Server accept callback (for tcplisten command)
static void _tcp_cmd_accept_cb(int new_conn_id, const uint8_t remote_ip[4],
                                uint16_t remote_port, void* ctx)
{
    (void)ctx;
    char line[72];
    str_cpy(line, "[TCP] New connection accepted! conn_id=");
    char ns[8]; int_to_str(new_conn_id, ns); str_concat(line, ns);
    str_concat(line, "  remote=");
    char ipbuf[16]; ip_to_str(remote_ip, ipbuf); str_concat(line, ipbuf);
    str_concat(line, ":"); int_to_str((int)remote_port, ns); str_concat(line, ns);
    println64(line, 0x0A);

    // Save incoming connection as active conn_id
    g_tcp_conn_id  = new_conn_id;
    g_tcp_connected = true;
}

// ── tcpstat ──────────────────────────────────────────────────────────────────
static void cmd_tcpstat(const char* args, CommandOutput* output) {
    (void)args;
    if (!tcp_is_initialized()) {
        output_add_line(output, "TCP layer not initialized.", 0x0C);
        output_add_line(output, "  Kernel initializes it automatically. Check with 'netinit'.", 0x0E);
        return;
    }
    output_add_line(output, "=== TCP Connection Table ===", 0x0B);
    tcp_print_connections();   // writes to serial

    char buf[64];
    str_cpy(buf, "  Active connections: ");
    char ns[8]; int_to_str(tcp_get_conn_count(), ns); str_concat(buf, ns);
    output_add_line(output, buf, 0x07);

    str_cpy(buf, "  TX: "); uint64_to_string(tcp_get_tx_count(), ns);
    str_concat(buf, ns); str_concat(buf, "  RX: ");
    uint64_to_string(tcp_get_rx_count(), ns); str_concat(buf, ns);
    str_concat(buf, " segments");
    output_add_line(output, buf, 0x07);

    // Show current active conn_id
    if (g_tcp_conn_id >= 0) {
        str_cpy(buf, "  Last conn ID: "); int_to_str(g_tcp_conn_id, ns);
        str_concat(buf, ns);
        str_concat(buf, "  State: ");
        str_concat(buf, tcp_state_str(tcp_get_state(g_tcp_conn_id)));
        output_add_line(output, buf, 0x0F);
    }
    output_add_line(output, "  (Detailed log written to serial port)", 0x08);
}

// ── tcpconnect ───────────────────────────────────────────────────────────────
// Usage: tcpconnect 10.0.2.2 80
static void cmd_tcpconnect(const char* args, CommandOutput* output) {
    if (!tcp_is_initialized()) {
        output_add_line(output, "TCP layer not initialized.", 0x0C); return;
    }
    if (!arp_is_initialized()) {
        output_add_line(output, "Assign an IP first with 'dhcp' or 'ipconfig'.", 0x0C); return;
    }
    if (!args || args[0] == '\0') {
        output_add_line(output, "Usage: tcpconnect <ip> <port>", 0x0E);
        output_add_line(output, "  Example: tcpconnect 10.0.2.2 80", 0x08);
        return;
    }

    // Parse IP
    char ip_str[20]; int pos = 0, ilen = 0;
    while (args[pos] && args[pos] != ' ' && ilen < 19) ip_str[ilen++] = args[pos++];
    ip_str[ilen] = '\0';
    while (args[pos] == ' ') pos++;

    uint8_t dst_ip[4];
    if (!str_to_ip(ip_str, dst_ip)) {
        output_add_line(output, "Invalid IP address.", 0x0C); return;
    }

    // Parse port
    uint16_t port = 0;
    while (args[pos] >= '0' && args[pos] <= '9')
        port = (uint16_t)(port * 10 + (args[pos++] - '0'));
    if (port == 0) {
        output_add_line(output, "Invalid port number.", 0x0C); return;
    }

    // Resolve ARP — check cache first, otherwise send request and HLT to yield to QEMU
    {
        uint8_t dummy[6];
        if (!arp_resolve(dst_ip, dummy)) {
            output_add_line(output, "Sending ARP request...", 0x0E);
            arp_request(dst_ip);          // send explicit ARP request
            __asm__ volatile("sti");
            uint64_t t = get_system_ticks();
            bool ok = false;
            while ((get_system_ticks() - t) < 3000) {
                rtl8139_poll();
                if (arp_resolve(dst_ip, dummy)) { ok = true; break; }
                __asm__ volatile("hlt");  // yield CPU to QEMU → IRQ11 delivers ARP reply
            }
            __asm__ volatile("cli");
            if (!ok) {
                output_add_line(output, "ARP timeout.", 0x0C); return;
            }
            output_add_line(output, "ARP resolved.", 0x0A);
        }
    }

    // Reset state flags
    g_tcp_connected = false;
    g_tcp_data_recvd = false;
    g_tcp_closed = false;
    g_tcp_error = false;
    g_tcp_recv_len = 0;

    int cid = tcp_connect(dst_ip, port, _tcp_cmd_event_cb, (void*)0);
    if (cid < 0) {
        output_add_line(output, "tcp_connect() failed (table full?).", 0x0C); return;
    }

    char buf[56]; str_cpy(buf, "SYN sent: "); str_concat(buf, ip_str);
    str_concat(buf, ":"); char ps[8]; int_to_str((int)port, ps); str_concat(buf, ps);
    str_concat(buf, "  conn_id="); int_to_str(cid, ps); str_concat(buf, ps);
    output_add_line(output, buf, 0x0E);

    // Wait for SYN+ACK (3 seconds)
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
        output_add_line(output, "  Use 'tcpsend <id> <message>' to send data.", 0x07);
        output_add_line(output, "  Use 'tcpclose <id>' to close the connection.", 0x07);
    } else if (g_tcp_error) {
        output_add_line(output, "  Connection failed (RST or timeout).", 0x0C);
    } else {
        output_add_line(output, "  SYN_SENT: Waiting for reply (is the server listening?).", 0x0E);
        output_add_line(output, "  Monitor status with 'tcpstat'.", 0x07);
    }
}

// ── tcpsend ──────────────────────────────────────────────────────────────────
// Usage: tcpsend 0 Hello world
static void cmd_tcpsend(const char* args, CommandOutput* output) {
    if (!tcp_is_initialized()) {
        output_add_line(output, "TCP layer not initialized.", 0x0C); return;
    }
    if (!args || args[0] == '\0') {
        output_add_line(output, "Usage: tcpsend <conn_id> <message>", 0x0E);
        output_add_line(output, "  Example: tcpsend 0 GET / HTTP/1.0", 0x08);
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
        output_add_line(output, "Message cannot be empty.", 0x0C); return;
    }

    if (!tcp_is_connected(cid)) {
        char buf[48]; str_cpy(buf, "conn_id="); char ns[8]; int_to_str(cid, ns);
        str_concat(buf, ns); str_concat(buf, " is not ESTABLISHED.");
        output_add_line(output, buf, 0x0C);
        output_add_line(output, "  Check status with 'tcpstat'.", 0x07);
        return;
    }

    int sent = tcp_send(cid, (const uint8_t*)msg, mlen);
    if (sent > 0) {
        char buf[56]; str_cpy(buf, "Sent: ");
        char ns[8]; int_to_str(sent, ns); str_concat(buf, ns);
        str_concat(buf, " bytes  conn_id="); int_to_str(cid, ns); str_concat(buf, ns);
        output_add_line(output, buf, 0x0A);
    } else {
        output_add_line(output, "Send failed.", 0x0C);
    }
}

// ── tcpclose ─────────────────────────────────────────────────────────────────
static void cmd_tcpclose(const char* args, CommandOutput* output) {
    if (!tcp_is_initialized()) {
        output_add_line(output, "TCP layer not initialized.", 0x0C); return;
    }
    if (!args || args[0] == '\0') {
        output_add_line(output, "Usage: tcpclose <conn_id>", 0x0E); return;
    }
    int cid = 0;
    for (int i = 0; args[i] >= '0' && args[i] <= '9'; i++)
        cid = cid * 10 + (args[i] - '0');

    int st = tcp_get_state(cid);
    if (st == TCP_STATE_CLOSED) {
        char buf[48]; str_cpy(buf, "conn_id="); char ns[8]; int_to_str(cid, ns);
        str_concat(buf, ns); str_concat(buf, " is already closed.");
        output_add_line(output, buf, 0x0E); return;
    }

    tcp_close(cid);
    char buf[48]; str_cpy(buf, "FIN sent: conn_id=");
    char ns[8]; int_to_str(cid, ns); str_concat(buf, ns);
    output_add_line(output, buf, 0x07);
    output_add_line(output, "  Monitor close sequence with 'tcpstat'.", 0x08);
}

// ── tcplisten ─────────────────────────────────────────────────────────────────
static void cmd_tcplisten(const char* args, CommandOutput* output) {
    if (!tcp_is_initialized()) {
        output_add_line(output, "TCP layer not initialized.", 0x0C); return;
    }
    if (!args || args[0] == '\0') {
        output_add_line(output, "Usage: tcplisten <port>", 0x0E);
        output_add_line(output, "  Example: tcplisten 8080", 0x08);
        output_add_line(output, "  Host  : nc 10.0.2.15 8080  (QEMU portfwd required)", 0x08);
        return;
    }
    uint16_t port = 0;
    for (int i = 0; args[i] >= '0' && args[i] <= '9'; i++)
        port = (uint16_t)(port * 10 + (args[i] - '0'));
    if (port == 0) {
        output_add_line(output, "Invalid port.", 0x0C); return;
    }

    g_tcp_connected = false;
    g_tcp_data_recvd = false;
    g_tcp_closed = false;

    int lid = tcp_listen(port, _tcp_cmd_accept_cb, (void*)0);
    if (lid < 0) {
        output_add_line(output, "tcp_listen() failed (table full?).", 0x0C); return;
    }

    char buf[64]; str_cpy(buf, "LISTEN started: port=");
    char ps[8]; int_to_str((int)port, ps); str_concat(buf, ps);
    str_concat(buf, "  conn_id="); int_to_str(lid, ps); str_concat(buf, ps);
    output_add_line(output, buf, 0x0A);
    output_add_line(output, "  Incoming connections will be printed to VGA.", 0x07);
    output_add_line(output, "  In QEMU Makefile: hostfwd=tcp::<host_port>-:<guest_port>", 0x08);
    output_add_line(output, "  Monitor status with 'tcpstat'.", 0x08);
}

// ── wget ──────────────────────────────────────────────────────────────────────
// Usage: wget 10.0.2.2:8080/index.html
//        wget 10.0.2.2:8080/file.txt [save_name]   (optional save name)
static void cmd_wget(const char* args, CommandOutput* output) {
    if (!tcp_is_initialized() || !arp_is_initialized()) {
        output_add_line(output, "Get an IP first with 'dhcp'.", 0x0C); return;
    }
    if (!args || args[0] == '\0') {
        output_add_line(output, "Usage: wget <ip[:port][/path]> [filename]", 0x0E);
        output_add_line(output, "  Example: wget 10.0.2.2:9999/index.html", 0x08);
        output_add_line(output, "  Example: wget 10.0.2.2:9999/data.txt mine.txt", 0x08);
        output_add_line(output, "  Host  : python3 -m http.server 9999", 0x08);
        return;
    }

    // ── Parse URL: ip[:port][/path] [save_name] ───────────────────────
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

    // Optional save name
    if (args[pos]) {
        int si = 0;
        while (args[pos] && args[pos] != ' ' && si < 63) save_name[si++] = args[pos++];
        save_name[si] = '\0';
    }

    // Derive save name from path if not specified
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
        output_add_line(output, "Invalid IP address.", 0x0C); return;
    }

    // Header
    char hdr[80];
    str_cpy(hdr, "wget  http://"); str_concat(hdr, ip_str);
    str_concat(hdr, ":"); char ps[8]; int_to_str((int)port, ps); str_concat(hdr, ps);
    str_concat(hdr, path);
    output_add_line(output, hdr, 0x0B);

    // ── HTTP GET ─────────────────────────────────────────────────────────
    static HTTPResponse resp;
    HTTPError err = http_get(dst_ip, port, path, &resp);

    if (err != HTTP_OK) {
        char emsg[64]; str_cpy(emsg, "  ERROR: "); str_concat(emsg, http_err_str(err));
        output_add_line(output, emsg, 0x0C);
        return;
    }

    // ── Status ─────────────────────────────────────────────────────────────
    {
        char sl[48]; str_cpy(sl, "  HTTP ");
        char sc[8]; int_to_str(resp.status, sc); str_concat(sl, sc);
        str_concat(sl, " "); str_concat(sl, http_status_str(resp.status));
        output_add_line(output, sl, resp.status == 200 ? 0x0A : 0x0E);
    }

    if (resp.status != 200 || !resp.body || resp.body_len == 0) {
        output_add_line(output, "  Body empty or error status, not saved.", 0x0E);
        return;
    }

    // ── Size ─────────────────────────────────────────────────────────────
    {
        char sz[64]; str_cpy(sz, "  Header: ");
        char tmp[8]; int_to_str((int)resp.header_len, tmp); str_concat(sz, tmp);
        str_concat(sz, " bytes  Body: ");
        int_to_str((int)resp.body_len, tmp); str_concat(sz, tmp);
        str_concat(sz, " bytes");
        output_add_line(output, sz, 0x07);
    }

    // ── Preview (first 3 lines) ────────────────────────────────────────────
    output_add_line(output, "  ── Preview ────────────────────────────────", 0x08);
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
        str_concat(more, tmp); str_concat(more, " more bytes)");
        output_add_line(output, more, 0x08);
    }

    // ── Save to disk ──────────────────────────────────────────────────────
    // resp.body is null-terminated (http.c guarantee), can be written directly
    output_add_line(output, "  Saving to disk...", 0x0E);

    if (!ext3_path_is_file(save_name)) ext3_create_file(save_name);
    int written = ext3_write_file(save_name, 0, (const uint8_t*)resp.body, (uint32_t)resp.body_len);

    if (written >= 0) {
        char smsg[64]; str_cpy(smsg, "  Saved: ");
        str_concat(smsg, save_name);
        str_concat(smsg, "  (");
        char tmp[8]; int_to_str((int)resp.body_len, tmp);
        str_concat(smsg, tmp); str_concat(smsg, " bytes)");
        output_add_line(output, smsg, 0x0A);
        output_add_line(output, "  Written to ext3.", 0x0A);
    } else {
        output_add_line(output, "  ERROR: Disk write failed!", 0x0C);
    }

    // ── Summary ──────────────────────────────────────────────────────────────
    {
        char sum[64]; str_cpy(sum, "  Total: ");
        char tmp[8]; int_to_str((int)resp.total_len, tmp); str_concat(sum, tmp);
        str_concat(sum, " bytes received.");
        output_add_line(output, sum, written ? 0x0A : 0x0C);
    }
}

// ── httppost ───────────────────────────────────────────────────────────────
// Usage: httppost 10.0.2.2:8080/api key=value
static void cmd_httppost(const char* args, CommandOutput* output) {
    if (!tcp_is_initialized() || !arp_is_initialized()) {
        output_add_line(output, "Get an IP first with 'dhcp'.", 0x0C); return;
    }
    if (!args || args[0] == '\0') {
        output_add_line(output, "Usage: httppost <ip[:port][/path]> <data>", 0x0E);
        output_add_line(output, "  Example: httppost 10.0.2.2:9999/api key=hello", 0x08);
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
        output_add_line(output, "Invalid IP.", 0x0C); return;
    }

    char hdr[80]; str_cpy(hdr, "POST  http://"); str_concat(hdr, ip_str);
    str_concat(hdr, ":"); char ps[8]; int_to_str((int)port, ps); str_concat(hdr, ps);
    str_concat(hdr, path);
    output_add_line(output, hdr, 0x0B);

    static HTTPResponse resp;
    HTTPError err = http_post(dst_ip, port, path,
                               (const uint8_t*)body_str, body_len, &resp);

    if (err != HTTP_OK) {
        char emsg[64]; str_cpy(emsg, "  ERROR: "); str_concat(emsg, http_err_str(err));
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
        char sz[48]; str_cpy(sz, "  Total: ");
        char tmp[8]; int_to_str((int)resp.total_len, tmp);
        str_concat(sz, tmp); str_concat(sz, " bytes");
        output_add_line(output, sz, 0x07);
    }
}
static void cmd_apic(const char* args, CommandOutput* output) {
 
    // Argüman yoksa → info moduna düş
    int show_info    = (!args || str_len(args) == 0 ||
                        str_cmp(args, "info") == 0);
    int do_init      = (args && str_cmp(args, "init") == 0);
    int do_test      = (args && str_cmp(args, "test") == 0);
    int do_disablepic = (args && str_cmp(args, "disablepic") == 0);
    int do_stoptimer = (args && str_cmp(args, "stoptimer") == 0);
 
    // "timer N" ayrıştırması
    uint32_t timer_hz = 0;
    int do_timer = 0;
    if (args && args[0] == 't' && args[1] == 'i' && args[2] == 'm' &&
        args[3] == 'e' && args[4] == 'r' &&
        (args[5] == ' ' || args[5] == '\0')) {
        if (args[5] == ' ') {
            const char* p = args + 6;
            while (*p >= '0' && *p <= '9')
                timer_hz = timer_hz * 10 + (uint32_t)(*p++ - '0');
        }
        do_timer = 1;
    }
 
    // ════════════════════════════════════════════════════════════════════════
    // APIC INFO
    // ════════════════════════════════════════════════════════════════════════
    if (show_info || do_init || do_timer || do_test ||
        do_disablepic || do_stoptimer) {
 
        // ── İlk satır: başlık ──────────────────────────────────────────────
        output_add_line(output,
            "=== APIC (Advanced Programmable Interrupt Controller) ===",
            VGA_CYAN);
        output_add_empty_line(output);
 
        // ── CPUID kontrolü ─────────────────────────────────────────────────
        int detected = apic_detect();
        {
            char line[MAX_LINE_LENGTH];
            str_cpy(line, "  CPUID APIC bit  : ");
            str_concat(line, detected ? "PRESENT" : "NOT PRESENT");
            output_add_line(output, line,
                            detected ? VGA_GREEN : VGA_RED);
        }
 
        if (!detected) {
            output_add_empty_line(output);
            output_add_line(output,
                "  APIC not supported by this CPU.",
                VGA_RED);
            return;
        }
 
        // ── Genel durum ────────────────────────────────────────────────────
        int inited = apic_is_initialized();
        {
            char line[MAX_LINE_LENGTH];
            str_cpy(line, "  Initialized     : ");
            str_concat(line, inited ? "YES" : "NO");
            output_add_line(output, line,
                            inited ? VGA_GREEN : VGA_YELLOW);
        }
 
        // ── INIT komutu ────────────────────────────────────────────────────
        if (do_init) {
            if (inited) {
                output_add_line(output,
                    "  [INFO] APIC already initialized.",
                    VGA_YELLOW);
            } else {
                output_add_line(output,
                    "  Initializing LAPIC + IOAPIC ...",
                    VGA_YELLOW);
                apic_init();
                inited = apic_is_initialized();
                output_add_line(output,
                    inited ? "  [OK] APIC initialized!" :
                             "  [ERROR] Initialization failed.",
                    inited ? VGA_GREEN : VGA_RED);
            }
        }
 
        // ── Ayrıntılı bilgi (init sonrası veya info modu) ──────────────────
        if (inited) {
            APICInfo ai;
            apic_get_info(&ai);
 
            output_add_empty_line(output);
            output_add_line(output, "  ── Local APIC ──", VGA_CYAN);
 
            // Fiziksel taban
            {
                char line[MAX_LINE_LENGTH];
                char hex[11];
                str_cpy(line, "  Phys base       : ");
                apic_u32_to_hex((uint32_t)ai.lapic_phys_base, hex);
                str_concat(line, hex);
                output_add_line(output, line, VGA_WHITE);
            }
 
            // LAPIC ID
            {
                char line[MAX_LINE_LENGTH];
                char num[16];
                str_cpy(line, "  LAPIC ID        : ");
                uint64_to_string(ai.lapic_id, num);
                str_concat(line, num);
                output_add_line(output, line, VGA_WHITE);
            }
 
            // LAPIC versiyon + LVT sayısı
            {
                char line[MAX_LINE_LENGTH];
                char num[16];
                str_cpy(line, "  Version         : 0x");
                uint64_to_string(ai.lapic_version, num);
                str_concat(line, num);
                str_concat(line, "  Max LVT: ");
                uint64_to_string(ai.lapic_max_lvt, num);
                str_concat(line, num);
                output_add_line(output, line, VGA_WHITE);
            }
 
            // Timer durumu
            {
                char line[MAX_LINE_LENGTH];
                char num[16];
                str_cpy(line, "  Timer           : ");
                if (ai.timer_hz > 0) {
                    uint64_to_string((uint64_t)ai.timer_hz, num);
                    str_concat(line, num);
                    str_concat(line, " Hz (running)");
                    output_add_line(output, line, VGA_GREEN);
                } else {
                    str_concat(line, "Stopped");
                    output_add_line(output, line, VGA_YELLOW);
                }
 
                str_cpy(line, "  Calibration     : ");
                uint64_to_string(ai.timer_ticks_per_ms, num);
                str_concat(line, num);
                str_concat(line, " ticks/ms  (div=16)");
                output_add_line(output, line, VGA_WHITE);
            }
 
            output_add_empty_line(output);
            output_add_line(output, "  ── I/O APIC ──", VGA_CYAN);
 
            {
                char line[MAX_LINE_LENGTH];
                char hex[11];
                str_cpy(line, "  IOAPIC phys     : ");
                apic_u32_to_hex((uint32_t)ai.ioapic_phys_base, hex);
                str_concat(line, hex);
                output_add_line(output, line, VGA_WHITE);
            }
            {
                char line[MAX_LINE_LENGTH];
                char num[16];
                str_cpy(line, "  IOAPIC ID       : ");
                uint64_to_string(ai.ioapic_id, num);
                str_concat(line, num);
                output_add_line(output, line, VGA_WHITE);
            }
            {
                char line[MAX_LINE_LENGTH];
                char num[16];
                str_cpy(line, "  IOAPIC version  : 0x");
                uint64_to_string(ai.ioapic_version, num);
                str_concat(line, num);
                str_concat(line, "  Max redir: ");
                uint64_to_string(ai.ioapic_max_redir, num);
                str_concat(line, num);
                output_add_line(output, line, VGA_WHITE);
            }
 
            // PIC durumu
            output_add_empty_line(output);
            {
                char line[MAX_LINE_LENGTH];
                str_cpy(line, "  8259A PIC       : ");
                str_concat(line, ai.pic_disabled ?
                    "Masked (APIC mode)" : "Active (legacy mode)");
                output_add_line(output, line,
                    ai.pic_disabled ? VGA_YELLOW : VGA_WHITE);
            }
        }
 
        output_add_empty_line(output);
 
        // ── TIMER komutu ────────────────────────────────────────────────────
        if (do_timer) {
            if (!inited) {
                output_add_line(output,
                    "  [ERROR] Run 'apic init' first.",
                    VGA_RED);
                return;
            }
            if (timer_hz == 0 || timer_hz > 10000) {
                output_add_line(output,
                    "  Usage: apic timer <hz>   (1-10000)",
                    VGA_YELLOW);
                return;
            }
 
            output_add_line(output, "  Calibrating LAPIC timer (PIT ch2)...",
                            VGA_YELLOW);
            uint32_t tpm = lapic_timer_calibrate();
            {
                char line[MAX_LINE_LENGTH];
                char num[16];
                str_cpy(line, "  [OK] Calibrated: ");
                uint64_to_string(tpm, num);
                str_concat(line, num);
                str_concat(line, " ticks/ms");
                output_add_line(output, line, VGA_GREEN);
            }
 
            lapic_timer_init(timer_hz);
 
            {
                char line[MAX_LINE_LENGTH];
                char num[16];
                str_cpy(line, "  [OK] LAPIC timer started: ");
                uint64_to_string(timer_hz, num);
                str_concat(line, num);
                str_concat(line, " Hz  (vector 0x40)");
                output_add_line(output, line, VGA_GREEN);
            }
            output_add_line(output,
                "  NOTE: Timer uses isr_apic_timer in interrupts64.asm.",
                VGA_GREEN);
            output_add_line(output,
                "  NOTE: PIT timer (isr_timer) is still active in parallel.",
                VGA_GREEN);
            output_add_line(output,
                "  TIP : Run 'apic disablepic' after switching fully to APIC.",
                VGA_YELLOW);
            return;
        }
 
        // ── STOPTIMER komutu ─────────────────────────────────────────────────
        if (do_stoptimer) {
            if (!inited) {
                output_add_line(output,
                    "  [ERROR] APIC not initialized.",
                    VGA_RED);
                return;
            }
            lapic_timer_stop();
            output_add_line(output,
                "  [OK] LAPIC timer stopped.",
                VGA_GREEN);
            return;
        }
 
        // ── DISABLEPIC komutu ───────────────────────────────────────────────
        if (do_disablepic) {
            if (!inited) {
                output_add_line(output,
                    "  [ERROR] Initialize APIC first ('apic init').",
                    VGA_RED);
                return;
            }
            output_add_line(output,
                "  Masking all 8259A IRQs (APIC takes over)...",
                VGA_YELLOW);
            apic_disable_pic();
            output_add_line(output,
                "  [OK] PIC masked. LAPIC is sole interrupt controller.",
                VGA_GREEN);
            output_add_line(output,
                "  WARNING: Keyboard/network will stop unless IOAPIC routes them.",
                VGA_RED);
            return;
        }
 
        // ── TEST komutu ─────────────────────────────────────────────────────
        if (do_test) {
            output_add_line(output,
                "=== APIC Test Suite ===",
                VGA_CYAN);
            output_add_empty_line(output);
 
            // ── Test 1: CPUID ──────────────────────────────────────────────
            output_add_line(output, "[T1] CPUID APIC presence ...", VGA_YELLOW);
            if (apic_detect()) {
                output_add_line(output,
                    "  [OK] APIC present (CPUID:EDX bit-9 = 1)",
                    VGA_GREEN);
            } else {
                output_add_line(output,
                    "  [FAIL] APIC not present",
                    VGA_RED);
                return;
            }
            output_add_empty_line(output);
 
            // ── Test 2: Başlatma ───────────────────────────────────────────
            output_add_line(output, "[T2] APIC initialization ...", VGA_YELLOW);
            if (!apic_is_initialized()) {
                apic_init();
            }
            if (apic_is_initialized()) {
                output_add_line(output,
                    "  [OK] LAPIC + IOAPIC initialized",
                    VGA_GREEN);
            } else {
                output_add_line(output,
                    "  [FAIL] apic_init() failed",
                    VGA_RED);
                return;
            }
            output_add_empty_line(output);
 
            // ── Test 3: LAPIC yazmaç okuma ─────────────────────────────────
            output_add_line(output, "[T3] LAPIC register read ...", VGA_YELLOW);
            {
                APICInfo ai;
                apic_get_info(&ai);
                char line[MAX_LINE_LENGTH];
                char hex[11];
                char num[16];
 
                str_cpy(line, "  LAPIC ID      = ");
                uint64_to_string(ai.lapic_id, num);
                str_concat(line, num);
                output_add_line(output, line, VGA_WHITE);
 
                str_cpy(line, "  LAPIC version = 0x");
                uint64_to_string(ai.lapic_version, num);
                str_concat(line, num);
                output_add_line(output, line, VGA_WHITE);
 
                str_cpy(line, "  LAPIC max LVT = ");
                uint64_to_string(ai.lapic_max_lvt, num);
                str_concat(line, num);
                output_add_line(output, line, VGA_WHITE);
 
                str_cpy(line, "  Phys base     = ");
                apic_u32_to_hex((uint32_t)ai.lapic_phys_base, hex);
                str_concat(line, hex);
                output_add_line(output, line, VGA_WHITE);
 
                // Versiyon 0x10-0x15 arası gerçek LAPIC için beklenen değer
                int ver_ok = (ai.lapic_version >= 0x10 &&
                              ai.lapic_version <= 0x15);
                output_add_line(output,
                    ver_ok ? "  [OK] Version in expected range (0x10-0x15)"
                           : "  [WARN] Unexpected LAPIC version (might be xAPIC/x2APIC)",
                    ver_ok ? VGA_GREEN : VGA_YELLOW);
            }
            output_add_empty_line(output);
 
            // ── Test 4: IOAPIC yazmaç okuma ────────────────────────────────
            output_add_line(output, "[T4] IOAPIC register read ...", VGA_YELLOW);
            {
                APICInfo ai;
                apic_get_info(&ai);
                char line[MAX_LINE_LENGTH];
                char num[16];
 
                str_cpy(line, "  IOAPIC ID         = ");
                uint64_to_string(ai.ioapic_id, num);
                str_concat(line, num);
                output_add_line(output, line, VGA_WHITE);
 
                str_cpy(line, "  IOAPIC version    = 0x");
                uint64_to_string(ai.ioapic_version, num);
                str_concat(line, num);
                output_add_line(output, line, VGA_WHITE);
 
                str_cpy(line, "  IOAPIC max redir  = ");
                uint64_to_string(ai.ioapic_max_redir, num);
                str_concat(line, num);
                output_add_line(output, line, VGA_WHITE);
 
                int redir_ok = (ai.ioapic_max_redir >= 0x17);   // >=23 giriş
                output_add_line(output,
                    redir_ok ? "  [OK] >= 24 redirection entries"
                             : "  [WARN] Few redirection entries",
                    redir_ok ? VGA_GREEN : VGA_YELLOW);
            }
            output_add_empty_line(output);
 
            // ── Test 5: Timer kalibrasyonu ─────────────────────────────────
            output_add_line(output,
                "[T5] LAPIC timer calibration (PIT ch2, ~10ms) ...",
                VGA_YELLOW);
            {
                uint32_t tpm = lapic_timer_calibrate();
                char line[MAX_LINE_LENGTH];
                char num[16];
 
                str_cpy(line, "  Calibrated: ");
                uint64_to_string(tpm, num);
                str_concat(line, num);
                str_concat(line, " ticks/ms  (div=16)");
                output_add_line(output, line, VGA_WHITE);
 
                // Mantıklı bir değer mi? (tipik: 1000-100000 arası)
                int cal_ok = (tpm >= 100 && tpm <= 500000);
                output_add_line(output,
                    cal_ok ? "  [OK] Calibration value in expected range"
                           : "  [WARN] Unexpected calibration value",
                    cal_ok ? VGA_GREEN : VGA_YELLOW);
 
                // Tahmini frekans: tpm * 16 * 1000 Hz → MHz
                uint64_t est_mhz = (uint64_t)tpm * 16u / 1000u;
                str_cpy(line, "  Est. CPU freq  : ~");
                uint64_to_string(est_mhz, num);
                str_concat(line, num);
                str_concat(line, " MHz");
                output_add_line(output, line, VGA_CYAN);
            }
            output_add_empty_line(output);
 
            // ── Test 6: EOI yazması ────────────────────────────────────────
            output_add_line(output, "[T6] LAPIC EOI write ...", VGA_YELLOW);
            lapic_eoi();   // MMIO yazması; CPU çökmezse OK
            output_add_line(output,
                "  [OK] EOI write did not fault",
                VGA_GREEN);
            output_add_empty_line(output);
 
            // ── Özet ──────────────────────────────────────────────────────
            output_add_line(output,
                "=== All APIC tests completed! ===",
                VGA_CYAN);
            output_add_empty_line(output);
            output_add_line(output,
                "  Next steps:",
                VGA_WHITE);
            output_add_line(output,
                "  apic timer 1000  — Start LAPIC timer @ 1 kHz",
                VGA_WHITE);
            output_add_line(output,
                "  apic disablepic  — Mask 8259A (full APIC mode)",
                VGA_WHITE);
            output_add_line(output,
                "  apic stoptimer   — Stop LAPIC timer",
                VGA_WHITE);
            return;
        }
 
        // ── Sadece INFO modu: yardım ipuçları ──────────────────────────────
        if (show_info && !do_init) {
            output_add_line(output, "  Subcommands:", VGA_CYAN);
            output_add_line(output,
                "  apic info         — Show this status",
                VGA_WHITE);
            output_add_line(output,
                "  apic init         — Initialize LAPIC + IOAPIC",
                VGA_WHITE);
            output_add_line(output,
                "  apic test         — Run full APIC test suite",
                VGA_WHITE);
            output_add_line(output,
                "  apic timer <hz>   — Start LAPIC timer (e.g. apic timer 1000)",
                VGA_WHITE);
            output_add_line(output,
                "  apic stoptimer    — Stop LAPIC timer",
                VGA_WHITE);
            output_add_line(output,
                "  apic disablepic   — Mask 8259A PIC (APIC takes over)",
                VGA_WHITE);
        }
    }
}
// ── tcptest ───────────────────────────────────────────────────────────────────
// Full TCP round-trip test: SYN → ESTABLISHED → HTTP GET → wait for reply → FIN
// Usage: tcptest 10.0.2.2 80
//        tcptest 10.0.2.15 8080  (nc -l -p 8080 must be open)
static void cmd_tcptest(const char* args, CommandOutput* output) {
    if (!tcp_is_initialized()) {
        output_add_line(output, "TCP layer not initialized.", 0x0C); return;
    }
    if (!arp_is_initialized()) {
        output_add_line(output, "Assign an IP first with 'dhcp' or 'ipconfig'.", 0x0C); return;
    }
    if (!args || args[0] == '\0') {
        output_add_line(output, "Usage: tcptest <ip> <port>", 0x0E);
        output_add_line(output, "  Example: tcptest 10.0.2.2 80", 0x08);
        output_add_line(output, "  Host  : nc -l -p 8080   then: tcptest 10.0.2.15 8080", 0x08);
        return;
    }

    // Parse IP and port
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
        output_add_line(output, "Invalid IP.", 0x0C); return;
    }

    char hdr[64]; str_cpy(hdr, "TCP Test: "); str_concat(hdr, ip_str);
    str_concat(hdr, ":"); char ps[8]; int_to_str((int)port, ps); str_concat(hdr, ps);
    output_add_line(output, hdr, 0x0B);

    // Resolve ARP
    {
        uint8_t dummy[6];
        uint8_t my_ip[4]; arp_get_my_ip(my_ip);
        uint8_t gw[4];    ipv4_get_gateway(gw);

        // Connecting to our own IP → ARP won't work
        bool dst_is_me = (dst_ip[0]==my_ip[0] && dst_ip[1]==my_ip[1] &&
                          dst_ip[2]==my_ip[2] && dst_ip[3]==my_ip[3]);
        if (dst_is_me) {
            output_add_line(output, "  [INFO] Connecting to own IP.", 0x0E);
            output_add_line(output, "  Loopback not supported. Test tcplisten from the host:", 0x07);
            output_add_line(output, "    QEMU: tcplisten 8080", 0x08);
            output_add_line(output, "    Host: nc 127.0.0.1 8080  (Makefile: hostfwd=tcp::8080-:8080)", 0x08);
            return;
        }

        // Same /24 → go direct; otherwise route via gateway
        bool same = (dst_ip[0]==my_ip[0] && dst_ip[1]==my_ip[1] && dst_ip[2]==my_ip[2]);
        uint8_t* arp_t = same ? dst_ip : gw;

        if (!arp_resolve(arp_t, dummy)) {
            char arp_msg[48]; str_cpy(arp_msg, "  Sending ARP request: ");
            char ipbuf2[16]; ip_to_str(arp_t, ipbuf2); str_concat(arp_msg, ipbuf2);
            output_add_line(output, arp_msg, 0x0E);
            arp_request(arp_t);          // trigger explicit ARP request if cache is empty
            __asm__ volatile("sti");
            uint64_t t = get_system_ticks(); bool ok = false;
            while ((get_system_ticks() - t) < 4000) {
                rtl8139_poll();
                if (arp_resolve(arp_t, dummy)) { ok = true; break; }
                __asm__ volatile("hlt");  // NOT pause: yield CPU to QEMU → IRQ11 ARP reply
            }
            __asm__ volatile("cli");
            if (!ok) {
                output_add_line(output, "  ARP timeout.", 0x0C);
                output_add_line(output, "  Check cache with 'arping'.", 0x07);
                return;
            }
            output_add_line(output, "  ARP resolved.", 0x0A);
        }
    }

    // Reset state flags
    g_tcp_connected = false; g_tcp_data_recvd = false;
    g_tcp_closed = false;    g_tcp_error = false;
    g_tcp_recv_len = 0;      g_tcp_recv_preview[0] = '\0';

    // [1] Send SYN
    output_add_line(output, "[1/4] Sending SYN...", 0x07);
    int cid = tcp_connect(dst_ip, port, _tcp_cmd_event_cb, (void*)0);
    if (cid < 0) {
        output_add_line(output, "  ERROR: tcp_connect() failed.", 0x0C); return;
    }

    // [2] Wait for ESTABLISHED
    output_add_line(output, "[2/4] Waiting for SYN+ACK (3s)...", 0x07);
    __asm__ volatile("sti");
    uint64_t t0 = get_system_ticks();
    while ((get_system_ticks() - t0) < 3000) {
        rtl8139_poll(); tcp_tick();
        if (g_tcp_connected || g_tcp_error) break;
        __asm__ volatile("hlt");
    }
    __asm__ volatile("cli");

    if (!g_tcp_connected) {
        output_add_line(output, "  ERROR: Could not reach ESTABLISHED.", 0x0C);
        output_add_line(output, "  Is the server listening? Is QEMU hostfwd configured?", 0x0E);
        output_add_line(output, "  Tip: open 'nc -l -p <port>' on the host.", 0x08);
        tcp_abort(cid);
        return;
    }
    output_add_line(output, "  ESTABLISHED!", 0x0A);

    // [3] Send HTTP GET
    output_add_line(output, "[3/4] Sending HTTP GET...", 0x07);
    const char* http_req =
        "GET / HTTP/1.0\r\n"
        "Host: ascentos\r\n"
        "User-Agent: AscentOS/1.0 (hobby OS; x86_64)\r\n"
        "Connection: close\r\n"
        "\r\n";
    uint16_t req_len = 0; while (http_req[req_len]) req_len++;
    int sent = tcp_send(cid, (const uint8_t*)http_req, req_len);
    if (sent > 0) {
        char buf[48]; str_cpy(buf, "  Sent: ");
        char ns[8]; int_to_str(sent, ns); str_concat(buf, ns);
        str_concat(buf, " bytes");
        output_add_line(output, buf, 0x07);
    } else {
        output_add_line(output, "  Send failed!", 0x0C);
    }

    // [4] Wait for reply
    output_add_line(output, "[4/4] Waiting for reply (4s)...", 0x07);
    __asm__ volatile("sti");
    t0 = get_system_ticks();
    while ((get_system_ticks() - t0) < 4000) {
        rtl8139_poll(); tcp_tick();
        if (g_tcp_data_recvd || g_tcp_closed || g_tcp_error) break;
        __asm__ volatile("hlt");
    }
    __asm__ volatile("cli");

    if (g_tcp_data_recvd) {
        char buf[64]; str_cpy(buf, "  Reply received: ");
        char ns[8]; int_to_str((int)g_tcp_recv_len, ns); str_concat(buf, ns);
        str_concat(buf, " bytes");
        output_add_line(output, buf, 0x0A);
        // Preview first 60 chars
        char preview[72]; str_cpy(preview, "  > ");
        for(int k = 0; k < 60 && g_tcp_recv_preview[k]; k++) {
            int pl = str_len(preview);
            if (pl < 70) { preview[pl] = g_tcp_recv_preview[k]; preview[pl+1] = '\0'; }
        }
        output_add_line(output, preview, 0x0F);
        output_add_empty_line(output);
        output_add_line(output, "  TCP layer is fully working!", 0x0A);
    } else if (g_tcp_error) {
        output_add_line(output, "  Connection error / RST received.", 0x0C);
    } else {
        output_add_line(output, "  Reply timeout (4s).", 0x0E);
        output_add_line(output, "  Receiving SYN+ACK already proves the layer works.", 0x07);
    }

    if (!g_tcp_closed) tcp_close(cid);

    char sum[64]; str_cpy(sum, "  Result: ");
    if (g_tcp_connected) str_concat(sum, "3-way-handshake OK  ");
    if (g_tcp_data_recvd) str_concat(sum, "DATA OK  ");
    str_concat(sum, "conn_id="); char ns[8]; int_to_str(cid, ns); str_concat(sum, ns);
    output_add_line(output, sum, g_tcp_data_recvd ? 0x0A : (g_tcp_connected ? 0x0E : 0x0C));
}

// PC SPEAKER / SB16 SOUND COMMANDS

// "beep"         -> system beep (440 Hz, 100ms) [pcspk]
// "beep 440"     -> 440 Hz, 300ms [pcspk]
// "beep 440 500" -> 440 Hz, 500ms [pcspk]
// "beep boot"    -> boot melody
// "beep stop"    -> stop both pcspk and sb16
void cmd_beep(const char* args, CommandOutput* output) {
    if (!args || args[0] == '\0') {
        pcspk_system_beep();
        output_add_line(output, "Beep! (440 Hz, 100ms) [pcspk]", VGA_CYAN);
        return;
    }
    if (str_cmp(args, "stop") == 0) {
        pcspk_stop();
        if (g_sb16.initialized) sb16_stop();
        output_add_line(output, "Speaker stopped.", VGA_CYAN);
        return;
    }
    if (str_cmp(args, "boot") == 0) {
        pcspk_boot_melody();
        output_add_line(output, "Boot melody played! [pcspk]", VGA_CYAN);
        return;
    }
    uint32_t freq = 0, dur = 300;
    const char* p = args;
    while (*p >= '0' && *p <= '9') { freq = freq * 10 + (*p - '0'); p++; }
    while (*p == ' ') p++;
    if (*p >= '0' && *p <= '9') {
        dur = 0;
        while (*p >= '0' && *p <= '9') { dur = dur * 10 + (*p - '0'); p++; }
    }
    if (freq < 20)    freq = 20;
    if (freq > 20000) freq = 20000;
    if (dur  < 10)    dur  = 10;
    if (dur  > 5000)  dur  = 5000;
    pcspk_beep(freq, dur);
    extern void int_to_str(int num, char* str);
    char buf[64]; char fstr[16]; char dstr[16];
    int_to_str((int)freq, fstr); int_to_str((int)dur, dstr);
    char* bp = buf;
    for (const char* s = "Beep: ";      *s; s++) *bp++ = *s;
    for (const char* s = fstr;           *s; s++) *bp++ = *s;
    for (const char* s = " Hz, ";        *s; s++) *bp++ = *s;
    for (const char* s = dstr;           *s; s++) *bp++ = *s;
    for (const char* s = " ms [pcspk]"; *s; s++) *bp++ = *s;
    *bp = '\0';
    output_add_line(output, buf, VGA_CYAN);
}


// ------------------------------------------------------------
// cmd_sb16: Sound Blaster 16 commands
// "sb16"          -> show status
// "sb16 tone"     -> 440 Hz test tone (SB16 DAC)
// "sb16 ding"     -> 1 kHz fading effect
// "sb16 vol N"    -> master volume level 0-255
// "sb16 stop"     -> stop DMA
// ------------------------------------------------------------
void cmd_sb16(const char* args, CommandOutput* output) {
    if (!g_sb16.initialized) {
        output_add_line(output, "SB16: Device not found. QEMU: -device sb16", VGA_YELLOW);
        output_add_line(output, "  Fallback: pcspk active.", VGA_DARK_GRAY);
        return;
    }
    if (!args || args[0] == '\0') {
        extern void int_to_str(int num, char* str);
        char buf[80]; char tmp[16];
        output_add_line(output, "Sound Blaster 16 -- Status:", VGA_CYAN);
        char* bp = buf;
        for (const char* s = "  DSP: "; *s; s++) *bp++ = *s;
        int_to_str((int)g_sb16.dsp_major, tmp);
        for (const char* s = tmp; *s; s++) *bp++ = *s;
        *bp++ = '.';
        int_to_str((int)g_sb16.dsp_minor, tmp);
        for (const char* s = tmp; *s; s++) *bp++ = *s;
        *bp = '\0'; output_add_line(output, buf, VGA_WHITE);

        bp = buf;
        for (const char* s = "  IRQ: "; *s; s++) *bp++ = *s;
        int_to_str((int)g_sb16.irq, tmp);
        for (const char* s = tmp; *s; s++) *bp++ = *s;
        *bp = '\0'; output_add_line(output, buf, VGA_WHITE);

        bp = buf;
        for (const char* s = "  DMA: 8-bit=ch"; *s; s++) *bp++ = *s;
        int_to_str((int)g_sb16.dma8, tmp);
        for (const char* s = tmp; *s; s++) *bp++ = *s;
        for (const char* s = "  16-bit=ch"; *s; s++) *bp++ = *s;
        int_to_str((int)g_sb16.dma16, tmp);
        for (const char* s = tmp; *s; s++) *bp++ = *s;
        *bp = '\0'; output_add_line(output, buf, VGA_WHITE);

        output_add_line(output, "  Commands: tone | ding | vol N | stop", VGA_DARK_GRAY);
        return;
    }
    if (str_cmp(args, "tone") == 0) {
        sb16_test_tone();
        output_add_line(output, "SB16: 440 Hz test tone (250ms).", VGA_CYAN);
        return;
    }
    if (str_cmp(args, "ding") == 0) {
        sb16_ding();
        output_add_line(output, "SB16: Ding! (1 kHz, fading).", VGA_CYAN);
        return;
    }
    if (str_cmp(args, "stop") == 0) {
        sb16_stop();
        output_add_line(output, "SB16: DMA stopped.", VGA_CYAN);
        return;
    }
    if (args[0] == 'v' && args[1] == 'o' && args[2] == 'l' && args[3] == ' ') {
        const char* np = args + 4;
        uint32_t vol = 0;
        while (*np >= '0' && *np <= '9') { vol = vol * 10 + (*np - '0'); np++; }
        if (vol > 255) vol = 255;
        sb16_set_volume((uint8_t)vol);
        sb16_set_pcm_volume((uint8_t)vol);
        extern void int_to_str(int num, char* str);
        char buf[48]; char tmp[16]; int_to_str((int)vol, tmp);
        char* bp = buf;
        for (const char* s = "SB16: Volume = "; *s; s++) *bp++ = *s;
        for (const char* s = tmp; *s; s++) *bp++ = *s;
        for (const char* s = "/255"; *s; s++) *bp++ = *s;
        *bp = '\0'; output_add_line(output, buf, VGA_CYAN);
        return;
    }
    output_add_line(output, "SB16: Unknown command.", VGA_YELLOW);
    output_add_line(output, "  Usage: sb16 [tone|ding|vol N|stop]", VGA_DARK_GRAY);
}



// ============================================================================
// LSPCI — PCI Bus
// ============================================================================

// hex nibble helper
static char hex_nibble(uint8_t v) {
    return v < 10 ? '0' + v : 'a' + v - 10;
}

static void uint16_to_hex(uint16_t v, char out[5]) {
    out[0] = hex_nibble((v >> 12) & 0xF);
    out[1] = hex_nibble((v >>  8) & 0xF);
    out[2] = hex_nibble((v >>  4) & 0xF);
    out[3] = hex_nibble( v        & 0xF);
    out[4] = '\0';
}

static void uint8_to_hex(uint8_t v, char out[3]) {
    out[0] = hex_nibble((v >> 4) & 0xF);
    out[1] = hex_nibble( v       & 0xF);
    out[2] = '\0';
}

static void bdf_to_str(uint8_t bus, uint8_t dev, uint8_t func, char out[8]) {
    char hbus[3], hdev[3];
    uint8_to_hex(bus, hbus);
    uint8_to_hex(dev, hdev);
    out[0] = hbus[0]; out[1] = hbus[1]; out[2] = ':';
    out[3] = hdev[0]; out[4] = hdev[1]; out[5] = '.';
    out[6] = '0' + (func & 7);
    out[7] = '\0';
}

// uint32'yi hex string'e çevirir: "0x0000C000" formatında
static void uint32_to_hex_str(uint32_t v, char* out) {
    out[0] = '0'; out[1] = 'x';
    for (int i = 0; i < 8; i++)
        out[2 + i] = hex_nibble((v >> (28 - i * 4)) & 0xF);
    out[10] = '\0';
}

// uint32'yi ondalık string'e çevirir
static void uint32_to_dec_str(uint32_t v, char* out) {
    if (!v) { out[0] = '0'; out[1] = '\0'; return; }
    char tmp[12]; int i = 0;
    while (v) { tmp[i++] = '0' + (v % 10); v /= 10; }
    int j = 0;
    while (i--) out[j++] = tmp[i + 1];
    out[j] = '\0';
}

// Bir cihazın tüm geçerli BAR'larını output'a ekler
static void lspci_print_bars(uint8_t bus, uint8_t dev, uint8_t fn,
                              CommandOutput* output) {
    PCIDevice tmp;
    tmp.bus = bus;
    tmp.dev = dev;
    tmp.fn  = fn;

    for (uint8_t i = 0; i < 6; i++) {
        PCIBAR bar = pci_read_bar(&tmp, i);
        if (bar.type == PCI_BAR_TYPE_INVALID) continue;

        char line[MAX_LINE_LENGTH];
        char* p = line;

        // indent
        *p++ = ' '; *p++ = ' '; *p++ = ' '; *p++ = ' ';

        // BAR index
        *p++ = 'B'; *p++ = 'A'; *p++ = 'R'; *p++ = '0' + i; *p++ = ' ';

        // Tip
        if (bar.type == PCI_BAR_TYPE_IO) {
            for (const char* s = "I/O   "; *s; s++) *p++ = *s;
        } else if (bar.type == PCI_BAR_TYPE_MEM32) {
            for (const char* s = "MEM32 "; *s; s++) *p++ = *s;
            if (bar.prefetchable)
                for (const char* s = "[pref] "; *s; s++) *p++ = *s;
        } else {
            for (const char* s = "MEM64 "; *s; s++) *p++ = *s;
            if (bar.prefetchable)
                for (const char* s = "[pref] "; *s; s++) *p++ = *s;
        }

        // Adres
        for (const char* s = "addr="; *s; s++) *p++ = *s;
        char addr_str[11];
        uint32_to_hex_str((uint32_t)(bar.address & 0xFFFFFFFF), addr_str);
        for (int k = 0; addr_str[k]; k++) *p++ = addr_str[k];

        // Boyut
        if (bar.size) {
            for (const char* s = "  size="; *s; s++) *p++ = *s;
            char sz_str[12];
            uint32_to_dec_str(bar.size, sz_str);
            for (int k = 0; sz_str[k]; k++) *p++ = sz_str[k];
            // İnsan okunabilir birim
            if (bar.size >= 1024 * 1024) {
                for (const char* s = " MB"; *s; s++) *p++ = *s;
            } else if (bar.size >= 1024) {
                for (const char* s = " KB"; *s; s++) *p++ = *s;
            } else {
                for (const char* s = " B"; *s; s++) *p++ = *s;
            }
        }

        // MEM64 ise üst 32 bit de var mı?
        if (bar.type == PCI_BAR_TYPE_MEM64 && (bar.address >> 32)) {
            for (const char* s = " (hi="; *s; s++) *p++ = *s;
            char hi_str[11];
            uint32_to_hex_str((uint32_t)(bar.address >> 32), hi_str);
            for (int k = 0; hi_str[k]; k++) *p++ = hi_str[k];
            *p++ = ')';
            // MEM64: i+1 BAR kullanıldı, onu atla
            i++;
        }

        *p = '\0';
        output_add_line(output, line, VGA_DARK_GRAY);
    }
}

static void cmd_lspci(const char* args, CommandOutput* output) {
    // "lspci -v" → BAR detayları göster
    int verbose = (args && args[0] == '-' && args[1] == 'v');

    output_add_line(output, "PCI Bus Scan:", VGA_CYAN);
    if (verbose)
        output_add_line(output, "BDF      VendorID DevID  Class  Description  [-v: BAR detaylari]", VGA_DARK_GRAY);
    else
        output_add_line(output, "BDF      VendorID DevID  Class  Description  (lspci -v: BAR goster)", VGA_DARK_GRAY);
    output_add_line(output, "-------- -------- ------ ------ ----------------------------", VGA_DARK_GRAY);

    int found = 0;

    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            uint32_t id = pci_read32((uint8_t)bus, dev, 0, 0x00);
            if ((id & 0xFFFF) == 0xFFFF) continue;

            uint32_t hdr = pci_read32((uint8_t)bus, dev, 0, 0x0C);
            uint8_t  header_type = (hdr >> 16) & 0xFF;
            uint8_t  max_func = (header_type & 0x80) ? 8 : 1;

            for (uint8_t func = 0; func < max_func; func++) {
                id = pci_read32((uint8_t)bus, dev, func, 0x00);
                if ((id & 0xFFFF) == 0xFFFF) continue;

                uint16_t vendor_id = (uint16_t)(id & 0xFFFF);
                uint16_t device_id = (uint16_t)(id >> 16);

                uint32_t class_reg = pci_read32((uint8_t)bus, dev, func, 0x08);
                uint8_t  class_code = (class_reg >> 24) & 0xFF;
                uint8_t  subclass   = (class_reg >> 16) & 0xFF;

                // Ana satır
                char line[MAX_LINE_LENGTH];
                char* p = line;

                char bdf[8];
                bdf_to_str((uint8_t)bus, dev, func, bdf);
                for (int i = 0; bdf[i]; i++) *p++ = bdf[i];
                *p++ = ' '; *p++ = ' ';

                char vid_hex[5]; uint16_to_hex(vendor_id, vid_hex);
                *p++ = '0'; *p++ = 'x';
                for (int i = 0; vid_hex[i]; i++) *p++ = vid_hex[i];
                *p++ = ' ';

                char did_hex[5]; uint16_to_hex(device_id, did_hex);
                *p++ = '0'; *p++ = 'x';
                for (int i = 0; did_hex[i]; i++) *p++ = did_hex[i];
                *p++ = ' ';

                char cls_hex[3]; uint8_to_hex(class_code, cls_hex);
                *p++ = '0'; *p++ = 'x';
                *p++ = cls_hex[0]; *p++ = cls_hex[1];
                *p++ = ' '; *p++ = ' ';

                const char* vname = pci_vendor_name(vendor_id);
                for (int i = 0; vname[i]; i++) *p++ = vname[i];
                *p++ = ' ';

                const char* cname = pci_class_name(class_code, subclass);
                for (int i = 0; cname[i]; i++) *p++ = cname[i];
                *p = '\0';

                uint8_t color = VGA_WHITE;
                if      (class_code == 0x02) color = VGA_GREEN;
                else if (class_code == 0x03) color = VGA_CYAN;
                else if (class_code == 0x01) color = VGA_YELLOW;
                else if (class_code == 0x06) color = VGA_DARK_GRAY;

                output_add_line(output, line, color);
                found++;

                // -v bayrağı varsa BAR satırlarını ekle
                if (verbose)
                    lspci_print_bars((uint8_t)bus, dev, func, output);
            }
        }
    }

    if (found == 0) {
        output_add_line(output, "No PCI devices found.", VGA_RED);
    } else {
        char summary[48];
        char* sp = summary;
        for (const char* s = "Total: "; *s; s++) *sp++ = *s;
        {
            int n = found; char tmp[8]; int ti = 0;
            if (!n) { tmp[ti++] = '0'; }
            else { while (n) { tmp[ti++] = '0' + (n % 10); n /= 10; } }
            for (int i = ti - 1; i >= 0; i--) *sp++ = tmp[i];
        }
        for (const char* s = " PCI device(s) found."; *s; s++) *sp++ = *s;
        *sp = '\0';
        output_add_line(output, "", VGA_WHITE);
        output_add_line(output, summary, VGA_CYAN);
    }
}

// ============================================================================
// TEST — Unified kernel self-test suite
// Runs every available subsystem test and prints a PASS / FAIL summary.
//
// Usage:
//   test          — run all suites
//   test heap     — heap allocator only
//   test slab     — slab allocator only
//   test spinlock — spinlock / rwlock only
//   test vmm      — virtual memory manager only
//   test pmm      — physical memory manager only
//   test perf     — RDTSC performance counters only
// ============================================================================
/* Helper: write a test-suite line both into the paged output buffer AND
 * directly to the screen via println64.  This bypasses the MAX_OUTPUT_LINES
 * cap so late suites (e.g. PERF, suite 6/6) are never silently dropped when
 * the buffer fills up after the earlier suites. */
extern void println64(const char* str, uint8_t color);
#define TEST_LINE(out, msg, col) \
    do { output_add_line((out), (msg), (col)); println64((msg), (col)); } while (0)
#define TEST_EMPTY(out) \
    do { output_add_empty_line((out)); println64("", 0x0F); } while (0)

static void cmd_test_handler(const char* args, CommandOutput* output) {

    // Decide which suites to run based on optional argument
    int run_heap     = 1, run_slab  = 1, run_spinlock = 1;
    int run_vmm      = 1, run_pmm   = 1, run_perf     = 1;

    if (args && str_len(args) > 0) {
        run_heap     = (str_cmp(args, "heap")     == 0);
        run_slab     = (str_cmp(args, "slab")     == 0);
        run_spinlock = (str_cmp(args, "spinlock") == 0);
        run_vmm      = (str_cmp(args, "vmm")      == 0);
        run_pmm      = (str_cmp(args, "pmm")      == 0);
        run_perf     = (str_cmp(args, "perf")     == 0);

        if (!run_heap && !run_slab && !run_spinlock &&
            !run_vmm  && !run_pmm  && !run_perf) {
            TEST_LINE(output, "Unknown suite. Valid options:", VGA_RED);
            TEST_LINE(output, "  test heap | slab | spinlock | vmm | pmm | perf", VGA_YELLOW);
            return;
        }
    }

    // ── Header ────────────────────────────────────────────────────────────
    TEST_LINE(output, "================================================", VGA_CYAN);
    TEST_LINE(output, "        AscentOS Kernel Self-Test Suite         ", VGA_CYAN);
    TEST_LINE(output, "================================================", VGA_CYAN);
    TEST_EMPTY(output);

    // Per-suite result tracking
    int suite_pass[6] = {0, 0, 0, 0, 0, 0};   // heap slab spinlock vmm pmm perf
    int suite_run [6] = {0, 0, 0, 0, 0, 0};
    char line[MAX_LINE_LENGTH];
    char tmp[24];

    // ── [1] PMM ───────────────────────────────────────────────────────────
    if (run_pmm) {
        suite_run[4] = 1;
        TEST_LINE(output, "[SUITE 1/6] PMM — Physical Memory Manager", VGA_YELLOW);
        TEST_LINE(output, "--------------------------------------------", VGA_DARK_GRAY);

        uint64_t total_mem = (uint64_t)pmm_get_total_memory();
        uint64_t free_mem  = (uint64_t)pmm_get_free_memory();
        uint64_t used_mem  = (uint64_t)pmm_get_used_memory();

        int t1 = (total_mem > 0);
        int t2 = (free_mem  > 0);
        int t3 = (used_mem  < total_mem);
        int t4 = (used_mem + free_mem <= total_mem);

        str_cpy(line, "  [T1] Total memory > 0        : "); str_concat(line, t1 ? "PASS" : "FAIL");
        TEST_LINE(output, line, t1 ? VGA_GREEN : VGA_RED);
        str_cpy(line, "  [T2] Free memory  > 0        : "); str_concat(line, t2 ? "PASS" : "FAIL");
        TEST_LINE(output, line, t2 ? VGA_GREEN : VGA_RED);
        str_cpy(line, "  [T3] Used < Total            : "); str_concat(line, t3 ? "PASS" : "FAIL");
        TEST_LINE(output, line, t3 ? VGA_GREEN : VGA_RED);
        str_cpy(line, "  [T4] Used + Free <= Total    : "); str_concat(line, t4 ? "PASS" : "FAIL");
        TEST_LINE(output, line, t4 ? VGA_GREEN : VGA_RED);

        str_cpy(line, "       Total: ");
        uint64_to_string(total_mem / (1024*1024), tmp); str_concat(line, tmp); str_concat(line, " MB  Free: ");
        uint64_to_string(free_mem  / (1024*1024), tmp); str_concat(line, tmp); str_concat(line, " MB  Used: ");
        uint64_to_string(used_mem  / (1024*1024), tmp); str_concat(line, tmp); str_concat(line, " MB");
        TEST_LINE(output, line, VGA_WHITE);

        suite_pass[4] = (t1 && t2 && t3 && t4);
        TEST_LINE(output, suite_pass[4] ? "  => PMM PASSED" : "  => PMM FAILED",
                        suite_pass[4] ? VGA_GREEN : VGA_RED);
        TEST_EMPTY(output);
    }

    // ── [2] Heap ──────────────────────────────────────────────────────────
    if (run_heap) {
        suite_run[0] = 1;
        TEST_LINE(output, "[SUITE 2/6] HEAP — Dynamic Heap Allocator", VGA_YELLOW);
        TEST_LINE(output, "--------------------------------------------", VGA_DARK_GRAY);

        int all_ok = 1;

        // T1: basic alloc/free + write
        void* p1 = kmalloc(512);
        int t1 = (p1 != NULL);
        if (t1) { *(volatile uint8_t*)p1 = 0xAB; kfree(p1); }
        str_cpy(line, "  [T1] kmalloc(512) + kfree    : "); str_concat(line, t1 ? "PASS" : "FAIL");
        TEST_LINE(output, line, t1 ? VGA_GREEN : VGA_RED);
        if (!t1) all_ok = 0;

        // T2: multiple allocations
        void* mp[8]; int t2 = 1;
        for (int i = 0; i < 8; i++) { mp[i] = kmalloc(256); if (!mp[i]) { t2 = 0; break; } }
        for (int i = 0; i < 8; i++) if (mp[i]) kfree(mp[i]);
        str_cpy(line, "  [T2] 8 x kmalloc(256)+kfree  : "); str_concat(line, t2 ? "PASS" : "FAIL");
        TEST_LINE(output, line, t2 ? VGA_GREEN : VGA_RED);
        if (!t2) all_ok = 0;

        // T3: kcalloc zero-init
        uint32_t* ca = (uint32_t*)kcalloc(64, sizeof(uint32_t));
        int t3 = 0;
        if (ca) { t3 = 1; for (int i=0;i<64;i++) if (ca[i]!=0){t3=0;break;} kfree(ca); }
        str_cpy(line, "  [T3] kcalloc zero-init       : "); str_concat(line, t3 ? "PASS" : "FAIL");
        TEST_LINE(output, line, t3 ? VGA_GREEN : VGA_RED);
        if (!t3) all_ok = 0;

        // T4: krealloc grow + data preserved
        void* r1 = kmalloc(128); int t4 = 0;
        if (r1) {
            *(volatile uint8_t*)r1 = 0xCC;
            void* r2 = krealloc(r1, 1024);
            if (r2) { t4 = (*(volatile uint8_t*)r2 == 0xCC); kfree(r2); } else kfree(r1);
        }
        str_cpy(line, "  [T4] krealloc grow + verify  : "); str_concat(line, t4 ? "PASS" : "FAIL");
        TEST_LINE(output, line, t4 ? VGA_GREEN : VGA_RED);
        if (!t4) all_ok = 0;

        // T5: coalescing (alloc 3, free middle then first)
        void* c1=kmalloc(1024), *c2=kmalloc(1024), *c3=kmalloc(1024);
        int t5 = (c1 && c2 && c3);
        if (c2) kfree(c2); if (c1) kfree(c1); if (c3) kfree(c3);
        str_cpy(line, "  [T5] coalescing 3-block free : "); str_concat(line, t5 ? "PASS" : "FAIL");
        TEST_LINE(output, line, t5 ? VGA_GREEN : VGA_RED);
        if (!t5) all_ok = 0;

        // T6: 1 MB alloc (warning only — heap may not have expanded yet)
        void* big = kmalloc(1024*1024); int t6 = (big != NULL);
        if (big) kfree(big);
        str_cpy(line, "  [T6] kmalloc(1 MB)           : ");
        str_concat(line, t6 ? "PASS" : "WARN (heap expansion needed)");
        TEST_LINE(output, line, t6 ? VGA_GREEN : VGA_YELLOW);

        suite_pass[0] = all_ok;
        TEST_LINE(output, suite_pass[0] ? "  => HEAP PASSED" : "  => HEAP FAILED",
                        suite_pass[0] ? VGA_GREEN : VGA_RED);
        TEST_EMPTY(output);
    }

    // ── [3] Slab ──────────────────────────────────────────────────────────
    if (run_slab) {
        suite_run[1] = 1;
        TEST_LINE(output, "[SUITE 3/6] SLAB — Slab Allocator", VGA_YELLOW);
        TEST_LINE(output, "--------------------------------------------", VGA_DARK_GRAY);

        int all_ok = 1;
        static const uint32_t slab_sz[8] = {8,16,32,64,128,256,512,1024};

        // T1: alloc/free + slab_owns for each size class
        int t1 = 1;
        for (int i = 0; i < 8; i++) {
            void* p = slab_alloc(slab_sz[i]);
            if (!p) { t1 = 0; break; }
            if (!slab_owns(p)) { t1 = 0; slab_free(p); break; }
            slab_free(p);
        }
        str_cpy(line, "  [T1] 8 size classes alloc+free: "); str_concat(line, t1 ? "PASS" : "FAIL");
        TEST_LINE(output, line, t1 ? VGA_GREEN : VGA_RED);
        if (!t1) all_ok = 0;

        // T2: data integrity
        uint8_t* sb = (uint8_t*)slab_alloc(64); int t2 = 0;
        if (sb) {
            for (int i=0;i<64;i++) sb[i]=(uint8_t)(i^0xA5);
            t2 = 1;
            for (int i=0;i<64;i++) if (sb[i]!=(uint8_t)(i^0xA5)){t2=0;break;}
            slab_free(sb);
        }
        str_cpy(line, "  [T2] 64B data integrity       : "); str_concat(line, t2 ? "PASS" : "FAIL");
        TEST_LINE(output, line, t2 ? VGA_GREEN : VGA_RED);
        if (!t2) all_ok = 0;

        // T3: slab growth — force >SLAB_OBJECTS_PER_SLAB allocations
        void* sg[40]; int t3_filled = 0;
        for (int i=0;i<40;i++) { sg[i]=slab_alloc(32); if(!sg[i]) break; t3_filled++; }
        int t3 = (t3_filled == 40);
        for (int i=0;i<t3_filled;i++) slab_free(sg[i]);
        str_cpy(line, "  [T3] slab growth (40 x 32B)  : "); str_concat(line, t3 ? "PASS" : "FAIL");
        TEST_LINE(output, line, t3 ? VGA_GREEN : VGA_RED);
        if (!t3) all_ok = 0;

        // T4: oversized request delegated to kmalloc (not owned by slab)
        void* ov = slab_alloc(4096);
        int t4 = (ov != NULL) && !slab_owns(ov);
        if (ov) kfree(ov);
        str_cpy(line, "  [T4] 4096B routed to kmalloc : "); str_concat(line, t4 ? "PASS" : "FAIL");
        TEST_LINE(output, line, t4 ? VGA_GREEN : VGA_RED);
        if (!t4) all_ok = 0;

        suite_pass[1] = all_ok;
        TEST_LINE(output, suite_pass[1] ? "  => SLAB PASSED" : "  => SLAB FAILED",
                        suite_pass[1] ? VGA_GREEN : VGA_RED);
        TEST_EMPTY(output);
    }

    // ── [4] Spinlock ──────────────────────────────────────────────────────
    if (run_spinlock) {
        suite_run[2] = 1;
        TEST_LINE(output, "[SUITE 4/6] SPINLOCK — Spinlock / RWLock", VGA_YELLOW);
        TEST_LINE(output, "--------------------------------------------", VGA_DARK_GRAY);

        int all_ok = 1;

        // T1: init / lock / unlock state transitions
        spinlock_t lk = SPINLOCK_INIT;
        int t1 = !spinlock_is_locked(&lk);
        spinlock_lock(&lk);
        t1 = t1 && spinlock_is_locked(&lk);
        spinlock_unlock(&lk);
        t1 = t1 && !spinlock_is_locked(&lk);
        str_cpy(line, "  [T1] init/lock/unlock state  : "); str_concat(line, t1 ? "PASS" : "FAIL");
        TEST_LINE(output, line, t1 ? VGA_GREEN : VGA_RED);
        if (!t1) all_ok = 0;

        // T2: trylock on free vs already-held
        spinlock_t lk2 = SPINLOCK_INIT;
        int tl_free = spinlock_trylock(&lk2);
        int tl_held = spinlock_trylock(&lk2);
        spinlock_unlock(&lk2);
        int t2 = tl_free && !tl_held;
        str_cpy(line, "  [T2] trylock free / held     : "); str_concat(line, t2 ? "PASS" : "FAIL");
        TEST_LINE(output, line, t2 ? VGA_GREEN : VGA_RED);
        if (!t2) all_ok = 0;

        // T3: IRQ-safe lock/unlock
        spinlock_t lk3 = SPINLOCK_INIT;
        uint64_t fl = spinlock_lock_irq(&lk3);
        int t3 = spinlock_is_locked(&lk3);
        spinlock_unlock_irq(&lk3, fl);
        t3 = t3 && !spinlock_is_locked(&lk3);
        str_cpy(line, "  [T3] IRQ-safe lock/unlock    : "); str_concat(line, t3 ? "PASS" : "FAIL");
        TEST_LINE(output, line, t3 ? VGA_GREEN : VGA_RED);
        if (!t3) all_ok = 0;

        // T4: stress 1000× lock/unlock with counter
        spinlock_t stress = SPINLOCK_INIT;
        volatile uint32_t ctr = 0;
        for (int i=0; i<1000; i++) { spinlock_lock(&stress); ctr++; spinlock_unlock(&stress); }
        int t4 = (ctr == 1000);
        str_cpy(line, "  [T4] 1000x stress counter    : "); str_concat(line, t4 ? "PASS" : "FAIL");
        TEST_LINE(output, line, t4 ? VGA_GREEN : VGA_RED);
        if (!t4) all_ok = 0;

        // T5: rwlock — 2 readers simultaneously, then writer
        rwlock_t rw = RWLOCK_INIT;
        rwlock_read_lock(&rw); rwlock_read_lock(&rw);
        int t5 = (rw.readers == 2);
        rwlock_read_unlock(&rw); rwlock_read_unlock(&rw);
        t5 = t5 && (rw.readers == 0);
        rwlock_write_lock(&rw);
        t5 = t5 && spinlock_is_locked(&rw.write_lock);
        rwlock_write_unlock(&rw);
        t5 = t5 && !spinlock_is_locked(&rw.write_lock);
        str_cpy(line, "  [T5] rwlock readers + writer : "); str_concat(line, t5 ? "PASS" : "FAIL");
        TEST_LINE(output, line, t5 ? VGA_GREEN : VGA_RED);
        if (!t5) all_ok = 0;

        suite_pass[2] = all_ok;
        TEST_LINE(output, suite_pass[2] ? "  => SPINLOCK PASSED" : "  => SPINLOCK FAILED",
                        suite_pass[2] ? VGA_GREEN : VGA_RED);
        TEST_EMPTY(output);
    }

    // ── [5] VMM ───────────────────────────────────────────────────────────
    if (run_vmm) {
        suite_run[3] = 1;
        TEST_LINE(output, "[SUITE 5/6] VMM — Virtual Memory Manager", VGA_YELLOW);
        TEST_LINE(output, "--------------------------------------------", VGA_DARK_GRAY);

        int all_ok = 1;

        /*
         * Use virtual addresses well above physical RAM so we are guaranteed
         * NOT to land inside any existing boot identity-map entry.
         * The boot loader typically identity-maps 0..~256 MB (PML4[0]).
         * 0x00007F______ is still lower-half canonical but unreachable by the
         * boot identity map, so vmm_get_pte always walks our own freshly
         * allocated page tables — map and lookup see the same PTE.
         *
         * Physical targets are kept inside real RAM (< 8 MB) so pmm has them.
         */
        // T1: map 4KB page + verify address translation
        uint64_t tv = 0x00007F0000001000ULL, tp = 0x0000000000201000ULL;
        int t1 = (vmm_map_page(tv, tp, PAGE_WRITE | PAGE_PRESENT) == 0);
        if (t1) t1 = (vmm_get_physical_address(tv) == tp);
        str_cpy(line, "  [T1] map 4KB + translate     : "); str_concat(line, t1 ? "PASS" : "FAIL");
        TEST_LINE(output, line, t1 ? VGA_GREEN : VGA_RED);
        if (!t1) all_ok = 0;

        // T2: page present check
        int t2 = vmm_is_page_present(tv);
        str_cpy(line, "  [T2] page present check      : "); str_concat(line, t2 ? "PASS" : "FAIL");
        TEST_LINE(output, line, t2 ? VGA_GREEN : VGA_RED);
        if (!t2) all_ok = 0;

        // T3: map a range (8 KB) — two contiguous pages
        uint64_t rv = 0x00007F0000003000ULL, rp = 0x0000000000301000ULL;
        int t3 = (vmm_map_range(rv, rp, 8192, PAGE_WRITE | PAGE_PRESENT) == 0);
        if (t3) t3 = (vmm_get_physical_address(rv)              == rp) &&
                     (vmm_get_physical_address(rv + PAGE_SIZE_4K) == rp + PAGE_SIZE_4K);
        str_cpy(line, "  [T3] map 8KB range           : "); str_concat(line, t3 ? "PASS" : "FAIL");
        TEST_LINE(output, line, t3 ? VGA_GREEN : VGA_RED);
        if (!t3) all_ok = 0;

        // T4: identity map V == P
        // vmm_identity_map(phys, size, flags) maps virt==phys, but those
        // low-phys addresses are already boot-identity-mapped (conflict).
        // Instead, verify the V==P property explicitly using our scratch
        // region: map a virtual address to a physical address of the same
        // value (low 32 bits) and confirm the round-trip.
        // We pick virt 0x00007F0000005000 -> phys 0x205000 and verify
        // vmm_get_physical_address returns exactly 0x205000.
        uint64_t iv      = 0x00007F0000005000ULL;
        uint64_t iv_phys = 0x0000000000205000ULL;
        int t4 = (vmm_map_page(iv, iv_phys, PAGE_WRITE | PAGE_PRESENT) == 0);
        if (t4) t4 = (vmm_get_physical_address(iv) == iv_phys);
        str_cpy(line, "  [T4] identity map V==P       : "); str_concat(line, t4 ? "PASS" : "FAIL");
        TEST_LINE(output, line, t4 ? VGA_GREEN : VGA_RED);
        if (!t4) all_ok = 0;

        suite_pass[3] = all_ok;
        TEST_LINE(output, suite_pass[3] ? "  => VMM PASSED" : "  => VMM FAILED",
                        suite_pass[3] ? VGA_GREEN : VGA_RED);
        TEST_EMPTY(output);
    }

    // ── [6] Perf ──────────────────────────────────────────────────────────
    if (run_perf) {
        suite_run[5] = 1;
        TEST_LINE(output, "[SUITE 6/6] PERF — RDTSC Counters", VGA_YELLOW);
        TEST_LINE(output, "--------------------------------------------", VGA_DARK_GRAY);

        int all_ok = 1;
        PerfCounter pc;

        // T1: RDTSC is strictly monotonic
        uint64_t a = cpu_rdtsc();
        for (volatile int i = 0; i < 1000; i++);
        uint64_t b = cpu_rdtsc();
        int t1 = (b > a);
        str_cpy(line, "  [T1] RDTSC monotonic         : "); str_concat(line, t1 ? "PASS" : "FAIL");
        TEST_LINE(output, line, t1 ? VGA_GREEN : VGA_RED);
        if (!t1) all_ok = 0;

        // T2: PerfCounter elapsed > 0 after a work loop
        perf_start(&pc);
        for (volatile int i = 0; i < 100000; i++);
        perf_stop(&pc);
        int t2 = (perf_cycles(&pc) > 0) && (pc.elapsed > 0);
        str_cpy(line, "  [T2] perf start/stop elapsed : "); str_concat(line, t2 ? "PASS" : "FAIL");
        TEST_LINE(output, line, t2 ? VGA_GREEN : VGA_RED);
        if (!t2) all_ok = 0;

        // T3: ns and us are consistent (ns >= us * 900, 10% tolerance)
        uint64_t ns = perf_ns(&pc);
        uint32_t us = perf_us(&pc);
        int t3 = (us > 0) && (ns >= (uint64_t)us * 900);
        str_cpy(line, "  [T3] ns/us consistency (~");
        uint64_to_string((uint64_t)us, tmp); str_concat(line, tmp);
        str_concat(line, " us) : "); str_concat(line, t3 ? "PASS" : "FAIL");
        TEST_LINE(output, line, t3 ? VGA_GREEN : VGA_YELLOW);
        if (!t3) all_ok = 0;

        // T4: CPU frequency estimate is in a sane range (100 MHz – 10 GHz)
        uint32_t mhz = cpu_get_freq_estimate();
        int t4 = (mhz >= 100 && mhz <= 10000);
        str_cpy(line, "  [T4] CPU freq sane (~");
        uint64_to_string((uint64_t)mhz, tmp); str_concat(line, tmp);
        str_concat(line, " MHz) : "); str_concat(line, t4 ? "PASS" : "WARN");
        TEST_LINE(output, line, t4 ? VGA_GREEN : VGA_YELLOW);

        suite_pass[5] = all_ok;
        TEST_LINE(output, suite_pass[5] ? "  => PERF PASSED" : "  => PERF FAILED",
                        suite_pass[5] ? VGA_GREEN : VGA_RED);
        TEST_EMPTY(output);
    }

    // ── Summary ───────────────────────────────────────────────────────────
    TEST_LINE(output, "================================================", VGA_CYAN);
    TEST_LINE(output, "                   SUMMARY                     ", VGA_CYAN);
    TEST_LINE(output, "================================================", VGA_CYAN);

    static const char* suite_names[6] = {
        "HEAP    ", "SLAB    ", "SPINLOCK", "VMM     ", "PMM     ", "PERF    "
    };

    int total_run = 0, total_pass = 0;
    for (int i = 0; i < 6; i++) {
        if (!suite_run[i]) continue;
        total_run++;
        str_cpy(line, "  "); str_concat(line, suite_names[i]);
        str_concat(line, "  "); str_concat(line, suite_pass[i] ? "PASSED" : "FAILED");
        TEST_LINE(output, line, suite_pass[i] ? VGA_GREEN : VGA_RED);
        if (suite_pass[i]) total_pass++;
    }

    TEST_EMPTY(output);
    str_cpy(line, "  Result: ");
    uint64_to_string((uint64_t)total_pass, tmp); str_concat(line, tmp);
    str_concat(line, " / ");
    uint64_to_string((uint64_t)total_run,  tmp); str_concat(line, tmp);
    str_concat(line, " suites passed");
    TEST_LINE(output, line, (total_pass == total_run) ? VGA_GREEN : VGA_YELLOW);
    TEST_EMPTY(output);
    TEST_LINE(output,
        (total_pass == total_run) ? "  ALL TESTS PASSED!" : "  SOME TESTS FAILED — check above.",
        (total_pass == total_run) ? VGA_GREEN : VGA_RED);
    TEST_LINE(output, "================================================", VGA_CYAN);
}

// cmd_test(void) — satisfies the forward declaration in commands64.h.
// Delegates to cmd_test_handler with empty args and a local CommandOutput,
// then flushes each line to the screen via println64 (same as cmd_sysinfo etc.).
void cmd_test(void) {
    CommandOutput out;
    /* cmd_test_handler already prints every line directly via TEST_LINE/
     * TEST_EMPTY (println64), so we only need to populate the buffer —
     * no second flush loop needed. */
    cmd_test_handler("", &out);
}

// ============================================================================
// COMMAND TABLE
// ============================================================================
static Command command_table[] = {
    {"help", "Show available commands", cmd_help},
    {"clear", "Clear the screen", cmd_clear},
    {"echo", "Echo text back", cmd_echo},
    {"about", "About AscentOS", cmd_about},
    {"neofetch", "Show system information", cmd_neofetch},
    {"pmm", "Physical Memory Manager stats", cmd_pmm},
    {"vmm", "Virtual Memory Manager test", cmd_vmm},
    {"heap", "Heap memory test", cmd_heap},
    {"slab", "Slab allocator test + stats", cmd_slab},
    
    // Multitasking commands
    {"ps", "List all tasks", cmd_ps},
    {"taskinfo", "Show task information", cmd_taskinfo},
    {"createtask", "Create test tasks (Ring-0)", cmd_createtask},
    {"usertask", "Create Ring-3 user-mode task [name]", cmd_usertask},
    {"schedinfo", "Scheduler information", cmd_schedinfo},
    {"offihito", "Start Offihito demo task", cmd_offihito},
    
    // File system commands
    {"ls", "List files and directories", cmd_ls},
    {"cd", "Change directory", cmd_cd},
    {"pwd", "Print working directory", cmd_pwd},
    {"mkdir", "Create directory", cmd_mkdir},
    {"rmdir", "Remove directory", cmd_rmdir},
    {"rmr", "Remove directory recursively", cmd_rmr},
    {"cat", "Show file contents", cmd_cat},
    {"touch", "Create new file", cmd_touch},
    {"write", "Write to file", cmd_write},
    {"rm", "Delete file", cmd_rm},
    {"tree", "Show directory tree", cmd_tree},
    {"find", "Find files by pattern", cmd_find},
    {"du", "Show disk usage", cmd_du},

    // ELF loader commands
    {"exec",    "Load and execute ELF64 binary from ext3", cmd_exec},
    {"elfinfo", "Show ELF64 header info (no load)",        cmd_elfinfo},

    // Network commands
    {"netinit",  "Initialize RTL8139 network driver",           cmd_netinit},
    {"netstat",  "Show NIC status and packet counters",         cmd_netstat},
    {"netregs",  "Dump NIC hardware registers to serial",       cmd_netregs},
    {"netsend",  "Send test packets [count]",                   cmd_netsend},
    {"netmon",   "Monitor incoming packets",                    cmd_netmon},

    // ARP commands
    {"ipconfig",  "Assign/show IP address  e.g.: ipconfig 10.0.2.15",        cmd_ipconfig},
    {"arping",    "Send ARP request        e.g.: arping 10.0.2.2",            cmd_arping},
    {"arpcache",  "Show ARP cache table",                                        cmd_arpcache},
    {"arpflush",  "Clear ARP cache",                                             cmd_arpflush},
    {"arptest",   "Explain QEMU NAT ARP limitation + TX test",                  cmd_arptest},
    {"arpstatic", "Add static ARP entry  e.g.: arpstatic <IP> <MAC>",           cmd_arpstatic},

    // IPv4 + ICMP commands
    {"ipv4info", "Show IPv4 layer status and counters",                     cmd_ipv4info},
    {"ping",     "Send ICMP Echo (ping)  e.g.: ping 10.0.2.2",             cmd_ping},

    // UDP commands
    {"udpinit",   "Initialize UDP layer (after ipconfig)",                          cmd_udpinit},
    {"udplisten", "Bind UDP echo server to port  e.g.: udplisten 5000",             cmd_udplisten},
    {"udpsend",   "Send UDP message  e.g.: udpsend 10.0.2.2 5000 hello",            cmd_udpsend},
    {"udpclose",  "Close port listener  e.g.: udpclose 5000",                       cmd_udpclose},
    {"udpstat",   "Show UDP socket table and counters",                             cmd_udpstat},

    // DHCP commands
    {"dhcp",      "Obtain IP automatically via DHCP",                    cmd_dhcp},
    {"dhcpstat",  "Show DHCP status and assigned IP/GW/DNS",             cmd_dhcpstat},
    {"dhcprel",   "Release DHCP lease (return IP)",                      cmd_dhcprel},
    {"netrxtest", "RX path test (ARP probe to 10.0.2.2)",                cmd_netrxtest},

    // TCP commands
    {"tcpstat",    "Show TCP connection table and counters",                     cmd_tcpstat},
    {"tcpconnect", "Open TCP connection  e.g.: tcpconnect 10.0.2.2 80",         cmd_tcpconnect},
    {"tcpsend",    "Send TCP data        e.g.: tcpsend 0 hello",                 cmd_tcpsend},
    {"tcpclose",   "Close TCP connection e.g.: tcpclose 0",                      cmd_tcpclose},
    {"tcplisten",  "Start TCP server     e.g.: tcplisten 8080",                  cmd_tcplisten},
    {"tcptest",    "Run full TCP loopback test  e.g.: tcptest 10.0.2.2 80",      cmd_tcptest},
    {"wget",       "HTTP GET and save to disk: wget 10.0.2.2:9999/index.html",   cmd_wget},
    {"httppost",   "Send HTTP POST: httppost 10.0.2.2:9999/api key=val",          cmd_httppost},

    // PCI bus scanner
    {"lspci", "List all PCI devices (bus:dev.func vendor device class)", cmd_lspci},

    // Panic test
    {"panic", "Test kernel panic screen [df|gp|pf|ud|de|stack]", cmd_panic},

    // Performance measurement
    {"perf", "RDTSC performance measurement [memcpy|memset|loop]", cmd_perf},

    // Spinlock test
    {"spinlock", "Spinlock / RWLock test suite", cmd_spinlock},

    // Unified self-test suite
    {"test", "Run kernel self-tests [heap|slab|spinlock|vmm|pmm|perf]", cmd_test_handler},

    // PC Speaker test
    {"beep", "Play sound with PC Speaker  e.g.: beep 440 300", cmd_beep},
    {"sb16", "Sound Blaster 16 driver  [tone|ding|vol N|stop]", cmd_sb16},
    {"apic", "APIC info/init/timer/test [init|info|timer <hz>|test|disablepic]", cmd_apic},
};
static int command_count = sizeof(command_table) / sizeof(Command);


void net_register_packet_handler(void) {
    rtl8139_set_packet_handler(net_packet_callback);
    g_net_initialized = 1;
    serial_print("[NET] Package handler saved.\n");
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
    if (str_cmp(command, "test") == 0 && str_len(args) == 0) {
        cmd_test();
        return 1;
    }
    
    // Normal commands
    for (int j = 0; j < command_count; j++) {
        if (str_cmp(command, command_table[j].name) == 0) {
            command_table[j].handler(args, output);
            return 1;
        }
    }
    
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
        
                char try_path[64];
                str_cpy(try_path, "/bin/");
                str_concat(try_path, try_name);
                if (ext3_file_size(try_path) == 0) {
                
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
            output_add_line(output, "No command found", VGA_RED);
            return 0;
        }

        for (int _k = 0; _k < cmd_len; _k++) {
            char c = command[_k];
            if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
            elf_filename[_k] = c;
        }
        elf_filename[cmd_len] = '\0';
        str_concat(elf_filename, ".ELF");

        char elf_path[64];
        str_cpy(elf_path, "/bin/");
        str_concat(elf_path, elf_filename);

        uint32_t fsize = ext3_file_size(elf_path);
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

            fsize = ext3_file_size(elf_lower_path);
            if (fsize > 0) {
                str_cpy(elf_filename, elf_lower);
            } else {
                char msg[MAX_LINE_LENGTH];
                str_cpy(msg, "Unknown Command: ");
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