/* ============================================================================
   UEFI OsLoader for AscentOS
   
   Bootloader compiled via: make uefi (runs uefi-build.sh)
   Kernel handoff: System V AMD64 ABI (rdi = LOADER_INFO*)
   ============================================================================ */
/* ============================================================================
   UEFI Common Definitions
   ============================================================================ */

#include <efi.h>
#include <efilib.h>
#include <stdio.h>
#include <string.h>

#define PAGE_SIZE 0x1000

/* UEFI Graphics Output Protocol GUIDs */
EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;

/* Simple file system protocol GUID (for loading kernel) */
EFI_GUID sfs_guid = SIMPLE_FILE_SYSTEM_PROTOCOL;

/* ============================================================================
   Memory Information Structure (passed to kernel via register handoff)
   ============================================================================ */

typedef struct {
    uint64_t total_memory;
    uint64_t usable_memory;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_bpp;
} LOADER_INFO;

/* ============================================================================
   Inline Assembly Helpers (x86-64)
   ============================================================================ */

/* Jump to kernel without return (64-bit rip handoff) */
static void __attribute__((noreturn)) jump_to_kernel(uint64_t entry, LOADER_INFO *info)
{
    /* Pass info pointer in rdi (System V AMD64 ABI first argument) */
    __asm__ __volatile__(
        "mov %0, %%rdi\n\t"     // rdi = info pointer
        "mov %1, %%rax\n\t"     // rax = kernel entry
        "jmp *%%rax\n\t"        // Jump to kernel (no return)
        :
        : "r"((uint64_t)info), "r"(entry)
        : "rax", "rdi"
    );
    
    while(1);  // Never reached
}

/* Graphics Output Protocol Interface */

static EFI_STATUS find_gop_mode(
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop,
    uint32_t target_width,
    uint32_t target_height,
    uint32_t *best_mode)
{
    if (!gop || !gop->Mode || !best_mode)
        return EFI_INVALID_PARAMETER;

    uint32_t max_mode = gop->Mode->MaxMode;
    uint32_t best = 0;
    int found = 0;

    Print(L"[UEFI] Available graphics modes:\n");

    uint32_t fallback_mode = 0;
    int fallback_found = 0;

    for (uint32_t mode = 0; mode < max_mode && mode < 32; mode++) {
        UINTN size = 0;
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = NULL;

        EFI_STATUS status = uefi_call_wrapper(gop->QueryMode, 4, gop, mode, &size, &info);
        
        if (EFI_ERROR(status)) continue;

        uint32_t w = info->HorizontalResolution;
        uint32_t h = info->VerticalResolution;

        Print(L"  Mode %d: %ux%u (stride=%u, fmt=%d)\n", mode, w, h,
              info->PixelsPerScanLine, info->PixelFormat);

        // Exact match preferred
        if (w == target_width && h == target_height && !found) {
            best = mode;
            found = 1;
        }
        // Fallback: en büyük çözünürlüğü seç (1280x720'den küçükse)
        if (!found && !fallback_found && w >= 1024 && h >= 768) {
            fallback_mode = mode;
            fallback_found = 1;
        }
    }

    if (!found) {
        if (fallback_found) {
            Print(L"[UEFI] No exact match for %ux%u, using fallback mode %d\n",
                  target_width, target_height, fallback_mode);
            best = fallback_mode;
        } else {
            Print(L"[UEFI] No exact match for %ux%u, using mode 0\n",
                  target_width, target_height);
            best = 0;
        }
    }

    *best_mode = best;
    return EFI_SUCCESS;
}

static EFI_STATUS set_gop_mode(
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop,
    uint32_t mode,
    LOADER_INFO *info)
{
    if (!gop || !info)
        return EFI_INVALID_PARAMETER;

    EFI_STATUS status = uefi_call_wrapper(gop->SetMode, 2, gop, mode);
    if (EFI_ERROR(status)) {
        Print(L"[UEFI] SetMode failed: 0x%lx\n", status);
        return status;
    }

    if (gop->Mode && gop->Mode->Info) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *minfo = gop->Mode->Info;
        
        info->framebuffer_addr = gop->Mode->FrameBufferBase;
        info->framebuffer_width = minfo->HorizontalResolution;
        info->framebuffer_height = minfo->VerticalResolution;
        info->framebuffer_pitch = minfo->PixelsPerScanLine * 4;  // 32-bit pixel
        info->framebuffer_bpp = 32;

        Print(L"[UEFI] GOP mode set: %ux%u @ 0x%lx\n",
              info->framebuffer_width, info->framebuffer_height, 
              info->framebuffer_addr);
    }

    return EFI_SUCCESS;
}

/* ============================================================================
   Kernel Loading (stub)
   
   Real implementation would load kernel.elf from disk/network.
   For now, this assumes kernel is embedded or loaded separately.
   ============================================================================ */
// ============================================================================

static uint64_t load_kernel_from_memory(void)
{
    /* In a real bootloader, this would:
       1. Open a file handle to kernel.elf
       2. Read and parse ELF header
       3. Load segments to their specified addresses
       4. Return entry point
       
       For UEFI QEMU testing with GRUB2 as pseudo-bootloader:
       Kernel is typically loaded at 0x100000 (1MB).
       This is just a fallback return value. */
    
    Print(L"[UEFI] Kernel loading stub (real implementation pending)\n");
    return 0x100000;  /* Default kernel entry point */
}

/* ============================================================================
   UEFI Main Entry Point
   ============================================================================ */

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
    InitializeLib(ImageHandle, SystemTable);

    Print(L"╔════════════════════════════════════════╗\n");
    Print(L"║  AscentOS UEFI OsLoader v1.0          ║\n");
    Print(L"║  Graphics Output Protocol Bootloader  ║\n");
    Print(L"╚════════════════════════════════════════╝\n");

    /* Allocate loader info structure */
    LOADER_INFO *info = AllocatePool(sizeof(LOADER_INFO));
    if (!info) {
        Print(L"[UEFI ERROR] Failed to allocate loader info\n");
        return EFI_OUT_OF_RESOURCES;
    }

    ZeroMem(info, sizeof(LOADER_INFO));

    /* Find and initialize GOP */
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    EFI_STATUS status = uefi_call_wrapper(
        BS->LocateProtocol,
        3,
        &gop_guid,
        NULL,
        (void**)&gop
    );

    if (EFI_ERROR(status)) {
        Print(L"[UEFI] GraphicsOutput not found (expected on legacy BIOS)\n");
        Print(L"[UEFI] Falling back to text mode\n");
        
        /* Set dummy framebuffer info for fallback */
        info->framebuffer_addr = 0x0B8000;  /* VGA text mode */
        info->framebuffer_width = 80;
        info->framebuffer_height = 25;
        info->framebuffer_bpp = 16;
    } else {
        Print(L"[UEFI] GraphicsOutput protocol found ✓\n");

        /* Find best mode */
        uint32_t best_mode = 0;
        status = find_gop_mode(gop, 1280, 720, &best_mode);

        if (!EFI_ERROR(status)) {
            status = set_gop_mode(gop, best_mode, info);
            if (EFI_ERROR(status)) {
                Print(L"[UEFI ERROR] Could not set GOP mode\n");
            }
        }
    }

    /* Get memory map */
    EFI_MEMORY_DESCRIPTOR *mmap = NULL;
    UINTN mmap_size = 0, map_key = 0, desc_size = 0;
    UINT32 desc_version = 0;

    /* First call to get size */
    status = uefi_call_wrapper(
        BS->GetMemoryMap,
        5,
        &mmap_size,
        mmap,
        &map_key,
        &desc_size,
        &desc_version
    );

    if (status == EFI_BUFFER_TOO_SMALL) {
        mmap_size += 2 * desc_size;  /* Extra space */
        mmap = AllocatePool(mmap_size);

        status = uefi_call_wrapper(
            BS->GetMemoryMap,
            5,
            &mmap_size,
            mmap,
            &map_key,
            &desc_size,
            &desc_version
        );
    }

    if (!EFI_ERROR(status)) {
        Print(L"[UEFI] Memory map obtained: %lu bytes\n", mmap_size);
        
        /* Calculate total usable memory */
        uint64_t total_mem = 0;
        EFI_MEMORY_DESCRIPTOR *desc = mmap;
        
        for (UINTN i = 0; i < mmap_size / desc_size; i++) {
            if (desc->Type == EfiConventionalMemory) {
                total_mem += desc->NumberOfPages * PAGE_SIZE;
            }
            desc = (EFI_MEMORY_DESCRIPTOR*)((UINT8*)desc + desc_size);
        }

        info->total_memory = total_mem;
        info->usable_memory = total_mem;

        Print(L"[UEFI] Total memory: %lu MB\n", total_mem / (1024 * 1024));
    }

    // ── 4. Load kernel ──────────────────────────────────────────────────
    Print(L"[UEFI] Loading kernel...\n");
    uint64_t kernel_entry = load_kernel_from_memory();

    // ── 5. Exit boot services ───────────────────────────────────────────
    Print(L"[UEFI] Exiting boot services...\n");
    
    status = uefi_call_wrapper(BS->ExitBootServices, 2, ImageHandle, map_key);
    if (EFI_ERROR(status)) {
        Print(L"[UEFI] ExitBootServices failed, retrying with updated map\n");
        
        // Get updated map and retry (EntireMemoryMap may have changed)
        mmap_size = 0;
        status = uefi_call_wrapper(BS->GetMemoryMap, 5, &mmap_size, NULL, &map_key, &desc_size, &desc_version);
        if (status != EFI_BUFFER_TOO_SMALL) {
            Print(L"[UEFI ERROR] Cannot get updated memory map\n");
            return status;
        }
    }

    /* Boot services are now unavailable - disable interrupts */
    __asm__ __volatile__("cli");

    /* Jump to kernel */
    Print(L"[UEFI] Handing off to kernel @ 0x%lx...\n", kernel_entry);

    /* Pass loader info in rdi and jump */
    jump_to_kernel(kernel_entry, info);

    /* Never reached */
    return EFI_SUCCESS;
}