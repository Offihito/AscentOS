; crt0.asm — AscentOS Userland Runtime Start
; execve() sonrası kernel stack'i şu şekilde kurar (user.ld @ 0x400000):
;
;   [RSP+0]  = argc          (uint64_t)
;   [RSP+8]  = argv[0]       (char*)
;   ...
;   [RSP+8*(argc+1)] = NULL  (argv sentinel)
;   ...sonrası envp[]...
;
; _start → main(argc, argv, envp) → exit()

section .text
global  _start
extern  main
extern  _exit
extern  environ         ; syscalls.c'deki global char **environ

_start:
    ; ── Stack hizalama ──────────────────────────────────────────
    ; RSP'yi 16-byte sınırına hizala (syscall frame bunu garanti etmez)
    and     rsp, ~15

    ; ── argc / argv / envp oku ──────────────────────────────────
    ; execve_build_stack() şunu koydu:
    ;   [RSP]    = argc
    ;   [RSP+8]  = argv base ptr
    ;   [RSP+16] = envp base ptr
    mov     rdi, [rsp]       ; argc  → arg1
    mov     rsi, [rsp + 8]   ; argv  → arg2
    mov     rdx, [rsp + 16]  ; envp  → arg3

    ; ── environ global'ını kur ──────────────────────────────────
    mov     [rel environ], rdx

    ; ── main() çağır ────────────────────────────────────────────
    call    main

    ; ── main'den dönüş: exit(retval) ────────────────────────────
    mov     rdi, rax         ; dönüş kodu → exit kodu
    mov     rax, 3           ; SYS_EXIT
    syscall

    ; Buraya ulaşılmamalı
    hlt