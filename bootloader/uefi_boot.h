#pragma once

#include <stdint.h>

// ============================================================================
// UEFI OsLoader Entry Point
// UEFI Boot Services'ten tarafından farklı sistemlerde çağrılabilir.
// Bootloader'ın kernel'e framebuffer bilgisini ve diğer firmwares info'sunu
// geçmesi için kullanılan mekanizma.
// ============================================================================

// efi_main argument: UEFI ImageHandle ve SystemTable
typedef struct {
    uint64_t image_handle;
    uint64_t system_table;
} EFI_ENTRY_ARGS;

// Kernel'e iletilecek bilgilendirme (bootloader tarafından doldurulur)
typedef struct {
    // Graphics bilgileri
    uint64_t gfx_fb_addr;
    uint32_t gfx_width;
    uint32_t gfx_height;
    uint32_t gfx_pitch;
    uint32_t gfx_mode;  // 1=GOP, 2=VESA, 3=Text
    
    // UEFI info
    uint64_t memory_map;
    uint32_t memory_map_size;
    
    // Debug/Version
    uint32_t loader_version;
    char loader_name[32];
} UEFI_BOOT_INFO;

extern UEFI_BOOT_INFO uefi_boot_info;

// ============================================================================
// UEFI Bootloader entrypoint (Assembly)
// x86-64 System V ABI'ye uygun:
//   rdi = ImageHandle
//   rsi = SystemTable
// ============================================================================

// efi_main — Bootloader main entry (C fonksiyonu)
int efi_main(void* image_handle, void* system_table);

// _start — Assembly entry (efi_main'ı çağırır)
extern void _start(void);
