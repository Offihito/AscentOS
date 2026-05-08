#ifndef SHM_H
#define SHM_H

#include <stddef.h>
#include <stdint.h>

// ═══════════════════════════════════════════════════════════════════════════
//  System V Shared Memory (SHM) Subsystem
//
//  Provides system-wide shared memory segments that can be attached to
//  multiple process address spaces. Built on top of the existing VMA and
//  VMM infrastructure with COW support.
//
//  API mirrors Linux shmget/shmat/shmdt/shmctl:
//    shmget(key, size, flags) → shmid
//    shmat(shmid, addr, flags) → mapped address
//    shmdt(addr)               → 0 on success
//    shmctl(shmid, cmd, buf)   → 0 on success
// ═══════════════════════════════════════════════════════════════════════════

// IPC flags
#define IPC_CREAT   0x0200    // Create segment if it doesn't exist
#define IPC_EXCL    0x0400    // Fail if segment already exists
#define IPC_RMID    0         // Remove segment (shmctl command)
#define IPC_STAT    2         // Get segment info (shmctl command)
#define IPC_PRIVATE 0         // Create a private (unkeyed) segment

// shmat flags
#define SHM_RDONLY  0x1000    // Attach read-only
#define SHM_RND     0x2000    // Round attach address to SHMLBA

// Limits
#define SHM_MAX_SEGMENTS  64
#define SHM_MAX_SIZE      (16 * 1024 * 1024)  // 16 MB max per segment
#define SHMLBA            4096                 // Segment low boundary alignment

// SHM segment info structure (returned by shmctl IPC_STAT)
struct shmid_ds {
    uint32_t shm_perm_key;    // IPC key
    uint32_t shm_perm_uid;    // Owner UID
    uint32_t shm_perm_gid;    // Owner GID
    uint32_t shm_perm_mode;   // Permission bits
    uint64_t shm_segsz;       // Size of segment (bytes)
    uint32_t shm_nattch;      // Number of current attaches
    uint64_t shm_atime;       // Last attach time
    uint64_t shm_dtime;       // Last detach time
    uint64_t shm_ctime;       // Creation time
    uint32_t shm_cpid;        // PID of creator
    uint32_t shm_lpid;        // PID of last shmat/shmdt
};

// ── Kernel API ──────────────────────────────────────────────────────────

// Initialize the shared memory subsystem
void shm_init(void);

// Syscall implementations
int64_t sys_shmget(uint64_t key, uint64_t size, uint64_t shmflg,
                   uint64_t a3, uint64_t a4, uint64_t a5);
int64_t sys_shmat(uint64_t shmid, uint64_t shmaddr, uint64_t shmflg,
                  uint64_t a3, uint64_t a4, uint64_t a5);
int64_t sys_shmdt(uint64_t shmaddr, uint64_t a1, uint64_t a2,
                  uint64_t a3, uint64_t a4, uint64_t a5);
int64_t sys_shmctl(uint64_t shmid, uint64_t cmd, uint64_t buf,
                   uint64_t a3, uint64_t a4, uint64_t a5);

// Print SHM segment status (for kernel shell)
void shm_print_status(void);

#endif // SHM_H
