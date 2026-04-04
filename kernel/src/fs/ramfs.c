#include "fs/ramfs.h"
#include "mm/heap.h"
#include "lib/string.h"

// ── Internal Structures ─────────────────────────────────────────────────────

typedef struct child_node {
    vfs_node_t *node;
    struct child_node *next;
} child_node_t;

// For files, device points to this
typedef struct {
    uint8_t *data;
    uint32_t capacity;
} ramfs_file_t;

// For directories, device points to this
typedef struct {
    child_node_t *children;
} ramfs_dir_t;

static uint32_t next_inode = 1;

// ── VFS Implementations ─────────────────────────────────────────────────────

static uint32_t ramfs_read(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    if (!node || !node->device) return 0;
    
    ramfs_file_t *file = (ramfs_file_t*)node->device;
    if (offset >= node->length) return 0;
    
    if (offset + size > node->length) {
        size = node->length - offset;
    }
    
    memcpy(buffer, file->data + offset, size);
    return size;
}

static uint32_t ramfs_write(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    if (!node || !node->device) return 0;
    
    ramfs_file_t *file = (ramfs_file_t*)node->device;
    
    // Auto-resize buffer if needed
    if (offset + size > file->capacity) {
        uint32_t new_cap = (offset + size) * 2; // Double the required size 
        if (new_cap < 512) new_cap = 512;
        
        uint8_t *new_data = kmalloc(new_cap);
        if (!new_data) return 0; // OOM
        
        if (file->data) {
            memcpy(new_data, file->data, node->length);
            kfree(file->data);
        }
        file->data = new_data;
        file->capacity = new_cap;
    }
    
    memcpy(file->data + offset, buffer, size);
    if (offset + size > node->length) {
        node->length = offset + size;
    }
    
    return size;
}

static struct dirent *ramfs_readdir(vfs_node_t *node, uint32_t index) {
    if (!node || !node->device) return 0;
    ramfs_dir_t *dir = (ramfs_dir_t*)node->device;
    
    // For standard compliance, the returned dirent is often dynamically allocated or statically buffered.
    // For simplicity without a thread-local static, we allocate a temp dirent per call.
    // Callers are normally responsible for providing a buffer, but our signature returns struct dirent*.
    // Using a static inside the function is OK for non-preemptive mono-core currently.
    static struct dirent d;
    memset(&d, 0, sizeof(struct dirent));
    
    if (index == 0) {
        strcpy(d.name, ".");
        d.ino = node->inode;
        return &d;
    }
    if (index == 1) {
        strcpy(d.name, "..");
        d.ino = node->inode; // Technically wrong parent inode, but acceptable for basic ramfs
        return &d;
    }
    
    index -= 2;
    child_node_t *curr = dir->children;
    for (uint32_t i = 0; i < index && curr; i++) {
        curr = curr->next;
    }
    
    if (curr) {
        strcpy(d.name, curr->node->name);
        d.ino = curr->node->inode;
        return &d;
    }
    
    return 0; // End of directory
}

static vfs_node_t *ramfs_finddir(vfs_node_t *node, char *name) {
    if (!node || !node->device) return 0;
    ramfs_dir_t *dir = (ramfs_dir_t*)node->device;
    
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return node;
    }
    
    child_node_t *curr = dir->children;
    while (curr) {
        if (strcmp(curr->node->name, name) == 0) {
            return curr->node;
        }
        curr = curr->next;
    }
    
    return 0;
}

// Helper to construct a new node linked to ramfs standard APIs
static vfs_node_t *ramfs_make_node(char *name, uint16_t perm, uint32_t type) {
    vfs_node_t *n = kmalloc(sizeof(vfs_node_t));
    if (!n) return 0;
    memset(n, 0, sizeof(vfs_node_t));
    
    strncpy(n->name, name, 127);
    n->mask = perm;
    n->uid = 0;
    n->gid = 0;
    n->flags = type;
    n->inode = next_inode++;
    n->length = 0;
    n->impl = 0;
    n->ptr = 0;
    
    if (type == FS_DIRECTORY) {
        ramfs_dir_t *d = kmalloc(sizeof(ramfs_dir_t));
        d->children = 0;
        n->device = d;
        n->readdir = ramfs_readdir;
        n->finddir = ramfs_finddir;
    } else if (type == FS_FILE) {
        ramfs_file_t *f = kmalloc(sizeof(ramfs_file_t));
        f->data = 0;
        f->capacity = 0;
        n->device = f;
        n->read = ramfs_read;
        n->write = ramfs_write;
    }
    // Block devices would be populated via ramfs_mount_node
    
    return n;
}

// Internal helper to add node to dir
static void ramfs_add_child(vfs_node_t *parent, vfs_node_t *child) {
    ramfs_dir_t *dir = (ramfs_dir_t*)parent->device;
    child_node_t *cn = kmalloc(sizeof(child_node_t));
    cn->node = child;
    cn->next = dir->children;
    dir->children = cn;
}

static int ramfs_create(vfs_node_t *node, char *name, uint16_t permission) {
    if (!node || (node->flags & 0x7) != FS_DIRECTORY) return -1;
    if (ramfs_finddir(node, name) != 0) return -1; // File exists
    
    vfs_node_t *new_node = ramfs_make_node(name, permission, FS_FILE);
    if (!new_node) return -1;
    
    ramfs_add_child(node, new_node);
    return 0;
}

static int ramfs_mkdir(vfs_node_t *node, char *name, uint16_t permission) {
    if (!node || (node->flags & 0x7) != FS_DIRECTORY) return -1;
    if (ramfs_finddir(node, name) != 0) return -1; // Directory exists
    
    vfs_node_t *new_node = ramfs_make_node(name, permission, FS_DIRECTORY);
    if (!new_node) return -1;
    
    // Assign directory pointers
    new_node->create = ramfs_create;
    new_node->mkdir = ramfs_mkdir;

    ramfs_add_child(node, new_node);
    return 0;
}

// ── Public APIs ─────────────────────────────────────────────────────────────

void ramfs_init(void) {
    next_inode = 1;
    // Create the root directory
    vfs_node_t *root = ramfs_make_node("/", 0755, FS_DIRECTORY);
    // Bind APIs
    root->create = ramfs_create;
    root->mkdir = ramfs_mkdir;
    
    fs_root = root;
    
    // Automatically create /dev and /mnt and /tmp
    ramfs_mkdir(fs_root, "dev", 0755);
    ramfs_mkdir(fs_root, "mnt", 0755);
    ramfs_mkdir(fs_root, "tmp", 0777);
}

void ramfs_mount_node(vfs_node_t *root, vfs_node_t *node) {
    if (!root || !node) return;
    if ((root->flags & 0x07) != FS_DIRECTORY) return;
    
    ramfs_add_child(root, node);
}
