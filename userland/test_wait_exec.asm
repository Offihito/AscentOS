section .rodata
    msg_parent_start db "[WAIT_EXEC] Parent: Calling fork()...", 10
    msg_parent_start_len equ $ - msg_parent_start

    msg_child db "[WAIT_EXEC] Child: Calling execve()...", 10
    msg_child_len equ $ - msg_child

    msg_parent_wait db "[WAIT_EXEC] Parent: Waiting for child...", 10
    msg_parent_wait_len equ $ - msg_parent_wait

    msg_parent_done db "[WAIT_EXEC] Parent: Child exited with status ", 0
    msg_parent_done_len equ $ - msg_parent_done

    nl db 10

    exec_path db "/mnt/test_execve.elf", 0

section .bss
    wstatus resd 1

section .text
    global _start

_start:
    ; print parent start
    mov rax, 1
    mov rdi, 1
    mov rsi, msg_parent_start
    mov rdx, msg_parent_start_len
    syscall

    ; fork
    mov rax, 57         ; SYS_FORK
    syscall

    cmp rax, 0
    je .child
    
.parent:
    ; print parent wait
    mov rax, 1
    mov rdi, 1
    mov rsi, msg_parent_wait
    mov rdx, msg_parent_wait_len
    syscall

    ; wait4(-1, &wstatus, 0, NULL)
    mov rax, 61         ; SYS_WAIT4
    mov rdi, -1
    mov rsi, wstatus
    mov rdx, 0
    mov r10, 0
    syscall

    ; print parent done
    mov rax, 1
    mov rdi, 1
    mov rsi, msg_parent_done
    mov rdx, msg_parent_done_len
    syscall

    ; extract status (wstatus >> 8)
    mov eax, dword [wstatus]
    shr eax, 8
    and eax, 0xFF

    ; print status code
    call print_num

    mov rax, 1
    mov rdi, 1
    mov rsi, nl
    mov rdx, 1
    syscall

    ; exit(0)
    mov rax, 60
    mov rdi, 0
    syscall

.child:
    ; print child execve msg
    mov rax, 1
    mov rdi, 1
    mov rsi, msg_child
    mov rdx, msg_child_len
    syscall

    ; execve("/mnt/test_execve.elf", NULL, NULL)
    mov rax, 59         ; SYS_EXECVE
    mov rdi, exec_path
    mov rsi, 0
    mov rdx, 0
    syscall
    
    ; if we get here, exec failed
    mov rax, 60
    mov rdi, 1
    syscall

; Simple print_num routine (clobbers rax, rdi, rsi, rdx, rcx, r8)
print_num:
    ; number is in eax
    mov rcx, 10
    xor rdx, rdx
    div rcx
    add al, '0'
    add dl, '0'
    
    push rdx
    push rax

    mov rax, 1
    mov rdi, 1
    mov rsi, rsp
    mov rdx, 1
    syscall

    add rsp, 8
    mov rax, 1
    mov rdi, 1
    mov rsi, rsp
    mov rdx, 1
    syscall

    add rsp, 8
    ret
