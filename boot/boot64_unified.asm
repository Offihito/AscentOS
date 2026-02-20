; boot64_unified.asm - 64-bit Higher Half Kernel Bootloader
; AscentOS - Ring-3 + TSS destekli GDT

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

; VESA framebuffer tag — hem GUI_MODE hem de TEXT_MODE kullanır
; TEXT_MODE: 1280x800 (geniş terminal alanı)
; GUI_MODE:  1920x1080 (tam HD)
%ifdef GUI_MODE
    align 8
    framebuffer_tag_start:
    dw 5
    dw 0
    dd framebuffer_tag_end - framebuffer_tag_start
    dd 1920
    dd 1080
    dd 32
    framebuffer_tag_end:
%elifdef TEXT_MODE
    align 8
    framebuffer_tag_start:
    dw 5
    dw 0
    dd framebuffer_tag_end - framebuffer_tag_start
    dd 1280        ; genişlik (pixel) — 1280/8 = 160 sütun
    dd 800         ; yükseklik (pixel) — 800/16 = 50 satır
    dd 32          ; bpp
    framebuffer_tag_end:
%endif

    align 8
    dw 0
    dw 0
    dd 8
multiboot_header_end:

; ============================================================================
; BSS
; ============================================================================
section .bss
align 4096
p4_table:       resb 4096
p3_table_low:   resb 4096
p3_table_high:  resb 4096
p2_table:       resb 16384
boot_stack_bottom: resb 65536
boot_stack_top:

; ============================================================================
; DATA
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

; Multiboot2 memory map bilgisi (C tarafından okunur)
global multiboot_mmap_addr
global multiboot_mmap_entry_size
global multiboot_mmap_total_size
multiboot_mmap_addr:       dq 0   ; memory map entry dizisinin fiziksel adresi
multiboot_mmap_entry_size: dd 0   ; her entry'nin boyutu (byte)
multiboot_mmap_total_size: dd 0   ; toplam entry verisi boyutu (byte)

; ============================================================================
; GDT - 64-bit  (Ring-0 + Ring-3 + TSS)
;
; Slot  Offset  Description
;   0    0x00   Null descriptor
;   1    0x08   Kernel Code  (Ring 0, 64-bit, L=1, DPL=0)
;   2    0x10   Kernel Data  (Ring 0, DPL=0)
;   3    0x18   User Data    (Ring 3, DPL=3)   <- SYSRET SS = 0x1B
;   4    0x20   User Code    (Ring 3, 64-bit, L=1, DPL=3)  <- SYSRET CS = 0x23
;   5    0x28   TSS Low      (16-byte system descriptor, low 8 bytes)
;   6    0x30   TSS High     (16-byte system descriptor, high 8 bytes)
;
; SYSRET 64-bit hesabı (Intel SDM Vol.2):
;   CS = STAR[63:48] + 16 | 3
;   SS = STAR[63:48] + 8  | 3
;
; STAR[63:48] = 0x10 olarak ayarlanır (syscall.h'da USER_CS_BASE=0x10):
;   SS = (0x10 + 8)  | 3 = 0x18 | 3 = 0x1B  -> User Data  (0x18, DPL=3) OK
;   CS = (0x10 + 16) | 3 = 0x20 | 3 = 0x23  -> User Code  (0x20, DPL=3) OK
;
; STAR[47:32] = 0x08 (Kernel CS):
;   SYSCALL: CS = 0x08, SS = 0x08+8 = 0x10  -> Kernel Data OK
; ============================================================================
align 16
global gdt64
global gdt64_pointer
gdt64:

.null: equ $ - gdt64
    ; 0x00: Null
    dq 0x0000000000000000

.code: equ $ - gdt64
    ; 0x08: Kernel Code (Ring 0, 64-bit)
    ; Access=0x9A: P=1,DPL=0,S=1,Type=1010(code,exec,read)
    ; Flags =0x20: G=0,L=1(64-bit),D=0
    dq 0x00209A0000000000

.data: equ $ - gdt64
    ; 0x10: Kernel Data (Ring 0)
    ; Access=0x92: P=1,DPL=0,S=1,Type=0010(data,write)
    dq 0x0000920000000000

.user_data: equ $ - gdt64
    ; 0x18: User Data (Ring 3)
    ; Access=0xF2: P=1,DPL=3,S=1,Type=0010(data,write)
    ; Flags =0xCF: G=1,D=1,L=0
    dq 0x00CFF20000000000

.user_code: equ $ - gdt64
    ; 0x20: User Code (Ring 3, 64-bit)
    ; Access=0xFA: P=1,DPL=3,S=1,Type=1010(code,exec,read)
    ; Flags =0xAF: G=1,D=0,L=1(64-bit)
    dq 0x00AFFA0000000000

.tss_low: equ $ - gdt64
    ; 0x28: TSS Descriptor low 8 bytes
    ; base ve limit C tarafindan tss_init() ile doldurulur
    dq 0x0000000000000000

.tss_high: equ $ - gdt64
    ; 0x30: TSS Descriptor high 8 bytes (base[63:32])
    ; C tarafindan tss_init() ile doldurulur
    dq 0x0000000000000000

.end:

; GDT Pointer (lgdt icin)
; limit = toplam boyut - 1 = (0x38 - 1) = 0x37
; base  = gdt64 sanal adresi (64-bit higher half'te)
gdt64_pointer:
    dw gdt64.end - gdt64 - 1    ; limit = 0x37
    dq gdt64                    ; base (64-bit, runtime'da higher half adrese guncellenir)

; ============================================================================
; TSS Verisi
; C tarafinda "extern tss_t kernel_tss" olarak erisilir.
; tss_init() bu alani sifirlar ve GDT descriptor'ini doldurur.
; ============================================================================
align 16
global kernel_tss
kernel_tss:
    times 104 db 0   ; sizeof(tss_t) = 104 byte, tamamen sifirlanmis

; Debug mesajlari
section .data
msg_boot_start:     db "[BOOT] AscentOS Higher Half Starting...", 0x0A, 0
msg_fb_addr:        db "[BOOT] Framebuffer at: 0x", 0
msg_fb_size:        db "[BOOT] Resolution: ", 0
msg_entering_long:  db "[BOOT] Entering long mode (Higher Half)...", 0x0A, 0

; ============================================================================
; CODE - 32-bit baslangic
; ============================================================================
section .text
bits 32
_start:
    mov esp, boot_stack_top
    mov edi, ebx
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

    ; -----------------------------------------------
    ; Multiboot2 Memory Map Tag (type=6) layout:
    ;   +0  uint32 type       = 6
    ;   +4  uint32 size       = tag toplam boyutu
    ;   +8  uint32 entry_size = her entry boyutu (genellikle 24)
    ;   +12 uint32 entry_version = 0
    ;   +16 entry[]          = memory map entry dizisi
    ; -----------------------------------------------
.found_mmap:
    mov eax, esi
    add eax, 16             ; entry dizisinin baslangici
    mov [multiboot_mmap_addr], eax
    mov eax, [esi + 8]      ; entry_size
    mov [multiboot_mmap_entry_size], eax
    mov ecx, [esi + 4]      ; tag toplam boyutu
    sub ecx, 16             ; entry verisi boyutu = toplam - header(16)
    mov [multiboot_mmap_total_size], ecx
    ; tag'i parse etmeye devam et (framebuffer da olabilir)
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
    ; P4 -> P3: Present + RW + User (U bit=2 zorunlu, ust seviye U=0 ise alt seviye erisilemez)
    mov eax, p3_table_low
    or eax, 0b111          ; P + RW + User
    mov [p4_table], eax
    mov eax, p3_table_high
    or eax, 0b111          ; P + RW + User
    mov [p4_table + 511 * 8], eax
    ; P3 -> P2: Present + RW + User
    mov eax, p2_table
    or eax, 0b111          ; P + RW + User
    mov [p3_table_low], eax
    add eax, 4096
    mov [p3_table_low + 8], eax
    add eax, 4096
    mov [p3_table_low + 16], eax
    add eax, 4096
    mov [p3_table_low + 24], eax
    mov eax, p2_table
    or eax, 0b111          ; P + RW + User
    mov [p3_table_high + 510 * 8], eax
    add eax, 4096
    mov [p3_table_high + 511 * 8], eax
    ; P2 page entries: 2MB pages, Present + RW + User + PS
    ; Flags = 0x87:
    ;   bit 0: Present     = 1
    ;   bit 1: Read/Write  = 1 (yazilabilir)
    ;   bit 2: User/Super  = 1 (Ring-3 erisebilir -- gelistirme asamasi)
    ;   bit 7: Page Size   = 1 (2MB page)
    ; NOT: Uretim kernelde bu flag ayri user/kernel sayfa tablolariyla
    ;      daha hassas yonetilmeli. Simdilik flat model kullaniyoruz.
    mov edi, p2_table
    mov eax, 0x00000087    ; P + RW + User + PS(2MB)
    mov ecx, 2048          ; 2048 x 2MB = 4GB
.map_p2:
    mov [edi], eax
    add eax, 0x200000      ; 2MB artir
    add edi, 8
    loop .map_p2
    ret

enable_paging:
    mov eax, p4_table
    mov cr3, eax
    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax
    mov ecx, 0xC0000080
    rdmsr
    or eax, 1 << 8
    wrmsr
    mov eax, cr0
    or eax, 1 << 31
    mov cr0, eax
    mov esi, msg_entering_long
    call serial_print_32
    ret

; ============================================================================
; 64-BIT LONG MODE
; ============================================================================
bits 64
long_mode_start:
    mov ax, gdt64.data
    mov ss, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    mov rsp, kernel_stack_top

    mov rdi, rbx

    mov rax, kernel_main
    call rax

    cli
.halt:
    hlt
    jmp .halt

; ============================================================================
; KERNEL STACK
; ============================================================================
section .bss
align 4096
kernel_stack_bottom:
    resb 65536
kernel_stack_top: