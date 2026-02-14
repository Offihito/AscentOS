#ifndef COMMANDS64_H
#define COMMANDS64_H

#include <stdint.h>

// Maximum command length
#define MAX_COMMAND_LENGTH 256
#define MAX_OUTPUT_LINES 50
#define MAX_LINE_LENGTH 128

// Command output structure
typedef struct {
    char lines[MAX_OUTPUT_LINES][MAX_LINE_LENGTH];
    uint8_t colors[MAX_OUTPUT_LINES];
    int line_count;
} CommandOutput;

// Command handler function type
typedef void (*CommandHandler)(const char* args, CommandOutput* output);

// Command structure
typedef struct {
    const char* name;
    const char* description;
    CommandHandler handler;
} Command;

// Color definitions
#define VGA_WHITE 0x0F
#define VGA_GREEN 0x0A
#define VGA_RED 0x0C
#define VGA_YELLOW 0x0E
#define VGA_CYAN 0x0B
#define VGA_MAGENTA 0x05
#define VGA_DARK_GRAY 0x08

// Helper string functions
int str_len(const char* str);
int str_cmp(const char* s1, const char* s2);
void str_cpy(char* dest, const char* src);
void str_concat(char* dest, const char* src);

// Memory functions (external from kernel64.c)
extern void* memset64(void* dest, int c, uint64_t n);
extern void* memcpy64(void* dest, const void* src, uint64_t n);

// Output management functions
void output_init(CommandOutput* output);
void output_add_line(CommandOutput* output, const char* line, uint8_t color);
void output_add_empty_line(CommandOutput* output);

// Command system functions
void init_commands64(void);
int execute_command64(const char* input, CommandOutput* output);
const Command* get_all_commands64(int* count);

// Individual command handlers
void cmd_hello(const char* args, CommandOutput* output);
void cmd_jew(const char* args, CommandOutput* output);
void cmd_help(const char* args, CommandOutput* output);
void cmd_clear(const char* args, CommandOutput* output);
void cmd_echo(const char* args, CommandOutput* output);
void cmd_about(const char* args, CommandOutput* output);
void cmd_neofetch(const char* args, CommandOutput* output);
void cmd_htop(const char* args, CommandOutput* output);
void cmd_reboot(const char* args, CommandOutput* output);
void cmd_cmatrix(const char* args, CommandOutput* output);

// File system commands
void cmd_ls(const char* args, CommandOutput* output);
void cmd_cat(const char* args, CommandOutput* output);
void cmd_touch(const char* args, CommandOutput* output);
void cmd_write(const char* args, CommandOutput* output);
void cmd_rm(const char* args, CommandOutput* output);

// Directory commands
void cmd_mkdir(const char* args, CommandOutput* output);
void cmd_rmdir(const char* args, CommandOutput* output);
void cmd_cd(const char* args, CommandOutput* output);
void cmd_pwd(const char* args, CommandOutput* output);

// Special commands (old style - direct VGA write)
void cmd_sysinfo(void);
void cmd_cpuinfo(void);
void cmd_meminfo(void);
void cmd_test(void);

// VMM test command
void cmd_vmm(const char* args, CommandOutput* output);

// ELF loader commands
void cmd_exec(const char* args, CommandOutput* output);
void cmd_elfinfo(const char* args, CommandOutput* output);

// Multitasking commands
void cmd_ps(const char* args, CommandOutput* output);
void cmd_taskinfo(const char* args, CommandOutput* output);
void cmd_createtask(const char* args, CommandOutput* output);
void cmd_schedinfo(const char* args, CommandOutput* output);
void cmd_offihito(const char* args, CommandOutput* output);

// Helper functions
void uint64_to_string(uint64_t num, char* str);
void int_to_str(int num, char* str);
void get_cpu_brand(char* brand);
void get_cpu_vendor(char* vendor);
uint64_t get_memory_info(void);
void format_memory_size(uint64_t kb, char* buffer);

// CPU usage helper functions
uint64_t rdtsc64(void);
uint32_t get_cpu_usage_64(void);

#endif // COMMANDS64_H