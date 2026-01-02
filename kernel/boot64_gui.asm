; boot64_gui.asm - 64-bit GUI Bootloader (VESA Framebuffer)
; Genişletilmiş bellek mapping ile (4GB)

global _start
extern kernel_main

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

    ; Framebuffer tag - VESA modu için
    align 8
    framebuffer_tag_start:
    dw 5                                    ; Type: framebuffer
    dw 0                                    ; Flags
    dd framebuffer_tag_end - framebuffer_tag_start
    dd 1920                                ; Genişlik: 1024
    dd 1080                                 ; Yükseklik: 768
    dd 32                                   ; Renk derinliği: 32-bit
    framebuffer_tag_end:

    ; Son tag
    align 8
    dw 0                                    ; Type: end
    dw 0                                    ; Flags
    dd 8                                    ; Size
multiboot_header_end:

; ============================================================================
; BSS - Başlatılmamış veri
; ============================================================================
section .bss
align 4096
p4_table:       resb 4096                   ; Level 4 page table
p3_table:       resb 4096                   ; Level 3 page table  
p2_table:       resb 16384                  ; Level 2 page table (4x boyut - 4GB için)
stack_bottom:   resb 65536                  ; 64KB stack
stack_top:

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
    ; Stack pointer'ı ayarla
    mov esp, stack_top
    
    ; Multiboot magic number kontrolü (EAX'te olmalı)
    cmp eax, 0x36d76289
    jne .bad_multiboot
    
    ; Multiboot info pointer'ı sakla
    mov edi, ebx
    
    ; Serial port'u başlat (erken debug için)
    call init_serial
    
    ; Debug mesajı
    mov esi, msg_boot_start
    call serial_print_32
    
    ; Multiboot bilgilerini parse et (framebuffer bilgisi)
    call parse_multiboot_info
    
    ; Framebuffer bilgisini göster
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
    
    ; Sistem kontrolleri
    call check_cpuid
    call check_long_mode
    
    ; 64-bit moda geçiş hazırlığı
    call setup_page_tables
    call enable_paging
    
    mov esi, msg_entering_long
    call serial_print_32
    
    ; GDT yükle ve 64-bit moda geç
    lgdt [gdt64.pointer]
    jmp gdt64.code:long_mode_start

.bad_multiboot:
    mov al, '0'
    jmp error

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
    
    ; Sonraki tag
    mov ecx, [esi + 4]                      ; Tag size
    add ecx, 7
    and ecx, ~7
    add esi, ecx
    jmp .tag_loop

.found_framebuffer:
    ; Framebuffer bilgilerini al
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

.done:
    pop esi
    pop ecx
    pop ebx
    pop eax
    ret

; ----------------------------------------------------------------------------
; CPUID desteği kontrolü
; ----------------------------------------------------------------------------
check_cpuid:
    pushfd
    pop eax
    mov ecx, eax
    xor eax, 1 << 21
    push eax
    popfd
    pushfd
    pop eax
    push ecx
    popfd
    xor eax, ecx
    jz .error
    ret
.error:
    mov al, '1'
    jmp error

; ----------------------------------------------------------------------------
; Long mode desteği kontrolü
; ----------------------------------------------------------------------------
check_long_mode:
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb .error
    
    mov eax, 0x80000001
    cpuid
    test edx, 1 << 29
    jz .error
    ret
.error:
    mov al, '2'
    jmp error

; ----------------------------------------------------------------------------
; 4-seviye sayfalama (4GB tam identity mapping)
; ----------------------------------------------------------------------------
setup_page_tables:
    ; P4'ü temizle
    mov edi, p4_table
    mov ecx, 1024
    xor eax, eax
    rep stosd
    
    ; P4[0] -> P3
    mov eax, p3_table
    or eax, 0b11
    mov [p4_table], eax
    
    ; P3[0] -> P2 (ilk 1GB: 0-1GB)
    mov eax, p2_table
    or eax, 0b11
    mov [p3_table], eax
    
    ; P3[1] -> P2+4096 (ikinci 1GB: 1-2GB)
    mov eax, p2_table
    add eax, 4096
    or eax, 0b11
    mov [p3_table + 8], eax
    
    ; P3[2] -> P2+8192 (üçüncü 1GB: 2-3GB)
    mov eax, p2_table
    add eax, 8192
    or eax, 0b11
    mov [p3_table + 16], eax
    
    ; P3[3] -> P2+12288 (dördüncü 1GB: 3-4GB)
    mov eax, p2_table
    add eax, 12288
    or eax, 0b11
    mov [p3_table + 24], eax
    
    ; P2'yi doldur (ilk 4GB - 2048 x 2MB pages)
    mov edi, p2_table
    mov eax, 0b10000011                     ; Present + Writable + Huge
    mov ecx, 2048                           ; 2048 entry = 4GB
.loop:
    mov [edi], eax
    add eax, 0x200000                       ; 2MB
    add edi, 8
    loop .loop
    ret

; ----------------------------------------------------------------------------
; Paging etkinleştir
; ----------------------------------------------------------------------------
enable_paging:
    mov eax, p4_table
    mov cr3, eax
    
    ; PAE
    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax
    
    ; Long mode enable
    mov ecx, 0xC0000080
    rdmsr
    or eax, 1 << 8
    wrmsr
    
    ; Paging enable
    mov eax, cr0
    or eax, 1 << 31
    mov cr0, eax
    ret

; ----------------------------------------------------------------------------
; Hata gösterimi
; ----------------------------------------------------------------------------
error:
    mov dword [0xb8000], 0x4f524f45
    mov dword [0xb8004], 0x4f3a4f52
    mov byte [0xb8008], al
    mov byte [0xb8009], 0x4f
    cli
.halt:
    hlt
    jmp .halt

; ============================================================================
; GDT
; ============================================================================
section .rodata
gdt64:
    dq 0
.code: equ $ - gdt64
    dq (1<<43) | (1<<44) | (1<<47) | (1<<53)
.data: equ $ - gdt64
    dq (1<<44) | (1<<47)
.pointer:
    dw $ - gdt64 - 1
    dq gdt64

msg_boot_start:     db "[BOOT] Starting...", 0x0A, 0
msg_fb_addr:        db "[BOOT] Framebuffer at: 0x", 0
msg_fb_size:        db "[BOOT] Resolution: ", 0
msg_entering_long:  db "[BOOT] Entering long mode (4GB mapped)...", 0x0A, 0

; ============================================================================
; 64-BIT LONG MODE
; ============================================================================
bits 64
long_mode_start:
    ; Segment register'ları sıfırla
    xor ax, ax
    mov ss, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    
    ; Kernel'e geç
    mov rdi, rbx
    call kernel_main
    
    cli
.halt:
    hlt
    jmp .halt