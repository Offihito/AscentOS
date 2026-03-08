/* ═══════════════════════════════════════════════════════════════
   AscentOS — user.ld  (x86_64 userland)
   PT_LOAD #1  R+X  → .text, .rodata, init/fini array
   PT_LOAD #2  R+W  → .data, .bss
   ═══════════════════════════════════════════════════════════════ */

OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

BASE_ADDR = 0x400000;

/* PHDRS — sections'tan ÖNCE tanımlanmalı */
PHDRS
{
    text  PT_LOAD FLAGS(5);   /* R+X */
    data  PT_LOAD FLAGS(6);   /* R+W */
}

SECTIONS
{
    . = BASE_ADDR;

    /* ── R+X ── */
    .text : ALIGN(0x1000)
    {
        *(.text._start)
        *(.text*)
        *(.gnu.linkonce.t*)
    } :text

    .rodata : ALIGN(0x10)
    {
        *(.rodata*)
        *(.gnu.linkonce.r*)
    } :text

    .init_array : ALIGN(0x10)
    {
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(.init_array*))
        PROVIDE_HIDDEN(__init_array_end = .);
    } :text

    .fini_array : ALIGN(0x10)
    {
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(.fini_array*))
        PROVIDE_HIDDEN(__fini_array_end = .);
    } :text

    /* ── R+W ── */
    . = ALIGN(0x1000);

    .data : ALIGN(0x1000)
    {
        *(.data*)
        *(.gnu.linkonce.d*)
    } :data

    .bss : ALIGN(0x10)
    {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        __bss_end = .;
    } :data

    /* Heap: sbrk() ve opendir()/_simple_malloc() bu sembolü kullanır */
    . = ALIGN(0x1000);
    PROVIDE(__heap_start = .);

    . = ALIGN(0x1000);

    /DISCARD/ :
    {
        *(.note.GNU-stack)
        *(.note*)
        *(.comment*)
        *(.eh_frame*)
        *(.debug*)
    }
}