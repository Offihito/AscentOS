; boot64_unified.asm - 64-bit Higher Half Kernel Bootloader
; AscentOS - GDT with Ring-3 + TSS support

global _start
extern kernel_main

%define KERNEL_VMA 0xFFFFFFFF80000000

; ============================================================================
; MULTIBOOT2 HEADER
; ============================================================================
section .multiboot
align 8
multiboot_header:
    dd 0xe85250d6
    dd 0
    dd multiboot_header_end - multiboot_header
    dd -(0xe85250d6 + 0 + (multiboot_header_end - multiboot_header))

    align 8
    dw 1
    dw 0
    dd 20
    dd 3
    dd 8
    dd 0

; VESA framebuffer tag
; TEXT_MODE: 1280x800   GUI_MODE: 1280x720
%ifdef GUI_MODE
    align 8
    framebuffer_tag_start:
    dw 5
    dw 0
    dd framebuffer_tag_end - framebuffer_tag_start
    dd 1280
    dd 720
    dd 32
    framebuffer_tag_end:
%elifdef TEXT_MODE
    align 8
    framebuffer_tag_start:
    dw 5
    dw 0
    dd framebuffer_tag_end - framebuffer_tag_start
    dd 1280        ; width (pixels) — 1280/8 = 160 cols
    dd 720         ; height (pixels) — 720/16 = 45 rows
    dd 32          ; bpp
    framebuffer_tag_end:
%endif

    align 8
    dw 0
    dw 0
    dd 8
multiboot_header_end:


section .bss
align 4096
p4_table:       resb 4096
p3_table_low:   resb 4096
p3_table_high:  resb 4096
p2_table:       resb 16384
boot_stack_bottom: resb 65536
boot_stack_top:


section .data
global framebuffer_addr
global framebuffer_pitch
global framebuffer_width
global framebuffer_height
global framebuffer_bpp

framebuffer_addr:   dq 0
framebuffer_pitch:  dd 0
framebuffer_width:  dd 0
framebuffer_height: dd 0
framebuffer_bpp:    db 0

; Multiboot2 memory map info (read by C side)
global multiboot_mmap_addr
global multiboot_mmap_entry_size
global multiboot_mmap_total_size
multiboot_mmap_addr:       dq 0   ; physical address of memory map entry array
multiboot_mmap_entry_size: dd 0   ; size of each entry in bytes
multiboot_mmap_total_size: dd 0   ; total size of entry data in bytes


align 16
global gdt64
global gdt64_pointer
gdt64:

.null: equ $ - gdt64
    dq 0x0000000000000000

.code: equ $ - gdt64
    ; Kernel Code (Ring 0, 64-bit) — Access=0x9A, Flags=0x20 (L=1)
    dq 0x00209A0000000000

.data: equ $ - gdt64
    ; Kernel Data (Ring 0) — Access=0x92
    dq 0x0000920000000000

.user_data: equ $ - gdt64
    ; User Data (Ring 3) — Access=0xF2, Flags=0xCF (G=1, D=1)
    dq 0x00CFF20000000000

.user_code: equ $ - gdt64
    ; User Code (Ring 3, 64-bit) — Access=0xFA, Flags=0xAF (G=1, L=1)
    dq 0x00AFFA0000000000

.tss_low: equ $ - gdt64
    ; TSS Descriptor low 8 bytes — filled by tss_init() in C
    dq 0x0000000000000000

.tss_high: equ $ - gdt64
    ; TSS Descriptor high 8 bytes (base[63:32]) — filled by tss_init() in C
    dq 0x0000000000000000

.end:

; GDT Pointer for lgdt — base updated to higher half address at runtime
gdt64_pointer:
    dw gdt64.end - gdt64 - 1    ; limit = 0x37
    dq gdt64


align 16
global kernel_tss
kernel_tss:
    times 104 db 0   ; sizeof(tss_t) = 104 bytes, zeroed

; Debug messages
section .data
msg_boot_start:     db "[BOOT] AscentOS Higher Half Starting...", 0x0A, 0
msg_fb_addr:        db "[BOOT] Framebuffer at: 0x", 0
msg_fb_size:        db "[BOOT] Resolution: ", 0
msg_entering_long:  db "[BOOT] Entering long mode (Higher Half)...", 0x0A, 0


section .text
bits 32
_start:
    mov esp, boot_stack_top
    mov edi, ebx                  ; save multiboot info pointer
    call check_multiboot
    call init_serial
    mov esi, msg_boot_start
    call serial_print_32
    call parse_multiboot_info
    call setup_page_tables
    call enable_paging
    lgdt [gdt64_pointer]
    jmp gdt64.code:long_mode_start

check_multiboot:
    cmp eax, 0x36d76289
    jne .no_multiboot
    ret
.no_multiboot:
    mov al, 'M'
    call serial_write_32
    hlt

; COM1 (0x3F8) — 38400 baud, 8N1
init_serial:
    mov dx, 0x3F8 + 1
    mov al, 0x00
    out dx, al
    mov dx, 0x3F8 + 3
    mov al, 0x80
    out dx, al
    mov dx, 0x3F8 + 0
    mov al, 0x03
    out dx, al
    mov dx, 0x3F8 + 1
    mov al, 0x00
    out dx, al
    mov dx, 0x3F8 + 3
    mov al, 0x03
    out dx, al
    mov dx, 0x3F8 + 2
    mov al, 0xC7
    out dx, al
    ret

serial_write_32:
    push dx
    push ax
    mov dx, 0x3F8 + 5
.wait:
    in al, dx
    test al, 0x20
    jz .wait
    pop ax
    mov dx, 0x3F8
    out dx, al
    pop dx
    ret

serial_print_32:
    push eax
    push esi
.loop:
    lodsb
    test al, al
    jz .done
    call serial_write_32
    jmp .loop
.done:
    pop esi
    pop eax
    ret

serial_print_hex_32:
    push eax
    push ebx
    push ecx
    mov ebx, eax
    mov ecx, 8
.loop:
    rol ebx, 4
    mov al, bl
    and al, 0x0F
    add al, '0'
    cmp al, '9'
    jle .print
    add al, 7
.print:
    call serial_write_32
    loop .loop
    pop ecx
    pop ebx
    pop eax
    ret

; Multiboot2 memory map tag (type=6) layout:
;   +0  uint32 type         = 6
;   +4  uint32 size         = total tag size
;   +8  uint32 entry_size   = size per entry (typically 24)
;   +12 uint32 entry_version= 0
;   +16 entry[]             = memory map entry array
parse_multiboot_info:
    push eax
    push ebx
    push ecx
    push esi
    mov esi, edi
    add esi, 8
.tag_loop:
    mov eax, [esi]
    test eax, eax
    jz .done
    cmp eax, 8
    je .found_framebuffer
    cmp eax, 6
    je .found_mmap
    mov ecx, [esi + 4]
    add esi, ecx
    add esi, 7
    and esi, ~7
    jmp .tag_loop

.found_mmap:
    mov eax, esi
    add eax, 16
    mov [multiboot_mmap_addr], eax
    mov eax, [esi + 8]
    mov [multiboot_mmap_entry_size], eax
    mov ecx, [esi + 4]
    sub ecx, 16                  ; entry data size = total - header(16)
    mov [multiboot_mmap_total_size], ecx
    mov ecx, [esi + 4]
    add esi, ecx
    add esi, 7
    and esi, ~7
    jmp .tag_loop

.found_framebuffer:
    mov eax, [esi + 8]
    mov [framebuffer_addr], eax
    mov eax, [esi + 12]
    mov [framebuffer_addr + 4], eax
    mov eax, [esi + 16]
    mov [framebuffer_pitch], eax
    mov eax, [esi + 20]
    mov [framebuffer_width], eax
    mov eax, [esi + 24]
    mov [framebuffer_height], eax
    mov al, [esi + 28]
    mov [framebuffer_bpp], al
    mov esi, msg_fb_addr
    call serial_print_32
    mov eax, [framebuffer_addr]
    call serial_print_hex_32
    mov al, 0x0A
    call serial_write_32
    mov esi, msg_fb_size
    call serial_print_32
    mov eax, [framebuffer_width]
    call serial_print_hex_32
    mov al, 'x'
    call serial_write_32
    mov eax, [framebuffer_height]
    call serial_print_hex_32
    mov al, 0x0A
    call serial_write_32
    jmp .done
.done:
    pop esi
    pop ecx
    pop ebx
    pop eax
    ret


setup_page_tables:
    mov edi, p4_table
    mov ecx, 4096
    xor eax, eax
    rep stosd
    mov edi, p3_table_low
    mov ecx, 8192
    xor eax, eax
    rep stosd
    mov edi, p2_table
    mov ecx, 16384
    xor eax, eax
    rep stosd

    mov eax, p3_table_low
    or eax, 0b111
    mov [p4_table], eax
    mov eax, p3_table_high
    or eax, 0b111
    mov [p4_table + 511 * 8], eax

    mov eax, p2_table
    or eax, 0b111
    mov [p3_table_low], eax
    add eax, 4096
    mov [p3_table_low + 8], eax
    add eax, 4096
    mov [p3_table_low + 16], eax
    add eax, 4096
    mov [p3_table_low + 24], eax

    mov eax, p2_table
    or eax, 0b111
    mov [p3_table_high + 510 * 8], eax
    add eax, 4096
    mov [p3_table_high + 511 * 8], eax

    mov edi, p2_table
    mov eax, 0x00000087         ; P + RW + User + PS(2MB)
    mov ecx, 2048               ; 2048 x 2MB = 4GB
.map_p2:
    mov [edi], eax
    add eax, 0x200000
    add edi, 8
    loop .map_p2
    ret

enable_paging:
    mov eax, p4_table
    mov cr3, eax
    mov eax, cr4
    or eax, 1 << 5              ; PAE
    mov cr4, eax
    mov ecx, 0xC0000080         ; EFER MSR
    rdmsr
    or eax, 1 << 8              ; LME
    wrmsr
    mov eax, cr0
    or eax, 1 << 31             ; PG
    mov cr0, eax
    mov esi, msg_entering_long
    call serial_print_32
    ret


bits 64
long_mode_start:
    mov ax, gdt64.data
    mov ss, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    mov rsp, kernel_stack_top

    mov rdi, rbx                ; pass multiboot info to kernel_main

    mov rax, kernel_main
    call rax

    cli
.halt:
    hlt
    jmp .halt


section .bss
align 4096
kernel_stack_bottom:
    resb 65536
kernel_stack_top: