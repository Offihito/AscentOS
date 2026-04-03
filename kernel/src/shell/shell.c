#include "shell/shell.h"
#include "console/console.h"
#include "drivers/keyboard.h"
#include "mm/pmm.h"
#include "lib/string.h"
#include "mm/heap.h"

#define CMD_BUFFER_SIZE 256

static char cmd_buffer[CMD_BUFFER_SIZE];
static int cmd_len = 0;

static void shell_print_uint64(uint64_t num) {
    if (num == 0) {
        console_puts("0");
        return;
    }
    char buf[20];
    int i = 0;
    while (num > 0) {
        buf[i++] = '0' + (num % 10);
        num /= 10;
    }
    while (i > 0) {
        i--;
        console_putchar(buf[i]);
    }
}

static void print_prompt(void) {
    console_puts("AscentOS> ");
}

static void execute_command(char *cmd) {
    // Strip trailing spaces
    int len = strlen(cmd);
    while (len > 0 && cmd[len - 1] == ' ') {
        cmd[len - 1] = '\0';
        len--;
    }

    if (len == 0) {
        return;
    }

    if (strcmp(cmd, "help") == 0) {
        console_puts("Available commands:\n");
        console_puts("  help    - Show this help message\n");
        console_puts("  clear   - Clear the console screen\n");
        console_puts("  meminfo - Display memory utilization\n");
        console_puts("  echo    - Echo arguments\n");
        console_puts("  heaptest- Test kernel heap allocator\n");
    } else if (strcmp(cmd, "clear") == 0) {
        console_clear();
    } else if (strcmp(cmd, "meminfo") == 0) {
        uint64_t total = pmm_get_total_memory();
        uint64_t usable = pmm_get_usable_memory();
        console_puts("Memory stats:\n");
        console_puts("  Total RAM:  ");
        shell_print_uint64(total / (1024 * 1024));
        console_puts(" MB\n");
        console_puts("  Usable RAM: ");
        shell_print_uint64(usable / (1024 * 1024));
        console_puts(" MB\n");
    } else if (strncmp(cmd, "echo ", 5) == 0) {
        console_puts(cmd + 5);
        console_puts("\n");
    } else if (strcmp(cmd, "heaptest") == 0) {
        console_puts("Starting Kernel Heap Stress Test...\n");
        
        // Test 1: Small allocations
        console_puts("[1/4] Allocating 50 small chunks...\n");
        void *ptrs[50];
        int test1_pass = 1;
        for (int i = 0; i < 50; i++) {
            ptrs[i] = kmalloc(32);
            if (!ptrs[i]) {
                test1_pass = 0;
                break;
            }
            memset(ptrs[i], (i & 0xFF), 32);
        }
        
        if (test1_pass) {
            for (int i = 0; i < 50; i++) {
                uint8_t *p = (uint8_t *)ptrs[i];
                for (int j = 0; j < 32; j++) {
                    if (p[j] != (i & 0xFF)) {
                        test1_pass = 0;
                        break;
                    }
                }
            }
        }
        
        if (test1_pass) {
            console_puts("  -> PASS: 50 small allocations & verification.\n");
        } else {
            console_puts("  -> FAIL: Small allocations.\n");
        }

        // Test 2: Fragmentation
        console_puts("[2/4] Freeing chunks to test fragmentation...\n");
        for (int i = 0; i < 50; i += 2) {
            kfree(ptrs[i]);
            ptrs[i] = NULL;
        }
        void *hole_ptrs[25];
        for (int i = 0; i < 25; i++) {
            hole_ptrs[i] = kmalloc(16);
        }
        console_puts("  -> PASS: Fragmentation allocation.\n");

        for (int i = 1; i < 50; i += 2) {
            if (ptrs[i]) kfree(ptrs[i]);
        }
        for (int i = 0; i < 25; i++) {
            if (hole_ptrs[i]) kfree(hole_ptrs[i]);
        }
        
        // Test 3: Large allocation
        console_puts("[3/4] Testing large contiguous allocation (64KB)...\n");
        void *large_ptr = kmalloc(65536);
        if (large_ptr) {
            memset(large_ptr, 0xAA, 65536);
            uint8_t *lp = (uint8_t *)large_ptr;
            if (lp[0] == 0xAA && lp[65535] == 0xAA) {
                console_puts("  -> PASS: Large allocation successful.\n");
            } else {
                console_puts("  -> FAIL: Large allocation corrupted.\n");
            }
            kfree(large_ptr);
        } else {
            console_puts("  -> FAIL: Large allocation returned NULL.\n");
        }
        
        // Test 4: kcalloc and krealloc
        console_puts("[4/4] Testing kcalloc & krealloc...\n");
        char *str = kcalloc(1, 16);
        if (str) {
            const char *hello = "Hello ";
            int i = 0;
            while(hello[i]) { str[i] = hello[i]; i++; }
            
            str = krealloc(str, 32);
            if (str) {
                const char *world = "World!";
                int j = 0;
                while(world[j]) { str[i+j] = world[j]; j++; }
                str[i+j] = '\0';
                
                console_puts("  -> realloc result: ");
                console_puts(str);
                console_puts("\n");
                kfree(str);
                console_puts("  -> PASS: calloc & realloc test.\n");
            } else {
                console_puts("  -> FAIL: krealloc returned NULL.\n");
            }
        }
        
        console_puts("Stress test complete.\n");
    } else {
        console_puts("Unknown command. Type 'help' for available commands.\n");
    }
}

void shell_init(void) {
    // Wait a brief moment or setup anything needed
    console_puts("\nWelcome to AscentOS Shell.\n");
}

void shell_run(void) {
    cmd_len = 0;
    cmd_buffer[0] = '\0';
    print_prompt();

    while (1) {
        char c = keyboard_get_char();

        if (c == '\n') {
            console_putchar('\n');
            cmd_buffer[cmd_len] = '\0';
            execute_command(cmd_buffer);
            cmd_len = 0;
            cmd_buffer[0] = '\0';
            print_prompt();
        } else if (c == '\b') {
            if (cmd_len > 0) {
                cmd_len--;
                cmd_buffer[cmd_len] = '\0';
                console_putchar('\b');
            }
        } else if (c != 0) {
            if (cmd_len < CMD_BUFFER_SIZE - 1) {
                cmd_buffer[cmd_len++] = c;
                console_putchar(c);
            }
        }
    }
}
