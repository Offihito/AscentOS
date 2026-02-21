/* user.ld — AscentOS Userland Linker Script
 *
 * Değişiklik (newlib port):
 *   __heap_start sembolü eklendi → _sbrk() bu adresten başlar
 *   __heap_end   sembolü eklendi → stack tabanı (isteğe bağlı guard)
 */

OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

SECTIONS {
    . = 0x400000;           /* Kod buradan başlar */

    .text  : {
        *(.text.unlikely .text.unlikely.*)
        *(.text*)
        *(.rodata*)
    }

    .data  : { *(.data*) }

    .bss   : {
        *(.bss*)
        *(COMMON)
    }

    /* ── newlib _sbrk için heap sınırı ────────────────────────
     * SBRK_MODE 0 (kernel sbrk): bu sembol kullanılmaz ama
     *   olması iyi pratik — kernel heap başlangıcını buradan alabilir.
     * SBRK_MODE 1 (userspace bump): _sbrk() bu adresten başlar.
     * ─────────────────────────────────────────────────────── */
    . = ALIGN(4096);
    PROVIDE(__heap_start = .);

    /* Stack üstü — opsiyonel, guard için kullanılabilir */
    PROVIDE(__heap_end   = 0x800000);   /* 8 MB kullanıcı alanı varsayılan */

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}