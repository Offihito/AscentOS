/* user.ld — AscentOS Userland Linker Script */

OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

SECTIONS {
    . = 0x400000;           /* Kod buradan başlar */

    .text  : { *(.text*)  *(.rodata*) }
    .data  : { *(.data*)              }
    .bss   : { *(.bss*)  *(COMMON)    }

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}