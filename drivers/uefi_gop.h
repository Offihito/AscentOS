#pragma once

#include <stdint.h>

// ============================================================================
// UEFI Graphics Output Protocol (GOP) Definitions
// UEFI Spec: https://uefi.org/specs/UEFI/2.10/12_Protocols_Parameter_Block.html#efi-graphics-output-protocol
// ============================================================================

// UEFI Status type
typedef uint64_t EFI_STATUS;
#define EFI_SUCCESS 0
#define EFI_INVALID_PARAMETER 2
#define EFI_DEVICE_ERROR 7
#define EFI_UNSUPPORTED 3

// UEFI Pixel format for GOP
typedef enum {
    PixelRedGreenBlueReserved8BitPerColor = 0,  // R8G8B8X8
    PixelBlueGreenRedReserved8BitPerColor = 1,  // B8G8R8X8
    PixelBitMask = 2,                            // Custom
    PixelBltOnly = 3,                            // No framebuffer access
    PixelFormatMax = 4,
} EFI_GRAPHICS_PIXEL_FORMAT;

// GOP Blit Operation
typedef enum {
    EfiBltVideoFill = 0,
    EfiBltVideoToBltBuffer = 1,
    EfiBltBufferToVideo = 2,
    EfiBltVideoToVideo = 3,
    EfiGraphicsOutputBltOperationMax = 4,
} EFI_GRAPHICS_OUTPUT_BLT_OPERATION;

// Pixel color representation
typedef union {
    uint32_t Raw;
    struct {
        uint8_t Blue;
        uint8_t Green;
        uint8_t Red;
        uint8_t Reserved;
    } Pixel;
} EFI_GRAPHICS_OUTPUT_BLT_PIXEL;

// Pixel information
typedef struct {
    uint32_t RedMask;
    uint32_t GreenMask;
    uint32_t BlueMask;
    uint32_t ReservedMask;
} EFI_PIXEL_BITMASK;

// Pixel format info
typedef struct {
    EFI_GRAPHICS_PIXEL_FORMAT Format;
    EFI_PIXEL_BITMASK PixelInformation;
    uint32_t PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

// GOP Mode
typedef struct {
    uint32_t MaxMode;
    uint32_t Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION* Info;
    uint64_t SizeOfInfo;
    uint64_t FrameBufferBase;
    uint64_t FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

// GOP Protocol
typedef struct EFI_GRAPHICS_OUTPUT_PROTOCOL {
    // Queries
    EFI_STATUS (*QueryMode)(
        struct EFI_GRAPHICS_OUTPUT_PROTOCOL* This,
        uint32_t ModeNumber,
        uint64_t* SizeOfInfo,
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION** Info
    );
    
    // Sets video mode
    EFI_STATUS (*SetMode)(
        struct EFI_GRAPHICS_OUTPUT_PROTOCOL* This,
        uint32_t ModeNumber
    );
    
    // Blit
    EFI_STATUS (*Blt)(
        struct EFI_GRAPHICS_OUTPUT_PROTOCOL* This,
        EFI_GRAPHICS_OUTPUT_BLT_PIXEL* BltBuffer,
        EFI_GRAPHICS_OUTPUT_BLT_OPERATION BltOperation,
        uint64_t SourceX, uint64_t SourceY,
        uint64_t DestinationX, uint64_t DestinationY,
        uint64_t Width, uint64_t Height,
        uint64_t Delta
    );
    
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE* Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

// ============================================================================
// GOP Driver Functions (called from bootloader)
// ============================================================================

// Find and initialize GOP
EFI_STATUS gop_find_mode(EFI_GRAPHICS_OUTPUT_PROTOCOL* gop,
                          uint32_t target_width,
                          uint32_t target_height,
                          uint32_t* best_mode);

// Set GOP mode and return framebuffer info
EFI_STATUS gop_set_mode(EFI_GRAPHICS_OUTPUT_PROTOCOL* gop,
                         uint32_t mode_num,
                         uint64_t* fb_addr,
                         uint32_t* width,
                         uint32_t* height,
                         uint32_t* pitch);

// Query available modes
void gop_list_modes(EFI_GRAPHICS_OUTPUT_PROTOCOL* gop);
