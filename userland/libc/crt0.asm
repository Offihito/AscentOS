; ═══════════════════════════════════════════════════════════════
;  AscentOS — crt0.asm  (x86_64, ELF64)
;  _start → [minimal TLS kurulumu] → main → exit
;
;  NEDEN __libc_start_main DEĞİL:
;    musl'ün __libc_start_main → __init_tls, aux vector (AT_PHDR,
;    AT_PHNUM) okur. Kernel aux vector sağlamıyor → ilk syscall'dan
;    önce garbage pointer → #PF.
;
;  NEDEN STATIK TLS BLOĞU:
;    musl malloc/mutex/strerror:
;      __pthread_self() = "mov rax, %fs:0"
;    FS_BASE=0 iken:
;      rax = *(uint64_t*)0 = 0xF000FF53F000FF53 (BIOS IVT) → #GP/#PF
;    Çözüm: _start'ta 256-byte static blok ayır,
;      blok[0] = &blok  (musl self-pointer kuralı),
;      arch_prctl(ARCH_SET_FS, &blok) → FS_BASE geçerli pointer.
;    malloc/mutex sıfır-başlatılmış = unlocked; tek iş parçacığı
;    için yeterli.
;
;  Kernel register düzeni (task_create_from_elf):
;    RDI = argc   RSI = argv*   RDX = envp*
;    Stack: [RSP] = argc, [RSP+8] = argv[0]*, ...
; ═══════════════════════════════════════════════════════════════

bits 64

; ── Statik TLS/pthread bloğu ─────────────────────────────────
; musl pthread struct ilk alanı 'self' (pthread*).
; Sıfır-başlatılmış 256 byte: tüm mutex'ler unlocked (0),
; errno alanı sıfır, vs.
section .data
align 64
_tls_block:
    dq  0               ; [+0]  pthread.self  — _start'ta doldurulur
    times 31 dq 0       ; [+8..+255]  mutex/errno/diğer alanlar = 0

section .text
extern main
extern exit
global _start

_start:
    xor  rbp, rbp               ; ABI: en dış frame pointer = 0

    ; ── Minimal TLS kurulumu ─────────────────────────────────
    ; 1. self pointer'ı yaz: _tls_block[0] = &_tls_block
    lea  rax, [rel _tls_block]
    mov  qword [rax], rax       ; pthread.self = &_tls_block

    ; 2. arch_prctl(ARCH_SET_FS=0x1002, &_tls_block)
    ;    Sonra: %fs:0 = &_tls_block → __pthread_self() geçerli
    mov  rdi, 0x1002            ; ARCH_SET_FS
    mov  rsi, rax               ; &_tls_block
    mov  eax, 158               ; SYS_arch_prctl
    syscall
    ; syscall: rcx, r11 clobber — RSP korunur

    ; ── argc / argv / envp ───────────────────────────────────
    ; Kernel argc'yi hem RDI'ye hem stack'e yazdı.
    ; arch_prctl RDI/RSI'yi clobber etti → stack'ten oku.
    pop  rdi                    ; argc  ← [RSP]
    mov  rsi, rsp               ; argv  ← RSP (argv[0]* dizisi)
    lea  rdx, [rsi + rdi*8 + 8] ; envp ← argv + argc + 1

    and  rsp, -16               ; 16-byte hizala

    call main                   ; main(argc, argv, envp)

    mov  rdi, rax
    call exit                   ; musl exit → atexit → sys_exit_group

    ; Buraya ulaşılmamalı
    xor  edi, edi
    mov  eax, 231               ; SYS_exit_group
    syscall
    ud2

; ── GNU-stack notu: yığın çalıştırılabilir DEĞİL ─────────────
section .note.GNU-stack noalloc noexec nowrite progbits