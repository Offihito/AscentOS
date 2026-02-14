#ifndef ELF64_H
#define ELF64_H

#include <stdint.h>

// ============================================================
//  ELF-64 Loader for AscentOS
//  Desteklenen format: ELF64, little-endian, x86-64
//  Executable (ET_EXEC) ve Position-Independent (ET_DYN)
//  ikilileri yükleyebilir.
// ============================================================

// ----------------------------------------------------------
// ELF Magic & Identification
// ----------------------------------------------------------
#define ELF_MAGIC0      0x7F
#define ELF_MAGIC1      'E'
#define ELF_MAGIC2      'L'
#define ELF_MAGIC3      'F'

// e_ident indices
#define EI_MAG0         0
#define EI_MAG1         1
#define EI_MAG2         2
#define EI_MAG3         3
#define EI_CLASS        4   // 1=32bit, 2=64bit
#define EI_DATA         5   // 1=LE, 2=BE
#define EI_VERSION      6
#define EI_OSABI        7
#define EI_NIDENT       16

// EI_CLASS values
#define ELFCLASS32      1
#define ELFCLASS64      2

// EI_DATA values
#define ELFDATA2LSB     1   // Little-endian

// e_type values
#define ET_NONE         0
#define ET_REL          1   // Relocatable
#define ET_EXEC         2   // Executable
#define ET_DYN          3   // Shared / PIE
#define ET_CORE         4

// e_machine values
#define EM_X86_64       62

// e_version
#define EV_CURRENT      1

// ----------------------------------------------------------
// ELF64 Header (64 bytes)
// ----------------------------------------------------------
typedef struct __attribute__((packed)) {
    uint8_t  e_ident[EI_NIDENT]; // Magic + class + data + version + os/abi
    uint16_t e_type;             // ET_EXEC / ET_DYN
    uint16_t e_machine;          // EM_X86_64
    uint32_t e_version;          // EV_CURRENT
    uint64_t e_entry;            // Entry point virtual address
    uint64_t e_phoff;            // Program header table offset
    uint64_t e_shoff;            // Section header table offset (unused by loader)
    uint32_t e_flags;            // Processor-specific flags
    uint16_t e_ehsize;           // ELF header size (64 bytes)
    uint16_t e_phentsize;        // Size of one program header entry
    uint16_t e_phnum;            // Number of program header entries
    uint16_t e_shentsize;        // Size of one section header entry
    uint16_t e_shnum;            // Number of section header entries
    uint16_t e_shstrndx;         // Section name string table index
} Elf64Header;

// ----------------------------------------------------------
// ELF64 Program Header (segment descriptor, 56 bytes)
// ----------------------------------------------------------
typedef struct __attribute__((packed)) {
    uint32_t p_type;    // Segment type
    uint32_t p_flags;   // Segment flags (PF_R/W/X)
    uint64_t p_offset;  // Offset in file
    uint64_t p_vaddr;   // Virtual address in memory
    uint64_t p_paddr;   // Physical address (usually = vaddr)
    uint64_t p_filesz;  // Bytes in file image (may be 0)
    uint64_t p_memsz;   // Bytes in memory (>= filesz, BSS zero-fill)
    uint64_t p_align;   // Alignment (must be power of two)
} Elf64Phdr;

// p_type values
#define PT_NULL         0
#define PT_LOAD         1   // Loadable segment
#define PT_DYNAMIC      2
#define PT_INTERP       3
#define PT_NOTE         4
#define PT_PHDR         6
#define PT_TLS          7

// p_flags bit masks
#define PF_X            0x1   // Execute
#define PF_W            0x2   // Write
#define PF_R            0x4   // Read

// ----------------------------------------------------------
// ELF64 Section Header (64 bytes, used only for diagnostics)
// ----------------------------------------------------------
typedef struct __attribute__((packed)) {
    uint32_t sh_name;
    uint32_t sh_type;
    uint64_t sh_flags;
    uint64_t sh_addr;
    uint64_t sh_offset;
    uint64_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint64_t sh_addralign;
    uint64_t sh_entsize;
} Elf64Shdr;

// ----------------------------------------------------------
// Loader result codes
// ----------------------------------------------------------
#define ELF_OK              0
#define ELF_ERR_NULL        1   // Null pointer passed
#define ELF_ERR_MAGIC       2   // Bad ELF magic
#define ELF_ERR_CLASS       3   // Not ELF64
#define ELF_ERR_ENDIAN      4   // Not little-endian
#define ELF_ERR_TYPE        5   // Not ET_EXEC or ET_DYN
#define ELF_ERR_MACHINE     6   // Not EM_X86_64
#define ELF_ERR_NOPHDR      7   // No LOAD segments found
#define ELF_ERR_PHENTSIZE   8   // Bad program header entry size
#define ELF_ERR_TOOBIG      9   // Segment too large for buffer
#define ELF_ERR_NOMEM       10  // Memory allocation failed

// ----------------------------------------------------------
// Loaded image descriptor
// Returned to the caller so it can execute or inspect the
// loaded binary.
// ----------------------------------------------------------
typedef struct {
    uint64_t entry;         // Resolved entry point (vaddr or load_base + offset)
    uint64_t load_base;     // Base address used for PIE binaries (0 for ET_EXEC)
    uint64_t load_min;      // Lowest loaded virtual address
    uint64_t load_max;      // Highest loaded virtual address
    int      segment_count; // Number of PT_LOAD segments mapped
} ElfImage;

// ----------------------------------------------------------
// Public API
// ----------------------------------------------------------

// Validate the ELF header in memory.
// buf      : pointer to the raw ELF bytes
// buf_size : total byte count available
// Returns ELF_OK on success, or an ELF_ERR_* code.
int elf64_validate(const uint8_t* buf, uint32_t buf_size);

// Load all PT_LOAD segments from the ELF image.
// The segments are mapped into the addresses specified by the
// ELF (ET_EXEC) or rebased to load_base (ET_DYN).
//
// For ET_DYN (PIE), pass the desired load_base.
// For ET_EXEC, load_base is ignored (pass 0).
//
// out : filled with load info on success
// Returns ELF_OK on success, or an ELF_ERR_* code.
int elf64_load(const uint8_t* buf, uint32_t buf_size,
               uint64_t load_base, ElfImage* out);

// High-level helper: validate + load + print results to CommandOutput.
// Reads the file named fat83_name from the FAT32 partition,
// then loads it and reports what it found.
// Returns ELF_OK on success.
int elf64_exec_from_fat32(const char* fat83_name,
                          uint64_t    load_base,
                          ElfImage*   out,
                          void*       cmd_output);

// Human-readable error string.
const char* elf64_strerror(int err);

// Diagnostic: print ELF header fields to CommandOutput.
void elf64_dump_header(const uint8_t* buf, void* cmd_output);

#endif // ELF64_H