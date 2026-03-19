/* 64-bit Kernel Linker Script */

ENTRY(_start)
OUTPUT_FORMAT(elf64-x86-64)

SECTIONS
{
    /* ── DMA-safe buffer (ISA DMA: max 16 MB) ──────────────────────────────
     * _dma_buf (sb16.c) buraya map edilir: fiziksel 0x10000 = 64 KB.
     * 0x500 – 0x7FFFF: BIOS data'nın üstü, kernel'den önce, < 16 MB.
     * AT(0x10000): LMA = fiziksel adres (identity mapped kernel).
     * Bu blok kernel yüklemesinden önce linker tarafından işlenir. */
    /* DMA-safe buffer: NOLOAD = ELF dosyasına yazılmaz.
     * Sadece VMA/LMA adresi 0x10000 olarak rezerve edilir.
     * GRUB bu segment için dosya okuması yapmaz → multiboot header
     * dosya içinde 0x10000 ileriye kaymaz, başta kalır.
     * _dma_buf içeriği boot sırasında sıfır (BSS gibi davranır).
     * sb16_init() _dma_ptr ve _dma_phys'i runtime'da hesaplar. */
    .dma_low 0x10000 (NOLOAD) : AT(0x10000) {
        *(.dma_low)
        . = ALIGN(8192);
    }

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