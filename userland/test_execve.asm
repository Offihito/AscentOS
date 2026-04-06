section .rodata
    msg db "[EXEC] Hello from the newly exec'd program!", 10
    msg_len equ $ - msg

section .text
    global _start

_start:
    ; write(1, msg, msg_len)
    mov rax, 1          ; SYS_WRITE
    mov rdi, 1          ; stdout
    mov rsi, msg
    mov rdx, msg_len
    syscall

    ; exit(42)
    mov rax, 60         ; SYS_EXIT
    mov rdi, 42         ; exit status 42
    syscall
