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

// ── Linux mmap constants ─────────────────────────────────────────────────────
#define PROT_NONE 0x0
#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define PROT_EXEC 0x4

#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20

#define MAP_FAILED ((uint64_t)-1)

// ── Errno constants ──────────────────────────────────────────────────────────
#define E_INVAL ((uint64_t)-22)
#define E_NOMEM ((uint64_t)-12)
#define E_BADF ((uint64_t)-9)

// ── Helpers ──────────────────────────────────────────────────────────────────

#define PAGE_ALIGN_UP(x) (((x) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))
#define PAGE_ALIGN_DOWN(x) ((x) & ~(PAGE_SIZE - 1))
#define HHDM_OFFSET pmm_get_hhdm_offset()
#define PML4_PHYS_TO_VIRT(p) ((uint64_t *)(((uint64_t)(p)) + HHDM_OFFSET))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

// User-space pointer validation: reject anything above canonical user range.
#define USER_ADDR_MAX 0x00007FFFFFFFFFFFULL
static inline bool is_user_pointer(uint64_t addr) {
  return addr <= USER_ADDR_MAX;
}

static uint64_t build_page_flags(uint64_t prot) {
  // PROT_NONE → no flags at all (page must stay non-present).
  // On x86-64 there is no "read disable" bit; PRESENT alone grants reads.
  if (prot == PROT_NONE)
    return 0;

  uint64_t flags = PAGE_FLAG_PRESENT | PAGE_FLAG_USER;
  if (prot & PROT_WRITE)
    flags |= PAGE_FLAG_RW;
  if (!(prot & PROT_EXEC))
    flags |= PAGE_FLAG_NX;
  return flags;
}

// ── Anonymous mapping ────────────────────────────────────────────────────
#define MMAP_REGION_BASE 0x7F0000000000ULL
#define MMAP_REGION_LIMIT 0x7FF000000000ULL

// ════════════════════════════════════════════════════════════════════════════
// INTERNAL HELPERS
// ════════════════════════════════════════════════════════════════════════════

static void safe_unmap_and_free(uint64_t *pml4, uint64_t va, uint64_t phys,
                                bool free_phys, const char *ctx) {
  (void)ctx;
  // Step 1: remove the PTE.  After this the frame is unreachable via this VA.
  vmm_unmap_page(pml4, va);

  // Step 2: only now is it safe to recycle the frame.
  if (free_phys && phys != 0) {
    pmm_free((void *)phys);
  }
}

// teardown_range:
//   Unmap [base, base+len) and free anonymous private frames.
//   Used by both MAP_FIXED pre-teardown and munmap proper.
//   Does NOT touch the VMA list — callers manage that themselves.
static void teardown_range(uint64_t *pml4, struct thread *t, uint64_t base,
                           uint64_t len, const char *ctx) {
  uint64_t end = base + len;

  for (uint64_t va = base; va < end; va += PAGE_SIZE) {
    uint64_t phys = vmm_virt_to_phys(pml4, va);
    if (phys == 0)
      continue; // Already unmapped, nothing to do.

    phys = PAGE_ALIGN_DOWN(phys); // Strip low flag bits VMM may leave set.

    // Only free frames we own: anonymous private mappings.
    bool free_phys = false;
    if (t) {
      struct vma *v = vma_find(&t->vmas, va);
      if (v && v->fd == -1 && (v->flags & MAP_PRIVATE) &&
          (v->flags & MAP_ANONYMOUS)) {
        free_phys = true;
      }
    }

    safe_unmap_and_free(pml4, va, phys, free_phys, ctx);
  }
}

// ════════════════════════════════════════════════════════════════════════════
// sys_mmap
// Linux ABI: mmap(addr, length, prot, flags, fd, offset)
//   rdi=addr  rsi=length  rdx=prot  r10=flags  r8=fd  r9=offset
// ════════════════════════════════════════════════════════════════════════════
static uint64_t sys_mmap(uint64_t addr, uint64_t length, uint64_t prot,
                         uint64_t flags, uint64_t fd, uint64_t offset) {
  (void)offset;

  klog_puts("[MMAP] addr=");
  klog_uint64(addr);
  klog_puts(" len=");
  klog_uint64(length);
  klog_puts(" prot=0x");
  klog_uint64(prot);
  klog_puts(" flags=0x");
  klog_uint64(flags);
  klog_puts("\n");

  // ── Validate flags ───────────────────────────────────────────────────────
  bool is_shared = (flags & MAP_SHARED) != 0;
  bool is_private = (flags & MAP_PRIVATE) != 0;

  if (!is_shared && !is_private) {
    klog_puts("[MMAP] Error: must specify MAP_SHARED or MAP_PRIVATE\n");
    return MAP_FAILED;
  }
  if (is_shared && is_private) {
    klog_puts(
        "[MMAP] Error: MAP_SHARED and MAP_PRIVATE are mutually exclusive\n");
    return MAP_FAILED;
  }
  if (length == 0) {
    klog_puts("[MMAP] Error: length == 0\n");
    return MAP_FAILED;
  }

  // ── File-backed mapping ──────────────────────────────────────────────────
  if (!(flags & MAP_ANONYMOUS) && (int64_t)fd != -1) {
    struct thread *t = sched_get_current();
    if (!t || fd >= MAX_FDS || !t->fds[fd]) {
      klog_puts("[MMAP] Error: invalid fd\n");
      return MAP_FAILED;
    }
    vfs_node_t *node = t->fds[fd];
    if (!node->mmap) {
      klog_puts("[MMAP] Error: device does not support mmap\n");
      return MAP_FAILED;
    }

    uint64_t result = node->mmap(node, length, prot, flags);
    if (result == MAP_FAILED || result == (uint64_t)-1)
      return MAP_FAILED;

    uint64_t aligned_len = PAGE_ALIGN_UP(length);
    int vma_idx = vma_add(&t->vmas, result, result + aligned_len, prot, flags,
                          (int)fd, 0);
    if (vma_idx < 0)
      klog_puts("[MMAP] Warning: failed to register VMA for file mapping\n");

    return result;
  }

  // ── Reject non-anonymous mappings with no fd ─────────────────────────────
  if (!(flags & MAP_ANONYMOUS)) {
    klog_puts("[MMAP] Error: non-anonymous mapping requires a valid fd\n");
    return MAP_FAILED;
  }

  // ── Anonymous mapping ────────────────────────────────────────────────────
  uint64_t aligned_len = PAGE_ALIGN_UP(length);
  uint64_t num_pages = aligned_len / PAGE_SIZE;

  struct thread *current = sched_get_current();
  uint64_t *pml4 = vmm_get_active_pml4();

  // ── Determine virtual address ────────────────────────────────────────────
  uint64_t vaddr;

  if (flags & MAP_FIXED) {
    // MAP_FIXED: addr must be non-null and page-aligned.
    if (addr == 0 || (addr & (PAGE_SIZE - 1))) {
      klog_puts(
          "[MMAP] Error: MAP_FIXED requires non-null page-aligned addr\n");
      return MAP_FAILED;
    }
    if (!is_user_pointer(addr) || !is_user_pointer(addr + aligned_len - 1)) {
      klog_puts("[MMAP] Error: MAP_FIXED addr outside user range\n");
      return MAP_FAILED;
    }
    vaddr = addr;

    // Tear down any existing mappings in the target range BEFORE allocating
    // new frames.  This ensures no stale PTEs exist when we begin mapping,
    // and that the old frames are back in PMM before new ones are requested.
    klog_puts("[MMAP] MAP_FIXED teardown [");
    klog_uint64(vaddr);
    klog_puts(", ");
    klog_uint64(vaddr + aligned_len);
    klog_puts(")\n");

    teardown_range(pml4, current, vaddr, aligned_len, "MAP_FIXED teardown");

    if (current)
      vma_remove(&current->vmas, vaddr, vaddr + aligned_len);

  } else {
    // Non-fixed: allocate dynamically utilizing AVL Interval Gap Finding.
    // Automatically drops disjoint chunks inside explicitly unmapped holes
    // rather than blindly advancing the ceiling ceiling!
    vaddr = vma_find_gap(&current->vmas, aligned_len, MMAP_REGION_BASE, MMAP_REGION_LIMIT);
    if (vaddr == 0 || vaddr + aligned_len > MMAP_REGION_LIMIT) {
      klog_puts("[MMAP] Error: mmap region exhausted (no mapping gaps found)\n");
      return MAP_FAILED;
    }
    
    // We update the bump tracker loosely for legacy device-mmaps
    current->mmap_next_addr = MAX(current->mmap_next_addr, vaddr + aligned_len);
  }

  // ── PROT_NONE shortcut ────────────────────────────────────────────────────
  // On x86-64 there is no "read-disable" bit; any PRESENT page is readable.
  // For a true PROT_NONE mapping we must NOT create any PTEs.  We only
  // record the VMA so the address range is reserved, but leave the pages
  // non-present.  Any access will page-fault, which is the correct
  // PROT_NONE behaviour.
  if (prot == PROT_NONE) {
    klog_puts("[MMAP] PROT_NONE: reserving VMA only (no PTEs)\n");
    goto register_vma;
  }

  // ── Allocate frames and map them ─────────────────────────────────────────
  // We record every physical frame we allocate so that OOM rollback can free
  // them precisely — no guessing, no leaks.
  //
  // Ordering contract: pmm_alloc → vmm_map_page → memset (via HHDM).
  // We never touch vmm_unmap_page before the frame is mapped; there is
  // nothing to unmap until the PTE exists.

  uint64_t page_flags = build_page_flags(prot);

  uint64_t mapped_count = 0; // How many pages have a live PTE so far.

  for (uint64_t i = 0; i < num_pages; i++) {
    // Step A: allocate a physical frame.
    void *phys = pmm_alloc();
    if (!phys) {
      klog_puts("[MMAP] OOM at page ");
      klog_uint64(i);
      klog_puts(" — rolling back\n");
      goto oom_rollback;
    }

    // Step B: install the PTE.
    if (!vmm_map_page(pml4, vaddr + i * PAGE_SIZE, (uint64_t)phys,
                      page_flags)) {
      klog_puts("[MMAP] vmm_map_page failed at page ");
      klog_uint64(i);
      klog_puts(" — rolling back\n");
      // The frame at index i has no PTE yet; free it directly.
      pmm_free(phys);
      goto oom_rollback;
    }
    mapped_count = i + 1;

    // Step C: zero the frame through the HHDM kernel alias.
    // We must NOT use the user-space VA from kernel context.
    void *kva = (void *)((uint64_t)phys + HHDM_OFFSET);
    memset(kva, 0, PAGE_SIZE);
  }

register_vma:
  // ── Register VMA ─────────────────────────────────────────────────────────
  if (current) {
    int vma_idx =
        vma_add(&current->vmas, vaddr, vaddr + aligned_len, prot, flags, -1, 0);
    if (vma_idx < 0)
      klog_puts(
          "[MMAP] Warning: VMA registration failed (mapping still valid)\n");
  }

  klog_puts("[MMAP] ");
  klog_uint64(aligned_len);
  klog_puts(" bytes at ");
  klog_uint64(vaddr);
  klog_puts(is_shared ? " SHARED\n" : " PRIVATE\n");

  return vaddr;

oom_rollback:
  // Undo every PTE we installed, then free the backing frames.
  // Ordering: unmap FIRST (PTE gone), free SECOND (frame recyclable).
  for (uint64_t j = 0; j < mapped_count; j++) {
    uint64_t phys = vmm_virt_to_phys(pml4, vaddr + j * PAGE_SIZE);
    if (phys != 0) {
      phys = PAGE_ALIGN_DOWN(phys);
      vmm_unmap_page(pml4, vaddr + j * PAGE_SIZE);
      pmm_free((void *)phys);
    }
  }
  return MAP_FAILED;
}

// ════════════════════════════════════════════════════════════════════════════
// sys_munmap
// Linux ABI: munmap(addr, length)
//   rdi=addr  rsi=length
// ════════════════════════════════════════════════════════════════════════════
static uint64_t sys_munmap(uint64_t addr, uint64_t length, uint64_t a2,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  // ── Validate arguments ───────────────────────────────────────────────────
  if (addr == 0 || length == 0)
    return E_INVAL;
  if (addr & (PAGE_SIZE - 1))
    return E_INVAL; // addr must be page-aligned (POSIX).
  if (!is_user_pointer(addr) || !is_user_pointer(addr + length - 1))
    return E_INVAL;

  struct thread *current = sched_get_current();
  if (!current)
    return E_INVAL;

  // Require the base address to be tracked.  This prevents userspace from
  // using munmap to tear down ELF segments or the stack by passing an
  // arbitrary address.
  if (!vma_find(&current->vmas, addr)) {
    klog_puts("[MUNMAP] addr not in VMA list, ignoring\n");
    return E_INVAL;
  }

  uint64_t aligned_len = PAGE_ALIGN_UP(length);
  uint64_t *pml4 = vmm_get_active_pml4();

  // Capture the MAP_SHARED flag from the first VMA for the log message.
  struct vma *first_vma = vma_find(&current->vmas, addr);
  bool is_shared = first_vma && (first_vma->flags & MAP_SHARED);

  klog_puts("[MUNMAP] [");
  klog_uint64(addr);
  klog_puts(", ");
  klog_uint64(addr + aligned_len);
  klog_puts(")\n");

  // ── Unmap and free ───────────────────────────────────────────────────────
  // teardown_range handles the unmap-before-free ordering and guards.
  // It consults the VMA list to decide whether each frame is owned by us.
  teardown_range(pml4, current, addr, aligned_len, "sys_munmap");

  // ── Remove VMAs ──────────────────────────────────────────────────────────
  // Do this after teardown so teardown_range can still query them above.
  vma_remove(&current->vmas, addr, addr + aligned_len);
  
  // ── Consolidate Fragmented Memory ────────────────────────────────────────
  vma_merge_adjacent(&current->vmas);

  klog_puts("[MUNMAP] Done ");
  klog_uint64(aligned_len);
  klog_puts(" bytes at ");
  klog_uint64(addr);
  klog_puts(is_shared ? " SHARED\n" : " PRIVATE\n");

  return 0;
}

// ════════════════════════════════════════════════════════════════════════════
// sys_brk  (unchanged from original — included for completeness)
// Linux ABI: brk(addr)  —  rdi=addr
// Returns the current/new program break.
// ════════════════════════════════════════════════════════════════════════════
static uint64_t sys_brk(uint64_t addr, uint64_t a1, uint64_t a2, uint64_t a3,
                        uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  struct thread *current = sched_get_current();
  if (!current)
    return 0;

  // Linux ABI: brk(0) returns current break.
  if (addr == 0) {
    klog_puts("[BRK] Query: base=");
    klog_uint64(current->brk_base);
    klog_puts(" current=");
    klog_uint64(current->brk_current);
    klog_puts("\n");
    return current->brk_current;
  }

  // Check bounds.
  if (!is_user_pointer(addr))
    return current->brk_current;

  uint64_t *pml4 = vmm_get_active_pml4();

  if (addr > current->brk_current) {
    // Expand heap
    uint64_t old_end = PAGE_ALIGN_UP(current->brk_current);
    uint64_t new_end = PAGE_ALIGN_UP(addr);

    klog_puts("[BRK] Expanding ");
    klog_uint64(current->brk_current);
    klog_puts(" → ");
    klog_uint64(addr);
    klog_puts(" (base=");
    klog_uint64(current->brk_base);
    klog_puts(", pages ");
    klog_uint64(old_end);
    klog_puts("→");
    klog_uint64(new_end);
    klog_puts(")\n");

    for (uint64_t page = old_end; page < new_end; page += PAGE_SIZE) {
      void *phys = pmm_alloc();
      if (!phys) {
        klog_puts("[BRK] OOM at page ");
        klog_uint64(page);
        klog_puts("\n");
        return current->brk_current; // Return unchanged break on OOM.
      }

      if (!vmm_map_page(pml4, page, (uint64_t)phys,
                        PAGE_FLAG_PRESENT | PAGE_FLAG_USER | PAGE_FLAG_RW |
                            PAGE_FLAG_NX)) {
        klog_puts("[BRK] vmm_map_page failed\n");
        pmm_free(phys);
        return current->brk_current;
      }

      void *kva = (void *)((uint64_t)phys + HHDM_OFFSET);
      memset(kva, 0, PAGE_SIZE);
    }

    // Add VMA for the expanded region so it's tracked.
    if (new_end > old_end) {
      vma_add(&current->vmas, old_end, new_end, PROT_READ | PROT_WRITE,
              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    }

    current->brk_current = addr;

  } else if (addr < current->brk_current && addr >= current->brk_base) {
    uint64_t old_end = PAGE_ALIGN_UP(current->brk_current);
    uint64_t new_end = PAGE_ALIGN_UP(addr);

    klog_puts("[BRK] Shrinking ");
    klog_uint64(current->brk_current);
    klog_puts(" → ");
    klog_uint64(addr);
    klog_puts("\n");

    for (uint64_t page = new_end; page < old_end; page += PAGE_SIZE) {
      uint64_t phys = vmm_virt_to_phys(pml4, page);
      if (phys != 0) {
        phys = PAGE_ALIGN_DOWN(phys);
        // brk pages are always anonymous+private, so always free.
        safe_unmap_and_free(pml4, page, phys, true, "sys_brk shrink");
      }
    }

    vma_remove(&current->vmas, new_end, old_end);
    current->brk_current = addr;
  }
  // addr < current->brk_base: silently ignore, return current break.

  return current->brk_current;
}

// ════════════════════════════════════════════════════════════════════════════
// sys_mprotect  (unchanged from original — included for completeness)
// Linux ABI: mprotect(addr, len, prot)
// ════════════════════════════════════════════════════════════════════════════
static uint64_t sys_mprotect(uint64_t addr, uint64_t len, uint64_t prot,
                             uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;

  if (addr & (PAGE_SIZE - 1))
    return E_INVAL;
  if (len == 0)
    return 0;

  uint64_t aligned_len = PAGE_ALIGN_UP(len);
  uint64_t num_pages = aligned_len / PAGE_SIZE;
  uint64_t *pml4 = vmm_get_active_pml4();
  uint64_t new_flags = build_page_flags(prot);

  for (uint64_t i = 0; i < num_pages; i++) {
    uint64_t va = addr + i * PAGE_SIZE;
    uint64_t phys = vmm_virt_to_phys(pml4, va);

    if (phys == 0) {
      // Page not present.  If the new prot is PROT_NONE, nothing to do.
      if (prot == PROT_NONE)
        continue;

      // Transitioning from PROT_NONE → accessible: allocate a frame.
      void *new_phys = pmm_alloc();
      if (!new_phys)
        return E_NOMEM;
      void *kva = (void *)((uint64_t)new_phys + HHDM_OFFSET);
      memset(kva, 0, PAGE_SIZE);
      if (!vmm_map_page(pml4, va, (uint64_t)new_phys, new_flags)) {
        pmm_free(new_phys);
        return E_NOMEM;
      }
      continue;
    }

    if (prot == PROT_NONE) {
      // Transitioning to PROT_NONE: remove PTE and free frame.
      vmm_unmap_page(pml4, va);
      pmm_free((void *)PAGE_ALIGN_DOWN(phys));
      continue;
    }

    // Remap with new protection. Unmap first so there is no window
    // where two PTEs for different protection levels coexist.
    vmm_unmap_page(pml4, va);
    if (!vmm_map_page(pml4, va, PAGE_ALIGN_DOWN(phys), new_flags))
      return E_NOMEM;
  }

  return 0;
}

// ════════════════════════════════════════════════════════════════════════════
// Public API
// ════════════════════════════════════════════════════════════════════════════

void syscall_register_mm(void) {
  syscall_register(SYS_MMAP, sys_mmap);
  syscall_register(SYS_MUNMAP, sys_munmap);
  syscall_register(SYS_BRK, sys_brk);
  syscall_register(SYS_MPROTECT, sys_mprotect);
}
// Called from process_exec when loading a new process image.
void mm_reset_mmap_state(struct thread *t) {
  if (!t)
    return;
  t->mmap_next_addr = MMAP_REGION_BASE;
}

// Allocate a virtual address range from the mmap region for device mmap
// handlers (e.g. framebuffer, DMA).  Returns the base VA, or 0 on failure.
uint64_t mm_alloc_mmap_region(uint64_t length) {
  if (length == 0)
    return 0;

  struct thread *current = sched_get_current();
  if (!current)
    return 0;

  uint64_t aligned_len = PAGE_ALIGN_UP(length);
  if (current->mmap_next_addr + aligned_len > MMAP_REGION_LIMIT)
    return 0;

  uint64_t vaddr = current->mmap_next_addr;
  current->mmap_next_addr += aligned_len;
  return vaddr;
}
