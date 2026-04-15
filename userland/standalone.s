.global _start

_start:
    # write(1, msg, len)
    mov     $1, %rax        # sys_write
    mov     $1, %rdi        # fd = stdout
    lea     msg(%rip), %rsi # buffer
    mov     $14, %rdx       # len
    syscall

    # exit(42)
    mov     $60, %rax       # sys_exit
    mov     $42, %rdi       # status
    syscall

msg:
    .ascii "Hello Kernel!\n"
