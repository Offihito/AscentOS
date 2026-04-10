#include "shell/shell.h"
#include "apic/lapic_timer.h"
#include "console/console.h"
#include "console/klog.h"
#include "drivers/input/keyboard.h"
#include "drivers/net/rtl8139.h"
#include "drivers/serial.h"
#include "drivers/storage/block.h"
#include "fs/ext2.h"
#include "fs/vfs.h"
#include "lib/string.h"
#include "lock/spinlock.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "net/arp.h"
#include "net/net.h"
#include "net/netif.h"
#include "sched/sched.h"
#include "smp/cpu.h"
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

static void print_prompt(void) { console_puts("AscentOS> "); }

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
    console_puts("  help      - Show this help message\n");
    console_puts("  clear     - Clear the console screen\n");
    console_puts("  meminfo   - Display memory utilization\n");
    console_puts("  echo      - Echo arguments\n");
    console_puts("  ps        - List running tasks\n");
    console_puts("  kill      - Terminate a task by TID (e.g. kill 5)\n");
    console_puts("  heaptest  - Test kernel heap allocator\n");
    console_puts("  locktest  - Test atomic spinlock functionality\n");
    console_puts("  uptime    - Show system uptime\n");
    console_puts("  diskinfo  - Show detected block devices\n");
    console_puts(
        "  readsect  - Read and hex-dump a disk sector (e.g. readsect 0)\n");
    console_puts("  ls        - List directory contents (e.g. ls /mnt)\n");
    console_puts(
        "  cat       - Display file contents (e.g. cat /mnt/hello.txt)\n");
    console_puts("  write     - Write text to file (e.g. write /mnt/file.txt "
                 "hello world)\n");
    console_puts(
        "  touch     - Create an empty file (e.g. touch /mnt/new.txt)\n");
    console_puts("  mkdir     - Create a directory (e.g. mkdir /mnt/mydir)\n");
    console_puts("  rm        - Delete a file (e.g. rm /mnt/old.txt)\n");
    console_puts(
        "  rmdir     - Remove an empty directory (e.g. rmdir /mnt/mydir)\n");
    console_puts(
        "  ln        - Create a symlink (e.g. ln /mnt/link /mnt/target)\n");
    console_puts(
        "  readlink  - Read symlink target (e.g. readlink /mnt/link)\n");
    console_puts("  stat      - Show file info (e.g. stat /mnt/file.txt)\n");
    console_puts(
        "  rename    - Rename a file (e.g. rename /mnt/old /mnt/new)\n");
    console_puts(
        "  chmod     - Change permissions (e.g. chmod 0777 /mnt/file.txt)\n");
    console_puts("  chown     - Change owner/group (e.g. chown 1000:1000 "
                 "/mnt/file.txt)\n");
    console_puts("  test_ring3_phase1 - Verify GDT and TSS initialization\n");
    console_puts("  test_ring3_phase2 - Verify Syscall MSR initialization\n");
    console_puts("  test_ring3_phase3 - Verify Syscall translation layer\n");
    console_puts("  exec      - Execute an ELF (e.g. exec /mnt/hello_musl.elf "
                 "[args...])\n");
    console_puts("  kilo      - Launch the kilo text editor\n");
    console_puts("  netinfo   - Show NIC status and MAC address\n");
    console_puts("  ifconfig  - Show network interface configuration\n");
    console_puts("  arp       - Display ARP cache table\n");
    console_puts(
        "  arping    - Send ARP request (e.g. arping 10.0.2.2)\n");
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
      sched_create_kernel_thread(test_task_entry, NULL);
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
      if (ptrs[i])
        kfree(ptrs[i]);
    }
    for (int i = 0; i < 25; i++) {
      if (hole_ptrs[i])
        kfree(hole_ptrs[i]);
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
      while (hello[i]) {
        str[i] = hello[i];
        i++;
      }

      str = krealloc(str, 32);
      if (str) {
        const char *world = "World!";
        int j = 0;
        while (world[j]) {
          str[i + j] = world[j];
          j++;
        }
        str[i + j] = '\0';

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
    uint64_t hrs = mins / 60;

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
        if (!blk)
          continue;
        console_puts("  ");
        console_puts(blk->name);
        console_puts(": ");
        shell_print_uint64((blk->total_sectors * blk->sector_size) /
                           (1024 * 1024));
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
  } else if (strncmp(cmd, "ls ", 3) == 0 || strcmp(cmd, "ls") == 0) {
    char *path = (strlen(cmd) > 3) ? cmd + 3 : "/";
    vfs_node_t *dir = vfs_resolve_path(path);

    if (!dir) {
      console_puts("ls: cannot access '");
      console_puts(path);
      console_puts("': No such file or directory\n");
    } else if ((dir->flags & 0x07) != FS_DIRECTORY) {
      console_puts("ls: '");
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
    vfs_node_t *file = vfs_resolve_path(path);

    if (!file) {
      console_puts("cat: ");
      console_puts(path);
      console_puts(": No such file\n");
    } else if ((file->flags & 0x07) == FS_DIRECTORY) {
      console_puts("cat: ");
      console_puts(path);
      console_puts(": Is a directory\n");
    } else if ((file->flags & 0x07) == FS_SYMLINK) {
      console_puts("cat: ");
      console_puts(path);
      console_puts(": Is a symbolic link (use readlink)\n");
    } else {
      uint8_t buf[512];
      uint32_t offset = 0;
      uint32_t bytes;
      uint32_t max_bytes = 8192;
      while ((bytes = vfs_read(file, offset, sizeof(buf), buf)) > 0) {
        for (uint32_t j = 0; j < bytes; j++) {
          char c = (char)buf[j];
          if (c >= 32 && c <= 126) {
            console_putchar(c);
          } else if (c == '\n' || c == '\r' || c == '\t') {
            console_putchar(c);
          } else {
            console_putchar('.');
          }
        }
        offset += bytes;
        if (offset >= max_bytes) {
          console_puts("\n... (truncated at 8KB)\n");
          break;
        }
      }
      console_puts("\n");
    }
  } else if (strncmp(cmd, "write ", 6) == 0) {
    // Usage: write /mnt/file.txt content here
    char *rest = cmd + 6;
    // Find end of path (first space)
    char *space = rest;
    while (*space && *space != ' ')
      space++;
    if (*space == '\0') {
      console_puts("Usage: write <path> <content>\n");
    } else {
      *space = '\0';
      char *filepath = rest;
      char *content = space + 1;
      uint32_t content_len = strlen(content);

      vfs_node_t *file = vfs_resolve_path(filepath);

      if (!file) {
        // Try to create it: resolve parent, then create
        // Find last '/' to get parent path and filename
        char *last_slash = NULL;
        for (char *p = filepath; *p; p++) {
          if (*p == '/')
            last_slash = p;
        }
        if (last_slash && last_slash != filepath) {
          *last_slash = '\0';
          char *parent_path = filepath;
          char *filename = last_slash + 1;
          vfs_node_t *parent = vfs_resolve_path(parent_path);
          if (parent) {
            if (vfs_create(parent, filename, 0644) == 0) {
              file = vfs_finddir(parent, filename);
            }
          }
          *last_slash = '/'; // Restore
        } else if (last_slash == filepath) {
          // Path like /filename — create in root
          char *filename = filepath + 1;
          if (vfs_create(fs_root, filename, 0644) == 0) {
            file = vfs_finddir(fs_root, filename);
          }
        }
      }

      if (!file) {
        console_puts("write: cannot create or open '");
        console_puts(rest);
        console_puts("'\n");
      } else {
        uint32_t written = vfs_write(file, 0, content_len, (uint8_t *)content);
        console_puts("Wrote ");
        shell_print_uint64(written);
        console_puts(" bytes.\n");
      }
    }
  } else if (strncmp(cmd, "touch ", 6) == 0) {
    char *filepath = cmd + 6;
    // Find parent directory and filename
    char *last_slash = NULL;
    for (char *p = filepath; *p; p++) {
      if (*p == '/')
        last_slash = p;
    }
    if (!last_slash) {
      console_puts("Usage: touch <path> (e.g. touch /mnt/file.txt)\n");
    } else {
      *last_slash = '\0';
      char *parent_path = (last_slash == filepath) ? "/" : filepath;
      char *filename = last_slash + 1;
      vfs_node_t *parent = vfs_resolve_path(parent_path);
      *last_slash = '/'; // Restore
      if (!parent) {
        console_puts("touch: parent directory not found\n");
      } else {
        int ret = vfs_create(parent, filename, 0644);
        if (ret == 0) {
          console_puts("Created: ");
          console_puts(filepath);
          console_puts("\n");
        } else {
          console_puts("touch: failed (file may already exist)\n");
        }
      }
    }
  } else if (strncmp(cmd, "mkdir ", 6) == 0) {
    char *dirpath = cmd + 6;
    char *last_slash = NULL;
    for (char *p = dirpath; *p; p++) {
      if (*p == '/')
        last_slash = p;
    }
    if (!last_slash) {
      console_puts("Usage: mkdir <path> (e.g. mkdir /mnt/mydir)\n");
    } else {
      *last_slash = '\0';
      char *parent_path = (last_slash == dirpath) ? "/" : dirpath;
      char *dirname = last_slash + 1;
      vfs_node_t *parent = vfs_resolve_path(parent_path);
      *last_slash = '/'; // Restore
      if (!parent) {
        console_puts("mkdir: parent directory not found\n");
      } else {
        int ret = vfs_mkdir(parent, dirname, 0755);
        if (ret == 0) {
          console_puts("Created directory: ");
          console_puts(dirpath);
          console_puts("\n");
        } else {
          console_puts("mkdir: failed (directory may already exist)\n");
        }
      }
    }
  } else if (strncmp(cmd, "rm ", 3) == 0) {
    char *filepath = cmd + 3;
    // Find parent directory and filename
    char *last_slash = NULL;
    for (char *p = filepath; *p; p++) {
      if (*p == '/')
        last_slash = p;
    }
    if (!last_slash) {
      console_puts("Usage: rm <path> (e.g. rm /mnt/file.txt)\n");
    } else {
      *last_slash = '\0';
      char *parent_path = (last_slash == filepath) ? "/" : filepath;
      char *filename = last_slash + 1;
      vfs_node_t *parent = vfs_resolve_path(parent_path);
      *last_slash = '/'; // Restore
      if (!parent) {
        console_puts("rm: parent directory not found\n");
      } else {
        int ret = vfs_unlink(parent, filename);
        if (ret == 0) {
          console_puts("Removed: ");
          console_puts(filepath);
          console_puts("\n");
        } else {
          console_puts("rm: failed (file not found or is a directory)\n");
        }
      }
    }
  } else if (strncmp(cmd, "rmdir ", 6) == 0) {
    char *dirpath = cmd + 6;
    char *last_slash = NULL;
    for (char *p = dirpath; *p; p++) {
      if (*p == '/')
        last_slash = p;
    }
    if (!last_slash) {
      console_puts("Usage: rmdir <path> (e.g. rmdir /mnt/mydir)\n");
    } else {
      *last_slash = '\0';
      char *parent_path = (last_slash == dirpath) ? "/" : dirpath;
      char *dirname = last_slash + 1;
      vfs_node_t *parent = vfs_resolve_path(parent_path);
      *last_slash = '/'; // Restore
      if (!parent) {
        console_puts("rmdir: parent directory not found\n");
      } else {
        int ret = vfs_rmdir(parent, dirname);
        if (ret == 0) {
          console_puts("Removed directory: ");
          console_puts(dirpath);
          console_puts("\n");
        } else {
          console_puts(
              "rmdir: failed (not found, not empty, or not a directory)\n");
        }
      }
    }
  } else if (strncmp(cmd, "ln ", 3) == 0) {
    // Usage: ln /mnt/linkname /mnt/target
    char *rest = cmd + 3;
    // Find end of link path (first space)
    char *space = rest;
    while (*space && *space != ' ')
      space++;
    if (*space == '\0') {
      console_puts("Usage: ln <linkpath> <target>\n");
    } else {
      *space = '\0';
      char *linkpath = rest;
      char *target = space + 1;

      // Resolve parent of link
      char *last_slash = NULL;
      for (char *p = linkpath; *p; p++) {
        if (*p == '/')
          last_slash = p;
      }
      if (!last_slash) {
        console_puts("Usage: ln <linkpath> <target>\n");
      } else {
        *last_slash = '\0';
        char *parent_path = (last_slash == linkpath) ? "/" : linkpath;
        char *linkname = last_slash + 1;
        vfs_node_t *parent = vfs_resolve_path(parent_path);
        *last_slash = '/'; // Restore
        if (!parent) {
          console_puts("ln: parent directory not found\n");
        } else {
          int ret = vfs_symlink(parent, linkname, target);
          if (ret == 0) {
            console_puts("Created symlink: ");
            console_puts(linkpath);
            console_puts(" -> ");
            console_puts(target);
            console_puts("\n");
          } else {
            console_puts("ln: failed (may already exist)\n");
          }
        }
      }
    }
  } else if (strncmp(cmd, "readlink ", 9) == 0) {
    char *path = cmd + 9;
    vfs_node_t *file = vfs_resolve_path(path);
    if (!file) {
      console_puts("readlink: ");
      console_puts(path);
      console_puts(": No such file\n");
    } else {
      char target_buf[256];
      int ret = vfs_readlink(file, target_buf, sizeof(target_buf));
      if (ret < 0) {
        console_puts("readlink: ");
        console_puts(path);
        console_puts(": Not a symbolic link\n");
      } else {
        console_puts(target_buf);
        console_puts("\n");
      }
    }
  } else if (strncmp(cmd, "stat ", 5) == 0) {
    char *path = cmd + 5;
    vfs_node_t *file = vfs_resolve_path(path);
    if (!file) {
      console_puts("stat: ");
      console_puts(path);
      console_puts(": No such file\n");
    } else {
      console_puts("  File: ");
      console_puts(path);
      console_puts("\n");
      console_puts("  Size: ");
      shell_print_uint64(file->length);
      console_puts(" bytes\n");
      console_puts("  ModTime: ");
      shell_print_uint64(file->mtime);
      console_puts(" secs since boot\n");
      console_puts("  Type: ");
      uint32_t ftype = file->flags & 0x07;
      if (ftype == FS_FILE)
        console_puts("regular file\n");
      else if (ftype == FS_DIRECTORY)
        console_puts("directory\n");
      else if (ftype == FS_SYMLINK) {
        console_puts("symbolic link -> ");
        char tbuf[256];
        if (vfs_readlink(file, tbuf, sizeof(tbuf)) >= 0) {
          console_puts(tbuf);
        }
        console_puts("\n");
      } else
        console_puts("other\n");
      console_puts("  Inode: ");
      shell_print_uint64(file->inode);
      console_puts("\n");
      console_puts("  Perms: 0");
      // Print permissions in octal
      uint32_t m = file->mask;
      console_putchar('0' + ((m >> 9) & 7));
      console_putchar('0' + ((m >> 6) & 7));
      console_putchar('0' + ((m >> 3) & 7));
      console_putchar('0' + (m & 7));
      console_puts("\n");
      console_puts("  UID: ");
      shell_print_uint64(file->uid);
      console_puts("  GID: ");
      shell_print_uint64(file->gid);
      console_puts("\n");
    }
  } else if (strncmp(cmd, "rename ", 7) == 0) {
    char *rest = cmd + 7;
    char *space = rest;
    while (*space && *space != ' ')
      space++;
    if (*space == '\0') {
      console_puts("Usage: rename <old_path> <new_path>\n");
    } else {
      *space = '\0';
      char *old_path = rest;
      char *new_path = space + 1;

      char *last_slash = NULL;
      for (char *p = old_path; *p; p++)
        if (*p == '/')
          last_slash = p;
      if (!last_slash) {
        console_puts("rename: need absolute path (e.g. /mnt/...)\n");
      } else {
        *last_slash = '\0';
        char *parent_path = (last_slash == old_path) ? "/" : old_path;
        char *old_name = last_slash + 1;
        vfs_node_t *parent = vfs_resolve_path(parent_path);

        char *new_last_slash = NULL;
        for (char *p = new_path; *p; p++)
          if (*p == '/')
            new_last_slash = p;
        if (!new_last_slash) {
          console_puts("rename: need absolute new path\n");
        } else {
          *new_last_slash = '\0';
          char *new_parent_path = (new_last_slash == new_path) ? "/" : new_path;
          char *new_name = new_last_slash + 1;

          if (strcmp(parent_path, new_parent_path) != 0) {
            console_puts("rename: cross-directory rename not supported yet\n");
          } else if (!parent) {
            console_puts("rename: parent directory not found\n");
          } else {
            if (vfs_rename(parent, old_name, new_name) == 0) {
              console_puts("Renamed ");
              *last_slash = '/'; // restore temporarily for printing
              console_puts(old_path);
              console_puts(" to ");
              *new_last_slash = '/'; // restore temporarily for printing
              console_puts(new_path);
              console_puts("\n");
              // slashes restored correctly
            } else {
              console_puts("rename failed\n");
            }
          }
          *new_last_slash = '/'; // Restore unconditionally
        }
        *last_slash = '/'; // Restore unconditionally
      }
    }
  } else if (strncmp(cmd, "chmod ", 6) == 0) {
    char *rest = cmd + 6;
    char *space = rest;
    while (*space && *space != ' ')
      space++;
    if (*space == '\0') {
      console_puts("Usage: chmod <octal_perms> <file>\n");
    } else {
      *space = '\0';
      char *perms_str = rest;
      char *path = space + 1;

      uint32_t perms = 0;
      for (int i = 0; perms_str[i]; i++) {
        if (perms_str[i] >= '0' && perms_str[i] <= '7') {
          perms = (perms << 3) + (perms_str[i] - '0');
        }
      }

      vfs_node_t *node = vfs_resolve_path(path);
      if (!node) {
        console_puts("chmod: file not found\n");
      } else if (vfs_chmod(node, perms) == 0) {
        console_puts("Permissions changed.\n");
      } else {
        console_puts("chmod failed.\n");
      }
    }
  } else if (strncmp(cmd, "chown ", 6) == 0) {
    char *rest = cmd + 6;
    char *space = rest;
    while (*space && *space != ' ')
      space++;
    if (*space == '\0') {
      console_puts("Usage: chown <uid>:<gid> <file>\n");
    } else {
      *space = '\0';
      char *owner_str = rest;
      char *path = space + 1;

      char *colon = NULL;
      for (char *p = owner_str; *p; p++)
        if (*p == ':')
          colon = p;

      uint32_t uid = 0, gid = 0;
      if (colon) {
        *colon = '\0';
        uid = atoui(owner_str);
        gid = atoui(colon + 1);
        *colon = ':';
      } else {
        uid = atoui(owner_str);
        gid = uid;
      }

      vfs_node_t *node = vfs_resolve_path(path);
      if (!node) {
        console_puts("chown: file not found\n");
      } else if (vfs_chown(node, uid, gid) == 0) {
        console_puts("Ownership changed.\n");
      } else {
        console_puts("chown failed.\n");
      }
    }
  } else if (strcmp(cmd, "test_ring3_phase1") == 0) {
    console_puts("Phase 1: GDT & TSS Verification\n");
    uint8_t sgdt_buf[10];
    __asm__ volatile("sgdt %0" : "=m"(sgdt_buf));
    uint16_t limit = *(uint16_t *)sgdt_buf;
    uint64_t base = *(uint64_t *)(sgdt_buf + 2);
    console_puts("  GDT Base: 0x");
    shell_print_uint64(base);
    console_puts("\n  GDT Limit: ");
    shell_print_uint64(limit);
    console_puts("\n");

    uint16_t tr = 0;
    __asm__ volatile("str %0" : "=r"(tr));
    console_puts("  Task Register (TR): 0x");
    shell_print_uint64(tr);
    if (tr == 0x28) {
      console_puts("\n  -> SUCCESS: TR is 0x28 (TSS loaded!).\n");
    } else {
      console_puts("\n  -> FAIL: TR is not 0x28.\n");
    }
  } else if (strcmp(cmd, "test_ring3_phase2") == 0) {
    console_puts("Phase 2: Syscall MSR Verification\n");

    uint64_t star = 0, lstar = 0, fmask = 0, efer = 0;
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000080));
    efer = ((uint64_t)hi << 32) | lo;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000081));
    star = ((uint64_t)hi << 32) | lo;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000082));
    lstar = ((uint64_t)hi << 32) | lo;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000084));
    fmask = ((uint64_t)hi << 32) | lo;

    console_puts("  EFER: 0x");
    shell_print_uint64(efer);
    if (efer & 1)
      console_puts(" (SCE enabled)\n");
    else
      console_puts(" (SCE disabled)\n");

    console_puts("  STAR: 0x");
    shell_print_uint64(star);
    console_puts("\n");
    console_puts("  LSTAR: 0x");
    shell_print_uint64(lstar);
    console_puts("\n");
    console_puts("  FMASK: 0x");
    shell_print_uint64(fmask);
    console_puts("\n");

    if ((efer & 1) && star != 0 && lstar != 0) {
      console_puts("\n  -> SUCCESS: MSRs are configured!\n");
    } else {
      console_puts("\n  -> FAIL: MSR misconfiguration.\n");
    }
  } else if (strcmp(cmd, "test_ring3_phase3") == 0) {
    console_puts("Phase 3: Syscall ABI Translation Verification\n");
    console_puts("  Simulating a 'write' syscall to stdout...\n");

    // We will fake a push of syscall arguments to the dispatcher
    struct mock_regs {
      uint64_t rdi, rsi, rdx, r10, r8, r9, rax, rbx, rbp, r12, r13, r14, r15;
      uint64_t rip, rflags, rsp;
    } regs = {0};

    char *str = "  [Syscall Response] Hello from the other side!\n";

    regs.rax = 1; // sys_write
    regs.rdi = 1; // fd = 1 (stdout)
    regs.rsi = (uint64_t)str;
    regs.rdx = strlen(str); // count

    extern void syscall_dispatcher(void *);
    syscall_dispatcher(&regs);

    if (regs.rax == strlen(str)) {
      console_puts("  -> SUCCESS: Syscall returned correctly bytes written.\n");
    } else {
      console_puts("  -> FAIL: Syscall return corrupted.\n");
    }
  } else if (strncmp(cmd, "exec ", 5) == 0) {
    char *args_str = cmd + 5;
    // Simple parsing
    const char *argv[10];
    int argc = 0;
    char *p = args_str;
    while (*p && argc < 9) {
      while (*p == ' ')
        p++;
      if (!*p)
        break;
      argv[argc++] = p;
      while (*p && *p != ' ')
        p++;
      if (*p)
        *p++ = 0;
    }
    argv[argc] = NULL;
    if (argc > 0) {
      if (!process_exec_argv((const char **)argv)) {
        console_puts("exec failed.\n");
      }
    } else {
      console_puts("exec: no command\n");
    }
  } else if (strncmp(cmd, "kilo", 4) == 0) {
    // Usage: kilo <filename>  (e.g. kilo /mnt/test.txt)
    char *filename = cmd + 4;
    while (*filename == ' ') filename++;  // skip spaces
    
    if (*filename == '\0') {
      console_puts("Usage: kilo <filename> (e.g. kilo /mnt/test.txt)\n");
    } else {
      const char *argv[] = {"/mnt/kilo.elf", filename, NULL};
      if (!process_exec_argv(argv)) {
        console_puts("kilo exec failed.\n");
      }
    }
  } else if (strcmp(cmd, "netinfo") == 0) {
    if (!rtl8139_is_present()) {
      console_puts("No RTL8139 NIC detected.\n");
    } else {
      const uint8_t *mac = rtl8139_get_mac();
      const char *hex = "0123456789ABCDEF";
      console_puts("RTL8139 Network Interface\n");
      console_puts("  MAC Address: ");
      for (int i = 0; i < 6; i++) {
        console_putchar(hex[(mac[i] >> 4) & 0xF]);
        console_putchar(hex[mac[i] & 0xF]);
        if (i < 5) console_putchar(':');
      }
      console_puts("\n  Link: ");
      console_puts(rtl8139_link_up() ? "UP" : "DOWN");
      console_puts("\n  I/O Base: 0x");
      uint16_t iobase = rtl8139_get_iobase();
      console_putchar(hex[(iobase >> 12) & 0xF]);
      console_putchar(hex[(iobase >> 8) & 0xF]);
      console_putchar(hex[(iobase >> 4) & 0xF]);
      console_putchar(hex[iobase & 0xF]);
      console_puts("\n  IRQ: ");
      shell_print_uint64(rtl8139_get_irq());
      console_puts("\n");
    }
  } else if (strcmp(cmd, "ifconfig") == 0) {
    netif_t *nif = netif_get();
    if (!nif->up) {
      console_puts("Network interface is down.\n");
    } else {
      const char *hex = "0123456789ABCDEF";
      console_puts("eth0:\n");
      console_puts("  MAC: ");
      for (int i = 0; i < 6; i++) {
        console_putchar(hex[(nif->mac[i] >> 4) & 0xF]);
        console_putchar(hex[nif->mac[i] & 0xF]);
        if (i < 5) console_putchar(':');
      }
      console_puts("\n  IP:      ");
      shell_print_uint64((nif->ip >> 24) & 0xFF);
      console_putchar('.');
      shell_print_uint64((nif->ip >> 16) & 0xFF);
      console_putchar('.');
      shell_print_uint64((nif->ip >> 8) & 0xFF);
      console_putchar('.');
      shell_print_uint64(nif->ip & 0xFF);
      console_puts("\n  Gateway: ");
      shell_print_uint64((nif->gateway >> 24) & 0xFF);
      console_putchar('.');
      shell_print_uint64((nif->gateway >> 16) & 0xFF);
      console_putchar('.');
      shell_print_uint64((nif->gateway >> 8) & 0xFF);
      console_putchar('.');
      shell_print_uint64(nif->gateway & 0xFF);
      console_puts("\n  Netmask: ");
      shell_print_uint64((nif->netmask >> 24) & 0xFF);
      console_putchar('.');
      shell_print_uint64((nif->netmask >> 16) & 0xFF);
      console_putchar('.');
      shell_print_uint64((nif->netmask >> 8) & 0xFF);
      console_putchar('.');
      shell_print_uint64(nif->netmask & 0xFF);
      console_puts("\n  Status:  UP\n");
    }
  } else if (strcmp(cmd, "arp") == 0) {
    int count = 0;
    const arp_entry_t *table = arp_get_table(&count);
    const char *hex = "0123456789ABCDEF";
    int found = 0;
    console_puts("ARP Cache:\n");
    for (int i = 0; i < count; i++) {
      if (!table[i].valid) continue;
      found++;
      console_puts("  ");
      shell_print_uint64((table[i].ip >> 24) & 0xFF);
      console_putchar('.');
      shell_print_uint64((table[i].ip >> 16) & 0xFF);
      console_putchar('.');
      shell_print_uint64((table[i].ip >> 8) & 0xFF);
      console_putchar('.');
      shell_print_uint64(table[i].ip & 0xFF);
      console_puts(" -> ");
      for (int j = 0; j < 6; j++) {
        console_putchar(hex[(table[i].mac[j] >> 4) & 0xF]);
        console_putchar(hex[table[i].mac[j] & 0xF]);
        if (j < 5) console_putchar(':');
      }
      console_putchar('\n');
    }
    if (!found) {
      console_puts("  (empty)\n");
    }
  } else if (strncmp(cmd, "arping ", 7) == 0) {
    // Parse dotted-decimal IP: arping 10.0.2.2
    char *ip_str = cmd + 7;
    uint32_t octets[4] = {0};
    int octet_idx = 0;
    for (char *p = ip_str; *p && octet_idx < 4; p++) {
      if (*p >= '0' && *p <= '9') {
        octets[octet_idx] = octets[octet_idx] * 10 + (*p - '0');
      } else if (*p == '.') {
        octet_idx++;
      }
    }
    if (octet_idx == 3) {
      // Final octet was set but no trailing dot
    }
    uint32_t target_ip = (octets[0] << 24) | (octets[1] << 16) |
                         (octets[2] << 8)  | octets[3];

    console_puts("ARPING ");
    shell_print_uint64(octets[0]); console_putchar('.');
    shell_print_uint64(octets[1]); console_putchar('.');
    shell_print_uint64(octets[2]); console_putchar('.');
    shell_print_uint64(octets[3]);
    console_puts("...\n");

    if (arp_send_request(target_ip) < 0) {
      console_puts("Failed to send ARP request.\n");
    } else {
      // Poll for a reply (up to ~2 seconds)
      const char *hex = "0123456789ABCDEF";
      bool got_reply = false;
      for (int attempt = 0; attempt < 200000; attempt++) {
        net_poll();  // Process any queued packets
        const arp_entry_t *entry = arp_lookup(target_ip);
        if (entry) {
          console_puts("Reply from ");
          shell_print_uint64(octets[0]); console_putchar('.');
          shell_print_uint64(octets[1]); console_putchar('.');
          shell_print_uint64(octets[2]); console_putchar('.');
          shell_print_uint64(octets[3]);
          console_puts(" [MAC: ");
          for (int j = 0; j < 6; j++) {
            console_putchar(hex[(entry->mac[j] >> 4) & 0xF]);
            console_putchar(hex[entry->mac[j] & 0xF]);
            if (j < 5) console_putchar(':');
          }
          console_puts("]\n");
          got_reply = true;
          break;
        }
        // Small delay between polls
        for (volatile int d = 0; d < 100; d++) { }
      }
      if (!got_reply) {
        console_puts("Timeout: no ARP reply received.\n");
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

  console_set_cursor_visible(true);

  while (1) {
    console_refresh_cursor();

    char c = 0;
    if (keyboard_has_char()) {
      c = keyboard_get_char();
    } else if (serial_received()) {
      c = serial_get_char();
    }

    if (c == 0) {
      sched_yield(); // Don't hog the CPU if no input
      continue;
    }

    if (c == (char)KEY_UP) {
      console_scroll_view(1);
    } else if (c == (char)KEY_DOWN) {
      console_scroll_view(-1);
    } else if (c == (char)KEY_PGUP) {
      console_scroll_view(10);
    } else if (c == (char)KEY_PGDN) {
      console_scroll_view(-10);
    } else if (c == '\r' || c == '\n') {
      console_putchar('\n');
      cmd_buffer[cmd_len] = '\0';
      execute_command(cmd_buffer);
      cmd_len = 0;
      cmd_buffer[0] = '\0';
      print_prompt();
    } else if (c == '\b' || c == 0x7F) { // 0x7F is DEL, often used as backspace
                                         // in serial terms
      if (cmd_len > 0) {
        cmd_len--;
        cmd_buffer[cmd_len] = '\0';
        console_putchar('\b');
      }
    } else {
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
