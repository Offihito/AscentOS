; boot64_higher_half.asm - 64-bit Higher Half Kernel Bootloader
; Kernel'i 0xFFFFFFFF80000000 adresinde çalıştırır
; Identity mapping (0-4GB) + Higher half mapping

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
    
    ; Higher half için sayfa tablolarını hazırla
    call setup_page_tables_higher_half
    call enable_paging
    
    mov esi, msg_entering_long
    call serial_print_32
    
    ; GDT yükle ve 64-bit moda geç
    lgdt [gdt64.pointer]
    jmp gdt64.code:long_mode_start

; ----------------------------------------------------------------------------
; Multiboot2 magic number kontrolü
; ----------------------------------------------------------------------------
check_multiboot:
    cmp eax, 0x36d76289
    jne .error
    ret
.error:
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
; Higher Half Kernel için sayfa tablolarını hazırla
; Identity mapping (0-4GB) + Higher half mapping (kernel)
; ----------------------------------------------------------------------------
setup_page_tables_higher_half:
    ; Tüm tabloları temizle
    mov edi, p4_table
    mov ecx, 6144                           ; 6 * 4096 bytes / 4
    xor eax, eax
    rep stosd
    
    ; === P4 Table Setup ===
    ; P4[0] -> p3_table_low (identity mapping için)
    mov eax, p3_table_low
    or eax, 0b11                            ; Present + Writable
    mov [p4_table], eax
    
    ; P4[511] -> p3_table_high (higher half için)
    ; 0xFFFFFFFF80000000 = P4[511], P3[510], P2[0]
    mov eax, p3_table_high
    or eax, 0b11
    mov [p4_table + 511*8], eax
    
    ; === P3 Low Table (Identity Mapping) ===
    ; P3[0] -> P2[0-511] (first 1GB)
    mov eax, p2_table
    or eax, 0b11
    mov [p3_table_low], eax
    
    ; P3[1] -> P2[512-1023] (second 1GB)
    mov eax, p2_table
    add eax, 4096
    or eax, 0b11
    mov [p3_table_low + 8], eax
    
    ; P3[2] -> P2[1024-1535] (third 1GB)
    mov eax, p2_table
    add eax, 8192
    or eax, 0b11
    mov [p3_table_low + 16], eax
    
    ; P3[3] -> P2[1536-2047] (fourth 1GB)
    mov eax, p2_table
    add eax, 12288
    or eax, 0b11
    mov [p3_table_low + 24], eax
    
    ; === P3 High Table (Higher Half Mapping) ===
    ; P3[510] -> P2[0-511] (kernel'in ilk 1GB'si)
    ; 0xFFFFFFFF80000000 için P3[510] kullanılır
    mov eax, p2_table
    or eax, 0b11
    mov [p3_table_high + 510*8], eax
    
    ; P3[511] -> P2[512-1023] (kernel'in ikinci 1GB'si)
    mov eax, p2_table
    add eax, 4096
    or eax, 0b11
    mov [p3_table_high + 511*8], eax
    
    ; === P2 Tables - 2MB pages ile doldur (4GB toplam) ===
    mov edi, p2_table
    mov eax, 0b10000011                     ; Present + Writable + Huge (2MB)
    mov ecx, 2048                           ; 2048 x 2MB = 4GB
.fill_p2:
    mov [edi], eax
    add eax, 0x200000                       ; 2MB artır
    add edi, 8
    loop .fill_p2
    
    ret

; ----------------------------------------------------------------------------
; Paging ve long mode'u etkinleştir
; ----------------------------------------------------------------------------
enable_paging:
    ; CR3'e P4 adresini yükle
    mov eax, p4_table
    mov cr3, eax
    
    ; PAE etkinleştir
    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax
    
    ; Long Mode Enable
    mov ecx, 0xC0000080
    rdmsr
    or eax, 1 << 8
    wrmsr
    
    ; Paging etkinleştir
    mov eax, cr0
    or eax, 1 << 31
    mov cr0, eax
    ret

; ----------------------------------------------------------------------------
; Hata gösterimi
; ----------------------------------------------------------------------------
error:
    mov dword [0xb8000], 0x4f524f45         ; "ER"
    mov dword [0xb8004], 0x4f3a4f52         ; "R:"
    mov byte [0xb8008], al
    mov byte [0xb8009], 0x4f
    cli
.halt:
    hlt
    jmp .halt

; ============================================================================
; GDT - 64-bit
; ============================================================================
section .rodata
gdt64:
    dq 0                                    ; Null descriptor
.code: equ $ - gdt64
    dq (1<<43) | (1<<44) | (1<<47) | (1<<53) ; Code segment
.data: equ $ - gdt64
    dq (1<<44) | (1<<47)                    ; Data segment
.pointer:
    dw $ - gdt64 - 1
    dq gdt64

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
    ; Segment register'ları sıfırla
    xor ax, ax
    mov ss, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    
    ; Yeni stack'i higher half adreste ayarla
    mov rsp, kernel_stack_top
    
    ; Multiboot info pointer'ı higher half adrese çevir
    ; EBX fiziksel adres içeriyor, bunu sanal adrese dönüştür
    mov rdi, rbx                            ; Fiziksel adres
    
    ; Artık higher half'te çalışıyoruz
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