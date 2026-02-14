// files64.c  –  AscentOS 64-bit VFS layer
//
// Changes vs. original:
//   • Removed all 512-byte content caps (was: content_size >= 512 → 511).
//   • dynamic_content64 now stores pointers into a large heap region so
//     individual files can be up to MAX_FILE_SIZE (1 MiB by default).
//   • Persistence rewritten to use fat32_read_file / fat32_write_file
//     instead of raw LBA sectors.  Each dynamic file becomes one FAT32
//     file named by its index (e.g. "F0000000.DAT") and the directory
//     table is stored as "DIRTABLE.DAT".
//   • All other logic (path handling, mkdir, rmdir, tree, find, du) is
//     unchanged from the original.

#include "files64.h"
#include "../apps/commands64.h"
#include "../kernel/disk64.h"
#include <stddef.h>

#ifndef MAX_FILES
#define MAX_FILES 2048
#endif

#ifndef MAX_DIRS
#define MAX_DIRS 512
#endif

#ifndef MAX_FILE_SIZE
#define MAX_FILE_SIZE (2048u * 1024u * 1024u)
#endif

// ------------------------------------------------------------------
//  Persistence sector / FAT32 file names
// ------------------------------------------------------------------
#define FAT32_DIRTABLE  "DIRTABLE.DAT"   // Saved dynamic dirs
#define FAT32_FTABLE    "FTABLE.DAT"     // Saved dynamic file metadata
#define FAT32_CONTENT_PREFIX "FC"        // FC + 6-hex-digit index + ".DAT"

// ------------------------------------------------------------------
//  Current working directory
// ------------------------------------------------------------------
static char current_dir[MAX_PATH_LENGTH] = "/";

// ------------------------------------------------------------------
//  Directory list
// ------------------------------------------------------------------
static Directory64* directories = NULL;
static int dir_count = 0;

// ================================================================
//  SYSTEM FILES  (static / read-only, embedded in the binary)
// ================================================================

static const char file_motd64[] =
    "AscentOS 64-bit v1.2 - Unix-like Edition\n"
    "Advanced Multi-Level Directory Tree System\n"
    "\n"
    "Welcome to 64-bit chaos with Unix structure!\n"
    "Type 'help' for available commands\n"
    "Type 'tree' to see directory structure\n";

static const char file_bashrc[] =
    "# AscentOS Bash Configuration\n"
    "export PATH=/bin:/usr/bin\n"
    "export HOME=/home\n"
    "alias ll='ls -la'\n"
    "alias ..='cd ..'\n";

static const char file_profile[] =
    "# System-wide profile\n"
    "PATH=/bin:/usr/bin:/usr/local/bin\n"
    "export PATH\n";

static const char file_hostname[] = "ascentos\n";

static const char file_hosts[] =
    "127.0.0.1   localhost\n"
    "127.0.1.1   ascentos\n"
    "::1         localhost ip6-localhost\n";

static const char file_fstab[] =
    "# <file system>  <mount point>  <type>  <options>  <dump>  <pass>\n"
    "/dev/sda1        /              fat32   defaults   0       1\n"
    "/dev/sda2        /home          fat32   defaults   0       2\n";

static const char file_passwd[] =
    "root:x:0:0:root:/root:/bin/bash\n"
    "user:x:1000:1000:User:/home/user:/bin/bash\n";

static const char file_readme[] =
    "AscentOS File System (FAT32 backend)\n"
    "=====================================\n"
    "\n"
    "This is a Unix-like VFS backed by a FAT32 partition.\n"
    "Individual files can be up to 2 GB (configurable via\n"
    "MAX_FILE_SIZE in files64.h).\n"
    "\n"
    "Directory Structure:\n"
    "/bin     - Essential command binaries\n"
    "/boot    - Boot loader files\n"
    "/dev     - Device files\n"
    "/etc     - System configuration files\n"
    "/home    - User home directories\n"
    "/lib     - System libraries\n"
    "/mnt     - Mount points\n"
    "/opt     - Optional software\n"
    "/proc    - Process information\n"
    "/root    - Root user home\n"
    "/tmp     - Temporary files\n"
    "/usr     - User programs\n"
    "/var     - Variable data\n"
    "\n"
    "Commands:\n"
    "  tree     - Show directory tree\n"
    "  find     - Find files by pattern\n"
    "  du       - Show disk usage\n"
    "  mkdir -p - Create nested directories\n";

static const char file_version[] = "AscentOS 1.2 (64-bit, FAT32)\n";
static const char file_null[]    = "";
static const char file_zero[]    = "";
static const char file_random[]  = "Random device simulation\n";

static const EmbeddedFile64 static_files64[] = {
    {"motd",       file_motd64,  sizeof(file_motd64)  - 1, 0, "/etc"},
    {"hostname",   file_hostname,sizeof(file_hostname) - 1, 0, "/etc"},
    {"hosts",      file_hosts,   sizeof(file_hosts)    - 1, 0, "/etc"},
    {"fstab",      file_fstab,   sizeof(file_fstab)    - 1, 0, "/etc"},
    {"passwd",     file_passwd,  sizeof(file_passwd)   - 1, 0, "/etc"},
    {"bashrc",     file_bashrc,  sizeof(file_bashrc)   - 1, 0, "/etc"},
    {"profile",    file_profile, sizeof(file_profile)  - 1, 0, "/etc"},
    {"README.txt", file_readme,  sizeof(file_readme)   - 1, 0, "/"},
    {"version",    file_version, sizeof(file_version)  - 1, 0, "/etc"},
    {"null",       file_null,    0,                        0, "/dev"},
    {"zero",       file_zero,    0,                        0, "/dev"},
    {"random",     file_random,  sizeof(file_random)   - 1, 0, "/dev"},
    {NULL, NULL, 0, 0, NULL}
};

// ================================================================
//  Dynamic storage
//  dynamic_content_pool: large flat heap area carved into
//  MAX_FILE_SIZE slots, one per dynamic file index.
//  dynamic_names64  : file names  (up to 64 bytes each)
//  dynamic_dirs64   : parent dirs (up to MAX_PATH_LENGTH each)
//  all_files64      : combined static + dynamic entries
// ================================================================

// We allocate from a large static heap so no libc malloc is needed.
// Total size: MAX_FILES * MAX_FILE_SIZE  (≈ 2048 * 1 MiB = 2 GiB!)
// That is too large for a flat array, so we use a pool of 64 KiB
// slots by default.  Adjust MAX_FILE_SIZE in files64.h as needed.
//
// For a kernel with limited RAM we cap the pool at 32 MiB and allow
// individual files up to MAX_FILE_SIZE within that pool.
//
// IMPORTANT: if you raise MAX_FILE_SIZE AND MAX_FILES you must also
// raise HEAP_POOL_SIZE below.
#define HEAP_POOL_SIZE  (32u * 1024u * 1024u)   /* 32 MiB total pool */

static char     dynamic_names64 [MAX_FILES][64];
static char     dynamic_dirs64  [MAX_FILES][MAX_PATH_LENGTH];
static EmbeddedFile64 all_files64[MAX_FILES];
static int      file_count64 = 0;

// Content pool – each dynamic file gets a pointer into this region.
// We sub-allocate sequentially; freed slots are NOT reclaimed (simple
// approach suitable for an OS demo).
static char     content_pool[HEAP_POOL_SIZE];
static uint32_t pool_offset = 0;

// Pointers to each dynamic file's content within the pool
static char* dynamic_content_ptr[MAX_FILES];

static char* pool_alloc(uint32_t size) {
    if (pool_offset + size > HEAP_POOL_SIZE) return NULL;
    char* p = &content_pool[pool_offset];
    pool_offset += size;
    return p;
}

// ================================================================
//  String helpers (same as original; kept here so files64.c is
//  self-contained – commands64.c also defines these externally)
// ================================================================

static int str_starts_with(const char* str, const char* prefix) {
    while (*prefix) {
        if (*str++ != *prefix++) return 0;
    }
    return 1;
}

static int str_contains(const char* str, const char* substr) {
    if (!*substr) return 1;
    for (int i = 0; str[i]; i++) {
        int j = 0;
        while (str[i + j] && substr[j] && str[i + j] == substr[j]) j++;
        if (!substr[j]) return 1;
    }
    return 0;
}

// ================================================================
//  Path helpers
// ================================================================
static void normalize_path(const char* input, char* output) {
    if (input[0] == '/') {
        str_cpy(output, input);
    } else {
        str_cpy(output, current_dir);
        if (output[str_len(output) - 1] != '/')
            str_concat(output, "/");
        str_concat(output, input);
    }
    int len = str_len(output);
    if (len > 1 && output[len - 1] == '/')
        output[len - 1] = '\0';
}

static int dir_exists(const char* path) {
    if (str_cmp(path, "/") == 0) return 1;
    for (int i = 0; i < dir_count; i++)
        if (str_cmp(directories[i].path, path) == 0) return 1;
    return 0;
}

static void get_parent_dir(const char* path, char* parent) {
    if (str_cmp(path, "/") == 0) { str_cpy(parent, "/"); return; }
    int last_slash = -1;
    for (int i = 0; path[i]; i++)
        if (path[i] == '/') last_slash = i;
    if (last_slash <= 0) {
        str_cpy(parent, "/");
    } else {
        for (int i = 0; i < last_slash; i++) parent[i] = path[i];
        parent[last_slash] = '\0';
    }
}

static void get_dir_name(const char* path, char* name) {
    int last_slash = -1;
    for (int i = 0; path[i]; i++)
        if (path[i] == '/') last_slash = i;
    if (last_slash < 0) str_cpy(name, path);
    else str_cpy(name, &path[last_slash + 1]);
}

// ================================================================
//  System directories
// ================================================================
static void create_system_dir(const char* path) {
    if (dir_count >= MAX_DIRS) return;
    str_cpy(directories[dir_count].path, path);
    directories[dir_count].is_dynamic    = 0;
    directories[dir_count].is_system     = 1;
    directories[dir_count].permissions   = 0755;
    directories[dir_count].created_time  = 0;
    dir_count++;
}

static void init_unix_tree(void) {
    create_system_dir("/bin");
    create_system_dir("/boot");
    create_system_dir("/dev");
    create_system_dir("/etc");
    create_system_dir("/home");
    create_system_dir("/lib");
    create_system_dir("/mnt");
    create_system_dir("/opt");
    create_system_dir("/proc");
    create_system_dir("/root");
    create_system_dir("/tmp");
    create_system_dir("/usr");
    create_system_dir("/var");
    create_system_dir("/usr/bin");
    create_system_dir("/usr/lib");
    create_system_dir("/usr/local");
    create_system_dir("/usr/local/bin");
    create_system_dir("/usr/local/lib");
    create_system_dir("/usr/share");
    create_system_dir("/var/log");
    create_system_dir("/var/tmp");
    create_system_dir("/var/cache");
    create_system_dir("/var/lib");
    create_system_dir("/home/user");
    create_system_dir("/etc/config");
    create_system_dir("/etc/init.d");
}

// ================================================================
//  FAT32 persistence helpers
// ================================================================

// Convert a uint32_t to 8 uppercase hex chars (no prefix).
static void u32_to_hex8(uint32_t v, char out[9]) {
    const char hex[] = "0123456789ABCDEF";
    for (int i = 7; i >= 0; i--) {
        out[i] = hex[v & 0xF];
        v >>= 4;
    }
    out[8] = '\0';
}

// Build the FAT32 filename for dynamic file index i.
// Result looks like "FC000001.DAT" (12 chars + NUL).
static void make_content_fname(int idx, char out[13]) {
    char hex[9];
    u32_to_hex8((uint32_t)idx, hex);
    // "FC" + first 6 hex digits + "." + last 2 hex digits clipped → too long.
    // Use simpler 8.3: "FC" + 4-digit hex (supports up to 65535 files) + ".DAT"
    // FC0000.DAT through FCFFFF.DAT
    out[0] = 'F';
    out[1] = 'C';
    out[2] = hex[4]; // nibbles 4-7 of the 8-char string = lower 16 bits
    out[3] = hex[5];
    out[4] = hex[6];
    out[5] = hex[7];
    out[6] = '.';
    out[7] = 'D';
    out[8] = 'A';
    out[9] = 'T';
    out[10] = '\0';
}

// ----------------------------------------------------------------
//  DIRTABLE.DAT format (binary, no alignment padding needed now):
//
//  [uint32_t dynamic_dir_count]
//  for each dynamic dir:
//    [uint16_t path_len] [path_len bytes of path string]
//
//  FTABLE.DAT format:
//  [uint32_t dynamic_file_count]
//  for each dynamic file:
//    [uint16_t name_len] [name]
//    [uint16_t dir_len]  [dir]
//    [uint32_t content_size]
//    (content is stored in a separate FCxxxxxx.DAT file)
// ----------------------------------------------------------------

// Maximum size for DIRTABLE / FTABLE – well within FAT32's range.
#define META_BUF_SIZE (64u * 1024u)  /* 64 KiB */
static uint8_t meta_buf[META_BUF_SIZE];

static void auto_save_files64(void) {
    // ---- Save directory table ----
    uint8_t* ptr = meta_buf;
    uint8_t* end = meta_buf + META_BUF_SIZE;

    uint32_t dyn_dirs = 0;
    for (int i = 0; i < dir_count; i++)
        if (directories[i].is_dynamic) dyn_dirs++;

    *(uint32_t*)ptr = dyn_dirs; ptr += 4;

    for (int i = 0; i < dir_count && ptr + 258 < end; i++) {
        if (!directories[i].is_dynamic) continue;
        uint16_t plen = (uint16_t)str_len(directories[i].path);
        *(uint16_t*)ptr = plen; ptr += 2;
        for (uint16_t j = 0; j < plen; j++) *ptr++ = (uint8_t)directories[i].path[j];
    }

    uint32_t dir_bytes = (uint32_t)(ptr - meta_buf);
    fat32_create_file(FAT32_DIRTABLE);           /* ignored if exists */
    fat32_write_file(FAT32_DIRTABLE, meta_buf, dir_bytes);

    // ---- Save file metadata table ----
    ptr = meta_buf;

    uint32_t dyn_files = 0;
    for (int i = 0; i < file_count64; i++)
        if (all_files64[i].is_dynamic) dyn_files++;

    *(uint32_t*)ptr = dyn_files; ptr += 4;

    for (int i = 0; i < file_count64 && ptr + 512 < end; i++) {
        if (!all_files64[i].is_dynamic) continue;

        uint16_t nlen = (uint16_t)str_len(all_files64[i].name);
        *(uint16_t*)ptr = nlen; ptr += 2;
        for (uint16_t j = 0; j < nlen; j++) *ptr++ = (uint8_t)all_files64[i].name[j];

        uint16_t dlen = (uint16_t)str_len(all_files64[i].directory);
        *(uint16_t*)ptr = dlen; ptr += 2;
        for (uint16_t j = 0; j < dlen; j++) *ptr++ = (uint8_t)all_files64[i].directory[j];

        *(uint32_t*)ptr = all_files64[i].size; ptr += 4;

        // ---- Save file content to its own FAT32 file ----
        char fname[13];
        make_content_fname(i, fname);
        fat32_create_file(fname);
        if (all_files64[i].size > 0) {
            fat32_write_file(fname,
                             (const uint8_t*)all_files64[i].content,
                             all_files64[i].size);
        }
    }

    uint32_t ft_bytes = (uint32_t)(ptr - meta_buf);
    fat32_create_file(FAT32_FTABLE);
    fat32_write_file(FAT32_FTABLE, meta_buf, ft_bytes);
}

// ================================================================
//  init_filesystem64()
// ================================================================
void init_filesystem64(void) {
    // One-time setup of static arrays
    static int initialised = 0;
    if (!initialised) {
        for (int i = 0; i < MAX_FILES; i++) {
            dynamic_names64[i][0]   = '\0';
            dynamic_dirs64[i][0]    = '\0';
            dynamic_content_ptr[i]  = NULL;
        }
        for (int i = 0; i < MAX_DIRS; i++) {
            directories = (Directory64*)((void*)0); /* placeholder */
        }
        initialised = 1;
    }

    // Directories live in a static array (no heap needed for metadata)
    static Directory64 dir_storage[MAX_DIRS];
    directories = dir_storage;
    file_count64 = 0;
    dir_count    = 0;
    pool_offset  = 0;

    // Root entry
    str_cpy(directories[dir_count].path, "/");
    directories[dir_count].is_dynamic   = 0;
    directories[dir_count].is_system    = 1;
    dir_count++;

    init_unix_tree();

    // Copy static files into all_files64
    for (int i = 0; static_files64[i].name != NULL; i++) {
        all_files64[file_count64++] = static_files64[i];
    }

    // ---- Try to mount FAT32 ----
    if (!fat32_mount()) {
        // First boot: format and return (no saved data yet)
        fat32_format();
        return;
    }

    // ---- Load directory table ----
    int dir_bytes = fat32_read_file(FAT32_DIRTABLE, meta_buf, META_BUF_SIZE);
    if (dir_bytes > 4) {
        uint8_t* ptr = meta_buf;
        uint32_t saved_dirs = *(uint32_t*)ptr; ptr += 4;
        for (uint32_t i = 0; i < saved_dirs && dir_count < MAX_DIRS; i++) {
            uint16_t plen = *(uint16_t*)ptr; ptr += 2;
            if (plen == 0 || plen >= MAX_PATH_LENGTH) break;
            for (uint16_t j = 0; j < plen; j++)
                directories[dir_count].path[j] = (char)*ptr++;
            directories[dir_count].path[plen] = '\0';
            directories[dir_count].is_dynamic  = 1;
            directories[dir_count].is_system   = 0;
            dir_count++;
        }
    }

    // ---- Load file metadata table ----
    int ft_bytes = fat32_read_file(FAT32_FTABLE, meta_buf, META_BUF_SIZE);
    if (ft_bytes <= 4) return;

    uint8_t* ptr = meta_buf;
    uint32_t saved_files = *(uint32_t*)ptr; ptr += 4;

    for (uint32_t i = 0; i < saved_files && file_count64 < MAX_FILES; i++) {
        int idx = file_count64;

        uint16_t nlen = *(uint16_t*)ptr; ptr += 2;
        if (nlen == 0 || nlen >= 64) break;
        for (uint16_t j = 0; j < nlen; j++) dynamic_names64[idx][j] = (char)*ptr++;
        dynamic_names64[idx][nlen] = '\0';

        uint16_t dlen = *(uint16_t*)ptr; ptr += 2;
        if (dlen >= MAX_PATH_LENGTH) break;
        for (uint16_t j = 0; j < dlen; j++) dynamic_dirs64[idx][j] = (char)*ptr++;
        dynamic_dirs64[idx][dlen] = '\0';

        uint32_t content_size = *(uint32_t*)ptr; ptr += 4;

        // Allocate pool space for this file's content (+1 for NUL)
        uint32_t alloc_size = content_size + 1;
        if (alloc_size > MAX_FILE_SIZE + 1) alloc_size = MAX_FILE_SIZE + 1;

        char* cbuf = pool_alloc(alloc_size);
        if (!cbuf) break;  /* pool exhausted */
        cbuf[0] = '\0';

        // Load content from its FAT32 file
        if (content_size > 0) {
            char fname[13];
            make_content_fname(idx, fname);
            int read = fat32_read_file(fname, (uint8_t*)cbuf,
                                       alloc_size - 1);
            if (read > 0) {
                cbuf[read] = '\0';
                content_size = (uint32_t)read;
            } else {
                content_size = 0;
            }
        }

        dynamic_content_ptr[idx] = cbuf;

        all_files64[idx].name      = dynamic_names64[idx];
        all_files64[idx].content   = cbuf;
        all_files64[idx].size      = content_size;
        all_files64[idx].is_dynamic= 1;
        all_files64[idx].directory = dynamic_dirs64[idx];

        file_count64++;
    }
}

void save_files_to_disk64(void) {
    auto_save_files64();
}

// ================================================================
//  File operations
// ================================================================
const EmbeddedFile64* fs_get_file64(const char* filename) {
    char full_path[MAX_PATH_LENGTH];
    normalize_path(filename, full_path);

    for (int i = 0; i < file_count64; i++) {
        char fp[MAX_PATH_LENGTH];
        str_cpy(fp, all_files64[i].directory);
        if (fp[str_len(fp) - 1] != '/') str_concat(fp, "/");
        str_concat(fp, all_files64[i].name);
        if (str_cmp(fp, full_path) == 0) return &all_files64[i];
    }

    for (int i = 0; i < file_count64; i++) {
        if (str_cmp(all_files64[i].directory, current_dir) == 0 &&
            str_cmp(all_files64[i].name, filename) == 0)
            return &all_files64[i];
    }
    return NULL;
}

int fs_touch_file64(const char* filename) {
    if (str_len(filename) == 0 || str_len(filename) >= 64) return 0;
    if (fs_get_file64(filename) != NULL) return 0;

    int idx = file_count64;
    if (idx >= MAX_FILES) return 0;

    // Allocate a minimal pool slot (just the NUL byte)
    char* cbuf = pool_alloc(1);
    if (!cbuf) return 0;
    cbuf[0] = '\0';

    str_cpy(dynamic_names64[idx], filename);
    str_cpy(dynamic_dirs64[idx],  current_dir);
    dynamic_content_ptr[idx] = cbuf;

    all_files64[idx].name      = dynamic_names64[idx];
    all_files64[idx].content   = cbuf;
    all_files64[idx].size      = 0;
    all_files64[idx].is_dynamic= 1;
    all_files64[idx].directory = dynamic_dirs64[idx];

    file_count64++;
    auto_save_files64();
    return 1;
}

int fs_write_file64(const char* name, const char* content) {
    if (str_len(name) == 0 || str_len(content) == 0) return 0;

    EmbeddedFile64* file = (EmbeddedFile64*)fs_get_file64(name);
    if (file == NULL || !file->is_dynamic) return 0;

    int idx = (int)(file - all_files64);
    uint32_t content_len = (uint32_t)str_len(content);

    // Cap at MAX_FILE_SIZE
    if (content_len > MAX_FILE_SIZE) content_len = MAX_FILE_SIZE;

    // If the existing pool slot is large enough, reuse it.
    // Otherwise allocate a new slot (the old one is wasted but that
    // is acceptable for a simple kernel demo).
    char* cbuf = dynamic_content_ptr[idx];

    // We don't track individual slot sizes, so always reallocate
    // to be safe.  Previous content is simply overwritten.
    cbuf = pool_alloc(content_len + 1);
    if (!cbuf) return 0;

    for (uint32_t j = 0; j < content_len; j++) cbuf[j] = content[j];
    cbuf[content_len] = '\0';

    dynamic_content_ptr[idx] = cbuf;
    file->content = cbuf;
    file->size    = content_len;

    auto_save_files64();
    return 1;
}

int fs_delete_file64(const char* name) {
    if (str_len(name) == 0) return 0;

    int file_index = -1;
    for (int i = 0; i < file_count64; i++) {
        if (str_cmp(all_files64[i].directory, current_dir) == 0 &&
            str_cmp(all_files64[i].name, name) == 0) {
            file_index = i;
            break;
        }
    }

    if (file_index == -1 || !all_files64[file_index].is_dynamic) return 0;

    // Remove the FAT32 content file
    char fname[13];
    make_content_fname(file_index, fname);
    fat32_delete_file(fname);

    // Shift entries down
    for (int i = file_index; i < file_count64 - 1; i++) {
        all_files64[i]           = all_files64[i + 1];
        dynamic_content_ptr[i]   = dynamic_content_ptr[i + 1];
        // Also fix name / dir pointers (they already sit in static arrays)
    }
    file_count64--;
    auto_save_files64();
    return 1;
}

// ================================================================
//  Directory listing
// ================================================================
int fs_list_files64(void* output_ptr) {
    CommandOutput* output = (CommandOutput*)output_ptr;

    if (file_count64 == 0 && dir_count == 0) {
        output_add_line(output, "Filesystem not initialized!", VGA_RED);
        return 0;
    }

    char header[MAX_LINE_LENGTH];
    str_cpy(header, "Contents of ");
    str_concat(header, current_dir);
    str_concat(header, ":");
    output_add_line(output, header, VGA_CYAN);
    output_add_empty_line(output);

    int found_dirs = 0;
    for (int i = 0; i < dir_count; i++) {
        char parent[MAX_PATH_LENGTH];
        get_parent_dir(directories[i].path, parent);
        if (str_cmp(parent, current_dir) != 0) continue;

        char line[MAX_LINE_LENGTH];
        str_cpy(line, "  [DIR]  ");
        char dir_name[MAX_PATH_LENGTH];
        get_dir_name(directories[i].path, dir_name);
        str_concat(line, dir_name);
        if (directories[i].is_system) str_concat(line, " (system)");
        output_add_line(output, line, VGA_CYAN);
        found_dirs++;
    }

    int found_files = 0;
    for (int i = 0; i < file_count64; i++) {
        if (str_cmp(all_files64[i].directory, current_dir) != 0) continue;

        char line[MAX_LINE_LENGTH];
        char size_str[32];
        str_cpy(line, "  ");
        str_concat(line, all_files64[i].name);
        str_concat(line, " (");
        uint64_to_string(all_files64[i].size, size_str);
        str_concat(line, size_str);
        str_concat(line, " bytes)");
        uint8_t color = all_files64[i].is_dynamic ? VGA_YELLOW : VGA_WHITE;
        output_add_line(output, line, color);
        found_files++;
    }

    if (found_dirs == 0 && found_files == 0)
        output_add_line(output, "  (empty)", VGA_DARK_GRAY);

    output_add_empty_line(output);

    char summary[MAX_LINE_LENGTH];
    char dirs_str[16], files_str[16];
    uint64_to_string(found_dirs,  dirs_str);
    uint64_to_string(found_files, files_str);
    str_cpy(summary, dirs_str);
    str_concat(summary, " directories, ");
    str_concat(summary, files_str);
    str_concat(summary, " files");
    output_add_line(output, summary, VGA_DARK_GRAY);
    return 1;
}

// ================================================================
//  Directory operations  (unchanged from original)
// ================================================================
int fs_mkdir64(const char* dirname) {
    if (str_len(dirname) == 0 || str_len(dirname) >= MAX_PATH_LENGTH) return 0;
    if (dir_count >= MAX_DIRS) return 0;

    char full_path[MAX_PATH_LENGTH];
    normalize_path(dirname, full_path);
    if (dir_exists(full_path)) return 0;

    char parent[MAX_PATH_LENGTH];
    get_parent_dir(full_path, parent);
    if (!dir_exists(parent)) return 0;

    str_cpy(directories[dir_count].path, full_path);
    directories[dir_count].is_dynamic   = 1;
    directories[dir_count].is_system    = 0;
    directories[dir_count].permissions  = 0755;
    directories[dir_count].created_time = 0;
    dir_count++;

    auto_save_files64();
    return 1;
}

int fs_rmdir64(const char* dirname) {
    if (str_len(dirname) == 0) return 0;

    char full_path[MAX_PATH_LENGTH];
    normalize_path(dirname, full_path);
    if (str_cmp(full_path, "/") == 0) return 0;

    int dir_index = -1;
    for (int i = 0; i < dir_count; i++) {
        if (str_cmp(directories[i].path, full_path) == 0) {
            dir_index = i; break;
        }
    }
    if (dir_index == -1)                      return 0;
    if (directories[dir_index].is_system)     return 0;
    if (!directories[dir_index].is_dynamic)   return 0;

    for (int i = 0; i < file_count64; i++)
        if (str_cmp(all_files64[i].directory, full_path) == 0) return 0;

    for (int i = 0; i < dir_count; i++) {
        if (i != dir_index && str_starts_with(directories[i].path, full_path)) {
            int len = str_len(full_path);
            if (directories[i].path[len] == '/') return 0;
        }
    }

    for (int i = dir_index; i < dir_count - 1; i++)
        directories[i] = directories[i + 1];
    dir_count--;

    auto_save_files64();
    return 1;
}

int fs_rmdir_recursive64(const char* dirname) {
    if (str_len(dirname) == 0) return 0;

    char full_path[MAX_PATH_LENGTH];
    normalize_path(dirname, full_path);
    if (str_cmp(full_path, "/") == 0) return 0;

    int dir_index = -1;
    for (int i = 0; i < dir_count; i++) {
        if (str_cmp(directories[i].path, full_path) == 0) {
            dir_index = i; break;
        }
    }
    if (dir_index == -1) return 0;
    if (directories[dir_index].is_system) return 0;

    // Remove all files under this subtree
    for (int i = file_count64 - 1; i >= 0; i--) {
        if (str_starts_with(all_files64[i].directory, full_path) &&
            all_files64[i].is_dynamic) {
            char fname[13];
            make_content_fname(i, fname);
            fat32_delete_file(fname);
            for (int j = i; j < file_count64 - 1; j++) {
                all_files64[j]          = all_files64[j + 1];
                dynamic_content_ptr[j]  = dynamic_content_ptr[j + 1];
            }
            file_count64--;
        }
    }

    // Remove all sub-dirs
    for (int i = dir_count - 1; i >= 0; i--) {
        if (str_starts_with(directories[i].path, full_path) &&
            str_cmp(directories[i].path, full_path) != 0 &&
            directories[i].is_dynamic && !directories[i].is_system) {
            for (int j = i; j < dir_count - 1; j++) directories[j] = directories[j + 1];
            dir_count--;
        }
    }

    // Remove the directory itself
    for (int i = 0; i < dir_count; i++) {
        if (str_cmp(directories[i].path, full_path) == 0 &&
            directories[i].is_dynamic && !directories[i].is_system) {
            for (int j = i; j < dir_count - 1; j++) directories[j] = directories[j + 1];
            dir_count--;
            break;
        }
    }

    auto_save_files64();
    return 1;
}

int fs_chdir64(const char* dirname) {
    if (str_len(dirname) == 0) return 0;
    if (str_cmp(dirname, ".") == 0) return 1;

    if (str_cmp(dirname, "..") == 0) {
        if (str_cmp(current_dir, "/") == 0) return 1;
        char parent[MAX_PATH_LENGTH];
        get_parent_dir(current_dir, parent);
        str_cpy(current_dir, parent);
        return 1;
    }

    char full_path[MAX_PATH_LENGTH];
    normalize_path(dirname, full_path);
    if (!dir_exists(full_path)) return 0;
    str_cpy(current_dir, full_path);
    return 1;
}

const char* fs_getcwd64(void) { return current_dir; }

const EmbeddedFile64* get_all_files_list64(int* count) {
    *count = file_count64;
    return all_files64;
}

// ================================================================
//  Advanced operations  (tree, find, du) – unchanged from original
// ================================================================
int fs_count_subdirs(const char* path) {
    int count = 0;
    for (int i = 0; i < dir_count; i++) {
        if (str_starts_with(directories[i].path, path) &&
            str_cmp(directories[i].path, path) != 0)
            count++;
    }
    return count;
}

int fs_count_files_in_tree(const char* path) {
    int count = 0;
    for (int i = 0; i < file_count64; i++)
        if (str_starts_with(all_files64[i].directory, path)) count++;
    return count;
}

static void draw_tree_recursive(void* output_ptr, const char* path,
                                int depth, char* prefix) {
    CommandOutput* output = (CommandOutput*)output_ptr;
    if (depth > 10) return;

    for (int i = 0; i < dir_count; i++) {
        char parent[MAX_PATH_LENGTH];
        get_parent_dir(directories[i].path, parent);
        if (str_cmp(parent, path) != 0) continue;

        char line[MAX_LINE_LENGTH];
        str_cpy(line, prefix);
        str_concat(line, "\xe2\x94\x9c\xe2\x94\x80\xe2\x94\x80 "); /* ├── */
        char dir_name[MAX_PATH_LENGTH];
        get_dir_name(directories[i].path, dir_name);
        str_concat(line, dir_name);
        str_concat(line, "/");
        output_add_line(output, line, VGA_CYAN);

        char new_prefix[MAX_LINE_LENGTH];
        str_cpy(new_prefix, prefix);
        str_concat(new_prefix, "\xe2\x94\x82   "); /* │   */
        draw_tree_recursive(output_ptr, directories[i].path,
                            depth + 1, new_prefix);
    }

    for (int i = 0; i < file_count64; i++) {
        if (str_cmp(all_files64[i].directory, path) != 0) continue;
        char line[MAX_LINE_LENGTH];
        str_cpy(line, prefix);
        str_concat(line, "\xe2\x94\x9c\xe2\x94\x80\xe2\x94\x80 ");
        str_concat(line, all_files64[i].name);
        uint8_t color = all_files64[i].is_dynamic ? VGA_YELLOW : VGA_WHITE;
        output_add_line(output, line, color);
    }
}

int fs_tree64(void* output_ptr) {
    CommandOutput* output = (CommandOutput*)output_ptr;
    output_add_line(output, "Directory Tree:", VGA_CYAN);
    output_add_line(output, "/", VGA_GREEN);

    char prefix[MAX_LINE_LENGTH] = "";
    draw_tree_recursive(output_ptr, "/", 0, prefix);
    output_add_empty_line(output);

    char stats[MAX_LINE_LENGTH];
    char dir_str[16], file_str[16];
    uint64_to_string(dir_count - 1, dir_str);
    uint64_to_string(file_count64, file_str);
    str_cpy(stats, dir_str);
    str_concat(stats, " directories, ");
    str_concat(stats, file_str);
    str_concat(stats, " files");
    output_add_line(output, stats, VGA_DARK_GRAY);
    return 1;
}

int fs_find64(const char* pattern, void* output_ptr) {
    CommandOutput* output = (CommandOutput*)output_ptr;
    char header[MAX_LINE_LENGTH];
    str_cpy(header, "Finding: ");
    str_concat(header, pattern);
    output_add_line(output, header, VGA_CYAN);
    output_add_empty_line(output);

    int found = 0;
    for (int i = 0; i < file_count64; i++) {
        if (!str_contains(all_files64[i].name, pattern)) continue;
        char line[MAX_LINE_LENGTH];
        str_cpy(line, all_files64[i].directory);
        if (line[str_len(line) - 1] != '/') str_concat(line, "/");
        str_concat(line, all_files64[i].name);
        uint8_t color = all_files64[i].is_dynamic ? VGA_YELLOW : VGA_WHITE;
        output_add_line(output, line, color);
        found++;
    }

    if (found == 0) {
        output_add_line(output, "No matches found", VGA_DARK_GRAY);
    } else {
        char summary[MAX_LINE_LENGTH];
        char count_str[16];
        uint64_to_string(found, count_str);
        str_cpy(summary, count_str);
        str_concat(summary, " matches found");
        output_add_empty_line(output);
        output_add_line(output, summary, VGA_DARK_GRAY);
    }
    return 1;
}

int fs_du64(const char* path, void* output_ptr) {
    CommandOutput* output = (CommandOutput*)output_ptr;

    char full_path[MAX_PATH_LENGTH];
    if (path == NULL || str_len(path) == 0)
        str_cpy(full_path, current_dir);
    else
        normalize_path(path, full_path);

    if (!dir_exists(full_path)) {
        output_add_line(output, "Directory not found", VGA_RED);
        return 0;
    }

    char header[MAX_LINE_LENGTH];
    str_cpy(header, "Disk usage for: ");
    str_concat(header, full_path);
    output_add_line(output, header, VGA_CYAN);
    output_add_empty_line(output);

    uint64_t total_size = 0;
    int file_cnt = 0;
    for (int i = 0; i < file_count64; i++) {
        if (str_starts_with(all_files64[i].directory, full_path)) {
            total_size += all_files64[i].size;
            file_cnt++;
        }
    }

    char line[MAX_LINE_LENGTH];
    char size_str[32];
    uint64_to_string(total_size, size_str);
    str_cpy(line, "Total size: ");
    str_concat(line, size_str);
    str_concat(line, " bytes");
    output_add_line(output, line, VGA_YELLOW);

    uint64_to_string(file_cnt, size_str);
    str_cpy(line, "File count: ");
    str_concat(line, size_str);
    output_add_line(output, line, VGA_WHITE);

    int dc = fs_count_subdirs(full_path);
    uint64_to_string(dc, size_str);
    str_cpy(line, "Directories: ");
    str_concat(line, size_str);
    output_add_line(output, line, VGA_WHITE);
    return 1;
}