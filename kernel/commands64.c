#include <stddef.h>
#include "commands64.h"
#include "files64.h"
#include "nano64.h"
#include "script64.h"  // ADD THIS LINE
#include "accounts64.h"
#include "wallpaper64.h"

extern void println64(const char* str, uint8_t color);
extern void print_str64(const char* str, uint8_t color);

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
    output_add_line(output, " htop      - Show system monitor", VGA_WHITE);
    output_add_empty_line(output);
    output_add_line(output, "File System Commands:", VGA_YELLOW);
    output_add_line(output, " ls        - List files and directories", VGA_WHITE);
    output_add_line(output, " cd        - Change directory", VGA_WHITE);
    output_add_line(output, " pwd       - Print working directory", VGA_WHITE);
    output_add_line(output, " mkdir     - Create directory", VGA_WHITE);
    output_add_line(output, " rmdir     - Remove directory", VGA_WHITE);
    output_add_line(output, " cat       - Show file content", VGA_WHITE);
    output_add_line(output, " touch     - Create new file", VGA_WHITE);
    output_add_line(output, " write     - Write to file", VGA_WHITE);
    output_add_line(output, " rm        - Delete file", VGA_WHITE);
    output_add_line(output, " kode      - Text editor", VGA_WHITE);
    output_add_empty_line(output);
    output_add_line(output, "System Commands:", VGA_YELLOW);
    output_add_line(output, " sysinfo   - System information", VGA_WHITE);
    output_add_line(output, " cpuinfo   - CPU information", VGA_WHITE);
    output_add_line(output, " meminfo   - Memory information", VGA_WHITE);
    output_add_line(output, " test      - Run 64-bit tests", VGA_WHITE);
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

    // Sistem bilgileri - art'ın boyutuna uygun şekilde yerleştirildi
    str_cpy(info_lines[0],  "AscentOS v0.1 64-bit");
    str_cpy(info_lines[1],  "---------------------");
    str_cpy(info_lines[3],  "OS: AscentOS x86_64 - Why So Serious?");
    str_cpy(info_lines[4],  "Kernel: Handcrafted chaos edition");
    str_cpy(info_lines[5],  "Uptime: Since you booted me, fool");
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

void cmd_htop(const char* args, CommandOutput* output) {
    (void)args;
    
    uint32_t cpu_usage = get_cpu_usage_64();
    
    char cpu_brand[49];
    get_cpu_brand(cpu_brand);
    
    output_add_line(output, "========================================", VGA_CYAN);
    output_add_line(output, "  ASCENTOS 64-BIT SYSTEM MONITOR v0.1", VGA_GREEN);
    output_add_line(output, "========================================", VGA_CYAN);
    output_add_empty_line(output);
    
    output_add_line(output, "CPU Usage:", VGA_YELLOW);
    
    char cpu_bar[64];
    str_cpy(cpu_bar, "  [");
    int bar_length = 20;
    int filled = (cpu_usage * bar_length) / 100;
    
    for (int i = 0; i < bar_length; i++) {
        if (i < filled) {
            str_concat(cpu_bar, "#");
        } else {
            str_concat(cpu_bar, ".");
        }
    }
    str_concat(cpu_bar, "] ");
    
    char percent_str[8];
    int_to_str(cpu_usage, percent_str);
    str_concat(cpu_bar, percent_str);
    str_concat(cpu_bar, "%");
    
    output_add_line(output, cpu_bar, VGA_GREEN);
    
    char cpu_info_line[MAX_LINE_LENGTH];
    str_cpy(cpu_info_line, "  CPU: ");
    
    int len = 0;
    while (cpu_brand[len] && len < 40) len++;
    
    for (int i = 0; i < len && i < 40; i++) {
        cpu_info_line[7 + i] = cpu_brand[i];
    }
    cpu_info_line[7 + len] = '\0';
    
    output_add_line(output, cpu_info_line, VGA_WHITE);
    output_add_line(output, "  Cores: 1 (64-bit single core madness)", VGA_WHITE);
    output_add_line(output, "  Architecture: x86_64 (Long Mode)", VGA_WHITE);
    output_add_empty_line(output);
    
    uint64_t total_memory_kb = get_memory_info();
    uint64_t used_kb = total_memory_kb * 65 / 100;
    uint64_t free_kb = total_memory_kb - used_kb;
    
    char mem_line[MAX_LINE_LENGTH];
    
    output_add_line(output, "Memory Usage:", VGA_YELLOW);
    
    str_cpy(mem_line, "  Total: ");
    char temp[32];
    format_memory_size(total_memory_kb, temp);
    str_concat(mem_line, temp);
    output_add_line(output, mem_line, VGA_WHITE);
    
    str_cpy(mem_line, "  Used:  ");
    format_memory_size(used_kb, temp);
    str_concat(mem_line, temp);
    str_concat(mem_line, " (65%)");
    output_add_line(output, mem_line, VGA_WHITE);
    
    str_cpy(mem_line, "  Free:  ");
    format_memory_size(free_kb, temp);
    str_concat(mem_line, temp);
    str_concat(mem_line, " (35%)");
    output_add_line(output, mem_line, VGA_WHITE);
    
    output_add_line(output, "  [#############.......] 65%", VGA_GREEN);
    output_add_empty_line(output);
    
    output_add_line(output, "Running Processes (64-bit):", VGA_YELLOW);
    output_add_line(output, "  PID    NAME              CPU%   MEM", VGA_CYAN);
    output_add_line(output, "  ----   ----              ----   ---", VGA_DARK_GRAY);
    
    char proc_line[MAX_LINE_LENGTH];
    str_cpy(proc_line, "  1      kernel64           ");
    int_to_str((cpu_usage * 45) / 100, temp);
    str_concat(proc_line, temp);
    str_concat(proc_line, "%    512K");
    output_add_line(output, proc_line, VGA_WHITE);
    
    str_cpy(proc_line, "  2      shell64            ");
    int_to_str((cpu_usage * 25) / 100, temp);
    str_concat(proc_line, temp);
    str_concat(proc_line, "%    256K");
    output_add_line(output, proc_line, VGA_WHITE);
    
    str_cpy(proc_line, "  3      vga_driver64       ");
    int_to_str((cpu_usage * 15) / 100, temp);
    str_concat(proc_line, temp);
    str_concat(proc_line, "%    128K");
    output_add_line(output, proc_line, VGA_WHITE);
    
    str_cpy(proc_line, "  4      keyboard_drv64     ");
    int_to_str((cpu_usage * 10) / 100, temp);
    str_concat(proc_line, temp);
    str_concat(proc_line, "%    64K");
    output_add_line(output, proc_line, VGA_WHITE);
    
    str_cpy(proc_line, "  5      fs_manager64       ");
    int_to_str((cpu_usage * 5) / 100, temp);
    str_concat(proc_line, temp);
    str_concat(proc_line, "%    32K");
    output_add_line(output, proc_line, VGA_WHITE);
    
    output_add_empty_line(output);
    
    output_add_line(output, "System Stats:", VGA_YELLOW);
    output_add_line(output, "  Uptime: Forever (or until you reboot)", VGA_WHITE);
    
    char load_line[MAX_LINE_LENGTH];
    str_cpy(load_line, "  Load Average: ");
    char load_str[16];
    int_to_str(cpu_usage / 100, load_str);
    str_concat(load_line, load_str);
    str_concat(load_line, ".");
    int_to_str((cpu_usage % 100) / 10, load_str);
    str_concat(load_line, load_str);
    str_concat(load_line, ", ");
    int_to_str(cpu_usage / 100, load_str);
    str_concat(load_line, load_str);
    str_concat(load_line, ".");
    int_to_str((cpu_usage % 100) / 10, load_str);
    str_concat(load_line, load_str);
    output_add_line(output, load_line, VGA_WHITE);
    
    output_add_line(output, "  Tasks: 5 total, 5 running", VGA_WHITE);
    output_add_line(output, "  Interrupts: Too many to count", VGA_WHITE);
    output_add_line(output, "  Mode: Long Mode (64-bit) Active", VGA_GREEN);
    
    int file_count = 0;
    get_all_files_list64(&file_count);
    str_cpy(proc_line, "  Files: ");
    int_to_str(file_count, temp);
    str_concat(proc_line, temp);
    str_concat(proc_line, " files tracked");
    output_add_line(output, proc_line, VGA_WHITE);
    
    output_add_empty_line(output);
    
    output_add_line(output, "Note: CPU usage calculated via RDTSC!", VGA_DARK_GRAY);
    output_add_line(output, "Run 'htop' again to see updated values.", VGA_GREEN
        // PART 2 of commands64.c - Add after htop function

);
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

void cmd_test(void) {
    println64("Running 64-bit tests...", VGA_CYAN);
    println64("", VGA_WHITE);
    
    // Pointer test
    void* ptr = (void*)0x123456789ABCDEF0ULL;
    print_str64("64-bit pointer: 0x", VGA_WHITE);
    char hex[20];
    uint64_t val = (uint64_t)ptr;
    for (int i = 0; i < 16; i++) {
        hex[i] = "0123456789ABCDEF"[(val >> (60 - i * 4)) & 0xF];
    }
    hex[16] = '\0';
    println64(hex, VGA_GREEN);
    
    // Sizeof test
    print_str64("sizeof(void*) = ", VGA_WHITE);
    if (sizeof(void*) == 8) {
        println64("8 bytes ✓", VGA_GREEN);
    } else {
        println64("ERROR!", VGA_RED);
    }
    
    print_str64("sizeof(long) = ", VGA_WHITE);
    char size_str[4];
    uint64_to_string(sizeof(long), size_str);
    print_str64(size_str, VGA_GREEN);
    println64(" bytes", VGA_WHITE);
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
// WALLPAPER COMMAND - Wallpaper management
// ===========================================

void cmd_wallpaper(const char* args, CommandOutput* output) {
#ifdef GUI_MODE
    // GUI mode: Full wallpaper support
    if (str_len(args) == 0) {
        output_add_line(output, "Wallpaper System Commands:", VGA_CYAN);
        output_add_line(output, "", VGA_WHITE);
        output_add_line(output, " wallpaper load <file> - Load BMP image as wallpaper", VGA_WHITE);
        output_add_line(output, " wallpaper mode <mode> - Set display mode", VGA_WHITE);
        output_add_line(output, " wallpaper info - Show current wallpaper info", VGA_WHITE);
        output_add_line(output, " wallpaper clear - Remove wallpaper", VGA_WHITE);
        output_add_line(output, "", VGA_WHITE);
        output_add_line(output, "Built-in Wallpapers:", VGA_YELLOW);
        output_add_line(output, " wallpaper blue - Blue gradient", VGA_WHITE);
        output_add_line(output, " wallpaper purple - Purple gradient", VGA_WHITE);
        output_add_line(output, " wallpaper green - Green gradient", VGA_WHITE);
        output_add_line(output, "", VGA_WHITE);
        output_add_line(output, "Display Modes:", VGA_YELLOW);
        output_add_line(output, " stretch - Stretch to fill screen", VGA_WHITE);
        output_add_line(output, " center - Center on screen", VGA_WHITE);
        output_add_line(output, " tile - Tile across screen", VGA_WHITE);
        output_add_line(output, " fit - Fit maintaining aspect ratio", VGA_WHITE);
        output_add_line(output, "", VGA_WHITE);
        output_add_line(output, "Example: wallpaper load myimage.bmp", VGA_GREEN);
        output_add_line(output, "Note: Only 24-bit and 32-bit BMP files supported", VGA_DARK_GRAY);
        return;
    }
   
    // Parse subcommand
    char subcmd[32];
    int i = 0;
    while (args[i] && args[i] != ' ' && i < 31) {
        subcmd[i] = args[i];
        i++;
    }
    subcmd[i] = '\0';
   
    // Skip spaces
    while (args[i] == ' ') i++;
    const char* subcmd_args = &args[i];
   
    // Load wallpaper from file
    if (str_cmp(subcmd, "load") == 0) {
        if (str_len(subcmd_args) == 0) {
            output_add_line(output, "Usage: wallpaper load <filename.bmp>", VGA_RED);
            return;
        }
       
        output_add_line(output, "Loading wallpaper...", VGA_YELLOW);
       
        if (wallpaper_load_bmp(subcmd_args)) {
            char msg[MAX_LINE_LENGTH];
            str_cpy(msg, "Wallpaper loaded successfully: ");
            str_concat(msg, subcmd_args);
            output_add_line(output, msg, VGA_GREEN);
            output_add_line(output, "Redrawing desktop...", VGA_CYAN);
           
            // Request full redraw
            extern bool needs_full_redraw;
            needs_full_redraw = true;
        } else {
            output_add_line(output, "Failed to load wallpaper!", VGA_RED);
            output_add_line(output, "Check that:", VGA_YELLOW);
            output_add_line(output, " - File exists (use 'ls' to check)", VGA_WHITE);
            output_add_line(output, " - File is a valid 24/32-bit BMP", VGA_WHITE);
            output_add_line(output, " - Image size is <= 800x600", VGA_WHITE);
        }
        return;
    }
   
    // Set wallpaper mode
    if (str_cmp(subcmd, "mode") == 0) {
        if (str_len(subcmd_args) == 0) {
            output_add_line(output, "Usage: wallpaper mode <stretch|center|tile|fit>", VGA_RED);
            return;
        }
       
        WallpaperMode mode;
        if (str_cmp(subcmd_args, "stretch") == 0) {
            mode = WALLPAPER_MODE_STRETCH;
        } else if (str_cmp(subcmd_args, "center") == 0) {
            mode = WALLPAPER_MODE_CENTER;
        } else if (str_cmp(subcmd_args, "tile") == 0) {
            mode = WALLPAPER_MODE_TILE;
        } else if (str_cmp(subcmd_args, "fit") == 0) {
            mode = WALLPAPER_MODE_FIT;
        } else {
            output_add_line(output, "Invalid mode. Use: stretch, center, tile, or fit", VGA_RED);
            return;
        }
       
        wallpaper_set_mode(mode);
       
        char msg[MAX_LINE_LENGTH];
        str_cpy(msg, "Wallpaper mode set to: ");
        str_concat(msg, subcmd_args);
        output_add_line(output, msg, VGA_GREEN);
       
        // Request redraw
        extern bool needs_full_redraw;
        needs_full_redraw = true;
        return;
    }
   
    // Show wallpaper info
    if (str_cmp(subcmd, "info") == 0) {
        char info[256];
        wallpaper_get_info(info, 256);
       
        output_add_line(output, "Current Wallpaper:", VGA_CYAN);
        output_add_line(output, info, VGA_YELLOW);
        return;
    }
   
    // Clear wallpaper
    if (str_cmp(subcmd, "clear") == 0) {
        wallpaper_unload();
        output_add_line(output, "Wallpaper cleared", VGA_GREEN);
       
        extern bool needs_full_redraw;
        needs_full_redraw = true;
        return;
    }
   
    // Built-in gradient wallpapers
    if (str_cmp(subcmd, "blue") == 0) {
        wallpaper_set_gradient_blue();
        output_add_line(output, "Blue gradient wallpaper applied", VGA_GREEN);
       
        extern bool needs_full_redraw;
        needs_full_redraw = true;
        return;
    }
   
    if (str_cmp(subcmd, "purple") == 0) {
        wallpaper_set_gradient_purple();
        output_add_line(output, "Purple gradient wallpaper applied", VGA_GREEN);
       
        extern bool needs_full_redraw;
        needs_full_redraw = true;
        return;
    }
   
    if (str_cmp(subcmd, "green") == 0) {
        wallpaper_set_gradient_green();
        output_add_line(output, "Green gradient wallpaper applied", VGA_GREEN);
       
        extern bool needs_full_redraw;
        needs_full_redraw = true;
        return;
    }
   
    output_add_line(output, "Unknown wallpaper command. Type 'wallpaper' for help.", VGA_RED);
#else
    // Text mode: Wallpaper not supported
    output_add_line(output, "Wallpaper command is only available in GUI mode.", VGA_RED);
    output_add_line(output, "To use wallpapers, run: make run-gui", VGA_YELLOW);
#endif
}
// Login command
void cmd_login(const char* args, CommandOutput* output) {
    if (!accounts_is_logged_in()) {
        if (str_len(args) == 0) {
            output_add_line(output, "Usage: login <username> <password>", VGA_RED);
            output_add_line(output, "Example: login root root", VGA_CYAN);
            output_add_empty_line(output);
            output_add_line(output, "Default accounts:", VGA_YELLOW);
            output_add_line(output, "  root/root   - Administrator", VGA_WHITE);
            output_add_line(output, "  guest/guest - Guest user", VGA_WHITE);
            return;
        }
        
        // Parse username and password
        char username[MAX_USERNAME_LEN];
        int i = 0;
        while (args[i] && args[i] != ' ' && i < MAX_USERNAME_LEN - 1) {
            username[i] = args[i];
            i++;
        }
        username[i] = '\0';
        
        while (args[i] == ' ') i++;
        const char* password = &args[i];
        
        if (str_len(username) == 0 || str_len(password) == 0) {
            output_add_line(output, "Error: Username and password required", VGA_RED);
            return;
        }
        
        if (accounts_login(username, password)) {
            char msg[MAX_LINE_LENGTH];
            str_cpy(msg, "Welcome back, ");
            str_concat(msg, username);
            str_concat(msg, "!");
            output_add_line(output, msg, VGA_GREEN);
            
            UserLevel level = accounts_get_current_level();
            str_cpy(msg, "Access level: ");
            str_concat(msg, accounts_level_to_string(level));
            output_add_line(output, msg, VGA_CYAN);
        } else {
            output_add_line(output, "Login failed: Invalid username or password", VGA_RED);
        }
    } else {
        char msg[MAX_LINE_LENGTH];
        str_cpy(msg, "Already logged in as: ");
        str_concat(msg, accounts_get_current_username());
        output_add_line(output, msg, VGA_YELLOW);
        output_add_line(output, "Use 'logout' first", VGA_CYAN);
    }
}

// Logout command
void cmd_logout(const char* args, CommandOutput* output) {
    (void)args;
    
    if (accounts_is_logged_in()) {
        char msg[MAX_LINE_LENGTH];
        str_cpy(msg, "Goodbye, ");
        str_concat(msg, accounts_get_current_username());
        str_concat(msg, "!");
        output_add_line(output, msg, VGA_GREEN);
        
        accounts_logout();
        output_add_line(output, "Logged out successfully", VGA_CYAN);
    } else {
        output_add_line(output, "Not logged in", VGA_YELLOW);
    }
}

// Whoami command
void cmd_whoami(const char* args, CommandOutput* output) {
    (void)args;
    
    if (accounts_is_logged_in()) {
        const char* username = accounts_get_current_username();
        UserLevel level = accounts_get_current_level();
        
        output_add_line(output, "Current User Information:", VGA_CYAN);
        output_add_empty_line(output);
        
        char msg[MAX_LINE_LENGTH];
        str_cpy(msg, "  Username: ");
        str_concat(msg, username);
        output_add_line(output, msg, VGA_WHITE);
        
        str_cpy(msg, "  Level: ");
        str_concat(msg, accounts_level_to_string(level));
        output_add_line(output, msg, VGA_WHITE);
        
        str_cpy(msg, "  Status: ");
        str_concat(msg, "Logged in");
        output_add_line(output, msg, VGA_GREEN);
    } else {
        output_add_line(output, "Not logged in (Guest mode)", VGA_YELLOW);
        output_add_line(output, "Use 'login' to access full features", VGA_CYAN);
    }
}

// List users command
void cmd_users(const char* args, CommandOutput* output) {
    (void)args;
    
    output_add_line(output, "User Accounts:", VGA_CYAN);
    output_add_line(output, "========================================", VGA_CYAN);
    output_add_line(output, "  Username           Level       Logins", VGA_YELLOW);
    output_add_line(output, "  --------           -----       ------", VGA_DARK_GRAY);
    
    char lines[MAX_USERS][128];
    int count = accounts_list_users(lines, MAX_USERS);
    
    for (int i = 0; i < count; i++) {
        output_add_line(output, lines[i], VGA_WHITE);
    }
    
    output_add_empty_line(output);
    
    char msg[MAX_LINE_LENGTH];
    str_cpy(msg, "Total users: ");
    char count_str[16];
    int_to_str(count, count_str);
    str_concat(msg, count_str);
    output_add_line(output, msg, VGA_GREEN);
}

// Create user command
void cmd_adduser(const char* args, CommandOutput* output) {
    // Check permission
    if (!accounts_has_permission(USER_LEVEL_ADMIN)) {
        output_add_line(output, "Permission denied: Admin access required", VGA_RED);
        output_add_line(output, "You need to login as admin or root", VGA_YELLOW);
        return;
    }
    
    if (str_len(args) == 0) {
        output_add_line(output, "Usage: adduser <username> <password> [level]", VGA_RED);
        output_add_line(output, "Levels: guest, user, admin", VGA_CYAN);
        output_add_line(output, "Example: adduser alice secret123 user", VGA_CYAN);
        return;
    }
    
    // Parse arguments
    char username[MAX_USERNAME_LEN];
    char password[MAX_PASSWORD_LEN];
    char level_str[16];
    
    int i = 0;
    while (args[i] && args[i] != ' ' && i < MAX_USERNAME_LEN - 1) {
        username[i] = args[i];
        i++;
    }
    username[i] = '\0';
    
    while (args[i] == ' ') i++;
    
    int j = 0;
    while (args[i] && args[i] != ' ' && j < MAX_PASSWORD_LEN - 1) {
        password[j++] = args[i++];
    }
    password[j] = '\0';
    
    while (args[i] == ' ') i++;
    
    j = 0;
    while (args[i] && j < 15) {
        level_str[j++] = args[i++];
    }
    level_str[j] = '\0';
    
    // Default to user level
    UserLevel level = USER_LEVEL_USER;
    if (str_len(level_str) > 0) {
        if (str_cmp(level_str, "guest") == 0) level = USER_LEVEL_GUEST;
        else if (str_cmp(level_str, "user") == 0) level = USER_LEVEL_USER;
        else if (str_cmp(level_str, "admin") == 0) level = USER_LEVEL_ADMIN;
        else {
            output_add_line(output, "Invalid level. Using 'user'", VGA_YELLOW);
        }
    }
    
    if (accounts_create_user(username, password, level)) {
        char msg[MAX_LINE_LENGTH];
        str_cpy(msg, "User created: ");
        str_concat(msg, username);
        str_concat(msg, " (");
        str_concat(msg, accounts_level_to_string(level));
        str_concat(msg, ")");
        output_add_line(output, msg, VGA_GREEN);
    } else {
        output_add_line(output, "Failed to create user", VGA_RED);
        output_add_line(output, "Username may already exist or user limit reached", VGA_YELLOW);
    }
}

// Delete user command
void cmd_deluser(const char* args, CommandOutput* output) {
    if (!accounts_has_permission(USER_LEVEL_ADMIN)) {
        output_add_line(output, "Permission denied: Admin access required", VGA_RED);
        return;
    }
    
    if (str_len(args) == 0) {
        output_add_line(output, "Usage: deluser <username>", VGA_RED);
        output_add_line(output, "Example: deluser alice", VGA_CYAN);
        return;
    }
    
    if (accounts_delete_user(args)) {
        char msg[MAX_LINE_LENGTH];
        str_cpy(msg, "User deleted: ");
        str_concat(msg, args);
        output_add_line(output, msg, VGA_GREEN);
    } else {
        output_add_line(output, "Failed to delete user", VGA_RED);
        output_add_line(output, "Cannot delete yourself, root, or non-existent users", VGA_YELLOW);
    }
}

// Change password command
void cmd_passwd(const char* args, CommandOutput* output) {
    if (!accounts_is_logged_in()) {
        output_add_line(output, "You must be logged in to change password", VGA_RED);
        return;
    }
    
    if (str_len(args) == 0) {
        output_add_line(output, "Usage: passwd <old_password> <new_password>", VGA_RED);
        output_add_line(output, "Example: passwd oldpass newpass", VGA_CYAN);
        return;
    }
    
    // Parse old and new password
    char old_pass[MAX_PASSWORD_LEN];
    char new_pass[MAX_PASSWORD_LEN];
    
    int i = 0;
    while (args[i] && args[i] != ' ' && i < MAX_PASSWORD_LEN - 1) {
        old_pass[i] = args[i];
        i++;
    }
    old_pass[i] = '\0';
    
    while (args[i] == ' ') i++;
    
    int j = 0;
    while (args[i] && j < MAX_PASSWORD_LEN - 1) {
        new_pass[j++] = args[i++];
    }
    new_pass[j] = '\0';
    
    if (str_len(new_pass) == 0) {
        output_add_line(output, "Error: New password required", VGA_RED);
        return;
    }
    
    if (accounts_change_password(old_pass, new_pass)) {
        output_add_line(output, "Password changed successfully!", VGA_GREEN);
        output_add_line(output, "Please remember your new password", VGA_CYAN);
    } else {
        output_add_line(output, "Failed to change password", VGA_RED);
        output_add_line(output, "Old password is incorrect", VGA_YELLOW);
    }
}

// SU (Switch User) command
void cmd_su(const char* args, CommandOutput* output) {
    if (str_len(args) == 0) {
        output_add_line(output, "Usage: su <username> <password>", VGA_RED);
        output_add_line(output, "Switch to another user account", VGA_CYAN);
        return;
    }
    
    // First logout current user
    if (accounts_is_logged_in()) {
        accounts_logout();
    }
    
    // Then try to login as new user
    cmd_login(args, output);
}
// ===========================================
// CMATRIX - Matrix Digital Rain Animation
// ===========================================

// Simple random number generator
static uint32_t matrix_seed = 12345;

uint32_t matrix_rand(void) {
    matrix_seed = matrix_seed * 1103515245 + 12345;
    return (matrix_seed / 65536) % 32768;
}
void cmd_cmatrix(const char* args, CommandOutput* output) {
    (void)args;
    
    // Get external VGA functions
    extern void clear_screen64(void);
    extern void putchar64(char c, uint8_t color);
    extern void set_position64(size_t row, size_t col);
    extern void get_screen_size64(size_t* width, size_t* height);
    
    size_t width, height;
    get_screen_size64(&width, &height);
    
    clear_screen64();
    
    // Matrix karakterler
    const char matrix_chars[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz!@#$%^&*()_+-=[]{}|;:,.<>?/~`";
    int char_count = sizeof(matrix_chars) - 1;
    
    // Her sütun için bir düşen karakter dizisi
    #define MAX_COLS 132
    typedef struct {
        int y;                  // Mevcut Y pozisyonu
        int length;            // Düşen karakterlerin uzunluğu
        int speed;             // Düşme hızı (frame sayısı)
        int speed_counter;     // Hız sayacı
        char chars[80];        // O sütundaki karakterler
    } MatrixColumn;
    
    static MatrixColumn columns[MAX_COLS];
    
    // Sütunları başlat
    for (int i = 0; i < (int)width && i < MAX_COLS; i++) {
        columns[i].y = -(matrix_rand() % (int)height);
        columns[i].length = 5 + (matrix_rand() % 15);
        columns[i].speed = 1 + (matrix_rand() % 3);
        columns[i].speed_counter = 0;
        
        for (int j = 0; j < 80; j++) {
            columns[i].chars[j] = matrix_chars[matrix_rand() % char_count];
        }
    }
    
    // Header
    set_position64(0, 0);
    const char* header = "CMATRIX - Press any key to exit...";
    for (int i = 0; header[i]; i++) {
        putchar64(header[i], VGA_WHITE);
    }
    
    // Animasyon döngüsü - 1500 frame (yaklaşık 25 saniye)
    for (int frame = 0; frame < 1500; frame++) {
        // Her sütunu güncelle
        for (int col = 0; col < (int)width && col < MAX_COLS; col++) {
            MatrixColumn* c = &columns[col];
            
            c->speed_counter++;
            if (c->speed_counter >= c->speed) {
                c->speed_counter = 0;
                c->y++;
                
                // Sütun ekranın altına çıktıysa yeniden başlat
                if (c->y - c->length > (int)height) {
                    c->y = -(matrix_rand() % 10);
                    c->length = 5 + (matrix_rand() % 15);
                    c->speed = 1 + (matrix_rand() % 3);
                    
                    // Yeni karakterler
                    for (int j = 0; j < 80; j++) {
                        c->chars[j] = matrix_chars[matrix_rand() % char_count];
                    }
                }
            }
            
            // Bu sütundaki karakterleri çiz
            for (int i = 0; i < c->length; i++) {
                int y = c->y - i;
                
                if (y >= 1 && y < (int)height) {  // Header'ı korumak için y >= 1
                    set_position64((size_t)y, (size_t)col);
                    
                    uint8_t color;
                    if (i == 0) {
                        // En üstteki karakter beyaz
                        color = VGA_WHITE;
                    } else if (i < 3) {
                        // Üstte açık yeşil
                        color = VGA_GREEN;
                    } else if (i < c->length / 2) {
                        // Ortada normal yeşil
                        color = VGA_GREEN;
                    } else {
                        // Altta koyu yeşil
                        color = 0x02;  // Dark green
                    }
                    
                    // Karakteri yazdır
                    int char_idx = (c->y - i) % 80;
                    if (char_idx < 0) char_idx += 80;
                    putchar64(c->chars[char_idx], color);
                }
                // Arkasındaki karakteri sil
                int clear_y = c->y - c->length;
                if (clear_y >= 1 && clear_y < (int)height) {
                    set_position64((size_t)clear_y, (size_t)col);
                    putchar64(' ', VGA_WHITE);
                }
            }
        }
        
        // Frame delay - basit busy wait
        for (volatile int d = 0; d < 100000; d++);
    }
    
    // Ekranı temizle ve normal moda dön
    clear_screen64();
    
    // Output'a mesaj ekle (normal shell'e dönüş için)
    output_add_line(output, "Matrix digital rain completed!", VGA_GREEN);
    output_add_line(output, "Welcome back to reality...", VGA_CYAN);
}
void cmd_script(const char* args, CommandOutput* output) {
    if (str_len(args) == 0) {
        output_add_line(output, "Usage:", VGA_CYAN);
        output_add_line(output, "  script list              - List all scripts", VGA_WHITE);
        output_add_line(output, "  script new <n> <d>       - Create new script", VGA_WHITE);
        output_add_line(output, "  script run <n>           - Run a script", VGA_WHITE);
        output_add_line(output, "  script show <n>          - Show script content", VGA_WHITE);
        output_add_line(output, "  script edit <n>          - Edit script", VGA_WHITE);
        output_add_line(output, "  script delete <n>        - Delete script", VGA_WHITE);
        output_add_line(output, "  script save <n>          - Save to .sh file", VGA_WHITE);
        output_add_line(output, "  script load <f>          - Load from .sh file", VGA_WHITE);
        output_add_empty_line(output);
        output_add_line(output, "You can also run scripts directly by name", VGA_GREEN);
        return;
    }
    
    // Parse subcommand
    char subcmd[32];
    int i = 0;
    while (args[i] && args[i] != ' ' && i < 31) {
        subcmd[i] = args[i];
        i++;
    }
    subcmd[i] = '\0';
    
    // Skip spaces
    while (args[i] == ' ') i++;
    const char* subcmd_args = &args[i];
    
    // List scripts
    if (str_cmp(subcmd, "list") == 0) {
        script_list(output);
        return;
    }
    
    // Create new script
    if (str_cmp(subcmd, "new") == 0) {
        if (str_len(subcmd_args) == 0) {
            output_add_line(output, "Usage: script new <name> <description>", VGA_RED);
            output_add_line(output, "Example: script new hello 'My greeting script'", VGA_CYAN);
            return;
        }
        
        // Parse name
        char name[32];
        int j = 0;
        while (subcmd_args[j] && subcmd_args[j] != ' ' && j < 31) {
            name[j] = subcmd_args[j];
            j++;
        }
        name[j] = '\0';
        
        // Skip spaces
        while (subcmd_args[j] == ' ') j++;
        const char* description = &subcmd_args[j];
        
        char desc[64];
        if (str_len(description) == 0) {
            str_cpy(desc, "User script");
        } else {
            str_cpy(desc, description);
        }
        
        if (script_create(name, desc, SCRIPT_TYPE_SHELL)) {
            char msg[MAX_LINE_LENGTH];
            str_cpy(msg, "Script created: ");
            str_concat(msg, name);
            output_add_line(output, msg, VGA_GREEN);
            output_add_line(output, "Use 'script edit <name>' to add commands", VGA_CYAN);
        } else {
            output_add_line(output, "Error: Could not create script", VGA_RED);
        }
        return;
    }
    
    // Run script
    if (str_cmp(subcmd, "run") == 0) {
        if (str_len(subcmd_args) == 0) {
            output_add_line(output, "Usage: script run <name>", VGA_RED);
            return;
        }
        
        if (!script_execute(subcmd_args, "", output)) {
            char msg[MAX_LINE_LENGTH];
            str_cpy(msg, "Error: Script not found: ");
            str_concat(msg, subcmd_args);
            output_add_line(output, msg, VGA_RED);
        }
        return;
    }
    
    // Show script
    if (str_cmp(subcmd, "show") == 0) {
        if (str_len(subcmd_args) == 0) {
            output_add_line(output, "Usage: script show <name>", VGA_RED);
            return;
        }
        
        if (!script_show(subcmd_args, output)) {
            char msg[MAX_LINE_LENGTH];
            str_cpy(msg, "Error: Script not found: ");
            str_concat(msg, subcmd_args);
            output_add_line(output, msg, VGA_RED);
        }
        return;
    }
    
    // Edit script
    if (str_cmp(subcmd, "edit") == 0) {
        if (str_len(subcmd_args) == 0) {
            output_add_line(output, "Usage: script edit <name>", VGA_RED);
            return;
        }
        
        if (script_edit(subcmd_args)) {
            char filename[64];
            str_cpy(filename, subcmd_args);
            str_concat(filename, ".sh");
            
            output_add_line(output, "Opening in editor...", VGA_GREEN);
            cmd_kode(filename, output);
        } else {
            char msg[MAX_LINE_LENGTH];
            str_cpy(msg, "Error: Script not found: ");
            str_concat(msg, subcmd_args);
            output_add_line(output, msg, VGA_RED);
        }
        return;
    }
    
    // Delete script
    if (str_cmp(subcmd, "delete") == 0) {
        if (str_len(subcmd_args) == 0) {
            output_add_line(output, "Usage: script delete <name>", VGA_RED);
            return;
        }
        
        if (script_delete(subcmd_args)) {
            char msg[MAX_LINE_LENGTH];
            str_cpy(msg, "Script deleted: ");
            str_concat(msg, subcmd_args);
            output_add_line(output, msg, VGA_GREEN);
        } else {
            char msg[MAX_LINE_LENGTH];
            str_cpy(msg, "Error: Script not found: ");
            str_concat(msg, subcmd_args);
            output_add_line(output, msg, VGA_RED);
        }
        return;
    }
    
    // Save to file
    if (str_cmp(subcmd, "save") == 0) {
        if (str_len(subcmd_args) == 0) {
            output_add_line(output, "Usage: script save <name>", VGA_RED);
            return;
        }
        
        if (script_save_to_file(subcmd_args)) {
            char msg[MAX_LINE_LENGTH];
            str_cpy(msg, "Script saved to: ");
            str_concat(msg, subcmd_args);
            str_concat(msg, ".sh");
            output_add_line(output, msg, VGA_GREEN);
        } else {
            output_add_line(output, "Error: Could not save script", VGA_RED);
        }
        return;
    }
    
    // Load from file
    if (str_cmp(subcmd, "load") == 0) {
        if (str_len(subcmd_args) == 0) {
            output_add_line(output, "Usage: script load <filename.sh>", VGA_RED);
            return;
        }
        
        if (script_load_from_file(subcmd_args)) {
            char msg[MAX_LINE_LENGTH];
            str_cpy(msg, "Script loaded from: ");
            str_concat(msg, subcmd_args);
            output_add_line(output, msg, VGA_GREEN);
        } else {
            output_add_line(output, "Error: Could not load script", VGA_RED);
        }
        return;
    }
    
    output_add_line(output, "Unknown subcommand. Use 'script' for help.", VGA_RED);
}
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
    {"htop", "System monitor", cmd_htop},
    
    // File system commands
    {"ls", "List files and directories", cmd_ls},
    {"cd", "Change directory", cmd_cd},
    {"pwd", "Print working directory", cmd_pwd},
    {"mkdir", "Create directory", cmd_mkdir},
    {"rmdir", "Remove directory", cmd_rmdir},
    {"cat", "Show file content", cmd_cat},
    {"touch", "Create new file", cmd_touch},
    {"write", "Write to file", cmd_write},
    {"rm", "Delete file", cmd_rm},
    {"kode", "Text editor", cmd_kode},
    
    // Script system
    {"script", "Script management system", cmd_script},
    
    // Account system
    {"login", "Login to user account", cmd_login},
    {"logout", "Logout from account", cmd_logout},
    {"whoami", "Show current user", cmd_whoami},
    {"users", "List all users", cmd_users},
    {"adduser", "Create new user (admin)", cmd_adduser},
    {"deluser", "Delete user (admin)", cmd_deluser},
    {"passwd", "Change password", cmd_passwd},
    {"su", "Switch user", cmd_su},
    
    // Special commands
    {"cmatrix", "Matrix digital rain", cmd_cmatrix},
    {"reboot", "Reboot the system", cmd_reboot},
    {"wallpaper", "Wallpaper management", cmd_wallpaper},
};
static int command_count = sizeof(command_table) / sizeof(Command);

// ===========================================
// COMMAND SYSTEM
// ===========================================

void init_commands64(void) {
    last_total_ticks = rdtsc64();
    init_filesystem64();
    init_scripts64();  // ADD THIS LINE
     accounts_init();
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
    if (str_cmp(command, "test") == 0) {
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
    
    // Try to execute as custom script
    UserScript* script = script_get(command);
    if (script) {
        script_execute(command, args, output);
        return 1;
    }
    
    output_add_line(output, "Unknown command: ", VGA_RED);
    output_add_line(output, command, VGA_RED);
    output_add_line(output, "Type 'help' for available commands", VGA_CYAN);
    output_add_line(output, "Type 'script list' for custom scripts", VGA_CYAN);
    return 0;
}

const Command* get_all_commands64(int* count) {
    *count = command_count;
    return command_table;
}