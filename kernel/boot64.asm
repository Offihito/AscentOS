; boot64.asm - 64-bit Bootloader (Multiboot2)
; Modernize edilmiş ve okunabilirlik artırılmış versiyon

global _start
extern kernel_main

; ============================================================================
; MULTIBOOT2 HEADER
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

    ; Son tag
    align 8
    dw 0
    dw 0
    dd 8
multiboot_header_end:

; ============================================================================
; BSS - Başlatılmamış veri
; ============================================================================
section .bss
align 4096
p4_table:       resb 4096                   ; Level 4 page table
p3_table:       resb 4096                   ; Level 3 page table  
p2_table:       resb 4096                   ; Level 2 page table
stack_bottom:   resb 65536                  ; 64KB stack
stack_top:

; ============================================================================
; CODE - 32-bit başlangıç kodu
; ============================================================================
section .text
bits 32
_start:
    mov esp, stack_top                      ; Stack pointer'ı ayarla
    mov edi, ebx                            ; Multiboot info pointer'ı sakla

    ; Sistem kontrolleri
    call check_multiboot
    call check_cpuid
    call check_long_mode

    ; 64-bit moda geçiş hazırlığı
    call setup_page_tables
    call enable_paging

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
; CPUID desteği kontrolü
; ----------------------------------------------------------------------------
check_cpuid:
    pushfd
    pop eax
    mov ecx, eax
    xor eax, 1 << 21                        ; ID bit'i toggle et
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
; Long mode (x86-64) desteği kontrolü
; ----------------------------------------------------------------------------
check_long_mode:
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb .error
    
    mov eax, 0x80000001
    cpuid
    test edx, 1 << 29                       ; LM bit kontrolü
    jz .error
    ret
.error:
    mov al, '2'
    jmp error

; ----------------------------------------------------------------------------
; 4-seviye sayfalama tablolarını oluştur (identity mapping)
; ----------------------------------------------------------------------------
setup_page_tables:
    ; P4 tablosunu temizle
    mov edi, p4_table
    mov ecx, 1024
    xor eax, eax
    rep stosd
    
    ; P4[0] -> P3 bağlantısı
    mov eax, p3_table
    or eax, 0b11                            ; Present + Writable
    mov [p4_table], eax
    
    ; P3[0] -> P2 bağlantısı
    mov eax, p2_table
    or eax, 0b11
    mov [p3_table], eax
    
    ; P2'yi 2MB page'lerle doldur (ilk 1GB)
    mov edi, p2_table
    mov eax, 0b10000011                     ; Present + Writable + Huge
    mov ecx, 512
.loop:
    mov [edi], eax
    add eax, 0x200000                       ; 2MB artır
    add edi, 8
    loop .loop
    ret

; ----------------------------------------------------------------------------
; Paging ve long mode'u etkinleştir
; ----------------------------------------------------------------------------
enable_paging:
    ; CR3'e P4 adresini yükle
    mov eax, p4_table
    mov cr3, eax
    
    ; PAE (Physical Address Extension) etkinleştir
    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax
    
    ; IA32_EFER MSR'de LME (Long Mode Enable) bit'ini set et
    mov ecx, 0xC0000080
    rdmsr
    or eax, 1 << 8
    wrmsr
    
    ; CR0'da paging'i etkinleştir
    mov eax, cr0
    or eax, 1 << 31
    mov cr0, eax
    ret

; ----------------------------------------------------------------------------
; Hata gösterimi (VGA text mode)
; ----------------------------------------------------------------------------
error:
    mov dword [0xb8000], 0x4f524f45         ; "ER" kırmızı
    mov dword [0xb8004], 0x4f3a4f52         ; "R:" kırmızı
    mov byte [0xb8008], al                  ; Hata kodu
    mov byte [0xb8009], 0x4f                ; Kırmızı renk
    cli
.halt:
    hlt
    jmp .halt

; ============================================================================
; GDT (Global Descriptor Table) - 64-bit
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

; ============================================================================
; 64-BIT LONG MODE
; ============================================================================
bits 64
long_mode_start:
    ; Segment register'larını sıfırla
    xor ax, ax
    mov ss, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; Kernel'e geç (multiboot info pointer RDI'de)
    mov rdi, rbx
    call kernel_main

    ; Kernel dönerse sistem durdur
    cli
.halt:
    hlt
    jmp .halt