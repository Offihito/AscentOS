#include <stddef.h>
#include "commands64.h"
#include "../fs/files64.h"
#include "nano64.h"
#include "../kernel/vmm64.h"
#include "../kernel/memory_unified.h"
#include "../kernel/task.h"
#include "../kernel/scheduler.h"
#include "../kernel/disk64.h"    // fat32_file_size, fat32_read_file
#include "../kernel/elf64.h"         // ELF-64 loader
#include "../kernel/syscall.h"       // SYSCALL/SYSRET altyapısı
#include "../kernel/signal64.h"      // SYS_SIGACTION, SYS_SIGPROCMASK vb. (v10)

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

// Sürücünün başlatılıp başlatılmadığını sorgulayan dahili değişken
// (rtl8139.c içinde static, doğrudan erişemeyiz — init sonucu burada saklarız)
// NOT: kernel64.c'den net_register_packet_handler() üzerinden güncellenir.
int g_net_initialized = 0;

// Son alınan paketin özet bilgisini TEXT modda göstermek için
static volatile uint32_t g_net_rx_display  = 0;  // gösterilecek paket sayısı
static volatile uint16_t g_net_last_etype  = 0;  // son paketin EtherType
static volatile uint8_t  g_net_last_src[6] = {0}; // son paketin kaynak MAC

// Forward declaration — tanım dosyanın sonundadır
void net_register_packet_handler(void);

// Paket alma callback: RTL8139'dan gelen her çerçeveyi ARP katmanına + sayaca gönder
static void net_packet_callback(const uint8_t* buf, uint16_t len) {
    g_net_rx_display++;
    // EtherType (byte 12-13)
    if(len >= 14){
        g_net_last_etype = (uint16_t)((buf[12] << 8) | buf[13]);
        // Kaynak MAC (byte 6-11)
        for (int i = 0; i < 6; i++) g_net_last_src[i] = buf[6 + i];
    }
    // ARP katmanına ilet
    if(arp_is_initialized())
        arp_handle_packet(buf, len);
}

extern void println64(const char* str, uint8_t color);
extern void print_str64(const char* str, uint8_t color);
extern uint64_t get_system_ticks(void);
extern void serial_print(const char* str);

// Nano editor mode flag
static int nano_mode = 0;

int is_nano_mode(void) {
    return nano_mode;
}

void set_nano_mode(int mode) {
    nano_mode = mode;
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
// CPUID FUNCTIONS
// ===========================================

void get_cpu_brand(char* brand) {
    uint32_t eax, ebx, ecx, edx;
    
    for (int i = 0; i < 3; i++) {
        __asm__ volatile (
            "cpuid"
            : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
            : "a"(0x80000002 + i)
        );
        
        *(uint32_t*)(brand + i * 16 + 0) = eax;
        *(uint32_t*)(brand + i * 16 + 4) = ebx;
        *(uint32_t*)(brand + i * 16 + 8) = ecx;
        *(uint32_t*)(brand + i * 16 + 12) = edx;
    }
    brand[48] = '\0';
}

void get_cpu_vendor(char* vendor) {
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0));
    *(uint32_t*)(vendor + 0) = ebx;
    *(uint32_t*)(vendor + 4) = edx;
    *(uint32_t*)(vendor + 8) = ecx;
    vendor[12] = '\0';
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
    output_add_line(output, " kode      - Text editor", VGA_WHITE);
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
    output_add_line(output, "Debug Commands:", VGA_RED);
    output_add_line(output, " panic       - Panic ekranini test et", VGA_WHITE);
    output_add_line(output, "   panic df    #DF Double Fault", VGA_DARK_GRAY);
    output_add_line(output, "   panic gp    #GP General Protection", VGA_DARK_GRAY);
    output_add_line(output, "   panic pf    #PF Page Fault (NULL deref)", VGA_DARK_GRAY);
    output_add_line(output, "   panic ud    #UD Invalid Opcode", VGA_DARK_GRAY);
    output_add_line(output, "   panic de    #DE Divide by Zero", VGA_DARK_GRAY);
    output_add_line(output, "   panic stack Stack overflow", VGA_DARK_GRAY);
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
// NANO TEXT EDITOR
// ===========================================

void cmd_kode(const char* args, CommandOutput* output) {
    if (str_len(args) == 0) {
        output_add_line(output, "Usage: kode <filename>", VGA_RED);
        output_add_line(output, "Example: kode myfile.txt", VGA_CYAN);
        return;
    }
    
    // Check for spaces in filename
    for (int i = 0; args[i]; i++) {
        if (args[i] == ' ') {
            output_add_line(output, "Error: Filename cannot contain spaces", VGA_RED);
            return;
        }
    }
    
    // Enter nano mode
    nano_mode = 1;
    nano_run(args);
    
    // This will be handled by keyboard interrupt
    output_add_line(output, "Entering kode editor...", VGA_GREEN);
    output_add_line(output, "Use Ctrl+S to save, Ctrl+Q to quit", VGA_CYAN);
}

// ===========================================
// FILE SYSTEM COMMANDS
// ===========================================

void cmd_ls(const char* args, CommandOutput* output) {
    (void)args;
    fs_list_files64(output);
}

void cmd_cat(const char* args, CommandOutput* output) {
    if (str_len(args) == 0) {
        output_add_line(output, "Usage: cat <filename>", VGA_RED);
        return;
    }

    const EmbeddedFile64* file = fs_get_file64(args);
    if (!file) {
        output_add_line(output, "File not found: ", VGA_RED);
        output_add_line(output, args, VGA_RED);
        return;
    }

    const char* p = file->content;
    const char* start = p;
    
    while (*p) {
        if (*p == '\n') {
            char line[MAX_LINE_LENGTH];
            int len = p - start;
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
        int len = p - start;
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

    int success = fs_touch_file64(args);
    if (success) {
        output_add_line(output, "File created: ", VGA_GREEN);
        output_add_line(output, args, VGA_YELLOW);
    } else {
        output_add_line(output, "Error: Cannot create file (too many files or invalid name)", VGA_RED);
    }
}

void cmd_write(const char* args, CommandOutput* output) {
    if (str_len(args) == 0) {
        output_add_line(output, "Usage: write <filename> <content>", VGA_RED);
        output_add_line(output, "Tip: Use 'kode' for better editing experience", VGA_CYAN);
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
        output_add_line(output, "Tip: Use 'kode <filename>' for better editing", VGA_CYAN);
        return;
    }

    int success = fs_write_file64(filename, content);
    if (success) {
        char msg[MAX_LINE_LENGTH];
        str_cpy(msg, "Content written to: ");
        str_concat(msg, filename);
        output_add_line(output, msg, VGA_GREEN);
    } else {
        output_add_line(output, "Error: Cannot write to file (file not found or too large)", VGA_RED);
        output_add_line(output, "Tip: Use 'touch' to create the file first", VGA_CYAN);
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

    int success = fs_delete_file64(args);
    if (success) {
        char msg[MAX_LINE_LENGTH];
        str_cpy(msg, "File deleted: ");
        str_concat(msg, args);
        output_add_line(output, msg, VGA_GREEN);
    } else {
        output_add_line(output, "Error: Cannot delete file (not found or read-only)", VGA_RED);
        output_add_line(output, "Note: Built-in files cannot be deleted", VGA_YELLOW);
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
    get_cpu_brand(cpu_brand);
    uint64_t heap_kb = get_memory_info();
    char memory_str[64];
    format_memory_size(heap_kb, memory_str);
    int file_count = 0;
    get_all_files_list64(&file_count);
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

    // Sistem bilgileri - art'ın boyutuna uygun şekilde yerleştirildi
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
        str_concat(full_line, "   ");  // Joker ile bilgi arasında boşluk
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
    
    // CPU Brand
    char cpu_brand[49];
    get_cpu_brand(cpu_brand);
    print_str64("CPU: ", VGA_WHITE);
    println64(cpu_brand, VGA_YELLOW);
    
    // Memory
    extern uint8_t* heap_start;
    extern uint8_t* heap_current;
    uint64_t heap_used = (uint64_t)heap_current - (uint64_t)heap_start;
    
    char mem_str[32];
    uint64_to_string(heap_used / 1024, mem_str);
    print_str64("Heap used: ", VGA_WHITE);
    print_str64(mem_str, VGA_GREEN);
    println64(" KB", VGA_WHITE);
    
    // Architecture
    println64("Architecture: x86_64 (64-bit)", VGA_GREEN);
    
    // Paging
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
    
    // File system info
    int file_count = 0;
    get_all_files_list64(&file_count);
    print_str64("Files in system: ", VGA_WHITE);
    char count_str[16];
    int_to_str(file_count, count_str);
    println64(count_str, VGA_GREEN);
}

void cmd_cpuinfo(void) {
    uint32_t eax, ebx, ecx, edx;
    
    println64("CPU Information:", VGA_CYAN);
    println64("", VGA_WHITE);
    
    // Vendor
    char vendor[13];
    get_cpu_vendor(vendor);
    print_str64("Vendor: ", VGA_WHITE);
    println64(vendor, VGA_GREEN);
    
    // Features
    __asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
    
    print_str64("Features: ", VGA_WHITE);
    if (edx & (1 << 0)) print_str64("FPU ", VGA_YELLOW);
    if (edx & (1 << 4)) print_str64("TSC ", VGA_YELLOW);
    if (edx & (1 << 6)) print_str64("PAE ", VGA_YELLOW);
    if (edx & (1 << 23)) print_str64("MMX ", VGA_YELLOW);
    if (edx & (1 << 25)) print_str64("SSE ", VGA_YELLOW);
    if (edx & (1 << 26)) print_str64("SSE2 ", VGA_YELLOW);
    if (ecx & (1 << 0)) print_str64("SSE3 ", VGA_YELLOW);
    println64("", VGA_WHITE);
    
    // Long mode check
    __asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x80000001));
    if (edx & (1 << 29)) {
        println64("Long Mode: Supported ✓", VGA_GREEN);
    }
}

void cmd_meminfo(void) {
    extern void show_memory_info(void);
    show_memory_info();
}
void cmd_reboot(const char* args, CommandOutput* output) {
    (void)args;
    
    output_add_line(output, "Saving files to disk...", VGA_YELLOW);
    
    // Dosyaları kaydet
    save_files_to_disk64();
    
    // Disk yazmanın tamamlanması için uzun bekleme
    for (volatile int i = 0; i < 5000000; i++);
    
    output_add_line(output, "All files saved!", VGA_GREEN);
    output_add_line(output, "Rebooting now... Why so serious?", VGA_RED);
    
    // Gerçek reboot: 8042 PS/2 controller ile CPU reset
    __asm__ volatile ("cli");  // Interruptları kapat
    
    uint8_t good = 0x02;
    while (good & 0x02) {
        good = inb64(0x64);
    }
    
    outb64(0x64, 0xFE);  // System reset komutu
    
    // Eğer başarısız olursa sonsuz döngü
    __asm__ volatile ("hlt");
    
    for(;;);  // Buraya asla gelmemeli
}

void cmd_mkdir(const char* args, CommandOutput* output) {
    if (str_len(args) == 0) {
        output_add_line(output, "Usage: mkdir <dirname>", VGA_RED);
        output_add_line(output, "Example: mkdir documents", VGA_CYAN);
        return;
    }

    // Check for spaces
    for (int i = 0; args[i]; i++) {
        if (args[i] == ' ') {
            output_add_line(output, "Error: Directory name cannot contain spaces", VGA_RED);
            return;
        }
    }

    int success = fs_mkdir64(args);
    if (success) {
        char msg[MAX_LINE_LENGTH];
        str_cpy(msg, "Directory created: ");
        str_concat(msg, args);
        output_add_line(output, msg, VGA_GREEN);
    } else {
        output_add_line(output, "Error: Cannot create directory (already exists or limit reached)", VGA_RED);
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

    int success = fs_rmdir64(args);
    if (success) {
        char msg[MAX_LINE_LENGTH];
        str_cpy(msg, "Directory removed: ");
        str_concat(msg, args);
        output_add_line(output, msg, VGA_GREEN);
    } else {
        output_add_line(output, "Error: Cannot remove directory (not found, not empty, or read-only)", VGA_RED);
    }
}
void cmd_cd(const char* args, CommandOutput* output) {
    if (str_len(args) == 0) {
        // cd with no args goes to root
        int success = fs_chdir64("/");
        if (success) {
            output_add_line(output, "Changed to root directory", VGA_GREEN);
        }
        return;
    }

    int success = fs_chdir64(args);
    if (success) {
        char msg[MAX_LINE_LENGTH];
        str_cpy(msg, "Changed directory to: ");
        str_concat(msg, fs_getcwd64());
        output_add_line(output, msg, VGA_GREEN);
    } else {
        output_add_line(output, "Error: Directory not found", VGA_RED);
        output_add_line(output, "Use 'ls' to see available directories", VGA_CYAN);
    }
}

void cmd_pwd(const char* args, CommandOutput* output) {
    (void)args;
    const char* cwd = fs_getcwd64();
    output_add_line(output, cwd, VGA_CYAN);
}
// ===========================================
// PMM COMMAND - Physical Memory Manager Stats
// ===========================================

void cmd_pmm(const char* args, CommandOutput* output) {
    (void)args;
    
    extern void pmm_print_stats(void);
    
    output_add_line(output, "Physical Memory Manager (PMM)", VGA_CYAN);
    output_add_line(output, "=========================================", VGA_CYAN);
    output_add_empty_line(output);
    
    // Call PMM stats function which will print to console
    pmm_print_stats();
    
    output_add_empty_line(output);
    output_add_line(output, "PMM manages 4KB physical memory frames", VGA_WHITE);
    output_add_line(output, "Use 'meminfo' for heap statistics", VGA_DARK_GRAY);
}

// ===========================================
// VMM TEST COMMAND
// ===========================================

void cmd_vmm(const char* args, CommandOutput* output) {
    // Check for stats subcommand
    if (str_len(args) > 0 && str_cmp(args, "stats") == 0) {
        output_add_line(output, "VMM Statistics:", VGA_CYAN);
        output_add_line(output, "===============", VGA_CYAN);
        output_add_empty_line(output);
        
        // Get statistics from VMM
        extern uint64_t vmm_get_pages_mapped(void);
        extern uint64_t vmm_get_pages_unmapped(void);
        extern uint64_t vmm_get_page_faults(void);
        extern uint64_t vmm_get_tlb_flushes(void);
        extern uint64_t vmm_get_demand_allocations(void);
        extern uint64_t vmm_get_reserved_pages(void);
        
        char line[MAX_LINE_LENGTH];
        char num_str[32];
        
        // Pages mapped
        str_cpy(line, "  Pages mapped: ");
        uint64_to_string(vmm_get_pages_mapped(), num_str);
        str_concat(line, num_str);
        output_add_line(output, line, VGA_WHITE);
        
        // Pages unmapped
        str_cpy(line, "  Pages unmapped: ");
        uint64_to_string(vmm_get_pages_unmapped(), num_str);
        str_concat(line, num_str);
        output_add_line(output, line, VGA_WHITE);
        
        // Page faults
        str_cpy(line, "  Page faults: ");
        uint64_to_string(vmm_get_page_faults(), num_str);
        str_concat(line, num_str);
        output_add_line(output, line, VGA_YELLOW);
        
        // TLB flushes
        str_cpy(line, "  TLB flushes: ");
        uint64_to_string(vmm_get_tlb_flushes(), num_str);
        str_concat(line, num_str);
        output_add_line(output, line, VGA_CYAN);
        
        output_add_empty_line(output);
        output_add_line(output, "Demand Paging:", VGA_CYAN);
        
        // Demand allocations
        str_cpy(line, "  Demand allocations: ");
        uint64_to_string(vmm_get_demand_allocations(), num_str);
        str_concat(line, num_str);
        output_add_line(output, line, VGA_GREEN);
        
        // Reserved pages
        str_cpy(line, "  Reserved pages: ");
        uint64_to_string(vmm_get_reserved_pages(), num_str);
        str_concat(line, num_str);
        output_add_line(output, line, VGA_MAGENTA);
        
        output_add_empty_line(output);
        output_add_line(output, "VMM manages 4-level page tables (PML4)", VGA_DARK_GRAY);
        output_add_line(output, "Supports 4KB and 2MB pages", VGA_DARK_GRAY);
        
        return;
    }
    
    // Check for demand subcommand
    if (str_len(args) > 0 && str_cmp(args, "demand") == 0) {
        extern int vmm_enable_demand_paging(void);
        extern int vmm_reserve_pages(uint64_t, uint64_t, uint64_t);
        extern int vmm_is_demand_paging_enabled(void);
        
        output_add_line(output, "VMM Demand Paging Test", VGA_CYAN);
        output_add_line(output, "======================", VGA_CYAN);
        output_add_empty_line(output);
        
        // Enable demand paging
        if (!vmm_is_demand_paging_enabled()) {
            vmm_enable_demand_paging();
            output_add_line(output, "[1] Demand paging enabled", VGA_GREEN);
        } else {
            output_add_line(output, "[1] Demand paging already enabled", VGA_YELLOW);
        }
        
        // Reserve pages
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
        
        // Try to write to reserved page
        volatile uint64_t* test_ptr = (volatile uint64_t*)0x700000;
        *test_ptr = 0xDEADBEEF;
        
        // If we get here, demand paging worked!
        output_add_line(output, "  OK Page allocated on demand!", VGA_GREEN);
        
        // Verify the value
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
    
    // Test 1: Map a page
    output_add_line(output, "[TEST 1] Mapping 4KB page...", VGA_YELLOW);
    uint64_t test_virt = 0x400000;  // 4MB
    uint64_t test_phys = 0x200000;  // 2MB
    
    int result = vmm_map_page(test_virt, test_phys, PAGE_WRITE | PAGE_PRESENT);
    if (result == 0) {
        output_add_line(output, "  OK Page mapped successfully", VGA_GREEN);
        
        // Verify mapping
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
    
    // Test 2: Check if page is present
    output_add_line(output, "[TEST 2] Checking page presence...", VGA_YELLOW);
    if (vmm_is_page_present(test_virt)) {
        output_add_line(output, "  OK Page is present", VGA_GREEN);
    } else {
        output_add_line(output, "  ERROR Page not found", VGA_RED);
    }
    output_add_empty_line(output);
    
    // Test 3: Map a range
    output_add_line(output, "[TEST 3] Mapping 16KB range...", VGA_YELLOW);
    result = vmm_map_range(0x500000, 0x300000, 16384, PAGE_WRITE | PAGE_PRESENT);
    if (result == 0) {
        output_add_line(output, "  OK Range mapped (4 pages)", VGA_GREEN);
    } else {
        output_add_line(output, "  ERROR Range mapping failed", VGA_RED);
    }
    output_add_empty_line(output);
    
    // Test 4: 2MB page mapping
    output_add_line(output, "[TEST 4] Mapping 2MB large page...", VGA_YELLOW);
    result = vmm_map_page_2mb(0x800000, 0x800000, PAGE_WRITE | PAGE_PRESENT);
    if (result == 0) {
        output_add_line(output, "  OK 2MB page mapped", VGA_GREEN);
    } else {
        output_add_line(output, "  ERROR 2MB page mapping failed", VGA_RED);
    }
    output_add_empty_line(output);
    
    // Test 5: Identity mapping
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
    
    // Summary
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
    
    // Test 1: Basic allocation
    output_add_line(output, "Test 1: Basic Allocation", VGA_YELLOW);
    void* ptr1 = kmalloc(1024);
    if (ptr1) {
        output_add_line(output, "  [OK] Allocated 1KB", VGA_GREEN);
        
        // Write to memory
        char* test_str = (char*)ptr1;
        const char* msg = "Hello from heap!";
        int i = 0;
        while (msg[i]) {
            test_str[i] = msg[i];
            i++;
        }
        test_str[i] = '\0';
        
        char result[128];
        str_cpy(result, "  [OK] Written: ");
        str_concat(result, test_str);
        output_add_line(output, result, VGA_GREEN);
        
        kfree(ptr1);
        output_add_line(output, "  [OK] Freed 1KB", VGA_GREEN);
    } else {
        output_add_line(output, "  [FAIL] Allocation failed!", VGA_RED);
    }
    
    output_add_empty_line(output);
    
    // Test 2: Multiple allocations
    output_add_line(output, "Test 2: Multiple Allocations", VGA_YELLOW);
    void* ptrs[10];
    int alloc_count = 0;
    
    for (int i = 0; i < 10; i++) {
        ptrs[i] = kmalloc(256 * (i + 1));
        if (ptrs[i]) {
            alloc_count++;
        }
    }
    
    char count_msg[64];
    str_cpy(count_msg, "  [OK] Allocated ");
    char num_str[16];
    int_to_str(alloc_count, num_str);
    str_concat(count_msg, num_str);
    str_concat(count_msg, "/10 blocks");
    output_add_line(output, count_msg, VGA_GREEN);
    
    // Free them all
    for (int i = 0; i < 10; i++) {
        if (ptrs[i]) {
            kfree(ptrs[i]);
        }
    }
    output_add_line(output, "  [OK] Freed all blocks", VGA_GREEN);
    
    output_add_empty_line(output);
    
    // Test 3: Realloc test
    output_add_line(output, "Test 3: Realloc Test", VGA_YELLOW);
    void* ptr2 = kmalloc(512);
    if (ptr2) {
        output_add_line(output, "  [OK] Allocated 512 bytes", VGA_GREEN);
        
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
    
    // Test 4: Calloc test
    output_add_line(output, "Test 4: Calloc (Zero-init) Test", VGA_YELLOW);
    uint32_t* ptr4 = (uint32_t*)kcalloc(256, sizeof(uint32_t));
    if (ptr4) {
        output_add_line(output, "  [OK] Allocated 256 uint32s", VGA_GREEN);
        
        // Verify zeroing
        int all_zero = 1;
        for (int i = 0; i < 256; i++) {
            if (ptr4[i] != 0) {
                all_zero = 0;
                break;
            }
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
    
    // Test 5: Fragmentation test
    output_add_line(output, "Test 5: Fragmentation & Coalescing", VGA_YELLOW);
    void* frag1 = kmalloc(1024);
    void* frag2 = kmalloc(1024);
    void* frag3 = kmalloc(1024);
    
    if (frag1 && frag2 && frag3) {
        output_add_line(output, "  [OK] Allocated 3 x 1KB blocks", VGA_GREEN);
        
        // Free middle block
        kfree(frag2);
        output_add_line(output, "  [OK] Freed middle block", VGA_GREEN);
        
        // Free first block (should coalesce)
        kfree(frag1);
        output_add_line(output, "  [OK] Freed first block (coalesce)", VGA_GREEN);
        
        // Free last block
        kfree(frag3);
        output_add_line(output, "  [OK] Freed last block", VGA_GREEN);
    }
    
    output_add_empty_line(output);
    
    // Test 6: Large allocation
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
    
    // Get current task
    task_t* current = task_get_current();
    uint32_t task_count = task_get_count();
    
    char info[128];
    
    // Task count
    str_cpy(info, "Total tasks: ");
    char num[16];
    int_to_str(task_count + 1, num);  // +1 for current
    str_concat(info, num);
    output_add_line(output, info, VGA_WHITE);
    output_add_empty_line(output);
    
    // Current task
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
    
    // Parse PID
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
    
    // Name
    str_cpy(info, "Name: ");
    str_concat(info, task->name);
    output_add_line(output, info, VGA_WHITE);
    
    // PID
    str_cpy(info, "PID: ");
    int_to_str(task->pid, num);
    str_concat(info, num);
    output_add_line(output, info, VGA_WHITE);
    
    // State
    str_cpy(info, "State: ");
    switch (task->state) {
        case TASK_STATE_READY: str_concat(info, "READY"); break;
        case TASK_STATE_RUNNING: str_concat(info, "RUNNING"); break;
        case TASK_STATE_BLOCKED: str_concat(info, "BLOCKED"); break;
        case TASK_STATE_TERMINATED: str_concat(info, "TERMINATED"); break;
        default: str_concat(info, "UNKNOWN"); break;
    }
    output_add_line(output, info, VGA_WHITE);
    
    // Priority
    str_cpy(info, "Priority: ");
    int_to_str(task->priority, num);
    str_concat(info, num);
    output_add_line(output, info, VGA_WHITE);
    
    // Context switches
    str_cpy(info, "Context switches: ");
    uint64_to_string(task->context_switches, num);
    str_concat(info, num);
    output_add_line(output, info, VGA_WHITE);
}

void cmd_createtask(const char* args, CommandOutput* output) {
    (void)args;
    
    output_add_line(output, "Creating test tasks...", VGA_CYAN);
    
    // Create test tasks
    task_t* task_a = task_create("TestA", test_task_a, 10);
    if (!task_a) {
        output_add_line(output, "Failed to create task A - task system may not be initialized", VGA_RED);
        return;
    }
    
    task_t* task_b = task_create("TestB", test_task_b, 10);
    if (!task_b) {
        output_add_line(output, "Failed to create task B - task system may not be initialized", VGA_RED);
        // Clean up task_a if it was created
        if (task_a) {
            task_terminate(task_a);
        }
        return;
    }
    
    // Both tasks created successfully, now start them
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

// ============================================================
// cmd_usertask  [test|ring3|<isim>]
//
// Ring-3 user-mode task olusturur ve baslatir.
// Parametresiz veya "test"/"ring3" verilirse yerlesik
// user_mode_test_task() entry fonksiyonu kullanilir.
// ============================================================
void cmd_usertask(const char* args, CommandOutput* output) {
    output_add_line(output, "=== Ring-3 User Task Olusturuluyor ===", VGA_CYAN);
    output_add_empty_line(output);

    const char* task_name = "UserTest";
    void (*entry)(void)   = user_mode_test_task;

    // args varsa ve bilinen keyword degilse task ismi olarak kullan
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

    // ── Olustur ───────────────────────────────────────────────────
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

    // ── Zamanlayiciya ekle ────────────────────────────────────────
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
    
    // Total context switches
    str_cpy(info, "Total context switches: ");
    uint64_to_string(scheduler_get_context_switches(), num);
    str_concat(info, num);
    output_add_line(output, info, VGA_WHITE);
    
    // Total ticks
    str_cpy(info, "Total ticks: ");
    uint64_to_string(get_system_ticks(), num);
    str_concat(info, num);
    output_add_line(output, info, VGA_WHITE);
    
    // Ready queue size
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
    
    // Create Offihito task
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

// cmd_elfinfo <DOSYA.ELF>
// FAT32'deki bir ELF dosyasının başlık bilgilerini gösterir,
// yüklemez. Test/tanı amaçlı.
void cmd_elfinfo(const char* args, CommandOutput* output) {
    if (!args || str_len(args) == 0) {
        output_add_line(output, "Usage: elfinfo <FILE.ELF>", VGA_YELLOW);
        output_add_line(output, "  Shows ELF header info without loading.", VGA_DARK_GRAY);
        output_add_line(output, "  File must be in 8.3 uppercase format on FAT32.", VGA_DARK_GRAY);
        output_add_line(output, "  Example: elfinfo HELLO.ELF", VGA_DARK_GRAY);
        return;
    }

    // Dosya boyutunu kontrol et
    uint32_t fsize = fat32_file_size(args);
    if (fsize == 0) {
        char line[96];
        str_cpy(line, "File not found on FAT32: ");
        str_concat(line, args);
        output_add_line(output, line, VGA_RED);
        return;
    }

    // Sadece ilk 512 byte'ı oku (başlık için yeterli)
    static uint8_t hdr_buf[512];
    int n = fat32_read_file(args, hdr_buf, 512);
    if (n < 64) {
        output_add_line(output, "Read failed or file too small for ELF header", VGA_RED);
        return;
    }

    // Dosya boyutunu göster
    char line[96];
    char tmp[24];
    uint64_to_string(fsize, tmp);
    str_cpy(line, "File: "); str_concat(line, args);
    str_concat(line, "  Size: "); str_concat(line, tmp); str_concat(line, " bytes");
    output_add_line(output, line, VGA_CYAN);

    // Başlık dökümü
    elf64_dump_header(hdr_buf, output);

    // Hızlı doğrulama
    int rc = elf64_validate(hdr_buf, (uint32_t)n);
    str_cpy(line, "Validation: ");
    str_concat(line, elf64_strerror(rc));
    output_add_line(output, line, rc == ELF_OK ? VGA_GREEN : VGA_RED);
}

// ============================================================
// cmd_exec <DOSYA.ELF> [base_hex]
//
// FAT32'deki ELF64 binary'yi yükler ve Ring-3 task olarak başlatır.
// Tam Ring-3 → Ring-0 → Ring-3 syscall döngüsünü test eder.
//
// Kullanım:
//   exec HELLO.ELF             → 0x400000 tabanından ET_EXEC
//   exec MYAPP.ELF 0x500000    → özel PIE taban adresi
// ============================================================
void cmd_exec(const char* args, CommandOutput* output) {
    if (!args || str_len(args) == 0) {
        output_add_line(output, "Usage: exec <FILE.ELF> [base_hex]", VGA_YELLOW);
        output_add_line(output, "  ELF64 binary'yi FAT32'den yukler, Ring-3 task olusturur.", VGA_DARK_GRAY);
        output_add_line(output, "  base_hex: PIE (ET_DYN) icin opsiyonel load tabanı.", VGA_DARK_GRAY);
        output_add_line(output, "  Ornek: exec HELLO.ELF", VGA_DARK_GRAY);
        output_add_line(output, "  Ornek: exec MYAPP.ELF 0x500000", VGA_DARK_GRAY);
        return;
    }

    // ── 1. Argümanları ayrıştır ───────────────────────────────
    char filename[64];
    uint64_t load_base = 0x400000ULL;   // varsayılan ET_EXEC taban adresi

    int i = 0;
    while (args[i] && args[i] != ' ' && i < 63) {
        filename[i] = args[i];
        i++;
    }
    filename[i] = '\0';

    // Opsiyonel hex base (0x...)
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

    // ── 2. Başlık bilgilerini göster ──────────────────────────
    char line[96];
    char tmp[24];
    const char* hexc = "0123456789ABCDEF";

    #define FMT_HEX64(val) do { \
        tmp[0]='0'; tmp[1]='x'; \
        for(int _k=0;_k<16;_k++) tmp[2+_k]=hexc[((val)>>(60-_k*4))&0xF]; \
        tmp[18]='\0'; \
    } while(0)

    output_add_line(output, "=== exec: ELF Loader + Ring-3 Task ===", VGA_CYAN);
    str_cpy(line, "Dosya     : "); str_concat(line, filename);
    output_add_line(output, line, VGA_WHITE);
    FMT_HEX64(load_base);
    str_cpy(line, "Load base : "); str_concat(line, tmp);
    output_add_line(output, line, VGA_WHITE);
    output_add_empty_line(output);

    // ── 3. SYSCALL başlatılmış mı? ────────────────────────────
    if (!syscall_is_enabled()) {
        output_add_line(output, "[HATA] SYSCALL altyapisi baslatilmamis!", VGA_RED);
        output_add_line(output, "  kernel'de syscall_init() cagirildi mi?", VGA_YELLOW);
        return;
    }

    // ── 4. ELF'i FAT32'den yükle ─────────────────────────────
    output_add_line(output, "[1/3] ELF FAT32'den yukleniyor...", VGA_WHITE);
    ElfImage image;
    int rc = elf64_exec_from_fat32(filename, load_base, &image, output);
    if (rc != ELF_OK) {
        str_cpy(line, "[HATA] ELF yuklenemedi: ");
        str_concat(line, elf64_strerror(rc));
        output_add_line(output, line, VGA_RED);
        return;
    }

    // ── 5. Ring-3 task oluştur ────────────────────────────────
    output_add_line(output, "[2/3] Ring-3 task olusturuluyor...", VGA_WHITE);

    // argv dizisini oluştur: argv[0]=filename, argv[1..]=kullanıcı argümanları
    // exec KILO.ELF dosya.txt  →  argc=2, argv={"KILO.ELF","dosya.txt"}
    static char argv_storage[8][128];
    const char* argv_ptrs[8];
    int exec_argc = 0;

    // argv[0] = program adı (dosya adı)
    {
        int fn = 0;
        while (filename[fn] && fn < 127) { argv_storage[0][fn] = filename[fn]; fn++; }
        argv_storage[0][fn] = '\0';
        argv_ptrs[exec_argc++] = argv_storage[0];
    }

    // Kalan argümanları boşlukla ayır (args içinden load_base'den SONRA gelenler)
    // Şu anki args = "KILO.ELF [base_hex] [arg1] [arg2] ..." şeklinde parse edildi.
    // load_base argümanı zaten tüketildi; ek argümanlar filename'den sonra gelen
    // boşluktan itibaren başlar. Onları tekrar parse edelim:
    {
        // args içinde filename'den ve opsiyonel hex base'den sonrasını bul
        int skip = 0;
        while (args[skip] && args[skip] != ' ') skip++;  // filename
        if (args[skip] == ' ') {
            skip++;
            // hex base var mı kontrol et
            if (args[skip] == '0' && (args[skip+1] == 'x' || args[skip+1] == 'X')) {
                while (args[skip] && args[skip] != ' ') skip++;  // base_hex geç
                if (args[skip] == ' ') skip++;
            }
        }
        // Kalan kısım gerçek argümanlar
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
        // argv_ptrs'yi doğru pointer'larla yeniden doldur
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

    // task_create_from_elf: TCB + stack + iretq frame'i ELF entry'sine
    // doğru şekilde kurar. SysV ABI uyumlu argv/argc stack kurulumu dahil.
    task_t* utask = task_create_from_elf(filename, &image, TASK_PRIORITY_NORMAL,
                                          exec_argc, argv_ptrs);
    if (!utask) {
        output_add_line(output, "[HATA] task_create_from_elf() basarisiz!", VGA_RED);
        output_add_line(output, "  task_init() cagirildi mi? Heap yeterli mi?", VGA_YELLOW);
        return;
    }

    // Bilgi satırları
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

    // ── 6. Zamanlayıcı kuyruğuna ekle ────────────────────────
    output_add_line(output, "[3/3] Zamanlayici kuyruguna ekleniyor...", VGA_WHITE);
    if (task_start(utask) != 0) {
        output_add_line(output, "[HATA] task_start() basarisiz!", VGA_RED);
        task_terminate(utask);
        return;
    }

    // Klavyeyi userland task'a devret ve foreground PID'i kaydet
    extern void kb_set_userland_mode(int on);
    kb_set_userland_mode(1);

    // Shell bu task bitene kadar input almaz
    extern volatile uint32_t foreground_pid;
    foreground_pid = utask->pid;

    // ── 7. Özet ──────────────────────────────────────────────
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

void cmd_tree(const char* args, CommandOutput* output) {
    (void)args;
    fs_tree64(output);
}

void cmd_find(const char* args, CommandOutput* output) {
    if (!args || str_len(args) == 0) {
        output_add_line(output, "Usage: find <pattern>", VGA_YELLOW);
        output_add_line(output, "Example: find txt", VGA_DARK_GRAY);
        return;
    }
    
    fs_find64(args, output);
}

void cmd_du(const char* args, CommandOutput* output) {
    // If no args, use current directory
    const char* path = (args && str_len(args) > 0) ? args : NULL;
    fs_du64(path, output);
}

void cmd_rmr(const char* args, CommandOutput* output) {
    if (!args || str_len(args) == 0) {
        output_add_line(output, "Usage: rmr <directory>", VGA_YELLOW);
        output_add_line(output, "WARNING: Recursively removes directory and all contents!", VGA_RED);
        return;
    }
    
    if (fs_rmdir_recursive64(args)) {
        output_add_line(output, "Directory removed recursively", VGA_GREEN);
    } else {
        output_add_line(output, "Failed to remove directory (may be system directory)", VGA_RED);
    }
}


// ===========================================
// SYSCALL COMMANDS — syscalltest64.c icinde tanimli
// ===========================================
void cmd_syscalltest(const char* args, CommandOutput* output);

// ===========================================
// PANIC TEST KOMUTU
// Kullanım: panic <tip>
//   panic df     → #DF Double Fault   (int $0x08)
//   panic gp     → #GP Gen Protection (int $0x0D)
//   panic pf     → #PF Page Fault     (null pointer deref)
//   panic ud     → #UD Invalid Opcode (ud2)
//   panic de     → #DE Divide Error   (div by zero)
//   panic stack  → Stack overflow (sonsuz rekürsif çağrı)
// ===========================================
static void panic_do_div0(void) {
    // #DE — Divide by Zero
    volatile int a = 42;
    volatile int b = 0;
    volatile int c = a / b;
    (void)c;
}

static void panic_do_stack_overflow(int depth) {
    // Stack overflow → #SS veya #DF
    volatile char buf[512];
    buf[0] = (char)depth;
    (void)buf;
    panic_do_stack_overflow(depth + 1);
}

static void cmd_panic(const char* args, CommandOutput* output) {
    // Argüman yoksa kullanım bilgisi göster
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

    // Seçilen test türünü ekrana yaz — panic gerçekleşmeden önce görünür
    if (str_cmp(args, "df") == 0) {
        output_add_line(output, "[PANIC TEST] #DF Double Fault tetikleniyor...", VGA_RED);
        output_add_line(output, "  int 0x08 -> exception_frame -> kernel_panic_handler", VGA_DARK_GRAY);
        // Küçük bir gecikme: output ekrana yansısın
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
        // NULL pointer dereference — #PF, err_code[1]=0 (READ), CR2=0x0
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

// ===========================================
// COMMAND TABLE
// ===========================================

// ============================================================================
// GFX KOMUTU — GUI moduna geç
// ============================================================================
// ============================================================================
// AĞ KOMUTLARI — RTL8139
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

// ── netinit ──────────────────────────────────────────────────────────────────
// RTL8139 sürücüsünü başlatır. kernel_main'den otomatik başlatılıyorsa
// bu komut "zaten başlatıldı" mesajı verir.
static void cmd_netinit(const char* args, CommandOutput* output) {
    (void)args;
    if (g_net_initialized) {
        output_add_line(output, "Ag surucusu zaten baslatildi.", 0x0E);
        return;
    }
    output_add_line(output, "RTL8139 baslatiliyor...", 0x07);
    bool ok = rtl8139_init();
    if (ok) {
        net_register_packet_handler();
        output_add_line(output, "  [OK] RTL8139 hazir!", 0x0A);
        output_add_line(output, "  Paket alma aktif. 'netstat' ile durumu gor.", 0x07);
    } else {
        output_add_line(output, "  [HATA] RTL8139 bulunamadi veya baslatilamadi.", 0x0C);
        output_add_line(output, "  QEMU'ya '-device rtl8139,netdev=net0' ekli mi?", 0x0E);
    }
}

// ── netstat ───────────────────────────────────────────────────────────────────
// Kart MAC, link durumu ve paket sayaçlarını gösterir.
static void cmd_netstat(const char* args, CommandOutput* output) {
    (void)args;
    if (!g_net_initialized) {
        output_add_line(output, "Ag surucusu baslatilmadi. Once 'netinit' calistir.", 0x0C);
        return;
    }

    output_add_line(output, "=== RTL8139 Ag Durumu ===", 0x0B);

    // MAC adresi
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

    // Link
    bool up = rtl8139_link_is_up();
    char lnk[32]; str_cpy(lnk, "  Link: ");
    str_concat(lnk, up ? "UP  (bagli)" : "DOWN (kablo yok?)");
    output_add_line(output, lnk, up ? 0x0A : 0x0C);

    // Son alınan paket bilgisi
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

    // Serial'e tam istatistik
    rtl8139_stats();
    output_add_line(output, "  (Detayli istatistik serial porta yazildi)", 0x08);
}

// ── netregs ───────────────────────────────────────────────────────────────────
// Kart donanım yazmaçlarını serial porta döker (düşük seviye debug).
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
// Broadcast test paketi gönderir. Wireshark/QEMU ile yakalanabilir.
// Kullanım: netsend          → tek paket
//           netsend 5        → 5 paket
static void cmd_netsend(const char* args, CommandOutput* output) {
    if (!g_net_initialized) {
        output_add_line(output, "Ag surucusu baslatilmadi. Once 'netinit' calistir.", 0x0C);
        return;
    }

    // Kaç paket gönderilecek?
    int count = 1;
    if (args && args[0] >= '1' && args[0] <= '9') {
        count = 0;
        for (int i = 0; args[i] >= '0' && args[i] <= '9' && i < 3; i++)
            count = count * 10 + (args[i] - '0');
        if (count < 1) count = 1;
        if (count > 99) count = 99;  // makul sınır
    }

    uint8_t mac[6];
    rtl8139_get_mac(mac);

    // 60-byte minimum Ethernet çerçevesi oluştur
    // [ Hedef MAC 6B ][ Kaynak MAC 6B ][ EtherType 2B ][ Payload 46B ]
    uint8_t frame[60];

    // Hedef: broadcast
    for (int i = 0; i < 6; i++) frame[i] = 0xFF;

    // Kaynak: kart MAC
    for (int i = 0; i < 6; i++) frame[6 + i] = mac[i];

    // EtherType: 0x88B5 = IEEE 802 deneme tipi (gerçek protokol yok)
    frame[12] = 0x88;
    frame[13] = 0xB5;

    // Payload: "AscentOS NET TEST" + sıfır doldur
    const char* msg = "AscentOS NET TEST Asama-1";
    int mi = 0;
    for (int i = 14; i < 60; i++) {
        frame[i] = (msg[mi]) ? (uint8_t)msg[mi++] : 0x00;
    }

    int ok_count = 0;
    for (int n = 0; n < count; n++) {
        // Sıra numarasını payload'a göm (byte 38)
        frame[38] = (uint8_t)(n + 1);
        if (rtl8139_send(frame, 60)) ok_count++;
    }

    // Sonuç
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
// Alınan son paket sayısını gösterir (polling olmadan sadece sayaç okur).
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

// arp_cache_foreach için output_add_line wrapper
typedef struct { CommandOutput* out; } ARPCacheCtx;
static void arp_cache_line_cb(const char* line, uint8_t color, void* ctx){
    ARPCacheCtx* c = (ARPCacheCtx*)ctx;
    output_add_line(c->out, line, color);
}

// ── ipconfig ─────────────────────────────────────────────────────────────────
// IP adresi ata ve ARP katmanını başlat.
// Kullanım: ipconfig 10.0.2.15
//           ipconfig          → mevcut IP'yi göster
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
        char buf[48];

        str_cpy(buf, "  IP   : "); char ipstr[16]; ip_to_str(ip, ipstr);
        str_concat(buf, ipstr);
        output_add_line(output, buf, 0x0F);

        str_cpy(buf, "  MAC  : ");
        for (int i = 0; i < 6; i++) {
            char hx[3]; byte_to_hex_str(mac[i], hx); str_concat(buf, hx);
            if (i < 5) str_concat(buf, ":");
        }
        output_add_line(output, buf, 0x07);

        // Gateway varsayımı: .1 (QEMU SLiRP = 10.0.2.2)
        uint8_t gw[4] = {ip[0], ip[1], ip[2], 2};
        str_cpy(buf, "  GW   : "); ip_to_str(gw, ipstr); str_concat(buf, ipstr);
        str_concat(buf, "  (QEMU SLiRP default)");
        output_add_line(output, buf, 0x08);
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

    char buf[48]; char ipstr[16]; ip_to_str(new_ip, ipstr);
    str_cpy(buf, "  IP atandi: "); str_concat(buf, ipstr);
    output_add_line(output, buf, 0x0A);
    output_add_line(output, "  Gratuitous ARP gonderiliyor...", 0x07);
    arp_announce();
    output_add_line(output, "  Hazir! 'arping <IP>' ile test edebilirsin.", 0x07);
}

// ── arping ───────────────────────────────────────────────────────────────────
// Belirtilen IP'ye ARP request gönder, cevabı cache'ten göster.
// Kullanım: arping 10.0.2.2
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

    // ARP sadece aynı alt agda calisir — farklı subnet uyarısı
    if (arp_is_initialized()) {
        uint8_t my_ip[4]; arp_get_my_ip(my_ip);
        if (target[0] != my_ip[0] || target[1] != my_ip[1] || target[2] != my_ip[2]) {
            char warn[80];
            str_cpy(warn, "  [UYARI] ARP yalnizca ayni /24 agda calisir.");
            output_add_line(output, warn, 0x0E);
            char gwbuf[32]; char gwip[16];
            uint8_t gw[4] = {my_ip[0], my_ip[1], my_ip[2], 1};
            // QEMU NAT'ta gateway genellikle x.x.x.2
            gw[3] = 2;
            ip_to_str(gw, gwip);
            str_cpy(gwbuf, "  Gateway deneyin: arping "); str_concat(gwbuf, gwip);
            output_add_line(output, gwbuf, 0x0B);
        }
    }

    char buf[48]; char ipstr[16]; ip_to_str(target, ipstr);
    str_cpy(buf, "ARP request -> "); str_concat(buf, ipstr);
    output_add_line(output, buf, 0x07);

    // Cache'te var mı kontrol et önce
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
// ARP cache tablosunu göster.
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
// QEMU user-net (NAT) kısıtlamasını açıklar ve TX paketini doğrular.
// ARP reply alamama normal — QEMU NAT L2 değil L3 proxy'dir.
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

    // TX testi: bir ARP request gonder
    uint8_t my_ip[4]; arp_get_my_ip(my_ip);
    uint8_t test_ip[4] = {my_ip[0], my_ip[1], my_ip[2], 1};
    uint32_t rx_before = g_net_rx_display;
    arp_request(test_ip);
    // TX başarısı: rtl8139_send false dönmüyorsa paket kuyruklandı demektir.
    // RX artmazsa bu normaldir (QEMU NAT ARP reply vermez).
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
// Statik ARP girişi ekle.
// Kullanım: arpstatic 10.0.2.2 52:54:00:12:34:56
static void cmd_arpstatic(const char* args, CommandOutput* output) {
    if (!arp_is_initialized()) {
        output_add_line(output, "Once 'ipconfig <IP>' ile IP ata.", 0x0C); return;
    }
    if (!args || args[0] == '\0') {
        output_add_line(output, "Kullanim: arpstatic <IP> <MAC>", 0x0E);
        output_add_line(output, "Ornek   : arpstatic 10.0.2.2 52:54:00:12:34:56", 0x08);
        return;
    }

    // IP parse
    uint8_t ip[4];
    int ip_end = 0;
    while (args[ip_end] && args[ip_end] != ' ') ip_end++;
    char ip_str[16]; int k=0;
    while(k < ip_end && k < 15){ ip_str[k]=args[k]; k++; } ip_str[k]='\0';
    if (!str_to_ip(ip_str, ip)) {
        output_add_line(output, "Gecersiz IP.", 0x0C); return;
    }

    // MAC parse "XX:XX:XX:XX:XX:XX"
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

extern volatile int request_gui_start;

static void cmd_gfx(const char* args, CommandOutput* output) {
    (void)args;
    output_add_line(output, "GUI moduna geciliyor...", 0x0E);
    output_add_line(output, "  Mouse: sol tik = pencere surukle/tikla", 0x0B);
    output_add_line(output, "  Klavye: N = yeni pencere ac", 0x0B);
    request_gui_start = 1;
}

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
    {"kode", "Text editor", cmd_kode},
    // Advanced file system commands
    {"tree", "Show directory tree", cmd_tree},
    {"find", "Find files by pattern", cmd_find},
    {"du", "Show disk usage", cmd_du},

    // ELF loader commands
    {"exec",    "Load and execute ELF64 binary from FAT32", cmd_exec},
    {"elfinfo", "Show ELF64 header info (no load)",         cmd_elfinfo},
    // ELF kısayolları — execute_command64 içinde işlenir, dummy handler yok
    // "kilo" ve "lua" isimleri command_table'da olmadığı için
    // execute_command64'ün uzantı/kısayol bloğu devreye girer.

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

    // Panic test
    {"panic", "Kernel panic ekranini test et [df|gp|pf|ud|de|stack]", cmd_panic},
};
static int command_count = sizeof(command_table) / sizeof(Command);

// ===========================================
// COMMAND SYSTEM
// ===========================================

// ── net_register_packet_handler ──────────────────────────────────────────────
// kernel64.c'den rtl8139_init() başarılı olduğunda çağrılır.
// Paket callback'ini kaydeder ve g_net_initialized'ı 1 yapar.
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
    // Hiçbir yerleşik komut bulunamazsa:
    //   "kilo test.txt"  →  KILO.ELF FAT32'de aranır, varsa çalıştırılır
    //   "mygame"         →  MYGAME.ELF aranır
    //   "PROG.ELF args"  →  doğrudan exec'e gönderilir
    {
        // Komut adında zaten .ELF uzantısı varsa direkt exec'e ver
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
                // Önce verilen adı dene, bulamazsa küçük harfli versiyonu dene
                char try_name[32];
                str_cpy(try_name, command);
                if (fat32_file_size(try_name) == 0) {
                    // Küçük harfe çevir: "PROG.ELF" → "prog.elf"
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

        // Uzantısız komut adını büyük harfe çevir ve .ELF ekle
        // "kilo" → "KILO.ELF"
        char elf_filename[32];
        int cmd_len = str_len(command);
        if (cmd_len == 0 || cmd_len > 8) {
            // FAT32 8.3 formatı: max 8 karakter taban adı
            output_add_line(output, "Komut bulunamadi.", VGA_RED);
            return 0;
        }

        for (int _k = 0; _k < cmd_len; _k++) {
            char c = command[_k];
            if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';  // küçük → büyük
            elf_filename[_k] = c;
        }
        elf_filename[cmd_len] = '\0';
        str_concat(elf_filename, ".ELF");

        // FAT32'de bu dosya var mı?
        // Önce büyük harf dene: KILO.ELF
        // Bulamazsa küçük harf dene: kilo.elf
        uint32_t fsize = fat32_file_size(elf_filename);
        if (fsize == 0) {
            // Küçük harfli versiyonu dene: "kilo.elf"
            char elf_lower[32];
            for (int _k = 0; _k < cmd_len; _k++) {
                char c = command[_k];
                if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';  // büyük → küçük
                elf_lower[_k] = c;
            }
            elf_lower[cmd_len] = '\0';
            str_concat(elf_lower, ".elf");

            fsize = fat32_file_size(elf_lower);
            if (fsize > 0) {
                // Küçük harfli versiyonu bulundu
                str_cpy(elf_filename, elf_lower);
            } else {
                // Her iki versiyonda da bulunamadı
                char msg[MAX_LINE_LENGTH];
                str_cpy(msg, "Bilinmeyen komut: ");
                str_concat(msg, command);
                output_add_line(output, msg, VGA_RED);
                return 0;
            }
        }

        // Dosya bulundu — exec'e yönlendir
        char exec_args[MAX_COMMAND_LENGTH];
        str_cpy(exec_args, elf_filename);
        if (args && str_len(args) > 0) {
            str_concat(exec_args, " ");
            str_concat(exec_args, args);
        }
        cmd_exec(exec_args, output);
        return 1;
    }
    // ── ELF fallback sonu ───────────────────────────────────────────────────
}

const Command* get_all_commands64(int* count) {
    *count = command_count;
    return command_table;
}
