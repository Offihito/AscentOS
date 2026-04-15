.section .data
msg:    .ascii "Hello from standalone assembly!\n"
len = . - msg

.section .text
.global main
main:
    # Linux x86-64 syscall write
    movl    $1, %eax
    movl    $1, %edi
    leaq    msg(%rip), %rsi
    movl    $len, %edx
    syscall

    # exit(0)
    xorl    %eax, %eax
    ret