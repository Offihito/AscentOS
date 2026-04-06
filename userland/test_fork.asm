; ── test_fork.asm ────────────────────────────────────────────────────────────
; Tests fork() and getpid() syscalls.
; Build: nasm -f elf64 test_fork.asm -o test_fork.o
;        ld -o test_fork.elf test_fork.o
; ─────────────────────────────────────────────────────────────────────────────

global _start

; Linux x86_64 syscall numbers
%define SYS_WRITE   1
%define SYS_GETPID  39
%define SYS_FORK    57
%define SYS_EXIT    60

section .data
    msg_start:     db "=== Fork Test ===", 10, 0
    msg_forking:   db "Calling fork()...", 10, 0
    msg_parent:    db "[PARENT] Fork returned child PID: ", 0
    msg_parent_pid:db "[PARENT] My PID: ", 0
    msg_child:     db "[CHILD]  I am the child! fork() returned: ", 0
    msg_child_pid: db "[CHILD]  My PID: ", 0
    msg_done:      db "Process exiting.", 10, 0
    newline:       db 10, 0

section .bss
    num_buf: resb 32

section .text

; ── Helper: strlen ───────────────────────────────────────────────────────────
; rdi = string pointer -> rax = length
_strlen:
    xor rax, rax
.loop:
    cmp byte [rdi + rax], 0
    je .done
    inc rax
    jmp .loop
.done:
    ret

; ── Helper: write null-terminated string to stdout ───────────────────────────
; rdi = string pointer
_write_str:
    push rbp
    mov rbp, rsp
    push rdi
    call _strlen
    mov rdx, rax        ; count
    pop rsi             ; buf = string pointer
    mov rdi, 1          ; fd = stdout
    mov rax, SYS_WRITE
    syscall
    pop rbp
    ret

; ── Helper: print uint64 in decimal ─────────────────────────────────────────
; rdi = number to print
_print_dec:
    push rbp
    mov rbp, rsp

    ; Convert number to decimal string (reversed)
    lea rcx, [rel num_buf]
    add rcx, 20             ; point to end of buffer
    mov byte [rcx], 0       ; null terminator
    dec rcx
    mov byte [rcx], 10      ; newline

    mov rax, rdi
    test rax, rax
    jnz .convert

    ; Special case: number is 0
    dec rcx
    mov byte [rcx], '0'
    jmp .print

.convert:
    mov r8, 10
.digit_loop:
    test rax, rax
    jz .print
    xor rdx, rdx
    div r8
    add dl, '0'
    dec rcx
    mov [rcx], dl
    jmp .digit_loop

.print:
    mov rdi, rcx
    call _write_str

    pop rbp
    ret

; ── Entry Point ──────────────────────────────────────────────────────────────
_start:
    ; Print start banner
    lea rdi, [rel msg_start]
    call _write_str

    ; Print our PID before forking
    lea rdi, [rel msg_forking]
    call _write_str

    ; ── Call fork() ──────────────────────────────────────────────────────────
    mov rax, SYS_FORK
    syscall
    ; rax = child PID in parent, 0 in child, negative on error

    ; Save fork return value
    mov r12, rax

    ; Check: are we parent or child?
    test rax, rax
    jz .child
    js .error

    ; ── PARENT PATH ─────────────────────────────────────────────────────────
.parent:
    lea rdi, [rel msg_parent]
    call _write_str
    mov rdi, r12            ; child PID
    call _print_dec

    ; Get and print our own PID
    mov rax, SYS_GETPID
    syscall
    mov r13, rax

    lea rdi, [rel msg_parent_pid]
    call _write_str
    mov rdi, r13
    call _print_dec

    jmp .exit

    ; ── CHILD PATH ──────────────────────────────────────────────────────────
.child:
    lea rdi, [rel msg_child]
    call _write_str
    mov rdi, r12            ; should be 0
    call _print_dec

    ; Get and print our own PID
    mov rax, SYS_GETPID
    syscall
    mov r13, rax

    lea rdi, [rel msg_child_pid]
    call _write_str
    mov rdi, r13
    call _print_dec

    jmp .exit

.error:
    ; Fork failed — just exit
    jmp .exit

.exit:
    lea rdi, [rel msg_done]
    call _write_str

    mov rax, SYS_EXIT
    xor rdi, rdi
    syscall
