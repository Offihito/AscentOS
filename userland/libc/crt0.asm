; crt0.asm — Program başlangıcı
; Kernel buraya atlar → main() çağırır → exit()

section .text
global  _start
extern  main

_start:
    and  rsp, ~15       ; 16-byte hizala
    xor  rdi, rdi       ; argc = 0  (şimdilik)
    xor  rsi, rsi       ; argv = NULL
    call main
    ; main'den dönüş → exit(rax)
    mov  rdi, rax
    mov  rax, 3         ; SYS_EXIT
    syscall
    hlt