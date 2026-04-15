// ── Memory Management Syscalls: mmap, munmap, brk ──────────────────────────
#include "../console/klog.h"
#include "../fs/vfs.h"
#include "../lib/string.h"
#include "../mm/pmm.h"
#include "../mm/vma.h"
#include "../mm/vmm.h"
#include "../sched/sched.h"
#include "syscall.h"
#include <stdint.h>

// MAX_FDS is defined in sched.h

// ── Linux mmap constants ────────────────────────────────────────────────────
#define PROT_NONE 0x0
#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define PROT_EXEC 0x4

#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20

#define MAP_FAILED ((uint64_t)-1)

// ── Process memory state (set by process_exec, reset on exit) ───────────────
// These are defined in process.c and set during ELF loading.
extern uint64_t
    process_brk_base; // End of the last PT_LOAD segment (initial brk)
extern uint64_t process_brk_current; // Current program break

// ── Simple mmap bump allocator ──────────────────────────────────────────────
// We hand out anonymous pages starting from a high address and bumping upward.
// This avoids collisions with the ELF segments (low addresses) and stack (very
// high).
#define MMAP_REGION_BASE 0x7F0000000000ULL
#define MMAP_REGION_LIMIT 0x7FF000000000ULL

static uint64_t mmap_next_addr = MMAP_REGION_BASE;

// ── Helpers ─────────────────────────────────────────────────────────────────
#define PAGE_SIZE 4096
#define PAGE_ALIGN_UP(x) (((x) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))
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
//   rdi = addr (hint), rsi = length, rdx = prot, r10 = flags, r8 = fd, r9 =
//   offset
static uint64_t sys_mmap(uint64_t addr, uint64_t length, uint64_t prot,
                         uint64_t flags, uint64_t fd, uint64_t offset) {
  (void)offset;

  // Validate flags: must have exactly one of MAP_SHARED or MAP_PRIVATE
  bool is_shared = (flags & MAP_SHARED) != 0;
  bool is_private = (flags & MAP_PRIVATE) != 0;

  if (!is_shared && !is_private) {
    klog_puts("[MMAP] Error: Must specify MAP_SHARED or MAP_PRIVATE\n");
    return MAP_FAILED;
  }

  if (is_shared && is_private) {
    klog_puts("[MMAP] Error: Cannot specify both MAP_SHARED and MAP_PRIVATE\n");
    return MAP_FAILED;
  }

  // ── File-backed mapping (fd != -1) ─────────────────────────────────────────
  // If fd is valid, delegate to the device's mmap handler
  if (!(flags & MAP_ANONYMOUS) && (int64_t)fd != -1) {
    struct thread *t = sched_get_current();
    if (!t || fd >= MAX_FDS || !t->fds[fd]) {
      klog_puts("[MMAP] Error: invalid fd for file-backed mapping\n");
      return MAP_FAILED;
    }

    vfs_node_t *node = t->fds[fd];
    if (!node->mmap) {
      klog_puts("[MMAP] Error: device does not support mmap\n");
      return MAP_FAILED;
    }

    // Call device-specific mmap handler
    uint64_t result = node->mmap(node, length, prot, flags);

    // Register in VMA list if successful
    if (result != MAP_FAILED && result != (uint64_t)-1) {
      uint64_t aligned_len = PAGE_ALIGN_UP(length);
      int vma_idx = vma_add(&t->vmas, result, result + aligned_len, prot, flags,
                            (int)fd, 0);
      if (vma_idx < 0) {
        klog_puts("[MMAP] Warning: Failed to register VMA for file mapping\n");
      }
    }

    return result;
  }

  // ── Anonymous mapping (MAP_ANONYMOUS) ─────────────────────────────────────
  if (!(flags & MAP_ANONYMOUS)) {
    klog_puts("[MMAP] Error: non-anonymous mapping requires valid fd\n");
    return MAP_FAILED;
  }

  if (length == 0) {
    return MAP_FAILED;
  }

  uint64_t aligned_len = PAGE_ALIGN_UP(length);
  uint64_t num_pages = aligned_len / PAGE_SIZE;

  // Determine the virtual address to use.
  // Linux treats non-MAP_FIXED addr as a hint, not a hard requirement.
  // For now we ignore hints to avoid accidental overlap/corruption.
  uint64_t vaddr;
  if (flags & MAP_FIXED) {
    if (addr == 0) {
      klog_puts("[MMAP] Error: MAP_FIXED with null addr\n");
      return MAP_FAILED;
    }
    // MAP_FIXED: use exact page-aligned address.
    vaddr = PAGE_ALIGN_DOWN(addr);
  } else {
    // Non-fixed: always allocate from our dedicated mmap region.
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

    if (!vmm_map_page(pml4, vaddr + i * PAGE_SIZE, (uint64_t)phys,
                      page_flags)) {
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

  // Register the mapping in the VMA list
  struct thread *current = sched_get_current();
  if (current) {
    int vma_idx =
        vma_add(&current->vmas, vaddr, vaddr + aligned_len, prot, flags, -1, 0);
    if (vma_idx < 0) {
      klog_puts("[MMAP] Warning: Failed to register VMA\n");
      // Continue anyway - mapping is valid
    }
  }

  klog_puts("[MMAP] Mapped ");
  klog_uint64(aligned_len);
  klog_puts(" bytes at ");
  klog_uint64(vaddr);
  klog_puts(is_shared ? " (SHARED)\n" : " (PRIVATE)\n");

  return vaddr;
}

// ── sys_munmap ──────────────────────────────────────────────────────────────
// Linux ABI: munmap(addr, length)
//   rdi = addr, rsi = length
static uint64_t sys_munmap(uint64_t addr, uint64_t length, uint64_t a2,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

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

  // munmap should operate only on tracked mmap VMAs. This avoids tearing down
  // unrelated ELF/stack/brk mappings if userspace passes a bad address.
  struct thread *current = sched_get_current();
  struct vma *first_vma = NULL;
  if (!current) {
    return (uint64_t)-22; // EINVAL
  }
  first_vma = vma_find(&current->vmas, addr);
  if (!first_vma) {
    return (uint64_t)-22; // EINVAL
  }

  // Logging label (best-effort, based on first VMA).
  bool is_shared = false;
  if (first_vma->flags & MAP_SHARED) {
    is_shared = true;
  }

  for (uint64_t i = 0; i < num_pages; i++) {
    uint64_t va = addr + i * PAGE_SIZE;
    struct vma *vma = vma_find(&current->vmas, va);
    if (!vma) {
      // Skip untracked pages rather than tearing down unknown mappings.
      continue;
    }

    uint64_t phys = vmm_virt_to_phys(pml4, va);

    if (phys != 0) {
      // Only anonymous private pages are owned by this mmap path and can
      // safely be returned to PMM. Device/file-backed mappings may not
      // come from pmm_alloc and must not be pmm_free'd here.
      bool should_free_phys = false;
      if (vma->fd == -1 && (vma->flags & MAP_PRIVATE) &&
          (vma->flags & MAP_ANONYMOUS)) {
        should_free_phys = true;
      }

      if (should_free_phys) {
        pmm_free((void *)PAGE_ALIGN_DOWN(phys));
      }
    }
    vmm_unmap_page(pml4, va);
  }

  // Remove from VMA list
  vma_remove(&current->vmas, addr, addr + aligned_len);

  klog_puts("[MUNMAP] Unmapped ");
  klog_uint64(aligned_len);
  klog_puts(" bytes at ");
  klog_uint64(addr);
  klog_puts(is_shared ? " (SHARED)\n" : " (PRIVATE)\n");

  return 0;
}

// ── sys_brk ─────────────────────────────────────────────────────────────────
// Linux ABI: brk(addr)
//   rdi = addr
// Returns: current/new program break on success.
// If addr == 0: return current break.
// If addr > current: expand (allocate pages).
// If addr < current: shrink (unmap pages).
static uint64_t sys_brk(uint64_t addr, uint64_t a1, uint64_t a2, uint64_t a3,
                        uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

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
                        PAGE_FLAG_PRESENT | PAGE_FLAG_USER | PAGE_FLAG_RW |
                            PAGE_FLAG_NX)) {
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

// ── sys_mprotect ────────────────────────────────────────────────────────────
// Linux ABI: mprotect(addr, len, prot)
//   rdi = addr, rsi = len, rdx = prot
static uint64_t sys_mprotect(uint64_t addr, uint64_t len, uint64_t prot,
                             uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;

  // addr must be page-aligned
  if (addr & (PAGE_SIZE - 1)) {
    return (uint64_t)-22; // EINVAL
  }

  if (len == 0) {
    return 0; // Success (nothing to do)
  }

  uint64_t aligned_len = PAGE_ALIGN_UP(len);
  uint64_t num_pages = aligned_len / PAGE_SIZE;
  uint64_t *pml4 = vmm_get_active_pml4();
  uint64_t new_flags = build_page_flags(prot);

  for (uint64_t i = 0; i < num_pages; i++) {
    uint64_t va = addr + i * PAGE_SIZE;
    uint64_t phys = vmm_virt_to_phys(pml4, va);

    if (phys == 0) {
      // Page not mapped - skip it (Linux allows this)
      continue;
    }

    // Remap with new protection flags
    vmm_unmap_page(pml4, va);
    if (!vmm_map_page(pml4, va, phys, new_flags)) {
      return (uint64_t)-12; // ENOMEM
    }
  }

  return 0; // Success
}

// ── Registration ────────────────────────────────────────────────────────────
void syscall_register_mm(void) {
  syscall_register(SYS_MMAP, sys_mmap);
  syscall_register(SYS_MUNMAP, sys_munmap);
  syscall_register(SYS_BRK, sys_brk);
  syscall_register(SYS_MPROTECT, sys_mprotect);
}

// ── Reset mmap state (called from process_exec on new process load) ─────────
void mm_reset_mmap_state(void) { mmap_next_addr = MMAP_REGION_BASE; }

// ── Allocate virtual address range from mmap region (for device mmap handlers)
// -- Returns the base virtual address, or 0 on failure.
uint64_t mm_alloc_mmap_region(uint64_t length) {
  if (length == 0)
    return 0;

  uint64_t aligned_len = PAGE_ALIGN_UP(length);

  if (mmap_next_addr + aligned_len > MMAP_REGION_LIMIT) {
    return 0; // Region exhausted
  }

  uint64_t vaddr = mmap_next_addr;
  mmap_next_addr = vaddr + aligned_len;
  return vaddr;
}
