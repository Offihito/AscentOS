ENTRY(_start)
OUTPUT_FORMAT(elf64-x86-64)

KERNEL_VMA = 0xFFFFFFFF80000000;
KERNEL_PHYS = 0x100000;

SECTIONS
{
    /* DMA-safe buffer (ISA DMA: max 16 MB) */
    .dma_low 0x10000 (NOLOAD) : AT(0x10000) {
        *(.dma_low)
        . = ALIGN(8192);
    }

    /* Higher half kernel */
    . = KERNEL_VMA + KERNEL_PHYS;

    .multiboot ALIGN(8) : AT(KERNEL_PHYS) {
        *(.multiboot)
    }

    .text ALIGN(4K) : AT(ADDR(.text) - KERNEL_VMA) {
        *(.text)
        *(.text.*)
    }

    .rodata ALIGN(4K) : AT(ADDR(.rodata) - KERNEL_VMA) {
        *(.rodata)
        *(.rodata.*)
    }

    .data ALIGN(4K) : AT(ADDR(.data) - KERNEL_VMA) {
        *(.data)
        *(.data.*)
    }

    .bss ALIGN(4K) : AT(ADDR(.bss) - KERNEL_VMA) {
        *(COMMON)
        *(.bss)
        *(.bss.*)
    }

    .stack ALIGN(4K) (NOLOAD) : {
        _kernel_stack_bottom = .;
        . += 64K;
        _kernel_stack_top = .;
        _boot_stack_bottom = .;
        . += 64K;
        _boot_stack_top = .;
    }

    . = ALIGN(4K);
    _kernel_end = .;
}

/* Exported symbols */
_kernel_start = KERNEL_VMA + KERNEL_PHYS;
_kernel_phys_start = KERNEL_PHYS;
_kernel_vma_base = KERNEL_VMA;

_text_start = ADDR(.text);
_text_end = ADDR(.text) + SIZEOF(.text);
_data_start = ADDR(.data);
_data_end = ADDR(.data) + SIZEOF(.data);
_bss_start = ADDR(.bss);
_bss_end = ADDR(.bss) + SIZEOF(.bss);