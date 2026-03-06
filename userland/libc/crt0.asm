; ═══════════════════════════════════════════════════════════════
;  AscentOS — crt0.asm  (x86_64, ELF64)
;  _start → main → exit (musl atexit zinciri)
; ═══════════════════════════════════════════════════════════════

bits 64

section .text
extern main
extern exit

global _start
_start:
    xor     rbp, rbp              ; ABI: frame pointer sıfırla

    ; argc, argv, envp — System V AMD64 stack düzeni
    pop     rdi                   ; argc
    mov     rsi, rsp              ; argv
    lea     rdx, [rsi + rdi*8 + 8] ; envp

    and     rsp, ~0xF             ; 16-byte hizala

    call    main                  ; rax = exit code

    mov     rdi, rax
    call    exit                  ; musl exit → atexit → sys_exit

    ; Fallback
    mov     rax, 60
    xor     rdi, rdi
    syscall
    ud2

; ── GNU-stack notu: yığın çalıştırılabilir DEĞİL ─────────────
; Bu section linker'a "executable stack istemiyoruz" der.
; NASM otomatik eklemez — elle tanımlanmalı.
section .note.GNU-stack noalloc noexec nowrite progbits
