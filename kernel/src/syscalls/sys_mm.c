// ── Memory Management Syscalls: mmap, munmap, brk ──────────────────────────
#include "syscall.h"
#include "../mm/vmm.h"
#include "../mm/pmm.h"
#include "../lib/string.h"
#include "../console/klog.h"
#include <stdint.h>

// ── Linux mmap constants ────────────────────────────────────────────────────
#define PROT_READ   0x1
#define PROT_WRITE  0x2
#define PROT_EXEC   0x4

#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20

#define MAP_FAILED ((uint64_t)-1)

// ── Process memory state (set by process_exec, reset on exit) ───────────────
// These are defined in process.c and set during ELF loading.
extern uint64_t process_brk_base;    // End of the last PT_LOAD segment (initial brk)
extern uint64_t process_brk_current; // Current program break

// ── Simple mmap bump allocator ──────────────────────────────────────────────
// We hand out anonymous pages starting from a high address and bumping upward.
// This avoids collisions with the ELF segments (low addresses) and stack (very high).
#define MMAP_REGION_BASE  0x7F0000000000ULL
#define MMAP_REGION_LIMIT 0x7FF000000000ULL

static uint64_t mmap_next_addr = MMAP_REGION_BASE;

// ── Helpers ─────────────────────────────────────────────────────────────────
#define PAGE_SIZE 4096
#define PAGE_ALIGN_UP(x)   (((x) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))
#define PAGE_ALIGN_DOWN(x) ((x) & ~(PAGE_SIZE - 1))

static uint64_t build_page_flags(uint64_t prot) {
    uint64_t flags = PAGE_FLAG_PRESENT | PAGE_FLAG_USER;
    if (prot & PROT_WRITE)
        flags |= PAGE_FLAG_RW;
    if (!(prot & PROT_EXEC))
        flags |= PAGE_FLAG_NX;
    return flags;
}

// ── sys_mmap ────────────────────────────────────────────────────────────────
// Linux ABI: mmap(addr, length, prot, flags, fd, offset)
//   rdi = addr (hint), rsi = length, rdx = prot, r10 = flags, r8 = fd, r9 = offset
static uint64_t sys_mmap(uint64_t addr, uint64_t length, uint64_t prot,
                          uint64_t flags, uint64_t fd, uint64_t offset) {
    (void)fd; (void)offset;

    // We only support anonymous private mappings for now.
    if (!(flags & MAP_ANONYMOUS)) {
        klog_puts("[MMAP] Error: Only MAP_ANONYMOUS supported\n");
        return MAP_FAILED;
    }

    if (length == 0) {
        return MAP_FAILED;
    }

    uint64_t aligned_len = PAGE_ALIGN_UP(length);
    uint64_t num_pages = aligned_len / PAGE_SIZE;

    // Determine the virtual address to use.
    uint64_t vaddr;
    if ((flags & MAP_FIXED) && addr != 0) {
        // MAP_FIXED: use exact address (must be page-aligned).
        vaddr = PAGE_ALIGN_DOWN(addr);
    } else if (addr != 0) {
        // Hint provided: try to use it (page-align it).
        vaddr = PAGE_ALIGN_DOWN(addr);
    } else {
        // No hint: pick from our bump allocator.
        vaddr = mmap_next_addr;
        if (vaddr + aligned_len > MMAP_REGION_LIMIT) {
            klog_puts("[MMAP] Error: mmap region exhausted\n");
            return MAP_FAILED;
        }
        mmap_next_addr = vaddr + aligned_len;
    }

    // Allocate physical pages and map them.
    uint64_t *pml4 = vmm_get_active_pml4();
    uint64_t page_flags = build_page_flags(prot);

    for (uint64_t i = 0; i < num_pages; i++) {
        void *phys = pmm_alloc();
        if (!phys) {
            // OOM: unmap what we already mapped and fail.
            for (uint64_t j = 0; j < i; j++) {
                vmm_unmap_page(pml4, vaddr + j * PAGE_SIZE);
                // Note: we lose the physical frame reference here.
                // A real OS would track it per-VMA and free it.
            }
            klog_puts("[MMAP] Error: OOM during allocation\n");
            return MAP_FAILED;
        }

        if (!vmm_map_page(pml4, vaddr + i * PAGE_SIZE, (uint64_t)phys, page_flags)) {
            klog_puts("[MMAP] Error: vmm_map_page failed\n");
            return MAP_FAILED;
        }
    }

    // Zero the mapped region (anonymous mappings must be zero-filled) via HHDM
    // We can't memset the user-space vaddr directly from kernel code
    for (uint64_t i = 0; i < num_pages; i++) {
        uint64_t phys = vmm_virt_to_phys(pml4, vaddr + i * PAGE_SIZE);
        if (phys) {
            void *kernel_virt = (void *)(phys + pmm_get_hhdm_offset());
            memset(kernel_virt, 0, PAGE_SIZE);
        }
    }

    klog_puts("[MMAP] Mapped ");
    klog_uint64(aligned_len);
    klog_puts(" bytes at ");
    klog_uint64(vaddr);
    klog_puts("\n");

    return vaddr;
}

// ── sys_munmap ──────────────────────────────────────────────────────────────
// Linux ABI: munmap(addr, length)
//   rdi = addr, rsi = length
static uint64_t sys_munmap(uint64_t addr, uint64_t length, uint64_t a2,
                            uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;

    if (addr == 0 || length == 0) {
        return (uint64_t)-22; // -EINVAL
    }

    // addr must be page-aligned.
    if (addr & (PAGE_SIZE - 1)) {
        return (uint64_t)-22; // -EINVAL
    }

    uint64_t aligned_len = PAGE_ALIGN_UP(length);
    uint64_t num_pages = aligned_len / PAGE_SIZE;
    uint64_t *pml4 = vmm_get_active_pml4();

    for (uint64_t i = 0; i < num_pages; i++) {
        uint64_t va = addr + i * PAGE_SIZE;
        // TODO: Recover physical frame and pmm_free() it.
        // Requires a vmm_virt_to_phys() lookup, which we don't have yet.
        vmm_unmap_page(pml4, va);
    }

    klog_puts("[MUNMAP] Unmapped ");
    klog_uint64(aligned_len);
    klog_puts(" bytes at ");
    klog_uint64(addr);
    klog_puts("\n");

    return 0;
}

// ── sys_brk ─────────────────────────────────────────────────────────────────
// Linux ABI: brk(addr)
//   rdi = addr
// Returns: current/new program break on success.
// If addr == 0: return current break.
// If addr > current: expand (allocate pages).
// If addr < current: shrink (unmap pages).
static uint64_t sys_brk(uint64_t addr, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;

    // Query current break.
    if (addr == 0) {
        return process_brk_current;
    }

    uint64_t *pml4 = vmm_get_active_pml4();

    if (addr > process_brk_current) {
        // ── Expand the heap ─────────────────────────────────────────────
        uint64_t old_end_page = PAGE_ALIGN_UP(process_brk_current);
        uint64_t new_end_page = PAGE_ALIGN_UP(addr);

        klog_puts("[BRK] Expanding. process_brk_current=");
        klog_uint64(process_brk_current);
        klog_puts(" process_brk_base=");
        klog_uint64(process_brk_base);
        klog_puts(" old_end_page=");
        klog_uint64(old_end_page);
        klog_puts(" new_end_page=");
        klog_uint64(new_end_page);
        klog_puts("\n");

        klog_puts("[BRK] Expanding from ");
        klog_uint64(process_brk_current);
        klog_puts(" to ");
        klog_uint64(addr);
        klog_puts(" (pages ");
        klog_uint64(old_end_page);
        klog_puts(" -> ");
        klog_uint64(new_end_page);
        klog_puts(")\n");

        for (uint64_t page = old_end_page; page < new_end_page; page += PAGE_SIZE) {
            // Check if page is already allocated (might be part of ELF segment)
            uint64_t existing_phys = vmm_virt_to_phys(pml4, page);
            if (existing_phys != 0) {
                klog_puts("[BRK] Page ");
                klog_uint64(page);
                klog_puts(" already mapped to phys ");
                klog_uint64(existing_phys);
                klog_puts(", skipping\n");
                continue;
            }
            
            void *phys = pmm_alloc();
            if (!phys) {
                // OOM: return current break (failure to expand).
                klog_puts("[BRK] OOM, cannot expand at page ");
                klog_uint64(page);
                klog_puts("\n");
                return process_brk_current;
            }
            klog_puts("[BRK] Allocating page ");
            klog_uint64(page);
            klog_puts(" -> phys ");
            klog_uint64((uint64_t)phys);
            klog_puts("\n");
            
            if (!vmm_map_page(pml4, page, (uint64_t)phys,
                             PAGE_FLAG_PRESENT | PAGE_FLAG_USER | PAGE_FLAG_RW | PAGE_FLAG_NX)) {
                klog_puts("[BRK] Error: vmm_map_page failed for page ");
                klog_uint64(page);
                klog_puts("\n");
                return process_brk_current;
            }
            
            // Zero via kernel-accessible HHDM address, not user virtual address
            void *kernel_virt = (void *)((uint64_t)phys + pmm_get_hhdm_offset());
            memset(kernel_virt, 0, PAGE_SIZE);
        }

        process_brk_current = addr;

        klog_puts("[BRK] Expanded to ");
        klog_uint64(addr);
        klog_puts("\n");

    } else if (addr < process_brk_current && addr >= process_brk_base) {
        // ── Shrink the heap ─────────────────────────────────────────────
        uint64_t old_end_page = PAGE_ALIGN_UP(process_brk_current);
        uint64_t new_end_page = PAGE_ALIGN_UP(addr);

        for (uint64_t page = new_end_page; page < old_end_page; page += PAGE_SIZE) {
            vmm_unmap_page(pml4, page);
        }

        process_brk_current = addr;

        klog_puts("[BRK] Shrunk to ");
        klog_uint64(addr);
        klog_puts("\n");
    }
    // If addr < process_brk_base, just return current (don't shrink below base).

    return process_brk_current;
}

// ── Registration ────────────────────────────────────────────────────────────
void syscall_register_mm(void) {
    syscall_register(SYS_MMAP,   sys_mmap);
    syscall_register(SYS_MUNMAP, sys_munmap);
    syscall_register(SYS_BRK,    sys_brk);
}

// ── Reset mmap state (called from process_exec on new process load) ─────────
void mm_reset_mmap_state(void) {
    mmap_next_addr = MMAP_REGION_BASE;
}
