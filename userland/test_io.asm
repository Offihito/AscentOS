; ── test_io.asm ──────────────────────────────────────────────────────────────
; Tests I/O syscalls: open, read, write, lseek, close
; Build: nasm -f elf64 test_io.asm -o test_io.o
;        ld -o test_io.elf test_io.o
; ─────────────────────────────────────────────────────────────────────────────

global _start

; Linux x86_64 syscall numbers
%define SYS_READ    0
%define SYS_WRITE   1
%define SYS_OPEN    2
%define SYS_CLOSE   3
%define SYS_LSEEK   8
%define SYS_EXIT    60

; lseek whence
%define SEEK_SET    0
%define SEEK_CUR    1
%define SEEK_END    2

section .bss
    read_buf: resb 256

section .data
    filename: db "/mnt/hello.txt", 0
    
    msg_open: db "=== TEST 1: open(/mnt/hello.txt) ===", 10, 0
    msg_open_ok: db "  [PASS] open succeeded, fd: ", 0
    msg_open_fail: db "  [FAIL] open failed", 10, 0
    
    msg_read: db "=== TEST 2: read(fd) ===", 10, 0
    msg_read_ok: db "  [PASS] read succeeded, content:", 10, "  '", 0
    msg_read_end: db "'", 10, 0
    msg_read_fail: db "  [FAIL] read failed", 10, 0
    
    msg_lseek: db "=== TEST 3: lseek(fd, 0, SEEK_SET) ===", 10, 0
    msg_lseek_ok: db "  [PASS] lseek succeeded", 10, 0
    msg_lseek_fail: db "  [FAIL] lseek failed", 10, 0
    
    msg_read2: db "=== TEST 4: read(fd) again ===", 10, 0
    
    msg_close: db "=== TEST 5: close(fd) ===", 10, 0
    msg_close_ok: db "  [PASS] close succeeded", 10, 0
    msg_close_fail: db "  [FAIL] close failed", 10, 0
    
    msg_done: db 10, "All I/O tests completed.", 10, 0

section .text

; Helper: strlen(rdi) -> rax
_strlen:
    xor rax, rax
.loop:
    cmp byte [rdi + rax], 0
    je .done
    inc rax
    jmp .loop
.done:
    ret

; Helper: write_str(rdi = string)
_write_str:
    push rbp
    mov rbp, rsp
    push rdi
    call _strlen
    mov rdx, rax      ; count
    pop rdi
    mov rsi, rdi      ; buf
    mov rdi, 1        ; stdout
    mov rax, SYS_WRITE
    syscall
    pop rbp
    ret

; Helper: print hex (rdi = number)
_print_hex:
    push rbp
    mov rbp, rsp
    sub rsp, 24

    lea rcx, [rbp - 24]
    mov byte [rcx], '0'
    mov byte [rcx + 1], 'x'

    mov rax, rdi
    lea r9, [rcx + 2]
    mov r8, 16

.hex_loop:
    rol rax, 4
    mov rdx, rax
    and rdx, 0x0F
    cmp rdx, 10
    jl .digit
    add rdx, 'a' - 10
    jmp .store
.digit:
    add rdx, '0'
.store:
    mov [r9], dl
    inc r9
    dec r8
    jnz .hex_loop

    mov byte [r9], 10      ; newline
    inc r9
    mov byte [r9], 0       ; null terminator

    mov rdi, rcx
    call _write_str

    add rsp, 24
    pop rbp
    ret

_start:
    ; TEST 1: open
    mov rdi, msg_open
    call _write_str
    
    mov rax, SYS_OPEN
    lea rdi, [rel filename]
    xor rsi, rsi
    xor rdx, rdx
    syscall
    
    cmp rax, 0
    jl .open_fail
    mov r12, rax ; Save FD in r12
    
    mov rdi, msg_open_ok
    call _write_str
    mov rdi, r12
    call _print_hex
    jmp .test2
    
.open_fail:
    mov rdi, msg_open_fail
    call _write_str
    jmp .done
    
.test2:
    ; TEST 2: read
    mov rdi, msg_read
    call _write_str
    
    mov rax, SYS_READ
    mov rdi, r12
    lea rsi, [rel read_buf]
    mov rdx, 20
    syscall
    
    cmp rax, 0
    jl .read_fail
    mov r13, rax ; bytes read
    
    mov rdi, msg_read_ok
    call _write_str
    
    ; write the read content
    mov rax, SYS_WRITE
    mov rdi, 1
    lea rsi, [rel read_buf]
    mov rdx, r13
    syscall
    
    mov rdi, msg_read_end
    call _write_str
    jmp .test3
    
.read_fail:
    mov rdi, msg_read_fail
    call _write_str
    jmp .done
    
.test3:
    ; TEST 3: lseek to 0
    mov rdi, msg_lseek
    call _write_str
    
    mov rax, SYS_LSEEK
    mov rdi, r12
    mov rsi, 0
    mov rdx, SEEK_SET
    syscall
    
    cmp rax, 0
    jl .lseek_fail
    
    mov rdi, msg_lseek_ok
    call _write_str
    jmp .test4
    
.lseek_fail:
    mov rdi, msg_lseek_fail
    call _write_str
    jmp .done
    
.test4:
    ; TEST 4: read again
    mov rdi, msg_read2
    call _write_str
    
    mov rax, SYS_READ
    mov rdi, r12
    lea rsi, [rel read_buf]
    mov rdx, 20
    syscall
    
    mov r13, rax
    mov rdi, msg_read_ok
    call _write_str
    
    mov rax, SYS_WRITE
    mov rdi, 1
    lea rsi, [rel read_buf]
    mov rdx, r13
    syscall
    
    mov rdi, msg_read_end
    call _write_str

    ; TEST 5: close
    mov rdi, msg_close
    call _write_str
    
    mov rax, SYS_CLOSE
    mov rdi, r12
    syscall
    
    cmp rax, 0
    jne .close_fail
    
    mov rdi, msg_close_ok
    call _write_str
    jmp .done
    
.close_fail:
    mov rdi, msg_close_fail
    call _write_str

.done:
    mov rdi, msg_done
    call _write_str
    
    mov rax, SYS_EXIT
    xor rdi, rdi
    syscall
