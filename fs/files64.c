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

#define PERSISTENCE_START_LBA 100
#define MAX_PERSISTENCE_SECTORS 400  // 400 sectors = ~200KB for deep trees

// Current working directory
static char current_dir[MAX_PATH_LENGTH] = "/";

// Directory list
static Directory64* directories = NULL;
static int dir_count = 0;

// ============ SYSTEM FILES - Unix-like default content ============

const char file_motd64[] = 
"AscentOS 64-bit v1.2 - Unix-like Edition\n"
"Advanced Multi-Level Directory Tree System\n"
"\n"
"Welcome to 64-bit chaos with Unix structure!\n"
"Type 'help' for available commands\n"
"Type 'tree' to see directory structure\n";

const char file_bashrc[] =
"# AscentOS Bash Configuration\n"
"export PATH=/bin:/usr/bin\n"
"export HOME=/home\n"
"alias ll='ls -la'\n"
"alias ..='cd ..'\n";

const char file_profile[] =
"# System-wide profile\n"
"PATH=/bin:/usr/bin:/usr/local/bin\n"
"export PATH\n";

const char file_hostname[] = "ascentos\n";

const char file_hosts[] =
"127.0.0.1   localhost\n"
"127.0.1.1   ascentos\n"
"::1         localhost ip6-localhost\n";

const char file_fstab[] =
"# <file system>  <mount point>  <type>  <options>  <dump>  <pass>\n"
"/dev/sda1        /              ext4    defaults   0       1\n"
"/dev/sda2        /home          ext4    defaults   0       2\n";

const char file_passwd[] =
"root:x:0:0:root:/root:/bin/bash\n"
"user:x:1000:1000:User:/home/user:/bin/bash\n";

const char file_readme[] =
"AscentOS File System\n"
"====================\n"
"\n"
"This is a Unix-like file system with multi-level directory support.\n"
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

const char file_version[] = "AscentOS 1.2 (64-bit)\n";

const char file_null[] = "";
const char file_zero[] = "";
const char file_random[] = "Random device simulation\n";

static const EmbeddedFile64 static_files64[] = {
    {"motd", file_motd64, sizeof(file_motd64)-1, 0, "/etc"},
    {"hostname", file_hostname, sizeof(file_hostname)-1, 0, "/etc"},
    {"hosts", file_hosts, sizeof(file_hosts)-1, 0, "/etc"},
    {"fstab", file_fstab, sizeof(file_fstab)-1, 0, "/etc"},
    {"passwd", file_passwd, sizeof(file_passwd)-1, 0, "/etc"},
    {"bashrc", file_bashrc, sizeof(file_bashrc)-1, 0, "/etc"},
    {"profile", file_profile, sizeof(file_profile)-1, 0, "/etc"},
    {"README.txt", file_readme, sizeof(file_readme)-1, 0, "/"},
    {"version", file_version, sizeof(file_version)-1, 0, "/etc"},
    {"null", file_null, 0, 0, "/dev"},
    {"zero", file_zero, 0, 0, "/dev"},
    {"random", file_random, sizeof(file_random)-1, 0, "/dev"},
    {NULL, NULL, 0, 0, NULL}
};

// Dinamik bellek alanları
static char (*dynamic_content64)[512] = NULL;
static char (*dynamic_names64)[64] = NULL;
static char (*dynamic_dirs64)[MAX_PATH_LENGTH] = NULL;
static EmbeddedFile64* all_files64 = NULL;
static int file_count64 = 0;


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

// ============ DIRECTORY HELPERS ============
static void normalize_path(const char* input, char* output) {
    if (input[0] == '/') {
        str_cpy(output, input);
    } else {
        str_cpy(output, current_dir);
        if (output[str_len(output) - 1] != '/') {
            str_concat(output, "/");
        }
        str_concat(output, input);
    }
    
    int len = str_len(output);
    if (len > 1 && output[len - 1] == '/') {
        output[len - 1] = '\0';
    }
}

static int dir_exists(const char* path) {
    if (str_cmp(path, "/") == 0) return 1;
    
    for (int i = 0; i < dir_count; i++) {
        if (str_cmp(directories[i].path, path) == 0) {
            return 1;
        }
    }
    return 0;
}

static void get_parent_dir(const char* path, char* parent) {
    if (str_cmp(path, "/") == 0) {
        str_cpy(parent, "/");
        return;
    }
    
    int last_slash = -1;
    for (int i = 0; path[i]; i++) {
        if (path[i] == '/') last_slash = i;
    }
    
    if (last_slash <= 0) {
        str_cpy(parent, "/");
    } else {
        for (int i = 0; i < last_slash; i++) {
            parent[i] = path[i];
        }
        parent[last_slash] = '\0';
    }
}

static void get_dir_name(const char* path, char* name) {
    int last_slash = -1;
    for (int i = 0; path[i]; i++) {
        if (path[i] == '/') last_slash = i;
    }
    
    if (last_slash < 0) {
        str_cpy(name, path);
    } else {
        str_cpy(name, &path[last_slash + 1]);
    }
}

// ============ SYSTEM DIRECTORIES ============
static void create_system_dir(const char* path) {
    if (dir_count >= MAX_DIRS) return;
    
    str_cpy(directories[dir_count].path, path);
    directories[dir_count].is_dynamic = 0;
    directories[dir_count].is_system = 1;
    directories[dir_count].permissions = 0755;
    directories[dir_count].created_time = 0;
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

// ============ PERSISTENCE ============
static void auto_save_files64(void) {
    static uint8_t buffer[512 * MAX_PERSISTENCE_SECTORS];
    uint8_t* ptr = buffer;
    
    for (int i = 0; i < 512 * MAX_PERSISTENCE_SECTORS; i++) buffer[i] = 0;
    
    uint32_t dynamic_dir_count = 0;
    for (int i = 0; i < dir_count; i++) {
        if (directories[i].is_dynamic) dynamic_dir_count++;
    }
    
    *((uint32_t*)ptr) = dynamic_dir_count;
    ptr += 4;
    
    for (int i = 0; i < dir_count; i++) {
        if (!directories[i].is_dynamic) continue;
        
        uint8_t path_len = str_len(directories[i].path);
        if (path_len >= MAX_PATH_LENGTH) path_len = MAX_PATH_LENGTH - 1;
        
        *ptr++ = path_len;
        for (uint8_t j = 0; j < path_len; j++) {
            *ptr++ = directories[i].path[j];
        }
        while ((uint64_t)ptr % 4 != 0) *ptr++ = 0;
    }
    
    uint32_t dynamic_count = 0;
    for (int i = 0; i < file_count64; i++) {
        if (all_files64[i].is_dynamic) dynamic_count++;
    }
    
    *((uint32_t*)ptr) = dynamic_count;
    ptr += 4;
    
    for (int i = 0; i < file_count64; i++) {
        if (!all_files64[i].is_dynamic) continue;
        
        uint8_t name_len = str_len(all_files64[i].name);
        if (name_len >= 64) name_len = 63;
        
        *ptr++ = name_len;
        for (uint8_t j = 0; j < name_len; j++) {
            *ptr++ = all_files64[i].name[j];
        }
        while ((uint64_t)ptr % 4 != 0) *ptr++ = 0;
        
        uint8_t dir_len = str_len(all_files64[i].directory);
        if (dir_len >= MAX_PATH_LENGTH) dir_len = MAX_PATH_LENGTH - 1;
        
        *ptr++ = dir_len;
        for (uint8_t j = 0; j < dir_len; j++) {
            *ptr++ = all_files64[i].directory[j];
        }
        while ((uint64_t)ptr % 4 != 0) *ptr++ = 0;
        
        uint32_t content_size = all_files64[i].size;
        if (content_size >= 512) content_size = 511;
        
        *((uint32_t*)ptr) = content_size;
        ptr += 4;
        
        for (uint32_t j = 0; j < content_size; j++) {
            *ptr++ = all_files64[i].content[j];
        }
        if (content_size > 0) *ptr++ = '\0';
        while ((uint64_t)ptr % 4 != 0) *ptr++ = 0;
    }
    
    for (int sec = 0; sec < MAX_PERSISTENCE_SECTORS; sec++) {
        disk_write_sector64(PERSISTENCE_START_LBA + sec, buffer + sec * 512);
    }
}

// ============ INITIALIZATION ============
void init_filesystem64(void) {
    if (dynamic_content64 == NULL) {
        static char heap_buffer[2 * 1024 * 1024];
        static uint32_t heap_offset = 0;
        
        dynamic_content64 = (char(*)[512])(&heap_buffer[heap_offset]);
        heap_offset += MAX_FILES * 512;
        
        dynamic_names64 = (char(*)[64])(&heap_buffer[heap_offset]);
        heap_offset += MAX_FILES * 64;
        
        dynamic_dirs64 = (char(*)[MAX_PATH_LENGTH])(&heap_buffer[heap_offset]);
        heap_offset += MAX_FILES * MAX_PATH_LENGTH;
        
        all_files64 = (EmbeddedFile64*)(&heap_buffer[heap_offset]);
        heap_offset += MAX_FILES * sizeof(EmbeddedFile64);
        
        directories = (Directory64*)(&heap_buffer[heap_offset]);
        heap_offset += MAX_DIRS * sizeof(Directory64);
        
        for (uint32_t i = 0; i < MAX_FILES; i++) {
            for (int j = 0; j < 512; j++) dynamic_content64[i][j] = 0;
            for (int j = 0; j < 64; j++) dynamic_names64[i][j] = 0;
            for (int j = 0; j < MAX_PATH_LENGTH; j++) dynamic_dirs64[i][j] = 0;
        }
        
        for (uint32_t i = 0; i < MAX_DIRS; i++) {
            for (int j = 0; j < MAX_PATH_LENGTH; j++) directories[i].path[j] = 0;
            directories[i].is_dynamic = 0;
            directories[i].is_system = 0;
        }
    }
    
    file_count64 = 0;
    dir_count = 0;
    
    str_cpy(directories[dir_count].path, "/");
    directories[dir_count].is_dynamic = 0;
    directories[dir_count].is_system = 1;
    dir_count++;
    
    init_unix_tree();
    
    for (int i = 0; static_files64[i].name != NULL; i++) {
        all_files64[file_count64] = static_files64[i];
        file_count64++;
    }
    
    static uint8_t buffer[512 * MAX_PERSISTENCE_SECTORS];
    
    for (int sec = 0; sec < MAX_PERSISTENCE_SECTORS; sec++) {
        if (!disk_read_sector64(PERSISTENCE_START_LBA + sec, buffer + sec * 512)) {
            return;
        }
    }
    
    uint8_t* ptr = buffer;
    
    uint32_t saved_dir_count = *((uint32_t*)ptr);
    ptr += 4;
    
    if (saved_dir_count > 0 && saved_dir_count < MAX_DIRS) {
        for (uint32_t i = 0; i < saved_dir_count; i++) {
            uint8_t path_len = *ptr++;
            if (path_len == 0 || path_len >= MAX_PATH_LENGTH) continue;
            
            for (uint8_t j = 0; j < path_len; j++) {
                directories[dir_count].path[j] = *ptr++;
            }
            directories[dir_count].path[path_len] = '\0';
            directories[dir_count].is_dynamic = 1;
            directories[dir_count].is_system = 0;
            dir_count++;
            
            while ((uint64_t)ptr % 4 != 0) ptr++;
        }
    }
    
    uint32_t dynamic_count = *((uint32_t*)ptr);
    ptr += 4;
    
    if (dynamic_count == 0 || dynamic_count > MAX_FILES - file_count64) {
        return;
    }
    
    for (uint32_t i = 0; i < dynamic_count; i++) {
        uint8_t name_len = *ptr++;
        if (name_len == 0 || name_len >= 64) continue;
        
        for (uint8_t j = 0; j < name_len; j++) {
            dynamic_names64[file_count64][j] = *ptr++;
        }
        dynamic_names64[file_count64][name_len] = '\0';
        
        while ((uint64_t)ptr % 4 != 0) ptr++;
        
        uint8_t dir_len = *ptr++;
        if (dir_len >= MAX_PATH_LENGTH) dir_len = MAX_PATH_LENGTH - 1;
        
        for (uint8_t j = 0; j < dir_len; j++) {
            dynamic_dirs64[file_count64][j] = *ptr++;
        }
        dynamic_dirs64[file_count64][dir_len] = '\0';
        
        while ((uint64_t)ptr % 4 != 0) ptr++;
        
        uint32_t content_size = *((uint32_t*)ptr);
        ptr += 4;
        
        if (content_size >= 512) content_size = 511;
        
        for (uint32_t j = 0; j < content_size; j++) {
            dynamic_content64[file_count64][j] = *ptr++;
        }
        dynamic_content64[file_count64][content_size] = '\0';
        
        if (content_size > 0 && *ptr == '\0') ptr++;
        while ((uint64_t)ptr % 4 != 0) ptr++;
        
        all_files64[file_count64].name = dynamic_names64[file_count64];
        all_files64[file_count64].content = dynamic_content64[file_count64];
        all_files64[file_count64].size = content_size;
        all_files64[file_count64].is_dynamic = 1;
        all_files64[file_count64].directory = dynamic_dirs64[file_count64];
        
        file_count64++;
    }
}

void save_files_to_disk64(void) {
    auto_save_files64();
}

// ============ FILE OPERATIONS ============
const EmbeddedFile64* fs_get_file64(const char* filename) {
    char full_path[MAX_PATH_LENGTH];
    normalize_path(filename, full_path);
    
    for (int i = 0; i < file_count64; i++) {
        char file_full_path[MAX_PATH_LENGTH];
        str_cpy(file_full_path, all_files64[i].directory);
        if (file_full_path[str_len(file_full_path) - 1] != '/') {
            str_concat(file_full_path, "/");
        }
        str_concat(file_full_path, all_files64[i].name);
        
        if (str_cmp(file_full_path, full_path) == 0) {
            return &all_files64[i];
        }
    }
    
    for (int i = 0; i < file_count64; i++) {
        if (str_cmp(all_files64[i].directory, current_dir) == 0 &&
            str_cmp(all_files64[i].name, filename) == 0) {
            return &all_files64[i];
        }
    }
    
    return NULL;
}

int fs_touch_file64(const char* filename) {
    if (str_len(filename) == 0 || str_len(filename) >= 64) return 0;
    
    if (fs_get_file64(filename) != NULL) return 0;
    
    int new_index = file_count64;
    if (new_index >= MAX_FILES) return 0;
    
    str_cpy(dynamic_names64[new_index], filename);
    str_cpy(dynamic_dirs64[new_index], current_dir);
    dynamic_content64[new_index][0] = '\0';
    
    all_files64[new_index].name = dynamic_names64[new_index];
    all_files64[new_index].content = dynamic_content64[new_index];
    all_files64[new_index].size = 0;
    all_files64[new_index].is_dynamic = 1;
    all_files64[new_index].directory = dynamic_dirs64[new_index];
    
    file_count64++;
    auto_save_files64();
    return 1;
}

int fs_write_file64(const char* name, const char* content) {
    if (str_len(name) == 0 || str_len(content) == 0) return 0;
    
    EmbeddedFile64* file = (EmbeddedFile64*)fs_get_file64(name);
    if (file == NULL || !file->is_dynamic) return 0;
    
    int idx = file - all_files64;
    
    int content_len = str_len(content);
    if (content_len >= 512) content_len = 511;
    
    for (int j = 0; j < content_len; j++) {
        dynamic_content64[idx][j] = content[j];
    }
    dynamic_content64[idx][content_len] = '\0';
    
    file->size = content_len;
    auto_save_files64();
    return 1;
}

int fs_delete_file64(const char* name) {
    if (str_len(name) == 0) return 0;
    
    int file_index = -1;
    for (int i = 0; i < file_count64; i++) {
        if ((str_cmp(all_files64[i].directory, current_dir) == 0 &&
             str_cmp(all_files64[i].name, name) == 0)) {
            file_index = i;
            break;
        }
    }
    
    if (file_index == -1 || !all_files64[file_index].is_dynamic) return 0;
    
    for (int i = file_index; i < file_count64 - 1; i++) {
        all_files64[i] = all_files64[i + 1];
    }
    file_count64--;
    auto_save_files64();
    return 1;
}

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
        
        if (str_cmp(parent, current_dir) == 0) {
            char line[MAX_LINE_LENGTH];
            str_cpy(line, "  [DIR]  ");
            
            char dir_name[MAX_PATH_LENGTH];
            get_dir_name(directories[i].path, dir_name);
            str_concat(line, dir_name);
            
            if (directories[i].is_system) {
                str_concat(line, " (system)");
            }
            
            output_add_line(output, line, VGA_CYAN);
            found_dirs++;
        }
    }
    
    int found_files = 0;
    for (int i = 0; i < file_count64; i++) {
        if (str_cmp(all_files64[i].directory, current_dir) == 0) {
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
    }
    
    if (found_dirs == 0 && found_files == 0) {
        output_add_line(output, "  (empty)", VGA_DARK_GRAY);
    }
    
    output_add_empty_line(output);
    
    char summary[MAX_LINE_LENGTH];
    char dirs_str[16], files_str[16];
    uint64_to_string(found_dirs, dirs_str);
    uint64_to_string(found_files, files_str);
    
    str_cpy(summary, dirs_str);
    str_concat(summary, " directories, ");
    str_concat(summary, files_str);
    str_concat(summary, " files");
    output_add_line(output, summary, VGA_DARK_GRAY);
    
    return 1;
}

// ============ DIRECTORY OPERATIONS ============
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
    directories[dir_count].is_dynamic = 1;
    directories[dir_count].is_system = 0;
    directories[dir_count].permissions = 0755;
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
            dir_index = i;
            break;
        }
    }
    
    if (dir_index == -1) return 0;
    if (directories[dir_index].is_system) return 0;
    if (!directories[dir_index].is_dynamic) return 0;
    
    for (int i = 0; i < file_count64; i++) {
        if (str_cmp(all_files64[i].directory, full_path) == 0) {
            return 0;
        }
    }
    
    for (int i = 0; i < dir_count; i++) {
        if (i != dir_index && str_starts_with(directories[i].path, full_path)) {
            int len = str_len(full_path);
            if (directories[i].path[len] == '/') {
                return 0;
            }
        }
    }
    
    for (int i = dir_index; i < dir_count - 1; i++) {
        directories[i] = directories[i + 1];
    }
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
            dir_index = i;
            break;
        }
    }
    
    if (dir_index == -1) return 0;
    if (directories[dir_index].is_system) return 0;
    
    for (int i = file_count64 - 1; i >= 0; i--) {
        if (str_starts_with(all_files64[i].directory, full_path)) {
            if (all_files64[i].is_dynamic) {
                for (int j = i; j < file_count64 - 1; j++) {
                    all_files64[j] = all_files64[j + 1];
                }
                file_count64--;
            }
        }
    }
    
    for (int i = dir_count - 1; i >= 0; i--) {
        if (str_starts_with(directories[i].path, full_path) && 
            str_cmp(directories[i].path, full_path) != 0) {
            if (directories[i].is_dynamic && !directories[i].is_system) {
                for (int j = i; j < dir_count - 1; j++) {
                    directories[j] = directories[j + 1];
                }
                dir_count--;
            }
        }
    }
    
    for (int i = 0; i < dir_count; i++) {
        if (str_cmp(directories[i].path, full_path) == 0) {
            if (directories[i].is_dynamic && !directories[i].is_system) {
                for (int j = i; j < dir_count - 1; j++) {
                    directories[j] = directories[j + 1];
                }
                dir_count--;
                break;
            }
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

const char* fs_getcwd64(void) {
    return current_dir;
}

const EmbeddedFile64* get_all_files_list64(int* count) {
    *count = file_count64;
    return all_files64;
}

// ============ ADVANCED OPERATIONS ============
int fs_count_subdirs(const char* path) {
    int count = 0;
    for (int i = 0; i < dir_count; i++) {
        if (str_starts_with(directories[i].path, path) &&
            str_cmp(directories[i].path, path) != 0) {
            count++;
        }
    }
    return count;
}

int fs_count_files_in_tree(const char* path) {
    int count = 0;
    for (int i = 0; i < file_count64; i++) {
        if (str_starts_with(all_files64[i].directory, path)) {
            count++;
        }
    }
    return count;
}

static void draw_tree_recursive(void* output_ptr, const char* path, int depth, char* prefix) {
    CommandOutput* output = (CommandOutput*)output_ptr;
    
    if (depth > 10) return;
    
    for (int i = 0; i < dir_count; i++) {
        char parent[MAX_PATH_LENGTH];
        get_parent_dir(directories[i].path, parent);
        
        if (str_cmp(parent, path) == 0) {
            char line[MAX_LINE_LENGTH];
            str_cpy(line, prefix);
            str_concat(line, "├── ");
            
            char dir_name[MAX_PATH_LENGTH];
            get_dir_name(directories[i].path, dir_name);
            str_concat(line, dir_name);
            str_concat(line, "/");
            
            output_add_line(output, line, VGA_CYAN);
            
            char new_prefix[MAX_LINE_LENGTH];
            str_cpy(new_prefix, prefix);
            str_concat(new_prefix, "│   ");
            
            draw_tree_recursive(output_ptr, directories[i].path, depth + 1, new_prefix);
        }
    }
    
    for (int i = 0; i < file_count64; i++) {
        if (str_cmp(all_files64[i].directory, path) == 0) {
            char line[MAX_LINE_LENGTH];
            str_cpy(line, prefix);
            str_concat(line, "├── ");
            str_concat(line, all_files64[i].name);
            
            uint8_t color = all_files64[i].is_dynamic ? VGA_YELLOW : VGA_WHITE;
            output_add_line(output, line, color);
        }
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
    
    int found_count = 0;
    
    for (int i = 0; i < file_count64; i++) {
        if (str_contains(all_files64[i].name, pattern)) {
            char line[MAX_LINE_LENGTH];
            str_cpy(line, all_files64[i].directory);
            if (line[str_len(line) - 1] != '/') {
                str_concat(line, "/");
            }
            str_concat(line, all_files64[i].name);
            
            uint8_t color = all_files64[i].is_dynamic ? VGA_YELLOW : VGA_WHITE;
            output_add_line(output, line, color);
            found_count++;
        }
    }
    
    if (found_count == 0) {
        output_add_line(output, "No matches found", VGA_DARK_GRAY);
    } else {
        char summary[MAX_LINE_LENGTH];
        char count_str[16];
        uint64_to_string(found_count, count_str);
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
    if (path == NULL || str_len(path) == 0) {
        str_cpy(full_path, current_dir);
    } else {
        normalize_path(path, full_path);
    }
    
    if (!dir_exists(full_path)) {
        output_add_line(output, "Directory not found", VGA_RED);
        return 0;
    }
    
    char header[MAX_LINE_LENGTH];
    str_cpy(header, "Disk usage for: ");
    str_concat(header, full_path);
    output_add_line(output, header, VGA_CYAN);
    output_add_empty_line(output);
    
    uint32_t total_size = 0;
    int file_count = 0;
    
    for (int i = 0; i < file_count64; i++) {
        if (str_starts_with(all_files64[i].directory, full_path)) {
            total_size += all_files64[i].size;
            file_count++;
        }
    }
    
    char line[MAX_LINE_LENGTH];
    char size_str[32];
    uint64_to_string(total_size, size_str);
    
    str_cpy(line, "Total size: ");
    str_concat(line, size_str);
    str_concat(line, " bytes");
    output_add_line(output, line, VGA_YELLOW);
    
    uint64_to_string(file_count, size_str);
    str_cpy(line, "File count: ");
    str_concat(line, size_str);
    output_add_line(output, line, VGA_WHITE);
    
    int dir_count_local = fs_count_subdirs(full_path);
    uint64_to_string(dir_count_local, size_str);
    str_cpy(line, "Directories: ");
    str_concat(line, size_str);
    output_add_line(output, line, VGA_WHITE);
    
    return 1;
}