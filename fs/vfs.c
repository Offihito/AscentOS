#include "vfs.h"
#include "files64.h"
#include "../commands/commands64.h"
#include <stddef.h>

// Extern declarations for globals from files64.c
extern EmbeddedFile64 all_files64[];
extern char* dynamic_content_ptr[];
extern char current_dir[];
extern Directory64* directories;
extern int dir_count;
extern int file_count64;
extern char dynamic_names64[][64];
extern char dynamic_dirs64[][256];
extern char content_pool[];
extern uint32_t pool_offset;

// ================================================================
//  fs_get_file64
// ================================================================
const EmbeddedFile64* fs_get_file64(const char* filename) {
    static char full_path[MAX_PATH_LENGTH];
    normalize_path(filename, full_path);

    for (int i = 0; i < file_count64; i++) {
        static char fp[MAX_PATH_LENGTH];
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

// ================================================================
//  fs_touch_file64
// ================================================================
int fs_touch_file64(const char* filename) {
    if (str_len(filename) == 0 || str_len(filename) >= 64) return 0;
    if (fs_get_file64(filename) != NULL) return 0;

    int idx = file_count64;
    if (idx >= MAX_FILES) return 0;

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
    return 1;
}

// ================================================================
//  fs_write_file64
// ================================================================
int fs_write_file64(const char* name, const char* content) {
    if (str_len(name) == 0) return 0;
    if (!content) return 0;

    EmbeddedFile64* file = (EmbeddedFile64*)fs_get_file64(name);
    if (file == NULL || !file->is_dynamic) return 0;

    int idx = (int)(file - all_files64);
    uint32_t content_len = (uint32_t)str_len(content);

    if (content_len > MAX_FILE_SIZE) content_len = MAX_FILE_SIZE;

    char* cbuf = pool_alloc(content_len + 1);
    if (!cbuf) return 0;

    for (uint32_t j = 0; j < content_len; j++) cbuf[j] = content[j];
    cbuf[content_len] = '\0';

    dynamic_content_ptr[idx] = cbuf;
    file->content = cbuf;
    file->size    = content_len;
    return 1;
}

// ================================================================
//  fs_delete_file64
// ================================================================
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
    return 1;
}

// ================================================================
//  fs_vfs_write  (sys_write bridge)
// ================================================================
int fs_vfs_write(const char* path, uint64_t offset,
                 const char* data, uint32_t len) {
    if (!path || !data || len == 0) return 0;

    const char* fname = path;
    for (const char* p = path; *p; p++)
        if (*p == '/') fname = p + 1;
    if (fname[0] == '\0') return -1;

    EmbeddedFile64* file = (EmbeddedFile64*)fs_get_file64(fname);
    if (!file || !file->is_dynamic) {
        if (!fs_touch_file64(fname)) return -1;
        file = (EmbeddedFile64*)fs_get_file64(fname);
        if (!file) return -1;
    }

    if (offset == 0) file->size = 0;
    uint32_t old_size = file->size;
    uint32_t new_size = (uint32_t)offset + len;
    if (new_size > MAX_FILE_SIZE) new_size = MAX_FILE_SIZE;

    char* nbuf = pool_alloc(new_size + 1);
    if (!nbuf) return -1;

    uint32_t copy_old = (uint32_t)offset < old_size ? (uint32_t)offset : old_size;
    if (copy_old > 0 && file->content)
        for (uint32_t i = 0; i < copy_old; i++) nbuf[i] = file->content[i];

    uint32_t write_len = len;
    if ((uint32_t)offset + write_len > MAX_FILE_SIZE)
        write_len = MAX_FILE_SIZE - (uint32_t)offset;
    for (uint32_t i = 0; i < write_len; i++)
        nbuf[(uint32_t)offset + i] = data[i];
    nbuf[new_size] = '\0';

    int idx = (int)(file - all_files64);
    dynamic_content_ptr[idx] = nbuf;
    file->content = nbuf;
    file->size    = new_size;
    return (int)write_len;
}