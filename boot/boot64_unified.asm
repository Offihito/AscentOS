; boot64_higher_half.asm - 64-bit Higher Half Kernel Bootloader
; Kernel'i 0xFFFFFFFF80000000 adresinde çalıştırır
; Identity mapping (0-4GB) + Higher half mapping
; UPDATED: Added Ring 3 segments - WORKING VERSION

global _start
extern kernel_main

; Higher half kernel sanal adresi
%define KERNEL_VMA 0xFFFFFFFF80000000

; ============================================================================
; MULTIBOOT2 HEADER - VESA Framebuffer Desteği
; ============================================================================
section .multiboot
align 8
multiboot_header:
    dd 0xe85250d6                           ; Magic number
    dd 0                                    ; Architecture: i386
    dd multiboot_header_end - multiboot_header
    dd -(0xe85250d6 + 0 + (multiboot_header_end - multiboot_header))

    ; Framebuffer bilgisi talebi
    align 8
    dw 1                                    ; Type: info request
    dw 0                                    ; Flags
    dd 20                                   ; Size
    dd 3                                    ; Memory map
    dd 8                                    ; Framebuffer
    dd 0                                    ; Padding

%ifdef GUI_MODE
    ; Framebuffer tag - VESA modu için (sadece GUI mode)
    align 8
    framebuffer_tag_start:
    dw 5                                    ; Type: framebuffer
    dw 0                                    ; Flags
    dd framebuffer_tag_end - framebuffer_tag_start
    dd 1920                                 ; Genişlik: 1920
    dd 1080                                 ; Yükseklik: 1080
    dd 32                                   ; Renk derinliği: 32-bit
    framebuffer_tag_end:
%endif

    ; Son tag
    align 8
    dw 0                                    ; Type: end
    dw 0                                    ; Flags
    dd 8                                    ; Size
multiboot_header_end:

; ============================================================================
; BSS - Başlatılmamış veri (fiziksel adreste)
; ============================================================================
section .bss
align 4096
p4_table:       resb 4096                   ; Level 4 page table
p3_table_low:   resb 4096                   ; Level 3 for identity mapping (0-4GB)
p3_table_high:  resb 4096                   ; Level 3 for higher half (kernel)
p2_table:       resb 16384                  ; Level 2 page tables (4x 1GB = 4GB)
boot_stack_bottom: resb 65536               ; 64KB boot stack
boot_stack_top:

; ============================================================================
; DATA - Framebuffer bilgisi için global değişkenler
; ============================================================================
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

; ============================================================================
; CODE - 32-bit başlangıç kodu
; ============================================================================
section .text
bits 32
_start:
    ; Stack pointer'ı ayarla (fiziksel adres kullan)
    mov esp, boot_stack_top
    
    ; Multiboot info pointer'ı sakla
    mov edi, ebx
    
    ; Multiboot magic number kontrolü
    call check_multiboot
    
    ; Serial port'u başlat (erken debug için)
    call init_serial
    
    ; Debug mesajı
    mov esi, msg_boot_start
    call serial_print_32
    
    ; Multiboot2 info'dan framebuffer bilgisini al
    call parse_multiboot_info
    
    ; Paging'i ayarla
    call setup_page_tables
    call enable_paging
    
    ; Long mode'a geç
    lgdt [gdt64.pointer]
    jmp gdt64.code:long_mode_start

; ----------------------------------------------------------------------------
; Multiboot magic number kontrolü
; ----------------------------------------------------------------------------
check_multiboot:
    cmp eax, 0x36d76289
    jne .no_multiboot
    ret
.no_multiboot:
    mov al, 'M'
    call serial_write_32
    hlt

; ----------------------------------------------------------------------------
; Serial port başlat (32-bit)
; ----------------------------------------------------------------------------
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

; ----------------------------------------------------------------------------
; Serial port yazma fonksiyonları
; ----------------------------------------------------------------------------
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

; ----------------------------------------------------------------------------
; Multiboot2 bilgilerini parse et
; ----------------------------------------------------------------------------
parse_multiboot_info:
    push eax
    push ebx
    push ecx
    push esi
    
    mov esi, edi                            ; Multiboot info
    add esi, 8                              ; İlk tag'e atla

.tag_loop:
    mov eax, [esi]                          ; Tag type
    test eax, eax                           ; End tag?
    jz .done
    
    cmp eax, 8                              ; Framebuffer tag?
    je .found_framebuffer
    
    ; Sonraki tag'e git
    mov ecx, [esi + 4]                      ; Tag size
    add esi, ecx
    add esi, 7
    and esi, ~7                             ; 8-byte align
    jmp .tag_loop

.found_framebuffer:
    mov eax, [esi + 8]                      ; Framebuffer address (low)
    mov [framebuffer_addr], eax
    mov eax, [esi + 12]                     ; Framebuffer address (high)
    mov [framebuffer_addr + 4], eax
    
    mov eax, [esi + 16]                     ; Pitch
    mov [framebuffer_pitch], eax
    
    mov eax, [esi + 20]                     ; Width
    mov [framebuffer_width], eax
    
    mov eax, [esi + 24]                     ; Height
    mov [framebuffer_height], eax
    
    mov al, [esi + 28]                      ; BPP
    mov [framebuffer_bpp], al
    
    ; Debug: framebuffer adresini yazdır
    mov esi, msg_fb_addr
    call serial_print_32
    mov eax, [framebuffer_addr]
    call serial_print_hex_32
    mov al, 0x0A
    call serial_write_32
    
    ; Resolution'ı yazdır
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

; ----------------------------------------------------------------------------
; Page tabloları ayarla (Identity + Higher Half)
; ----------------------------------------------------------------------------
setup_page_tables:
    ; P4 tabloyu temizle
    mov edi, p4_table
    mov ecx, 4096
    xor eax, eax
    rep stosd
    
    ; P3 tabloları temizle
    mov edi, p3_table_low
    mov ecx, 8192
    xor eax, eax
    rep stosd
    
    ; P2 tabloları temizle
    mov edi, p2_table
    mov ecx, 16384
    xor eax, eax
    rep stosd
    
    ; P4[0] -> P3_low (identity mapping için)
    mov eax, p3_table_low
    or eax, 0b11
    mov [p4_table], eax
    
    ; P4[511] -> P3_high (higher half için)
    mov eax, p3_table_high
    or eax, 0b11
    mov [p4_table + 511 * 8], eax
    
    ; P3_low[0..3] -> P2[0..3] (0-4GB identity mapping)
    mov eax, p2_table
    or eax, 0b11
    mov [p3_table_low], eax
    
    add eax, 4096
    mov [p3_table_low + 8], eax
    
    add eax, 4096
    mov [p3_table_low + 16], eax
    
    add eax, 4096
    mov [p3_table_low + 24], eax
    
    ; P3_high[510..511] -> P2[0..3] (kernel higher half)
    mov eax, p2_table
    or eax, 0b11
    mov [p3_table_high + 510 * 8], eax
    
    add eax, 4096
    mov [p3_table_high + 511 * 8], eax
    
    ; P2 entries: 2MB pages (0-4GB)
    mov edi, p2_table
    mov eax, 0x00000083                     ; Present, writable, 2MB
    mov ecx, 2048                           ; 2048 * 2MB = 4GB
.map_p2:
    mov [edi], eax
    add eax, 0x200000                       ; 2MB
    add edi, 8
    loop .map_p2
    
    ret

; ----------------------------------------------------------------------------
; Paging'i etkinleştir
; ----------------------------------------------------------------------------
enable_paging:
    ; CR3'e P4 adresini yükle
    mov eax, p4_table
    mov cr3, eax
    
    ; PAE'yi etkinleştir (CR4.PAE)
    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax
    
    ; Long mode'u etkinleştir (EFER.LME)
    mov ecx, 0xC0000080
    rdmsr
    or eax, 1 << 8
    wrmsr
    
    ; Paging'i etkinleştir (CR0.PG)
    mov eax, cr0
    or eax, 1 << 31
    mov cr0, eax
    
    ; Debug mesajı
    mov esi, msg_entering_long
    call serial_print_32
    
    ret

; ============================================================================
; GDT - 64-bit WITH RING 3 SUPPORT
; ============================================================================
section .rodata
align 16
gdt64:
    ; 0x00: Null descriptor
    dq 0x0000000000000000
    
.code: equ $ - gdt64
    ; 0x08: Kernel Code Segment (Ring 0, 64-bit)
    ; Access: Present(1) DPL(00) Code(1) Exec(1) Readable(1) Accessed(0) = 10011010b = 0x9A
    ; Flags: Granularity(0) Long(1) = 0010b = 0x2
    dq 0x00209A0000000000
    
.data: equ $ - gdt64
    ; 0x10: Kernel Data Segment (Ring 0)
    ; Access: Present(1) DPL(00) Data(0) Writable(1) = 10010010b = 0x92
    dq 0x0000920000000000
    
.user_data: equ $ - gdt64
    ; 0x18: User Data Segment (Ring 3)
    ; Access: Present(1) DPL(11) Data(0) Writable(1) = 11110010b = 0xF2
    dq 0x0000F20000000000
    
.user_code: equ $ - gdt64
    ; 0x20: User Code Segment (Ring 3, 64-bit)
    ; Access: Present(1) DPL(11) Code(1) Exec(1) Readable(1) = 11111010b = 0xFA
    ; Flags: Granularity(0) Long(1) = 0010b = 0x2
    dq 0x0020FA0000000000

.pointer:
    dw $ - gdt64 - 1            ; GDT limit
    dq gdt64                    ; GDT base

; Debug mesajları
msg_boot_start:     db "[BOOT] AscentOS Higher Half Starting...", 0x0A, 0
msg_fb_addr:        db "[BOOT] Framebuffer at: 0x", 0
msg_fb_size:        db "[BOOT] Resolution: ", 0
msg_entering_long:  db "[BOOT] Entering long mode (Higher Half)...", 0x0A, 0

; ============================================================================
; 64-BIT LONG MODE
; ============================================================================
bits 64
long_mode_start:
    ; Segment register'ları kernel data segment'e ayarla
    mov ax, gdt64.data
    mov ss, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    
    ; Yeni stack'i higher half adreste ayarla
    mov rsp, kernel_stack_top
    
    ; Multiboot info pointer'ı higher half adrese çevir
    mov rdi, rbx                            ; Fiziksel adres
    
    ; Kernel main fonksiyonunu çağır (higher half adreste)
    mov rax, kernel_main
    call rax
    
    ; Kernel dönerse sistem durdur
    cli
.halt:
    hlt
    jmp .halt

; ============================================================================
; KERNEL STACK (Higher Half'te)
; ============================================================================
section .bss
align 4096
kernel_stack_bottom:
    resb 65536                              ; 64KB kernel stack
kernel_stack_top: