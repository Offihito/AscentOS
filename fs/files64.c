// files64.c  –  AscentOS 64-bit VFS layer (pure in-memory)

#include "files64.h"
#include "vfs.h"
#include "../commands/commands64.h"
#include <stddef.h>

#ifndef MAX_FILES
#define MAX_FILES 2048
#endif

#ifndef MAX_DIRS
#define MAX_DIRS 512
#endif

#ifndef MAX_FILE_SIZE
#define MAX_FILE_SIZE (1u * 1024u * 1024u)
#endif

// ------------------------------------------------------------------
//  Current working directory
// ------------------------------------------------------------------
char current_dir[MAX_PATH_LENGTH] = "/";

// ------------------------------------------------------------------
//  Directory list
// ------------------------------------------------------------------
Directory64* directories = NULL;
int dir_count = 0;

// ================================================================
//  Dynamic storage
//  content_pool  : flat heap carved into pool_alloc slots
//  dynamic_names64 / dynamic_dirs64 : per-slot name & dir buffers
//  all_files64   : flat file table (no static entries)
// ================================================================
#define HEAP_POOL_SIZE  (32u * 1024u * 1024u)   /* 32 MiB total pool */

char           dynamic_names64 [MAX_FILES][64];
char           dynamic_dirs64  [MAX_FILES][MAX_PATH_LENGTH];
EmbeddedFile64 all_files64     [MAX_FILES];
int            file_count64 = 0;

char     content_pool[HEAP_POOL_SIZE];
uint32_t pool_offset = 0;
char*    dynamic_content_ptr[MAX_FILES];

char* pool_alloc(uint32_t size) {
    if (pool_offset + size > HEAP_POOL_SIZE) return NULL;
    char* p = &content_pool[pool_offset];
    pool_offset += size;
    return p;
}

// ================================================================
//  String helpers
// ================================================================
int str_starts_with(const char* str, const char* prefix) {
    while (*prefix) {
        if (*str++ != *prefix++) return 0;
    }
    return 1;
}

int str_contains(const char* str, const char* substr) {
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
void normalize_path(const char* input, char* output) {
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

int dir_exists(const char* path) {
    if (str_cmp(path, "/") == 0) return 1;
    for (int i = 0; i < dir_count; i++)
        if (str_cmp(directories[i].path, path) == 0) return 1;
    return 0;
}

void get_parent_dir(const char* path, char* parent) {
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

void get_dir_name(const char* path, char* name) {
    int last_slash = -1;
    for (int i = 0; path[i]; i++)
        if (path[i] == '/') last_slash = i;
    if (last_slash < 0) str_cpy(name, path);
    else str_cpy(name, &path[last_slash + 1]);
}

// ================================================================
//  init_filesystem64
// ================================================================
void init_filesystem64(void) {
    static int initialised = 0;
    if (!initialised) {
        for (int i = 0; i < MAX_FILES; i++) {
            dynamic_names64[i][0]  = '\0';
            dynamic_dirs64[i][0]   = '\0';
            dynamic_content_ptr[i] = NULL;
        }
        initialised = 1;
    }

    static Directory64 dir_storage[MAX_DIRS];
    directories  = dir_storage;
    file_count64 = 0;
    dir_count    = 0;
    pool_offset  = 0;

    // Root entry
    str_cpy(directories[dir_count].path, "/");
    directories[dir_count].is_dynamic  = 0;
    directories[dir_count].is_system   = 1;
    dir_count++;
}

void save_files_to_disk64(void) {
    /* no-op: pure in-memory VFS has no persistence backend */
}

// ================================================================
//  File operations
// ================================================================








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
        output_add_line(output, line, VGA_YELLOW);
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
//  Directory operations
// ================================================================
int fs_mkdir64(const char* dirname) {
    if (str_len(dirname) == 0 || str_len(dirname) >= MAX_PATH_LENGTH) return 0;
    if (dir_count >= MAX_DIRS) return 0;

    static char full_path[MAX_PATH_LENGTH];
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
    return 1;
}

int fs_rmdir64(const char* dirname) {
    if (str_len(dirname) == 0) return 0;

    static char full_path[MAX_PATH_LENGTH];
    normalize_path(dirname, full_path);
    if (str_cmp(full_path, "/") == 0) return 0;

    int dir_index = -1;
    for (int i = 0; i < dir_count; i++) {
        if (str_cmp(directories[i].path, full_path) == 0) {
            dir_index = i; break;
        }
    }
    if (dir_index == -1)                    return 0;
    if (directories[dir_index].is_system)   return 0;
    if (!directories[dir_index].is_dynamic) return 0;

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
    return 1;
}

int fs_rmdir_recursive64(const char* dirname) {
    if (str_len(dirname) == 0) return 0;

    static char full_path[MAX_PATH_LENGTH];
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

    for (int i = file_count64 - 1; i >= 0; i--) {
        if (str_starts_with(all_files64[i].directory, full_path) &&
            all_files64[i].is_dynamic) {
            for (int j = i; j < file_count64 - 1; j++) {
                all_files64[j]         = all_files64[j + 1];
                dynamic_content_ptr[j] = dynamic_content_ptr[j + 1];
            }
            file_count64--;
        }
    }

    for (int i = dir_count - 1; i >= 0; i--) {
        if (str_starts_with(directories[i].path, full_path) &&
            str_cmp(directories[i].path, full_path) != 0 &&
            directories[i].is_dynamic && !directories[i].is_system) {
            for (int j = i; j < dir_count - 1; j++) directories[j] = directories[j + 1];
            dir_count--;
        }
    }

    for (int i = 0; i < dir_count; i++) {
        if (str_cmp(directories[i].path, full_path) == 0 &&
            directories[i].is_dynamic && !directories[i].is_system) {
            for (int j = i; j < dir_count - 1; j++) directories[j] = directories[j + 1];
            dir_count--;
            break;
        }
    }
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

    static char full_path[MAX_PATH_LENGTH];
    normalize_path(dirname, full_path);
    if (!dir_exists(full_path)) return 0;
    str_cpy(current_dir, full_path);
    return 1;
}

const char* fs_getcwd64(void) { return current_dir; }

int fs_list_dirs64(void* output_ptr) {
    CommandOutput* output = (CommandOutput*)output_ptr;
    for (int i = 0; i < dir_count; i++) {
        output_add_line(output, directories[i].path, VGA_CYAN);
    }
    return dir_count;
}

// ================================================================
//  SYS_STAT / SYS_ACCESS helpers
// ================================================================
int fs_path_is_file(const char* path) {
    if (!path || path[0] == '\0') return 0;
    return (fs_get_file64(path) != NULL) ? 1 : 0;
}

int fs_path_is_dir(const char* path) {
    if (!path || path[0] == '\0') return 0;
    static char norm[MAX_PATH_LENGTH];
    normalize_path(path, norm);
    if (norm[0] == '/' && norm[1] == '\0') return 1;
    for (int i = 0; i < dir_count; i++)
        if (str_cmp(directories[i].path, norm) == 0) return 1;
    return 0;
}

uint32_t fs_path_filesize(const char* path) {
    if (!path) return 0;
    const EmbeddedFile64* f = fs_get_file64(path);
    if (!f) return 0;
    return f->size;
}

// ================================================================
//  fs_unlink64
// ================================================================
int fs_unlink64(const char* path) {
    if (!path || path[0] == '\0') return -1;
    if (fs_path_is_dir(path)) return -1;

    const EmbeddedFile64* f = fs_get_file64(path);
    if (!f) return -1;

    static char dir_part[MAX_PATH_LENGTH];
    static char name_part[MAX_PATH_LENGTH];
    int  len = str_len(path);
    int  sep = -1;
    for (int i = len - 1; i >= 0; i--) {
        if (path[i] == '/') { sep = i; break; }
    }
    if (sep <= 0) {
        str_cpy(dir_part, "/");
        str_cpy(name_part, (sep == 0) ? path + 1 : path);
    } else {
        for (int i = 0; i < sep; i++) dir_part[i] = path[i];
        dir_part[sep] = '\0';
        str_cpy(name_part, path + sep + 1);
    }

    int file_index = -1;
    for (int i = 0; i < file_count64; i++) {
        if (str_cmp(all_files64[i].directory, dir_part)  == 0 &&
            str_cmp(all_files64[i].name,      name_part) == 0) {
            file_index = i;
            break;
        }
    }
    if (file_index < 0 || !all_files64[file_index].is_dynamic) return -1;

    for (int i = file_index; i < file_count64 - 1; i++) {
        all_files64[i].size       = all_files64[i + 1].size;
        all_files64[i].is_dynamic = all_files64[i + 1].is_dynamic;
        dynamic_content_ptr[i]    = dynamic_content_ptr[i + 1];
        all_files64[i].content    = dynamic_content_ptr[i];
        str_cpy(dynamic_names64[i],
                all_files64[i + 1].name      ? all_files64[i + 1].name      : "");
        str_cpy(dynamic_dirs64[i],
                all_files64[i + 1].directory ? all_files64[i + 1].directory : "/");
        all_files64[i].name      = dynamic_names64[i];
        all_files64[i].directory = dynamic_dirs64[i];
    }
    if (file_count64 > 0) {
        int last = file_count64 - 1;
        dynamic_names64[last][0]  = '\0';
        dynamic_dirs64[last][0]   = '\0';
        dynamic_content_ptr[last] = (char*)0;
        all_files64[last].name      = dynamic_names64[last];
        all_files64[last].directory = dynamic_dirs64[last];
        all_files64[last].content   = (char*)0;
        all_files64[last].size      = 0;
        all_files64[last].is_dynamic= 0;
    }
    file_count64--;
    return 0;
}

// ================================================================
//  fs_rename64
// ================================================================
int fs_rename64(const char* oldpath, const char* newpath) {
    if (!oldpath || !newpath) return -1;
    if (oldpath[0] == '\0' || newpath[0] == '\0') return -1;

    static char old_norm[MAX_PATH_LENGTH];
    static char new_norm[MAX_PATH_LENGTH];
    normalize_path(oldpath, old_norm);
    normalize_path(newpath, new_norm);

    if (fs_path_is_dir(old_norm)) {
        if (fs_path_is_dir(new_norm)) return -1;

        for (int i = 0; i < dir_count; i++) {
            if (str_cmp(directories[i].path, old_norm) == 0) {
                str_cpy(directories[i].path, new_norm);
                break;
            }
        }

        int old_len = str_len(old_norm);
        for (int i = 0; i < file_count64; i++) {
            if (!all_files64[i].is_dynamic) continue;
            if (str_cmp(all_files64[i].directory, old_norm) == 0) {
                str_cpy((char*)all_files64[i].directory, new_norm);
            } else if (str_len(all_files64[i].directory) > (uint32_t)old_len) {
                int match = 1;
                for (int k = 0; k < old_len; k++) {
                    if (all_files64[i].directory[k] != old_norm[k]) { match = 0; break; }
                }
                if (match && all_files64[i].directory[old_len] == '/') {
                    static char new_dir[MAX_PATH_LENGTH];
                    str_cpy(new_dir, new_norm);
                    str_concat(new_dir, (char*)all_files64[i].directory + old_len);
                    str_cpy((char*)all_files64[i].directory, new_dir);
                }
            }
        }
        return 0;
    }

    if (!fs_path_is_file(old_norm)) return -1;

    static char new_dir[MAX_PATH_LENGTH];
    static char new_name[MAX_PATH_LENGTH];
    int nlen = str_len(new_norm);
    int nsep = -1;
    for (int i = nlen - 1; i >= 0; i--) {
        if (new_norm[i] == '/') { nsep = i; break; }
    }
    if (nsep <= 0) {
        str_cpy(new_dir,  "/");
        str_cpy(new_name, (nsep == 0) ? new_norm + 1 : new_norm);
    } else {
        for (int i = 0; i < nsep; i++) new_dir[i] = new_norm[i];
        new_dir[nsep] = '\0';
        str_cpy(new_name, new_norm + nsep + 1);
    }

    static char old_dir[MAX_PATH_LENGTH];
    static char old_name[MAX_PATH_LENGTH];
    int olen = str_len(old_norm);
    int osep = -1;
    for (int i = olen - 1; i >= 0; i--) {
        if (old_norm[i] == '/') { osep = i; break; }
    }
    if (osep <= 0) {
        str_cpy(old_dir,  "/");
        str_cpy(old_name, (osep == 0) ? old_norm + 1 : old_norm);
    } else {
        for (int i = 0; i < osep; i++) old_dir[i] = old_norm[i];
        old_dir[osep] = '\0';
        str_cpy(old_name, old_norm + osep + 1);
    }

    for (int i = 0; i < file_count64; i++) {
        if (str_cmp(all_files64[i].directory, old_dir)  == 0 &&
            str_cmp(all_files64[i].name,      old_name) == 0 &&
            all_files64[i].is_dynamic) {
            str_cpy(dynamic_names64[i], new_name);
            str_cpy(dynamic_dirs64[i],  new_dir);
            all_files64[i].name      = dynamic_names64[i];
            all_files64[i].directory = dynamic_dirs64[i];
            return 0;
        }
    }
    return -1;
}

// ================================================================
//  fs_truncate64
// ================================================================
int fs_truncate64(const char* path, uint64_t length) {
    if (!path || path[0] == '\0') return -1;
    if (length > MAX_FILE_SIZE) return -1;

    static char norm[MAX_PATH_LENGTH];
    normalize_path(path, norm);

    EmbeddedFile64* file = (EmbeddedFile64*)fs_get_file64(norm);
    if (!file || !file->is_dynamic) return -1;

    int idx = (int)(file - all_files64);
    uint32_t new_size = (uint32_t)length;

    char* nbuf = pool_alloc(new_size + 1);
    if (!nbuf && new_size > 0) return -1;

    uint32_t copy_len = (new_size < file->size) ? new_size : file->size;

    if (copy_len > 0 && file->content) {
        for (uint32_t i = 0; i < copy_len; i++)
            nbuf[i] = file->content[i];
    }
    for (uint32_t i = copy_len; i < new_size; i++)
        nbuf[i] = '\0';
    if (new_size > 0) nbuf[new_size] = '\0';

    dynamic_content_ptr[idx] = nbuf;
    file->content = nbuf;
    file->size    = new_size;
    return 0;
}

// ================================================================
//  fs_getdents64
// ================================================================
int fs_getdents64(const char* path, dirent64_t* buf, int buf_size) {
    if (!path || !buf || buf_size <= 0) return -1;

    char norm[MAX_PATH_LENGTH];
    normalize_path(path, norm);

    int is_root = (norm[0] == '/' && norm[1] == '\0');
    if (!is_root) {
        int found = 0;
        for (int i = 0; i < dir_count; i++) {
            if (str_cmp(directories[i].path, norm) == 0) { found = 1; break; }
        }
        if (!found) return -1;
    }

    char norm_slash[MAX_PATH_LENGTH];
    str_cpy(norm_slash, norm);
    int nslen = str_len(norm_slash);
    if (norm_slash[nslen - 1] != '/') {
        norm_slash[nslen]     = '/';
        norm_slash[nslen + 1] = '\0';
    }

    int total_bytes = 0;
    uint64_t ino = 1;

    #define WRITE_DIRENT(nm, tp) do {                                \
        int nlen_ = str_len(nm);                                     \
        int reclen_ = (int)(19 + nlen_ + 1 + 7) & ~7;               \
        if (total_bytes + reclen_ > buf_size) goto done;             \
        dirent64_t* de = (dirent64_t*)((char*)buf + total_bytes);    \
        de->d_ino    = ino++;                                         \
        de->d_off    = (uint64_t)(total_bytes + reclen_);            \
        de->d_reclen = (uint16_t)reclen_;                            \
        de->d_type   = (tp);                                         \
        for (int _i = 0; _i <= nlen_; _i++) de->d_name[_i] = (nm)[_i]; \
        total_bytes += reclen_;                                       \
    } while(0)

    WRITE_DIRENT(".",  DT_DIR);
    WRITE_DIRENT("..", DT_DIR);

    for (int i = 0; i < dir_count; i++) {
        char parent[MAX_PATH_LENGTH];
        get_parent_dir(directories[i].path, parent);
        if (str_cmp(parent, norm) != 0 &&
            str_cmp(parent, norm_slash) != 0) continue;

        char dname[MAX_PATH_LENGTH];
        get_dir_name(directories[i].path, dname);
        if (dname[0] == '\0') continue;

        WRITE_DIRENT(dname, DT_DIR);
    }

    for (int i = 0; i < file_count64; i++) {
        const char* fdir = all_files64[i].directory;
        int match = (str_cmp(fdir, norm) == 0 ||
                     str_cmp(fdir, norm_slash) == 0);
        if (is_root && (fdir[0] == '\0' ||
                        (fdir[0] == '/' && fdir[1] == '\0'))) match = 1;
        if (!match) continue;
        WRITE_DIRENT(all_files64[i].name, DT_REG);
    }

    #undef WRITE_DIRENT

done:
    return total_bytes;
}

const EmbeddedFile64* get_all_files_list64(int* count) {
    *count = file_count64;
    return all_files64;
}

// ================================================================
//  Advanced operations: tree, find, du
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
        str_concat(line, "\xe2\x94\x9c\xe2\x94\x80\xe2\x94\x80 ");
        char dir_name[MAX_PATH_LENGTH];
        get_dir_name(directories[i].path, dir_name);
        str_concat(line, dir_name);
        str_concat(line, "/");
        output_add_line(output, line, VGA_CYAN);

        char new_prefix[MAX_LINE_LENGTH];
        str_cpy(new_prefix, prefix);
        str_concat(new_prefix, "\xe2\x94\x82   ");
        draw_tree_recursive(output_ptr, directories[i].path, depth + 1, new_prefix);
    }

    for (int i = 0; i < file_count64; i++) {
        if (str_cmp(all_files64[i].directory, path) != 0) continue;
        char line[MAX_LINE_LENGTH];
        str_cpy(line, prefix);
        str_concat(line, "\xe2\x94\x9c\xe2\x94\x80\xe2\x94\x80 ");
        str_concat(line, all_files64[i].name);
        output_add_line(output, line, VGA_YELLOW);
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
        output_add_line(output, line, VGA_YELLOW);
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

    static char full_path[MAX_PATH_LENGTH];
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

