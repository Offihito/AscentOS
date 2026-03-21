/* linker64.ld — AscentOS Higher Half Kernel (64-bit)
 *
 * VMA : 0xFFFFFFFF80000000 + KERNEL_PHYS  (kernel virtual space)
 * LMA : 0x100000                           (GRUB kernel'i buraya yükler)
 *
 * Dönüşüm formülleri (vmm64.c ile tutarlı):
 *   phys → virt : phys + KERNEL_VMA
 *   virt → phys : virt - KERNEL_VMA
 */

ENTRY(_start)
OUTPUT_FORMAT(elf64-x86-64)

KERNEL_VMA  = 0xFFFFFFFF80000000;  /* higher half tabanı           */
KERNEL_PHYS = 0x100000;            /* fiziksel yükleme adresi (1 MB) */

SECTIONS
{
    /* ── DMA-safe buffer (ISA DMA: max 16 MB) ──────────────────────────────
     * NOLOAD: GRUB bu segmenti dosyadan okumaz, sıfır başlatılmış sayılır.
     * sb16_init() _dma_ptr/_dma_phys değerlerini runtime'da hesaplar.     */
    .dma_low 0x10000 (NOLOAD) : AT(0x10000) {
        *(.dma_low)
        . = ALIGN(8192);
    }

    /* ════════════════════════════════════════════════════════════════════
     * HIGHER HALF KERNEL
     * VMA = KERNEL_VMA + KERNEL_PHYS (0xFFFFFFFF80100000)
     * LMA = KERNEL_PHYS              (0x100000)           – GRUB buraya yükler
     * ════════════════════════════════════════════════════════════════════ */

    . = KERNEL_VMA + KERNEL_PHYS;

    /* Multiboot2 header — dosyanın en başında, fiziksel 0x100000'de */
    .multiboot ALIGN(8) : AT(KERNEL_PHYS) {
        *(.multiboot)
    }

    /* Kod segmenti */
    .text ALIGN(4K) : AT(ADDR(.text) - KERNEL_VMA) {
        *(.text)
        *(.text.*)
    }

    /* Salt-okunur veri */
    .rodata ALIGN(4K) : AT(ADDR(.rodata) - KERNEL_VMA) {
        *(.rodata)
        *(.rodata.*)
    }

    /* Veri segmenti (.data, .bss) */
    .data ALIGN(4K) : AT(ADDR(.data) - KERNEL_VMA) {
        *(.data)
        *(.data.*)
    }

    /* Sıfırlanmış veri — GRUB, LMA aralığının dosya dışı kısmını sıfırlar */
    .bss ALIGN(4K) : AT(ADDR(.bss) - KERNEL_VMA) {
        *(COMMON)
        *(.bss)
        *(.bss.*)
    }

    /* ── Kernel & Boot stack'leri (NOLOAD) ─────────────────────────────────
     * Dosyaya yazılmaz; sadece sanal adres aralığı rezerve edilir.
     * Boot kodu 32-bit modda V2P() makrosu ile fiziksel adresi hesaplar.  */
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

/* ── Exported semboller ──────────────────────────────────────────────────── */
_kernel_start      = KERNEL_VMA + KERNEL_PHYS;   /* ilk VMA adresi          */
_kernel_phys_start = KERNEL_PHYS;                /* fiziksel yükleme adresi  */
_kernel_vma_base   = KERNEL_VMA;                 /* vmm64.c / pmm.c için     */

_text_start  = ADDR(.text);
_text_end    = ADDR(.text)   + SIZEOF(.text);
_data_start  = ADDR(.data);
_data_end    = ADDR(.data)   + SIZEOF(.data);
_bss_start   = ADDR(.bss);
_bss_end     = ADDR(.bss)    + SIZEOF(.bss);
/* Stack sembolleri .stack section'ında tanımlanır:
 *   _kernel_stack_bottom, _kernel_stack_top
 *   _boot_stack_bottom,   _boot_stack_top   */