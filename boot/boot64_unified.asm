; boot64_unified.asm — AscentOS Higher Half Kernel Boot
;
; HIGHER HALF GEÇİŞ MİMARİSİ
; ─────────────────────────────────────────────────────────────────────────────
; Linker tüm bölümleri VMA = KERNEL_VMA + offset olarak yerleştirir;
; fiziksel yükleme adresi (LMA) ise 0x100000'den başlar.
;
; 32-bit kod GRUB'un bıraktığı fiziksel adreste çalışır (sayfalama henüz yok).
; Sembollere doğrudan erişmek yerine V2P() makrosu fiziksel adresi verir:
;
;   V2P(sym)  =  sym - KERNEL_VMA          ; 32-bit güvenli
;
; Göreceli dallar (call/jmp) her iki alanda da aynı displacement'ı ürettiğinden
; V2P() gerektirmez; yalnızca MUTLAK adres yüklemeleri ve bellek operandları
; V2P() ile dönüştürülür.
;
; Geçiş sırası:
;   _start (32-bit, fiziksel)
;     → setup_page_tables   : hem kimlik (PML4[0]) hem higher-half (PML4[511]) kurar
;     → enable_paging       : PAE + LME + PG
;     → lgdt [V2P(gdt64_pointer)]  : fiziksel GDT tabanıyla yükle
;     → push+retf           : 64-bit long mode'a gir (fiziksel adres)
;   long_mode_start (64-bit, fiziksel kimlik eşlemesi)
;     → mov rax, higher_half_start ; mutlak 64-bit VMA adresi
;     → jmp rax             : higher-half VMA'ya sıçra
;   higher_half_start (64-bit, KERNEL_VMA)
;     → RSP = _kernel_stack_top    ; higher-half stack
;     → GDT pointer base → VMA    ; LGDT'yi virtual adresiyle yenile
;     → call kernel_main
; ─────────────────────────────────────────────────────────────────────────────

global _start
extern kernel_main

%define KERNEL_VMA  0xFFFFFFFF80000000

; V2P : VMA → fiziksel adres (32-bit modda kullanılır)
; Sonuç her zaman küçük pozitif sayıdır, 32-bit'e sığar.
%define V2P(sym)  ((sym) - KERNEL_VMA)

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
    dd 32
    dd 3
    dd 8
    dd 9
    dd 10
    dd 11
    dd 12
    dd 0

; VESA framebuffer tag
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
    dd 1280
    dd 720
    dd 32
    framebuffer_tag_end:
%endif

    align 8
    dw 0
    dw 0
    dd 8
multiboot_header_end:


; ============================================================================
; SAYFA TABLOLARI (BSS — higher-half VMA, fiziksel erişim için V2P() kullanılır)
; ============================================================================
extern _kernel_stack_top
extern _boot_stack_top

section .bss
align 4096
p4_table:       resb 4096
p3_table_low:   resb 4096
p3_table_high:  resb 4096
p2_table:       resb 16384   ; 4 × 4096 → 0..4 GB (kimlik + higher-half)


; ============================================================================
; DATA — fiziksel adresle erişilmesi gereken değişkenler için
;         32-bit kodda V2P() ile yazılır, C kodu VMA ile okur.
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
align 4
global boot_is_uefi
boot_is_uefi:       dd 0

global multiboot_mmap_addr
global multiboot_mmap_entry_size
global multiboot_mmap_total_size
multiboot_mmap_addr:       dq 0   ; mmap giriş dizisinin FİZİKSEL adresi
multiboot_mmap_entry_size: dd 0
multiboot_mmap_total_size: dd 0

; ── GDT ─────────────────────────────────────────────────────────────────────
; gdt64_pointer.base ilk LGDT için fiziksel adres (V2P(gdt64)) tutar.
; higher_half_start'ta VMA ile güncellenir.
align 16
global gdt64
global gdt64_pointer
gdt64:

.null: equ $ - gdt64
    dq 0x0000000000000000

.code: equ $ - gdt64
    dq 0x00209A0000000000   ; Kernel Code  Ring-0 64-bit

.data: equ $ - gdt64
    dq 0x0000920000000000   ; Kernel Data  Ring-0

.user_data: equ $ - gdt64
    dq 0x00CFF20000000000   ; User Data    Ring-3

.user_code: equ $ - gdt64
    dq 0x00AFFA0000000000   ; User Code    Ring-3 64-bit

.tss_low: equ $ - gdt64
    dq 0x0000000000000000   ; TSS low  — tss_init() tarafından doldurulur

.tss_high: equ $ - gdt64
    dq 0x0000000000000000   ; TSS high — tss_init() tarafından doldurulur

.end:

; Boot zamanında fiziksel GDT tabanı; higher_half_start'ta VMA ile güncellenir.
gdt64_pointer:
    dw gdt64.end - gdt64 - 1   ; limit
    dq V2P(gdt64)               ; base = fiziksel adres (boot LGDT için)

align 16
global kernel_tss
kernel_tss:
    times 104 db 0

; ── Debug mesajları ──────────────────────────────────────────────────────────
section .data
msg_boot_start:    db "[BOOT] AscentOS Higher Half Kernel Starting...", 0x0A, 0
msg_fb_addr:       db "[BOOT] Framebuffer at: 0x", 0
msg_fb_size:       db "[BOOT] Resolution: ", 0
msg_entering_long: db "[BOOT] Entering long mode (Higher Half)...", 0x0A, 0
msg_higher_half:   db "[BOOT] Jumped to higher half VMA", 0x0A, 0


; ============================================================================
; 32-BIT ENTRY POINT
; ============================================================================
section .text
bits 32
_start:
    ; Boot stack'i FİZİKSEL adresle kur (paging henüz yok)
    mov esp, V2P(_boot_stack_top)

    mov edi, ebx            ; multiboot2 bilgi pointer'ını sakla
    call check_multiboot
    call init_serial
    mov esi, V2P(msg_boot_start)
    call serial_print_32
    call parse_multiboot_info
    call setup_page_tables
    call enable_paging

    ; GDT'yi fiziksel tabanla yükle (paging aktif, kimlik eşleme geçerli)
    lgdt [V2P(gdt64_pointer)]

    ; 64-bit long mode'a geç: jmp ile 32-bit segment:offset kullanamayız
    ; çünkü long_mode_start VMA'sının alt 32 biti kimlik bölgesinde değil.
    ; push+retf çözümü: fiziksel adresi stack'e at, retf ile sıçra.
    push dword gdt64.code
    push dword V2P(long_mode_start)   ; fiziksel adres — kimlik eşlemesinde
    retf

; ─────────────────────────────────────────────────────────────────────────────
check_multiboot:
    cmp eax, 0x36d76289
    jne .no_multiboot
    ret
.no_multiboot:
    mov al, 'M'
    call serial_write_32
    hlt

; ─────────────────────────────────────────────────────────────────────────────
; COM1 — 38400 baud, 8N1
init_serial:
    mov dx, 0x3F8 + 1 ; IER: interrupt disable
    mov al, 0x00
    out dx, al
    mov dx, 0x3F8 + 3 ; LCR: DLAB=1
    mov al, 0x80
    out dx, al
    mov dx, 0x3F8 + 0 ; baud divisor low = 3 → 38400
    mov al, 0x03
    out dx, al
    mov dx, 0x3F8 + 1 ; baud divisor high = 0
    mov al, 0x00
    out dx, al
    mov dx, 0x3F8 + 3 ; LCR: 8N1
    mov al, 0x03
    out dx, al
    mov dx, 0x3F8 + 2 ; FCR: FIFO enable
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

; ─────────────────────────────────────────────────────────────────────────────
; Multiboot2 memory map tag (type=6) ve framebuffer tag (type=8)
; Veri, .data bölümündeki global değişkenlere V2P() aracılığıyla yazılır.
; ─────────────────────────────────────────────────────────────────────────────
parse_multiboot_info:
    push eax
    push ebx
    push ecx
    push esi
    mov esi, edi        ; edi = multiboot_info fiziksel ptr (GRUB'dan)
    add esi, 8          ; ilk tag'a atla (başlık 8 byte)
.tag_loop:
    mov eax, [esi]      ; tag type
    test eax, eax
    jz .done
    cmp eax, 9
    je .found_efi
    cmp eax, 10
    je .found_efi
    cmp eax, 11
    je .found_efi
    cmp eax, 12
    je .found_efi
    cmp eax, 8
    je .found_framebuffer
    cmp eax, 6
    je .found_mmap
    mov ecx, [esi + 4]  ; tag size
    add esi, ecx
    add esi, 7
    and esi, ~7
    jmp .tag_loop

.found_mmap:
    ; mmap giriş dizisinin başlangıç fiziksel adresi
    mov eax, esi
    add eax, 16
    mov [V2P(multiboot_mmap_addr)], eax   ; fiziksel ptr sakla
    xor eax, eax                          ; üst 32 bit = 0
    mov [V2P(multiboot_mmap_addr) + 4], eax
    mov eax, [esi + 8]
    mov [V2P(multiboot_mmap_entry_size)], eax
    mov ecx, [esi + 4]
    sub ecx, 16
    mov [V2P(multiboot_mmap_total_size)], ecx
    ; sonraki tag'a geç
    mov ecx, [esi + 4]
    add esi, ecx
    add esi, 7
    and esi, ~7
    jmp .tag_loop

.found_efi:
    ; Multiboot2 EFI tags:
    ; 9  = EFI32 image handle pointer
    ; 10 = EFI64 image handle pointer
    ; 11 = EFI32 system table pointer
    ; 12 = EFI64 system table pointer
    mov dword [V2P(boot_is_uefi)], 1
    mov ecx, [esi + 4]
    add esi, ecx
    add esi, 7
    and esi, ~7
    jmp .tag_loop

.found_framebuffer:
    mov eax, [esi + 8]
    mov [V2P(framebuffer_addr)], eax
    mov eax, [esi + 12]
    mov [V2P(framebuffer_addr) + 4], eax
    mov eax, [esi + 16]
    mov [V2P(framebuffer_pitch)], eax
    mov eax, [esi + 20]
    mov [V2P(framebuffer_width)], eax
    mov eax, [esi + 24]
    mov [V2P(framebuffer_height)], eax
    mov al, [esi + 28]
    mov [V2P(framebuffer_bpp)], al
    ; seri porta yaz
    mov esi, V2P(msg_fb_addr)
    call serial_print_32
    mov eax, [V2P(framebuffer_addr)]
    call serial_print_hex_32
    mov al, 0x0A
    call serial_write_32
    mov esi, V2P(msg_fb_size)
    call serial_print_32
    mov eax, [V2P(framebuffer_width)]
    call serial_print_hex_32
    mov al, 'x'
    call serial_write_32
    mov eax, [V2P(framebuffer_height)]
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


; ─────────────────────────────────────────────────────────────────────────────
; setup_page_tables
;
; Fiziksel adresler V2P() ile hesaplanır.
;
; Kurulan eşleme:
;   PML4[0]   → p3_table_low   → 0x000000..0xFFFFFFFF (4 GB kimlik)
;   PML4[511] → p3_table_high  → 0xFFFFFFFF80000000..+2GB
;              yani: fiziksel 0x0 → sanal KERNEL_VMA
;
; C kodunda dönüşüm:
;   phys → virt : phys + KERNEL_VMA
;   virt → phys : virt - KERNEL_VMA
; ─────────────────────────────────────────────────────────────────────────────
setup_page_tables:
    ; ── Tabloları sıfırla ──────────────────────────────────────────────────
    mov edi, V2P(p4_table)
    mov ecx, 4096
    xor eax, eax
    rep stosd

    mov edi, V2P(p3_table_low)
    mov ecx, 8192           ; p3_table_low + p3_table_high
    xor eax, eax
    rep stosd

    mov edi, V2P(p2_table)
    mov ecx, 16384
    xor eax, eax
    rep stosd

    ; ── PML4 girişleri ─────────────────────────────────────────────────────
    ; PML4[0] → p3_table_low  (kimlik eşlemesi — vmm_init tarafından temizlenir)
    mov eax, V2P(p3_table_low)
    or  eax, 0b111                      ; Present + RW + User
    mov [V2P(p4_table)], eax

    ; PML4[511] → p3_table_high  (higher-half kernel)
    mov eax, V2P(p3_table_high)
    or  eax, 0b111
    mov [V2P(p4_table) + 511 * 8], eax

    ; ── Kimlik PDPT (p3_table_low[0..3] → 4 adet p2 sayfası) ──────────────
    mov eax, V2P(p2_table)
    or  eax, 0b111
    mov [V2P(p3_table_low)], eax
    add eax, 4096
    mov [V2P(p3_table_low) + 8], eax
    add eax, 4096
    mov [V2P(p3_table_low) + 16], eax
    add eax, 4096
    mov [V2P(p3_table_low) + 24], eax

    ; ── Higher-half PDPT ─────────────────────────────────────────────────────
    ; KERNEL_VMA = 0xFFFFFFFF80000000
    ;   PML4_INDEX = 511, PDPT_INDEX = 510
    ;
    ; p3_table_high[510] → fiziksel 0..1 GB = sanal KERNEL_VMA .. +1 GB
    ; p3_table_high[511] → fiziksel 1..2 GB = sanal KERNEL_VMA+1GB .. +2 GB
    ;
    ; Kernel 0x100000 (phys) = KERNEL_VMA + 0x100000 (sanal) — PDPT[510] kapsar ✓
    ;
    ; Framebuffer 0xFD000000 (~4 GB): phys_to_virt(x) = x + KERNEL_VMA overflow
    ; eder → higher-half üzerinden erişilemez. Kimlik eşlemesi (PML4[0])
    ; korunduğu için fiziksel adresle doğrudan erişim mümkün kalır.
    mov eax, V2P(p2_table)
    or  eax, 0b111
    mov [V2P(p3_table_high) + 510 * 8], eax   ; 0..1 GB
    add eax, 4096
    mov [V2P(p3_table_high) + 511 * 8], eax   ; 1..2 GB

    ; ── p2_table: 0..4 GB — kimlik + higher-half ──────────────────────────────
    ; 2048 giriş × 2 MB = 4 GB
    mov edi, V2P(p2_table)
    mov eax, 0x00000087         ; P + RW + User + PS (2MB)
    mov ecx, 2048               ; 2048 × 2MB = 4 GB
.map_p2:
    mov [edi], eax
    add eax, 0x200000
    add edi, 8
    loop .map_p2
    ret

; ─────────────────────────────────────────────────────────────────────────────
enable_paging:
    ; CR3 = p4_table'ın FİZİKSEL adresi
    mov eax, V2P(p4_table)
    mov cr3, eax

    ; CR4: PAE etkinleştir
    mov eax, cr4
    or  eax, 1 << 5
    mov cr4, eax

    ; EFER MSR: Long Mode Enable
    mov ecx, 0xC0000080
    rdmsr
    or  eax, 1 << 8
    wrmsr

    ; CR0: Paging Enable
    mov eax, cr0
    or  eax, 1 << 31
    mov cr0, eax

    mov esi, V2P(msg_entering_long)
    call serial_print_32
    ret


; ============================================================================
; 64-BIT TRAMPOLIN — fiziksel kimlik eşlemesinde çalışır
;
; Göreceli adreslemeden kaçınmak için long_mode_start minimal tutulur:
; yalnızca segment register'ları kurar ve higher-half VMA'ya sıçrar.
; ============================================================================
bits 64
long_mode_start:
    ; Segment register'ları kur (LDT olmadığı için ds=es=fs=gs=ss=0 OK)
    mov ax, gdt64.data
    mov ss, ax
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; Higher-half VMA'ya 64-bit mutlak sıçrama
    ; Kimlik eşlemesindeki kod yürütmesini burada bırak.
    mov rax, higher_half_start
    jmp rax


; ============================================================================
; HIGHER HALF ENTRY — artık KERNEL_VMA alanında çalışıyoruz
; ============================================================================
higher_half_start:
    ; Kernel stack'i higher-half adresinde kur
    mov rsp, _kernel_stack_top

    ; GDT pointer'ı VMA ile güncelle:
    ;   gdt64_pointer + 0..1 : limit  (değişmez)
    ;   gdt64_pointer + 2..9 : base   (VMA ile güncelle)
    lea rax, [rel gdt64]
    mov qword [rel gdt64_pointer + 2], rax
    lgdt [rel gdt64_pointer]

    ; RBP = 0 (çağrı yığınının kökü — stack unwinder için)
    xor rbp, rbp

    ; kernel_main(0) — mevcut kernel multiboot_info argümanını okumadığı için 0
    xor rdi, rdi
    call kernel_main

    ; kernel_main dönmemeli; dönürse döngüde bekle
    cli
.halt:
    hlt
    jmp .halt

    ; ── İsteğe bağlı: kimlik eşlemesini kaldır ───────────────────────────
    ; vmm_init() zaten PML4[0..255]'i temizlediğinden bu blok normalde
    ; çalışmaz. İhtiyaç duyulursa vmm_init()'in önüne alınabilir.
    ;
    ;   lea rax, [rel p4_table]       ; p4_table VMA'sı (higher-half eşlemeli)
    ;   mov qword [rax], 0            ; PML4[0] = 0 → kimlik eşlemesi kalkar
    ;   mov rax, cr3
    ;   mov cr3, rax                  ; TLB flush