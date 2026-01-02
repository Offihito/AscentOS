/* 64-bit Kernel Linker Script */

ENTRY(_start)
OUTPUT_FORMAT(elf64-x86-64)

SECTIONS
{
    /* Kernel 1MB'den başlar (higher half kernel için 0xFFFFFFFF80000000 da kullanılabilir) */
    . = 1M;
    
    /* Multiboot header en başta */
    .multiboot ALIGN(8) : {
        *(.multiboot)
    }
    
    /* Kod segmenti */
    .text ALIGN(4K) : {
        *(.text)
        *(.text.*)
    }
    
    /* Read-only data */
    .rodata ALIGN(4K) : {
        *(.rodata)
        *(.rodata.*)
    }
    
    /* Data segmenti */
    .data ALIGN(4K) : {
        *(.data)
        *(.data.*)
    }
    
    /* BSS segmenti (sıfırlanmış data) */
    .bss ALIGN(4K) : {
        *(COMMON)
        *(.bss)
        *(.bss.*)
    }
    
    /* Stack (64KB) */
    . = ALIGN(4K);
    .stack : {
        *(.stack)
    }
    
    /* Kernel sonu işareti */
    . = ALIGN(4K);
    _kernel_end = .;
}

/* Semboller */
_kernel_start = 1M;
_text_start = ADDR(.text);
_text_end = ADDR(.text) + SIZEOF(.text);
_data_start = ADDR(.data);
_data_end = ADDR(.data) + SIZEOF(.data);
_bss_start = ADDR(.bss);
_bss_end = ADDR(.bss) + SIZEOF(.bss);