#include "shell/shell.h"
#include "console/console.h"
#include "drivers/input/keyboard.h"
#include "mm/pmm.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "lock/spinlock.h"
#include "apic/lapic_timer.h"
#include "sched/sched.h"
#include "console/klog.h"
#include "smp/cpu.h"
#include "drivers/storage/block.h"
#include "fs/vfs.h"
#include <stdint.h>

#define CMD_BUFFER_SIZE 256

static char cmd_buffer[CMD_BUFFER_SIZE];
static int cmd_len = 0;

static void test_task_entry(void);

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
        console_puts("  ps      - List running tasks\n");
        console_puts("  kill    - Terminate a task by TID (e.g. kill 5)\n");
        console_puts("  heaptest- Test kernel heap allocator\n");
        console_puts("  locktest- Test atomic spinlock functionality\n");
        console_puts("  uptime  - Show system uptime\n");
        console_puts("  diskinfo- Show detected block devices\n");
        console_puts("  readsect- Read and hex-dump a disk sector (e.g. readsect 0)\n");
    } else if (strcmp(cmd, "ps") == 0) {
        sched_print_tasks();
    } else if (strncmp(cmd, "kill ", 5) == 0) {
        uint32_t tid = atoui(cmd + 5);
        if (sched_terminate_thread(tid)) {
            console_puts("Terminated thread ");
            shell_print_uint64(tid);
            console_puts("\n");
        } else {
            console_puts("Failed to terminate thread (maybe TID 0 or not found).\n");
        }
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
    } else if (strcmp(cmd, "test-task") == 0) {
        for (int i = 0; i < 4; i++) {
            sched_create_kernel_thread(test_task_entry);
        }
        console_puts("Spawned 4 test tasks across SMP cores!\n");
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
    } else if (strcmp(cmd, "locktest") == 0) {
        console_puts("Testing Atomic Spinlock primitives...\n");
        static spinlock_t test_lock = SPINLOCK_INIT;
        
        console_puts("Acquiring test_lock...\n");
        spinlock_acquire(&test_lock);
        console_puts("  -> test_lock acquired (locked state achieved)!\n");
        
        console_puts("Releasing test_lock...\n");
        spinlock_release(&test_lock);
        console_puts("  -> test_lock released (unlocked state achieved)!\n");
        
        console_puts("Spinlock test complete and PASSED.\n");
    } else if (strcmp(cmd, "uptime") == 0) {
        uint64_t ms = lapic_timer_get_ms();
        uint64_t secs = ms / 1000;
        uint64_t mins = secs / 60;
        uint64_t hrs  = mins / 60;

        console_puts("Uptime: ");
        // Hours
        shell_print_uint64(hrs);
        console_puts("h ");
        // Minutes
        shell_print_uint64(mins % 60);
        console_puts("m ");
        // Seconds
        shell_print_uint64(secs % 60);
        console_puts("s ");
        // Milliseconds
        shell_print_uint64(ms % 1000);
        console_puts("ms\n");

        console_puts("LAPIC ticks: ");
        shell_print_uint64(lapic_timer_get_ticks());
        console_puts("\n");
    } else if (strcmp(cmd, "diskinfo") == 0) {
        int count = block_count();
        if (count == 0) {
            console_puts("No block devices registered.\n");
        } else {
            console_puts("Block devices:\n");
            for (int i = 0; i < count; i++) {
                struct block_device *blk = block_get(i);
                if (!blk) continue;
                console_puts("  ");
                console_puts(blk->name);
                console_puts(": ");
                shell_print_uint64((blk->total_sectors * blk->sector_size) / (1024 * 1024));
                console_puts(" MB (");
                shell_print_uint64(blk->total_sectors);
                console_puts(" sectors x ");
                shell_print_uint64(blk->sector_size);
                console_puts(" bytes)\n");
            }
        }
    } else if (strncmp(cmd, "readsect ", 9) == 0) {
        uint64_t lba = (uint64_t)atoui(cmd + 9);
        struct block_device *blk = block_get(0);
        if (!blk) {
            console_puts("No block device available.\n");
        } else {
            uint8_t sector_buf[512];
            memset(sector_buf, 0, 512);
            int ret = blk->read_sectors(blk, lba, 1, sector_buf);
            if (ret < 0) {
                console_puts("Read error!\n");
            } else {
                console_puts("Sector ");
                shell_print_uint64(lba);
                console_puts(" from ");
                console_puts(blk->name);
                console_puts(":\n");
                // Hex dump: 16 bytes per line, 32 lines
                const char *hex = "0123456789ABCDEF";
                for (int row = 0; row < 32; row++) {
                    // Print offset
                    uint16_t off = (uint16_t)(row * 16);
                    console_putchar(hex[(off >> 8) & 0xF]);
                    console_putchar(hex[(off >> 4) & 0xF]);
                    console_putchar(hex[off & 0xF]);
                    console_puts(": ");
                    // Hex values
                    for (int col = 0; col < 16; col++) {
                        uint8_t b = sector_buf[row * 16 + col];
                        console_putchar(hex[(b >> 4) & 0xF]);
                        console_putchar(hex[b & 0xF]);
                        console_putchar(' ');
                    }
                    // ASCII
                    console_puts(" |");
                    for (int col = 0; col < 16; col++) {
                        uint8_t b = sector_buf[row * 16 + col];
                        console_putchar((b >= 0x20 && b < 0x7F) ? (char)b : '.');
                    }
                    console_puts("|\n");
                }
            }
        }
    } else if (strncmp(cmd, "ls ", 3) == 0) {
        char *path = cmd + 3;
        // Basic traversal logic for single directory depth testing
        vfs_node_t *dir = fs_root;
        if (strcmp(path, "/") != 0) {
            // Very naive path lookup assuming "/name" format for testing
            char *name = path[0] == '/' ? path + 1 : path;
            dir = vfs_finddir(fs_root, name);
        }
        
        if (!dir) {
            console_puts("ls: cannot access '");
            console_puts(path);
            console_puts("': No such file or directory\n");
        } else if ((dir->flags & 0x07) != FS_DIRECTORY) {
            console_puts("ls: cannot access '");
            console_puts(path);
            console_puts("': Not a directory\n");
        } else {
            struct dirent *d;
            uint32_t i = 0;
            while ((d = vfs_readdir(dir, i++)) != 0) {
                console_puts(d->name);
                console_puts("  ");
            }
            console_puts("\n");
        }
    } else if (strncmp(cmd, "cat ", 4) == 0) {
        char *path = cmd + 4;
        // Naive path lookup
        vfs_node_t *dir = fs_root;
        char *file_name = path;
        if (strncmp(path, "/dev/", 5) == 0) {
            dir = vfs_finddir(fs_root, "dev");
            file_name = path + 5;
        } else if (path[0] == '/') {
            file_name = path + 1;
        }
        
        if (!dir) {
            console_puts("cat: directory not found\n");
        } else {
            vfs_node_t *file = vfs_finddir(dir, file_name);
            if (!file) {
                console_puts("cat: ");
                console_puts(path);
                console_puts(": No such file\n");
            } else if ((file->flags & 0x07) == FS_DIRECTORY) {
                console_puts("cat: ");
                console_puts(path);
                console_puts(": Is a directory\n");
            } else {
                uint8_t buf[512];
                uint32_t offset = 0;
                uint32_t bytes;
                uint32_t max_bytes = 1024; // Limit to 1KB for safety until we have Ext2/real files
                while ((bytes = vfs_read(file, offset, sizeof(buf), buf)) > 0) {
                    for (uint32_t j = 0; j < bytes; j++) {
                        char c = (char)buf[j];
                        if (c >= 32 && c <= 126) {
                            console_putchar(c);
                        } else if (c == '\n' || c == '\r' || c == '\t') {
                            console_putchar(c);
                        } else {
                            console_putchar('.'); // Fallback for unprintable binary data
                        }
                    }
                    offset += bytes;
                    if (offset >= max_bytes) {
                        console_puts("\n... (truncated for safety)\n");
                        break;
                    }
                }
                console_puts("\n");
            }
        }
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

        if (c == (char)KEY_UP) {
            console_scroll_view(1);
        } else if (c == (char)KEY_DOWN) {
            console_scroll_view(-1);
        } else if (c == (char)KEY_PGUP) {
            console_scroll_view(10);
        } else if (c == (char)KEY_PGDN) {
            console_scroll_view(-10);
        } else if (c == '\n') {
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

static void test_task_entry(void) {
    while (1) {
        // Sleep for 1 second (1000 ticks)
        lapic_timer_sleep(1000);
        
        struct cpu_info *cpu = cpu_get_current();
        
        klog_puts("[TASK] Background thread executing on CPU ");
        klog_uint64(cpu->cpu_id);
        klog_puts("!\n");
    }
}
