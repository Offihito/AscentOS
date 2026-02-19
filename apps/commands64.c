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

void cmd_jew(const char* args, CommandOutput* output) {
    (void)args;
    output_add_line(output, "A DALLIR? THATS A BIG PRABLEM", VGA_YELLOW);
}

void cmd_help(const char* args, CommandOutput* output) {
    (void)args;
    output_add_line(output, "Available commands:", VGA_CYAN);
    output_add_line(output, " hello     - Say hello", VGA_WHITE);
    output_add_line(output, " clear     - Clear screen", VGA_WHITE);
    output_add_line(output, " help      - Show this help", VGA_WHITE);
    output_add_line(output, " jew       - JEW JEW JEW", VGA_WHITE);
    output_add_line(output, " echo      - Echo text", VGA_WHITE);
    output_add_line(output, " about     - About AscentOS", VGA_WHITE);
    output_add_line(output, " neofetch  - Show system info", VGA_WHITE);
    output_add_line(output, " pmm       - Physical Memory Manager stats", VGA_WHITE);
    output_add_line(output, " vmm       - Virtual Memory Manager test", VGA_WHITE);
    output_add_empty_line(output);
    output_add_line(output, "ELF Loader Commands:", VGA_YELLOW);
    output_add_line(output, " exec      - Load ELF64 + Ring-3 task olustur", VGA_WHITE);
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
    output_add_line(output, "       Also Fuck Lalyn and Kamil", VGA_RED);
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

    str_cpy(info_lines[16], "Fuck Lalyn and Kamil forever");
    str_cpy(info_lines[17], "Why so serious? ;) Type 'help'");

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

    // task_create_user() entry point olarak ELF'in giriş adresini kullan.
    // Fonksiyon pointer cast: void(*)(void) — flat memory'de güvenli.
    void (*entry_fn)(void) = (void (*)(void))image.entry;

    task_t* utask = task_create_user(filename, entry_fn, TASK_PRIORITY_NORMAL);
    if (!utask) {
        output_add_line(output, "[HATA] task_create_user() basarisiz!", VGA_RED);
        output_add_line(output, "  task_init() cagirildi mi? Heap yeterli mi?", VGA_YELLOW);
        return;
    }

    // ELF entry point'i context'e doğrudan yaz (cast artifact'ı olmadan kesin override)
    utask->context.rip    = image.entry;
    utask->context.rflags = 0x202;   // IF=1 (kesmeler açık)

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

    // Klavyeyi userland task'a devret
    extern void kb_set_userland_mode(int on);
    kb_set_userland_mode(1);

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
// SYSCALL COMMANDS
// ===========================================

// syscallinfo: MSR durumu, segment bilgisi ve tam syscall tablosunu göster
void cmd_syscallinfo(const char* args, CommandOutput* output) {
    (void)args;
    char line[96];
    char tmp[32];
    const char* hexc = "0123456789ABCDEF";

    // hex64 yardimcisi: tmp[0..17] = "0x" + 16 hex digit
    #define HEX64(v) do { \
        tmp[0]='0'; tmp[1]='x'; \
        for(int _i=0;_i<16;_i++) tmp[2+_i]=hexc[((v)>>(60-_i*4))&0xF]; \
        tmp[18]='\0'; \
    } while(0)

    output_add_line(output, "=== SYSCALL/SYSRET Infrastructure ===", VGA_CYAN);
    output_add_empty_line(output);

    // ── Durum ──────────────────────────────────────────────────
    int enabled = syscall_is_enabled();
    output_add_line(output,
        enabled ? "Status : ACTIVE (SYSCALL/SYSRET ready)"
                : "Status : NOT INITIALIZED",
        enabled ? VGA_GREEN : VGA_RED);

    // ── Mevcut task bilgisi ────────────────────────────────────
    {
        task_t* cur = task_get_current();
        if (cur) {
            str_cpy(line, "Task   : ");
            str_concat(line, cur->name);
            str_concat(line, "  PID=");
            int_to_str((int)cur->pid, tmp); str_concat(line, tmp);
            str_concat(line, "  prio=");
            int_to_str((int)cur->priority, tmp); str_concat(line, tmp);
            str_concat(line, cur->privilege_level == 0 ? "  Ring-0" : "  Ring-3");
            output_add_line(output, line, VGA_WHITE);
        }
    }

    output_add_empty_line(output);
    output_add_line(output, "── MSR Registers ────────────────────", VGA_YELLOW);

    // IA32_EFER
    uint64_t efer = rdmsr(MSR_EFER);
    HEX64(efer);
    str_cpy(line, "EFER  : "); str_concat(line, tmp);
    str_concat(line, (efer & EFER_SCE) ? "  SCE=1" : "  SCE=0 (!)");
    str_concat(line, (efer & EFER_LMA) ? " LMA=1" : " LMA=0");
    str_concat(line, (efer & EFER_NXE) ? " NXE=1" : "");
    output_add_line(output, line, (efer & EFER_SCE) ? VGA_GREEN : VGA_RED);

    // IA32_STAR
    uint64_t star = rdmsr(MSR_STAR);
    HEX64(star);
    str_cpy(line, "STAR  : "); str_concat(line, tmp);
    output_add_line(output, line, VGA_WHITE);

    // STAR decoded
    uint64_t kcs  = (star >> 32) & 0xFFFF;
    uint64_t ucb  = (star >> 48) & 0xFFFF;   // USER_CS_BASE
    uint64_t kss  = kcs + 8;
    uint64_t ucs  = (ucb + 16) | 3;
    uint64_t uss  = (ucb + 8)  | 3;
    #define HEX16(v) do { \
        int _v=(int)(v); \
        tmp[0]='0'; tmp[1]='x'; tmp[2]=hexc[(_v>>8)&0xF]; \
        tmp[3]=hexc[(_v>>4)&0xF]; tmp[4]=hexc[_v&0xF]; tmp[5]='\0'; \
    } while(0)

    HEX16(kcs); str_cpy(line, "  SYSCALL  CS="); str_concat(line, tmp);
    HEX16(kss); str_concat(line, " SS="); str_concat(line, tmp);
    str_concat(line, "  (Kernel Ring-0)");
    output_add_line(output, line, VGA_WHITE);

    HEX16(ucs); str_cpy(line, "  SYSRET   CS="); str_concat(line, tmp);
    HEX16(uss); str_concat(line, " SS="); str_concat(line, tmp);
    str_concat(line, "  (User Ring-3)");
    output_add_line(output, line, VGA_WHITE);

    // IA32_LSTAR
    uint64_t lstar = rdmsr(MSR_LSTAR);
    HEX64(lstar);
    str_cpy(line, "LSTAR : "); str_concat(line, tmp);
    str_concat(line, "  (syscall_entry)");
    output_add_line(output, line, VGA_CYAN);

    // IA32_FMASK
    uint64_t fmask = rdmsr(MSR_FMASK);
    HEX64(fmask);
    str_cpy(line, "FMASK : "); str_concat(line, tmp);
    str_concat(line, (fmask & 0x200) ? "  IF masked" : "  IF NOT masked (!)");
    str_concat(line, (fmask & 0x400) ? " DF masked" : "");
    output_add_line(output, line, (fmask & 0x200) ? VGA_GREEN : VGA_RED);

    // CSTAR (compat)
    uint64_t cstar = rdmsr(MSR_CSTAR);
    HEX64(cstar);
    str_cpy(line, "CSTAR : "); str_concat(line, tmp);
    str_concat(line, "  (32-bit compat, unused)");
    output_add_line(output, line, VGA_DARK_GRAY);

    // ── Syscall Tablosu ───────────────────────────────────────
    output_add_empty_line(output);
    output_add_line(output, "── Syscall Table ────────────────────", VGA_YELLOW);
    output_add_line(output, " 1  SYS_WRITE       write(fd,buf,len)          -> bytes", VGA_GREEN);
    output_add_line(output, " 2  SYS_READ        read(fd,buf,len)           -> bytes", VGA_GREEN);
    output_add_line(output, " 3  SYS_EXIT        exit(code)                 -> noreturn", VGA_GREEN);
    output_add_line(output, " 4  SYS_GETPID      getpid()                   -> pid", VGA_GREEN);
    output_add_line(output, " 5  SYS_YIELD       yield()                    -> 0", VGA_GREEN);
    output_add_line(output, " 6  SYS_SLEEP       sleep(ticks)               -> 0", VGA_GREEN);
    output_add_line(output, " 7  SYS_UPTIME      uptime()                   -> ticks", VGA_GREEN);
    output_add_line(output, " 8  SYS_DEBUG       debug(msg)                 -> 0", VGA_GREEN);
    output_add_line(output, " 9  SYS_OPEN        open(path,flags)           -> fd", VGA_GREEN);
    output_add_line(output, "10  SYS_CLOSE       close(fd)                  -> 0", VGA_GREEN);
    output_add_line(output, "11  SYS_GETPPID     getppid()                  -> ppid", VGA_GREEN);
    output_add_line(output, "12  SYS_SBRK        sbrk(incr)                 -> old_brk", VGA_GREEN);
    output_add_line(output, "13  SYS_GETPRIORITY getpriority()              -> 0-255", VGA_GREEN);
    output_add_line(output, "14  SYS_SETPRIORITY setpriority(prio)          -> 0", VGA_GREEN);
    output_add_line(output, "15  SYS_GETTICKS    getticks()                 -> ticks", VGA_GREEN);
    output_add_line(output, "── v3 New ───────────────────────────", VGA_YELLOW);
    output_add_line(output, "16  SYS_MMAP        mmap(addr,len,prot,fl,fd,off) -> addr", VGA_CYAN);
    output_add_line(output, "17  SYS_MUNMAP      munmap(addr,len)           -> 0", VGA_CYAN);
    output_add_line(output, "18  SYS_BRK         brk(addr)                  -> new_brk", VGA_CYAN);
    output_add_line(output, "19  SYS_FORK        fork()                     -> pid|0", VGA_CYAN);
    output_add_line(output, "20  SYS_EXECVE      execve(path,argv,envp)     -> err", VGA_CYAN);
    output_add_line(output, "21  SYS_WAITPID     waitpid(pid,*st,opts)      -> pid", VGA_CYAN);
    output_add_line(output, "22  SYS_PIPE        pipe(fd[2])                -> 0", VGA_CYAN);
    output_add_line(output, "23  SYS_DUP2        dup2(old,new)              -> new", VGA_CYAN);

    output_add_empty_line(output);
    output_add_line(output, "── Error Codes ──────────────────────", VGA_YELLOW);
    output_add_line(output, "  0  OK      -1 EINVAL  -2 ENOSYS  -3 EPERM", VGA_WHITE);
    output_add_line(output, " -4  ENOENT  -5 EBADF   -6 ENOMEM  -7 EBUSY", VGA_WHITE);
    output_add_line(output, " -8  EMFILE  -9 EAGAIN -10 ECHILD -11 EFAULT", VGA_WHITE);
    output_add_line(output, "-12  ENOSPC -13 ERANGE", VGA_WHITE);

    output_add_empty_line(output);
    output_add_line(output, "Run 'syscalltest' to execute all 23 tests.", VGA_GREEN);

    #undef HEX64
    #undef HEX16
}

// ============================================================
// YARDIMCI MAKROLAR – syscalltest icinde inline ASM kisaltmalari
// ============================================================
#define _SC0(num) \
    __asm__ volatile("syscall":"=a"(ret):"a"((uint64_t)(num)):"rcx","r11")
#define _SC1(num,a1) \
    __asm__ volatile("syscall":"=a"(ret):"a"((uint64_t)(num)),"D"((uint64_t)(a1)):"rcx","r11","memory")
#define _SC2(num,a1,a2) \
    __asm__ volatile("syscall":"=a"(ret):"a"((uint64_t)(num)),"D"((uint64_t)(a1)),"S"((uint64_t)(a2)):"rcx","r11","memory")
#define _SC3(num,a1,a2,a3) \
    __asm__ volatile("syscall":"=a"(ret):"a"((uint64_t)(num)),"D"((uint64_t)(a1)),"S"((uint64_t)(a2)),"d"((uint64_t)(a3)):"rcx","r11","memory")

// u64 -> decimal string
static void u64_to_dec(uint64_t v, char* out) {
    if (v == 0) { out[0]='0'; out[1]='\0'; return; }
    int i = 0; uint64_t t = v;
    while (t > 0) { out[i++] = '0' + (t % 10); t /= 10; }
    out[i] = '\0';
    for (int a = 0, b = i-1; a < b; a++, b--) {
        char c = out[a]; out[a] = out[b]; out[b] = c;
    }
}

// PASS/FAIL satiri olustur ve output buffer'a ekle
static void sc_result(CommandOutput* output, int idx, const char* name,
                      int64_t ret_val, int pass_cond,
                      const char* extra, int* pass, int* fail) {
    char line[96]; char tmp[24];
    line[0]='['; line[1]='0'+(idx/10); line[2]='0'+(idx%10);
    line[3]=']'; line[4]=' '; line[5]='\0';
    str_concat(line, name);
    str_concat(line, " ret=");
    int_to_str((int)ret_val, tmp);
    str_concat(line, tmp);
    if (extra && extra[0]) { str_concat(line, " "); str_concat(line, extra); }
    str_concat(line, pass_cond ? "  PASS" : "  FAIL");
    output_add_line(output, line, pass_cond ? VGA_GREEN : VGA_RED);
    pass_cond ? (*pass)++ : (*fail)++;
}

// ============================================================
// CMD_SYSCALLTEST – syscall test paketi v3 (23 test)
//
// NOTLAR:
//   - SYS_WRITE serial'a yazar (VGA değil) — kasıtlı.
//   - SYS_READ non-blocking; veri yoksa 0 döner = PASS.
//   - SYS_FORK: kernel context'te smoke test; çocuk hemen exit eder.
//   - SYS_MMAP: 6-arg syscall; R10 ile ayrı asm bloğu kullanılır.
//   - SYS_EXECVE: stub olduğu için ENOSYS(-2) beklenir.
// ============================================================
void cmd_syscalltest(const char* args, CommandOutput* output) {
    (void)args;

    if (!syscall_is_enabled()) {
        output_add_line(output, "ERROR: SYSCALL not initialized!", VGA_RED);
        output_add_line(output, "Call syscall_init() first.", VGA_YELLOW);
        return;
    }

    output_add_line(output, "=== SYSCALL Test Suite v3 (23 tests) ===", VGA_CYAN);
    output_add_line(output, "  (SYS_WRITE -> serial, SYS_FORK smoke only)", VGA_YELLOW);
    output_add_empty_line(output);

    uint64_t ret;
    int pass = 0, fail = 0;
    char tmp[32]; char line[96];
    const char* hexc = "0123456789ABCDEF";

    #define HEX64S(v) do { \
        tmp[0]='0'; tmp[1]='x'; \
        for(int _i=0;_i<16;_i++) tmp[2+_i]=hexc[((v)>>(60-_i*4))&0xF]; \
        tmp[18]='\0'; \
    } while(0)

    // ── [01] SYS_WRITE fd=1 ────────────────────────────────────
    static const char wbuf[] = "[SYS_WRITE fd=1 test]\n";
    uint64_t wlen = 22;
    _SC3(SYS_WRITE, 1, wbuf, wlen);
    sc_result(output, 1, "SYS_WRITE(fd=1)", (int64_t)ret,
              ret == wlen, "(serial out)", &pass, &fail);

    // ── [02] SYS_WRITE fd=2 ────────────────────────────────────
    static const char ebuf[] = "[SYS_WRITE fd=2 test]\n";
    uint64_t elen = 22;
    _SC3(SYS_WRITE, 2, ebuf, elen);
    sc_result(output, 2, "SYS_WRITE(fd=2)", (int64_t)ret,
              ret == elen, "(serial out)", &pass, &fail);

    // ── [03] SYS_WRITE fd=0 stdin → EBADF ─────────────────────
    _SC3(SYS_WRITE, 0, wbuf, wlen);
    sc_result(output, 3, "SYS_WRITE(fd=0)", (int64_t)ret,
              (int64_t)ret == (int64_t)SYSCALL_ERR_BADF,
              "expect EBADF", &pass, &fail);

    // ── [04] SYS_WRITE fd=99 geçersiz → EBADF ─────────────────
    _SC3(SYS_WRITE, 99, wbuf, wlen);
    sc_result(output, 4, "SYS_WRITE(fd=99)", (int64_t)ret,
              (int64_t)ret == (int64_t)SYSCALL_ERR_BADF,
              "expect EBADF", &pass, &fail);

    // ── [05] SYS_READ fd=0 non-blocking ───────────────────────
    char rbuf[32];
    _SC3(SYS_READ, 0, rbuf, 16);
    {
        int ok = ((int64_t)ret >= 0);
        str_cpy(line, "[05] SYS_READ(fd=0) bytes=");
        int_to_str((int)ret, tmp); str_concat(line, tmp);
        str_concat(line, ok ? " (ok)  PASS" : "  FAIL");
        output_add_line(output, line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [06] SYS_READ fd=1 → EBADF ────────────────────────────
    _SC3(SYS_READ, 1, rbuf, 16);
    sc_result(output, 6, "SYS_READ(fd=1)", (int64_t)ret,
              (int64_t)ret == (int64_t)SYSCALL_ERR_BADF,
              "expect EBADF", &pass, &fail);

    // ── [07] SYS_GETPID ────────────────────────────────────────
    _SC0(SYS_GETPID);
    {
        int ok = ((int64_t)ret >= 0);
        str_cpy(line, "[07] SYS_GETPID pid=");
        int_to_str((int)ret, tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        output_add_line(output, line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [08] SYS_GETPPID ───────────────────────────────────────
    _SC0(SYS_GETPPID);
    {
        int ok = ((int64_t)ret >= 0);
        str_cpy(line, "[08] SYS_GETPPID ppid=");
        int_to_str((int)ret, tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        output_add_line(output, line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [09] SYS_UPTIME ────────────────────────────────────────
    _SC0(SYS_UPTIME);
    {
        int ok = ((int64_t)ret >= 0);
        u64_to_dec(ret, tmp);
        str_cpy(line, "[09] SYS_UPTIME ticks="); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        output_add_line(output, line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [10] SYS_GETTICKS ──────────────────────────────────────
    _SC0(SYS_GETTICKS);
    {
        int ok = ((int64_t)ret >= 0);
        u64_to_dec(ret, tmp);
        str_cpy(line, "[10] SYS_GETTICKS ticks="); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        output_add_line(output, line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [11] SYS_YIELD ─────────────────────────────────────────
    _SC0(SYS_YIELD);
    sc_result(output, 11, "SYS_YIELD", (int64_t)ret,
              (int64_t)ret == 0, "expect 0", &pass, &fail);

    // ── [12] SYS_SLEEP(0) ──────────────────────────────────────
    _SC1(SYS_SLEEP, 0);
    sc_result(output, 12, "SYS_SLEEP(0)", (int64_t)ret,
              (int64_t)ret == 0, "expect 0", &pass, &fail);

    // ── [13] SYS_DEBUG ─────────────────────────────────────────
    static const char dmsg[] = "syscalltest v3 debug probe";
    _SC1(SYS_DEBUG, dmsg);
    sc_result(output, 13, "SYS_DEBUG", (int64_t)ret,
              (int64_t)ret == 0, "(serial log)", &pass, &fail);

    // ── [14] SYS_SETPRIORITY / SYS_GETPRIORITY ─────────────────
    _SC0(SYS_GETPRIORITY);
    uint64_t old_prio = ret;
    uint64_t test_prio = (old_prio < 205) ? (old_prio + 50) : (old_prio - 50);
    _SC1(SYS_SETPRIORITY, test_prio);
    uint64_t set_ret = ret;
    _SC0(SYS_GETPRIORITY);
    uint64_t new_prio = ret;
    _SC1(SYS_SETPRIORITY, old_prio);
    {
        int ok = ((int64_t)set_ret == 0 && new_prio == test_prio);
        str_cpy(line, "[14] SYS_SETPRIORITY set=");
        u64_to_dec(test_prio, tmp); str_concat(line, tmp);
        str_concat(line, " got=");
        u64_to_dec(new_prio, tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        output_add_line(output, line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [15] INVALID syscall → ENOSYS ─────────────────────────
    _SC0(9999);
    sc_result(output, 15, "INVALID(9999)", (int64_t)ret,
              (int64_t)ret == (int64_t)SYSCALL_ERR_NOSYS,
              "expect ENOSYS(-2)", &pass, &fail);

    // ================================================================
    // v3 YENİ TESTLER
    // ================================================================
    output_add_empty_line(output);
    output_add_line(output, "── v3 New Tests ─────────────────────", VGA_YELLOW);

    // ── [16] SYS_BRK(0) sorgula ───────────────────────────────
    _SC1(SYS_BRK, 0);
    {
        int ok = ((int64_t)ret > 0);
        HEX64S(ret);
        str_cpy(line, "[16] SYS_BRK(0) cur_brk="); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        output_add_line(output, line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [17] SYS_MMAP anonim 4096 bytes ───────────────────────
    // 6-argümanlı syscall: R10=flags, R8=fd, R9=offset
    uint64_t mmap_addr = 0;
    {
        register uint64_t r10v asm("r10") = (uint64_t)(MAP_ANONYMOUS | MAP_PRIVATE);
        register uint64_t r8v  asm("r8")  = (uint64_t)(int64_t)(-1);
        register uint64_t r9v  asm("r9")  = 0ULL;
        __asm__ volatile ("syscall"
            : "=a"(ret)
            : "a"((uint64_t)SYS_MMAP),
              "D"((uint64_t)0),
              "S"((uint64_t)4096),
              "d"((uint64_t)(PROT_READ|PROT_WRITE)),
              "r"(r10v), "r"(r8v), "r"(r9v)
            : "rcx", "r11", "memory");
        mmap_addr = ret;
    }
    {
        int ok = (ret != (uint64_t)MAP_FAILED && ret != 0);
        HEX64S(ret);
        str_cpy(line, "[17] SYS_MMAP(anon,4096) addr="); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        output_add_line(output, line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;

        // Belleğe yaz/oku kısa doğrulama
        if (ok) {
            volatile char* mp = (volatile char*)mmap_addr;
            mp[0] = 0x42; mp[1] = 0x43;
            int rw_ok = (mp[0] == 0x42 && mp[1] == 0x43);
            output_add_line(output,
                rw_ok ? "       mmap R/W verify OK" : "       mmap R/W verify FAIL",
                rw_ok ? VGA_GREEN : VGA_RED);
        }
    }

    // ── [18] SYS_MUNMAP ───────────────────────────────────────
    if (mmap_addr && mmap_addr != (uint64_t)MAP_FAILED) {
        _SC2(SYS_MUNMAP, mmap_addr, 4096);
        sc_result(output, 18, "SYS_MUNMAP", (int64_t)ret,
                  (int64_t)ret == 0, "expect 0", &pass, &fail);
    } else {
        output_add_line(output, "[18] SYS_MUNMAP  SKIP (mmap failed)", VGA_YELLOW);
    }

    // ── [19] SYS_EXECVE stub → ENOSYS ─────────────────────────
    static const char exec_path[] = "/bin/sh";
    _SC3(SYS_EXECVE, exec_path, 0, 0);
    sc_result(output, 19, "SYS_EXECVE(stub)", (int64_t)ret,
              (int64_t)ret == (int64_t)SYSCALL_ERR_NOSYS,
              "expect ENOSYS", &pass, &fail);

    // ── [20] SYS_PIPE ─────────────────────────────────────────
    int pipe_fds[2] = {-1, -1};
    _SC1(SYS_PIPE, pipe_fds);
    {
        int ok = ((int64_t)ret == 0 && pipe_fds[0] >= 3 && pipe_fds[1] >= 3);
        str_cpy(line, "[20] SYS_PIPE rfd=");
        int_to_str(pipe_fds[0], tmp); str_concat(line, tmp);
        str_concat(line, " wfd=");
        int_to_str(pipe_fds[1], tmp); str_concat(line, tmp);
        str_concat(line, ok ? "  PASS" : "  FAIL");
        output_add_line(output, line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;
    }

    // ── [21] PIPE write + read yuvarlak trip ──────────────────
    if (pipe_fds[0] >= 3 && pipe_fds[1] >= 3) {
        static const char pmsg[] = "PIPE_DATA_OK";
        uint64_t pmsg_len = 12;
        _SC3(SYS_WRITE, pipe_fds[1], pmsg, pmsg_len);
        uint64_t write_ret = ret;

        char pbuf[32];
        _SC3(SYS_READ, pipe_fds[0], pbuf, pmsg_len);
        uint64_t read_ret = ret;

        // Kaba karşılaştırma (str_cmp bağımlılığı yok; ilk 4 byte)
        int data_ok = (read_ret == pmsg_len &&
                       pbuf[0]=='P' && pbuf[1]=='I' && pbuf[2]=='P' && pbuf[3]=='E');
        str_cpy(line, "[21] PIPE write=");
        int_to_str((int)write_ret, tmp); str_concat(line, tmp);
        str_concat(line, " read=");
        int_to_str((int)read_ret, tmp); str_concat(line, tmp);
        str_concat(line, data_ok ? "  PASS" : "  FAIL");
        output_add_line(output, line, data_ok ? VGA_GREEN : VGA_RED);
        data_ok ? pass++ : fail++;
    } else {
        output_add_line(output, "[21] PIPE R/W  SKIP", VGA_YELLOW);
    }

    // ── [22] SYS_DUP2 ─────────────────────────────────────────
    // wfd'yi fd 8'e kopyala, fd 8'e yaz, kapat
    int dup_ok_pre = (pipe_fds[1] >= 3);
    if (dup_ok_pre) {
        _SC2(SYS_DUP2, pipe_fds[1], 8);
        int dup_pass = ((int64_t)ret == 8);
        str_cpy(line, "[22] SYS_DUP2(wfd->8) ret=");
        int_to_str((int)ret, tmp); str_concat(line, tmp);
        str_concat(line, dup_pass ? "  PASS" : "  FAIL");
        output_add_line(output, line, dup_pass ? VGA_GREEN : VGA_RED);
        dup_pass ? pass++ : fail++;

        // Temizle
        _SC1(SYS_CLOSE, pipe_fds[0]);
        _SC1(SYS_CLOSE, pipe_fds[1]);
        _SC1(SYS_CLOSE, 8);
    } else {
        output_add_line(output, "[22] SYS_DUP2  SKIP", VGA_YELLOW);
    }

    // ── [23] SYS_FORK smoke test ────────────────────────────────
    // Kernel context'te fork çağrısı: çökmemelidir.
    // Ebeveyn: ret > 0 (child_pid). Çocuk: ret = 0.
    _SC0(SYS_FORK);
    {
        int64_t fork_ret = (int64_t)ret;
        // ret >= 0 ise çökmedi (başarı); negatif = hata = fail
        // NOT: fork başarılı olursa iki bağlamda da devam eder;
        //      çocuk konteksti hemen scheduler'a geçer, burası ebeveyn.
        int ok = (fork_ret >= 0);
        str_cpy(line, "[23] SYS_FORK ret=");
        int_to_str((int)fork_ret, tmp); str_concat(line, tmp);
        str_concat(line, fork_ret > 0 ? " (parent,child_pid)" :
                         fork_ret == 0 ? " (child ctx)" : " (err)");
        str_concat(line, ok ? "  PASS" : "  FAIL");
        output_add_line(output, line, ok ? VGA_GREEN : VGA_RED);
        ok ? pass++ : fail++;

        // Ebeveyndeysek, çocuğun pid'ini bilgi olarak göster
        if (fork_ret > 0) {
            str_cpy(line, "       child_pid=");
            int_to_str((int)fork_ret, tmp); str_concat(line, tmp);
            output_add_line(output, line, VGA_WHITE);
        }
    }

    // ── Özet ───────────────────────────────────────────────────
    output_add_empty_line(output);
    str_cpy(line, "Result: ");
    {char b[8]; int_to_str(pass, b); str_concat(line, b);}
    str_concat(line, "/23 passed  (");
    {char b[8]; int_to_str(fail, b); str_concat(line, b);}
    str_concat(line, " failed)");
    output_add_line(output, line, fail == 0 ? VGA_GREEN : VGA_YELLOW);
    if (fail == 0)
        output_add_line(output, "All v3 syscall tests passed!", VGA_GREEN);
    else
        output_add_line(output, "Failed tests: check serial log.", VGA_RED);

    #undef HEX64S
}

#undef _SC0
#undef _SC1
#undef _SC2
#undef _SC3

// ===========================================
// COMMAND TABLE
// ===========================================

static Command command_table[] = {
    {"hello", "Say hello", cmd_hello},
    {"jew", "JEW JEW JEW", cmd_jew},
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

    // SYSCALL/SYSRET commands
    {"syscallinfo", "Show SYSCALL MSR configuration",    cmd_syscallinfo},
    {"syscalltest", "Run SYSCALL test suite (23 tests)", cmd_syscalltest},
};
static int command_count = sizeof(command_table) / sizeof(Command);

// ===========================================
// COMMAND SYSTEM
// ===========================================

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
    
    // Command not found
    return 0;  // veya hata mesajı döndürebilirsiniz
}

const Command* get_all_commands64(int* count) {
    *count = command_count;
    return command_table;
}