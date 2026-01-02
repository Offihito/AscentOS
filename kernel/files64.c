#include "files64.h"
#include "commands64.h"
#include "disk64.h"
#include <stddef.h>

#ifndef MAX_FILES
#define MAX_FILES 32
#endif

#ifndef MAX_DIRS
#define MAX_DIRS 16
#endif

#define PERSISTENCE_START_LBA 100
#define MAX_PERSISTENCE_SECTORS 20

// Current working directory
static char current_dir[MAX_PATH_LENGTH] = "/";

// Directory list
static Directory64 directories[MAX_DIRS];
static int dir_count = 0;

// Static files
const char file_motd64[] = 
"JonklerOS 64-bit v0.1\n"
"Why So Serious?\n"
"\n"
"Welcome to 64-bit chaos!\n"
"Fuck Lalyn and Kamil\n";

const char file_secret64[] = 
"Secret message: The Jonkler was here in 64-bit mode.\n"
"He-he-he-ha-ha-ha!\n"
"Now with MORE bits!\n";

static const unsigned char file_joker_bmp[] = {

};

const char file_help64[] = 
"Available commands:\n"
"help     - this list\n"
"clear    - clear screen\n"
"reboot   - reboot\n"
"neofetch - system info\n"
"htop     - system monitor\n"
"ls       - list files and dirs\n"
"cat      - show file content\n"
"touch    - create new empty file\n"
"write    - write content to file\n"
"rm       - delete file\n"
"mkdir    - create directory\n"
"rmdir    - remove directory\n"
"cd       - change directory\n"
"pwd      - print working directory\n"
"about    - about the OS\n"
"hello    - say hello\n"
"jew      - ...\n"
"sysinfo  - detailed system info\n"
"cpuinfo  - CPU information\n"
"meminfo  - memory information\n"
"test     - run 64-bit tests\n";

static const EmbeddedFile64 static_files64[] = {
    {"motd.txt", file_motd64, sizeof(file_motd64)-1, 0, "/"},
    {"secret.txt", file_secret64, sizeof(file_secret64)-1, 0, "/"},
    {"help.txt", file_help64, sizeof(file_help64)-1, 0, "/"},
    {"joker.bmp", (const char*)file_joker_bmp, sizeof(file_joker_bmp), 0, "/"},  // YENİ SATIR
    {NULL, NULL, 0, 0, NULL}
};

static char dynamic_content64[MAX_FILES][256];
static char dynamic_names64[MAX_FILES][32];
static char dynamic_dirs64[MAX_FILES][MAX_PATH_LENGTH];
static EmbeddedFile64 all_files64[MAX_FILES];
static int file_count64 = 0;

// Helper: normalize path
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
    
    // Remove trailing slash unless it's root
    int len = str_len(output);
    if (len > 1 && output[len - 1] == '/') {
        output[len - 1] = '\0';
    }
}

// Helper: check if directory exists
static int dir_exists(const char* path) {
    if (str_cmp(path, "/") == 0) return 1;
    
    for (int i = 0; i < dir_count; i++) {
        if (str_cmp(directories[i].path, path) == 0) {
            return 1;
        }
    }
    return 0;
}

// Helper: get parent directory
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

void init_filesystem64(void) {
    file_count64 = 0;
    dir_count = 0;
    
    // Root directory always exists
    str_cpy(directories[dir_count].path, "/");
    directories[dir_count].is_dynamic = 0;
    dir_count++;

    // Add static files
    for (int i = 0; static_files64[i].name != NULL; i++) {
        all_files64[file_count64] = static_files64[i];
        file_count64++;
    }

    // Load from disk
    uint8_t buffer[512 * MAX_PERSISTENCE_SECTORS];
    if (!disk_read_sector64(PERSISTENCE_START_LBA, buffer)) {
        return;
    }

    uint8_t* ptr = buffer;
    
    // Read directory count
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
            dir_count++;
            
            while ((uint64_t)ptr % 4 != 0) ptr++;
        }
    }
    
    // Read file count
    uint32_t dynamic_count = *((uint32_t*)ptr);
    ptr += 4;

    if (dynamic_count == 0 || dynamic_count > MAX_FILES - file_count64) {
        return;
    }

    for (uint32_t i = 0; i < dynamic_count; i++) {
        uint8_t name_len = *ptr++;
        if (name_len == 0 || name_len >= 32) continue;

        for (uint8_t j = 0; j < name_len; j++) {
            dynamic_names64[file_count64][j] = *ptr++;
        }
        dynamic_names64[file_count64][name_len] = '\0';

        while ((uint64_t)ptr % 4 != 0) ptr++;
        
        // Read directory path
        uint8_t dir_len = *ptr++;
        if (dir_len >= MAX_PATH_LENGTH) dir_len = MAX_PATH_LENGTH - 1;
        
        for (uint8_t j = 0; j < dir_len; j++) {
            dynamic_dirs64[file_count64][j] = *ptr++;
        }
        dynamic_dirs64[file_count64][dir_len] = '\0';
        
        while ((uint64_t)ptr % 4 != 0) ptr++;

        uint32_t content_size = *((uint32_t*)ptr);
        ptr += 4;

        if (content_size >= 256) content_size = 255;

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

void auto_save_files64(void) {
    uint8_t buffer[512 * MAX_PERSISTENCE_SECTORS];
    memset64(buffer, 0, sizeof(buffer));

    uint8_t* ptr = buffer;

    // Write directory count (exclude root)
    uint32_t dynamic_dir_count = 0;
    for (int i = 0; i < dir_count; i++) {
        if (directories[i].is_dynamic) dynamic_dir_count++;
    }
    
    *((uint32_t*)ptr) = dynamic_dir_count;
    ptr += 4;
    
    // Write directories
    for (int i = 0; i < dir_count; i++) {
        if (!directories[i].is_dynamic) continue;
        
        uint8_t path_len = (uint8_t)str_len(directories[i].path);
        *ptr++ = path_len;
        for (uint8_t j = 0; j < path_len; j++) {
            *ptr++ = directories[i].path[j];
        }
        
        while ((uint64_t)ptr % 4 != 0) *ptr++ = 0;
    }

    // Write file count
    uint32_t dynamic_count = 0;
    for (int i = 0; i < file_count64; i++) {
        if (all_files64[i].is_dynamic) dynamic_count++;
    }

    *((uint32_t*)ptr) = dynamic_count;
    ptr += 4;

    // Write files
    for (int i = 0; i < file_count64; i++) {
        if (!all_files64[i].is_dynamic) continue;

        uint8_t name_len = (uint8_t)str_len(all_files64[i].name);
        *ptr++ = name_len;
        for (uint8_t j = 0; j < name_len; j++) {
            *ptr++ = all_files64[i].name[j];
        }

        while ((uint64_t)ptr % 4 != 0) *ptr++ = 0;
        
        // Write directory path
        uint8_t dir_len = (uint8_t)str_len(all_files64[i].directory);
        *ptr++ = dir_len;
        for (uint8_t j = 0; j < dir_len; j++) {
            *ptr++ = all_files64[i].directory[j];
        }
        
        while ((uint64_t)ptr % 4 != 0) *ptr++ = 0;

        uint32_t content_size = all_files64[i].size;
        *((uint32_t*)ptr) = content_size;
        ptr += 4;

        for (uint32_t j = 0; j < content_size; j++) {
            *ptr++ = all_files64[i].content[j];
        }
        if (content_size > 0) *ptr++ = '\0';

        while ((uint64_t)ptr % 4 != 0) *ptr++ = 0;
    }

    // Write all sectors
    for (int sec = 0; sec < MAX_PERSISTENCE_SECTORS; sec++) {
        disk_write_sector64(PERSISTENCE_START_LBA + sec, buffer + sec * 512);
    }
}

void save_files_to_disk64(void) {
    auto_save_files64();
}

// =============== File Operations ===============

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
    
    // Try in current directory
    for (int i = 0; i < file_count64; i++) {
        if (str_cmp(all_files64[i].directory, current_dir) == 0 &&
            str_cmp(all_files64[i].name, filename) == 0) {
            return &all_files64[i];
        }
    }
    
    return NULL;
}

int fs_touch_file64(const char* filename) {
    if (str_len(filename) == 0 || str_len(filename) >= 32) return 0;

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
    if (content_len >= 256) content_len = 255;

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

    // List directories first
    int found_dirs = 0;
    for (int i = 0; i < dir_count; i++) {
        char parent[MAX_PATH_LENGTH];
        get_parent_dir(directories[i].path, parent);
        
        if (str_cmp(parent, current_dir) == 0) {
            char line[MAX_LINE_LENGTH];
            str_cpy(line, "  [DIR]  ");
            
            // Get just the directory name
            const char* dir_name = directories[i].path;
            int last_slash = -1;
            for (int j = 0; dir_name[j]; j++) {
                if (dir_name[j] == '/') last_slash = j;
            }
            if (last_slash >= 0) dir_name = &directories[i].path[last_slash + 1];
            
            str_concat(line, dir_name);
            output_add_line(output, line, VGA_CYAN);
            found_dirs++;
        }
    }

    // List files
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
    output_add_line(output, "Commands: cd <dir> | mkdir <dir> | rmdir <dir> | touch/rm <file>", VGA_DARK_GRAY);

    return 1;
}

// =============== Directory Operations ===============

int fs_mkdir64(const char* dirname) {
    if (str_len(dirname) == 0 || str_len(dirname) >= MAX_PATH_LENGTH) return 0;
    
    if (dir_count >= MAX_DIRS) return 0;
    
    char full_path[MAX_PATH_LENGTH];
    normalize_path(dirname, full_path);
    
    if (dir_exists(full_path)) return 0;
    
    str_cpy(directories[dir_count].path, full_path);
    directories[dir_count].is_dynamic = 1;
    dir_count++;
    
    auto_save_files64();
    return 1;
}

int fs_rmdir64(const char* dirname) {
    if (str_len(dirname) == 0) return 0;
    
    char full_path[MAX_PATH_LENGTH];
    normalize_path(dirname, full_path);
    
    if (str_cmp(full_path, "/") == 0) return 0;  // Can't remove root
    
    int dir_index = -1;
    for (int i = 0; i < dir_count; i++) {
        if (str_cmp(directories[i].path, full_path) == 0) {
            dir_index = i;
            break;
        }
    }
    
    if (dir_index == -1 || !directories[dir_index].is_dynamic) return 0;
    
    // Check if directory has files
    for (int i = 0; i < file_count64; i++) {
        if (str_cmp(all_files64[i].directory, full_path) == 0) {
            return 0;  // Directory not empty
        }
    }
    
    // Remove directory
    for (int i = dir_index; i < dir_count - 1; i++) {
        directories[i] = directories[i + 1];
    }
    dir_count--;
    
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