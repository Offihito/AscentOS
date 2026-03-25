#ifndef ELF64_H
#define ELF64_H

#include <stdint.h>

// ELF Magic & Identification
#define ELF_MAGIC0      0x7F
#define ELF_MAGIC1      'E'
#define ELF_MAGIC2      'L'
#define ELF_MAGIC3      'F'

#define EI_MAG0         0
#define EI_MAG1         1
#define EI_MAG2         2
#define EI_MAG3         3
#define EI_CLASS        4   
#define EI_DATA         5   
#define EI_VERSION      6
#define EI_OSABI        7
#define EI_NIDENT       16

#define ELFCLASS32      1
#define ELFCLASS64      2

#define ELFDATA2LSB     1   

#define ET_NONE         0
#define ET_REL          1  
#define ET_EXEC         2  
#define ET_DYN          3  
#define ET_CORE         4

#define EM_X86_64       62

#define EV_CURRENT      1

// ELF64 Header (64 bytes)
typedef struct __attribute__((packed)) {
    uint8_t  e_ident[EI_NIDENT];
    uint16_t e_type;            
    uint16_t e_machine;        
    uint32_t e_version;         
    uint64_t e_entry;           
    uint64_t e_phoff;           
    uint64_t e_shoff;           
    uint32_t e_flags;           
    uint16_t e_ehsize;         
    uint16_t e_phentsize;      
    uint16_t e_phnum;           
    uint16_t e_shentsize;       
    uint16_t e_shnum;          
    uint16_t e_shstrndx;         
} Elf64Header;

// ELF64 Program Header (segment descriptor, 56 bytes)
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

#define PT_NULL         0
#define PT_LOAD         1   
#define PT_DYNAMIC      2
#define PT_INTERP       3
#define PT_NOTE         4
#define PT_PHDR         6
#define PT_TLS          7

#define PF_X            0x1   // Execute
#define PF_W            0x2   // Write
#define PF_R            0x4   // Read

// ELF64 Section Header (64 bytes, used only for diagnostics)
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

// Loader result codes
#define ELF_OK              0
#define ELF_ERR_NULL        1   
#define ELF_ERR_MAGIC       2   
#define ELF_ERR_CLASS       3   
#define ELF_ERR_ENDIAN      4   
#define ELF_ERR_TYPE        5   
#define ELF_ERR_MACHINE     6   
#define ELF_ERR_NOPHDR      7   
#define ELF_ERR_PHENTSIZE   8   
#define ELF_ERR_TOOBIG      9   
#define ELF_ERR_NOMEM       10  

typedef struct {
    uint64_t entry;         
    uint64_t load_base;     
    uint64_t load_min;      
    uint64_t load_max;      
    int      segment_count; 
} ElfImage;

// Public API
int elf64_validate(const uint8_t* buf, uint32_t buf_size);


int elf64_load(const uint8_t* buf, uint32_t buf_size,
               uint64_t load_base, ElfImage* out);

int elf64_exec_from_ext3(const char* path,
                         uint64_t    load_base,
                         ElfImage*   out,
                         void*       cmd_output);

const char* elf64_strerror(int err);

void elf64_dump_header(const uint8_t* buf, void* cmd_output);

#endif 