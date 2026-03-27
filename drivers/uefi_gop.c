// uefi_gop.c - UEFI Graphics Output Protocol Driver
// UEFI bootloader tarafından kullanılır (pre-kernel stage)
// GOP protokolünü kullanarak framebuffer bilgilerini elde eder

#include "uefi_gop.h"
#include <stddef.h>

// ============================================================================
// Helper Functions (bootloader stage — print fonksiyonları basit olabilir)
// ============================================================================

// Basit print (bootloader'in Print-like fonksiyonunu çağırır)
// Gerçek implementasyon bootloader tarafından sağlanır
extern void uefi_print_str(const char* str);
extern void uefi_print_uint64(uint64_t num);
extern void uefi_print_hex(uint64_t num, int width);

static void print_str(const char* str) {
    if (!str) return;
    uefi_print_str(str);
}

static void print_uint32(uint32_t num) {
    char buf[16], *p = buf + 15;
    *p = '\0';
    if (num == 0) { buf[14] = '0'; uefi_print_str(&buf[14]); return; }
    while (num > 0) { *(--p) = '0' + (num % 10); num /= 10; }
    uefi_print_str(p);
}

static void print_hex(uint64_t num) {
    uefi_print_hex(num, 16);
}

// ============================================================================
// GOP Mode Operations
// ============================================================================

EFI_STATUS gop_find_mode(EFI_GRAPHICS_OUTPUT_PROTOCOL* gop,
                          uint32_t target_width,
                          uint32_t target_height,
                          uint32_t* best_mode)
{
    if (!gop || !gop->Mode || !best_mode)
        return EFI_INVALID_PARAMETER;

    uint32_t max_mode = gop->Mode->MaxMode;
    uint32_t best_match = 0;
    int found = 0;

    print_str("[GOP] Searching for mode ");
    print_uint32(target_width);
    print_str("x");
    print_uint32(target_height);
    print_str("...\n");

    // Mode 0'dan başla (genellikle native/preferred mode)
    for (uint32_t mode = 0; mode < max_mode; mode++) {
        uint64_t info_size = 0;
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION* info = NULL;

        EFI_STATUS status = gop->QueryMode(gop, mode, &info_size, &info);
        if (status != EFI_SUCCESS) continue;

        uint32_t w = info->PixelsPerScanLine;
        uint32_t h = 0;
        // Height bilgisi QueryMode'dan doğrudan dönmez; mode'dan tahmin yapılır
        // Genellikle 16:9, 4:3 oranlarıdır

        print_str("  Mode ");
        print_uint32(mode);
        print_str(": ");
        print_uint32(w);
        print_str("x?");
        print_str(" Format=");
        print_uint32(info->Format);
        print_str("\n");

        // İlk bulduğumuz mode'u seç (genellikle en uygun)
        if (w == target_width && !found) {
            best_match = mode;
            found = 1;
        }
    }

    if (!found) {
        // Fallback: Mode 0 (genellikle preferred)
        best_match = 0;
        print_str("[GOP] Using default mode 0 (not exact match)\n");
    }

    *best_mode = best_match;
    return EFI_SUCCESS;
}

EFI_STATUS gop_set_mode(EFI_GRAPHICS_OUTPUT_PROTOCOL* gop,
                         uint32_t mode_num,
                         uint64_t* fb_addr,
                         uint32_t* width,
                         uint32_t* height,
                         uint32_t* pitch)
{
    if (!gop || !fb_addr || !width || !height || !pitch)
        return EFI_INVALID_PARAMETER;

    // Mode'u ayarla
    EFI_STATUS status = gop->SetMode(gop, mode_num);
    if (status != EFI_SUCCESS) {
        print_str("[GOP ERROR] SetMode failed: 0x");
        print_hex(status);
        print_str("\n");
        return status;
    }

    // Mode bilgisini oku
    if (!gop->Mode) {
        print_str("[GOP ERROR] Mode pointer is NULL\n");
        return EFI_DEVICE_ERROR;
    }

    *fb_addr = gop->Mode->FrameBufferBase;
    *width = gop->Mode->Info->PixelsPerScanLine;
    *pitch = *width * 4;  // RGBA/ARGB genellikle 32-bit
    
    // Height bilgisi: Some UEFI impls. bunları sağlamaz
    // Tahmin: 16:9 oranı
    if (*width == 1280) *height = 720;
    else if (*width == 1920) *height = 1080;
    else if (*width == 1024) *height = 768;
    else if (*width == 800) *height = 600;
    else *height = *width * 9 / 16;

    print_str("[GOP] Mode set successfully:\n");
    print_str("  FB Address: 0x");
    print_hex(*fb_addr);
    print_str("\n  Resolution: ");
    print_uint32(*width);
    print_str("x");
    print_uint32(*height);
    print_str("\n  Pitch: ");
    print_uint32(*pitch);
    print_str(" bytes/line\n");

    return EFI_SUCCESS;
}

void gop_list_modes(EFI_GRAPHICS_OUTPUT_PROTOCOL* gop)
{
    if (!gop || !gop->Mode) {
        print_str("[GOP ERROR] Invalid GOP pointer\n");
        return;
    }

    uint32_t max_mode = gop->Mode->MaxMode;
    print_str("[GOP] Available modes: ");
    print_uint32(max_mode);
    print_str("\n");

    for (uint32_t mode = 0; mode < max_mode && mode < 16; mode++) {
        uint64_t info_size = 0;
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION* info = NULL;

        EFI_STATUS status = gop->QueryMode(gop, mode, &info_size, &info);
        if (status != EFI_SUCCESS) continue;

        print_str("  [");
        print_uint32(mode);
        print_str("] ");
        print_uint32(info->PixelsPerScanLine);
        print_str("px Format=");
        print_uint32(info->Format);
        print_str("\n");
    }
}
