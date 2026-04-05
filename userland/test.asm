global _start

section .text
_start:
    ; syscall: write(1, msg, msg_len)
    mov rax, 1       ; sys_write
    mov rdi, 1       ; fd = stdout
    mov rsi, msg     ; buffer
    mov rdx, msg_len ; count
    syscall

    ; syscall: exit(60)
    mov rax, 60      ; sys_exit
    mov rdi, 0       ; status = 0
    syscall

section .data
msg db "Hello from Ring 3 Userspace (ELF)!", 10
msg_len equ $ - msg
