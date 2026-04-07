;; Assembly test for ioctl, set_tid_address, and exit_group syscalls
;; Tests raw syscall interface without libc - SIMPLIFIED VERSION

[bits 64]

; Constants
SYS_WRITE             equ 1
SYS_IOCTL             equ 16
SYS_SET_TID_ADDRESS   equ 218
SYS_EXIT_GROUP        equ 231

; Common ioctl commands
TCGETS    equ 0x5401
FIOCLEX   equ 0x20006601
FIONREAD  equ 0x541B

; File descriptors
STDOUT    equ 1

; Memory layout
section .bss
    term_buffer: resq 8        ; 64 bytes for termios struct
    tid_storage: resq 1        ; 8 bytes for TID storage
    bytes_avail: resq 1        ; 8 bytes for bytes available

section .rodata
    test_header: db "=== Assembly Syscall Test ===\n"
    test_header_len: equ $ - test_header
    
    test_set_tid: db "Testing set_tid_address...\n"
    test_set_tid_len: equ $ - test_set_tid
    
    test_set_tid_ok: db "set_tid_address succeeded!\n"
    test_set_tid_ok_len: equ $ - test_set_tid_ok
    
    test_tcgets: db "Testing TCGETS...\n"
    test_tcgets_len: equ $ - test_tcgets
    
    test_tcgets_ok: db "TCGETS succeeded!\n"
    test_tcgets_ok_len: equ $ - test_tcgets_ok
    
    test_fioclex: db "Testing FIOCLEX...\n"
    test_fioclex_len: equ $ - test_fioclex
    
    test_fioclex_ok: db "FIOCLEX succeeded!\n"
    test_fioclex_ok_len: equ $ - test_fioclex_ok
    
    test_fionread: db "Testing FIONREAD...\n"
    test_fionread_len: equ $ - test_fionread
    
    test_fionread_ok: db "FIONREAD succeeded!\n"
    test_fionread_ok_len: equ $ - test_fionread_ok
    
    test_done: db "\nAll assembly tests completed!\n"
    test_done_len: equ $ - test_done
    
    exit_msg: db "Exiting with exit_group...\n"
    exit_msg_len: equ $ - exit_msg
    
    newline: db "\n"
    newline_len: equ $ - newline

section .text
    global _start

; Helper: write string to stdout
; Input: rsi = string pointer, rdx = length
write_string:
    mov rax, SYS_WRITE
    mov rdi, STDOUT
    syscall
    ret

_start:
    ; Print header
    lea rsi, [rel test_header]
    mov rdx, test_header_len
    call write_string
    
    ; ============ Test 1: set_tid_address ============
    lea rsi, [rel test_set_tid]
    mov rdx, test_set_tid_len
    call write_string
    
    ; Call set_tid_address(&tid_storage)
    lea rdi, [rel tid_storage]
    mov rax, SYS_SET_TID_ADDRESS
    syscall
    
    ; Check if it succeeded (we expect rax > 0 for valid TID)
    cmp rax, 0
    jle .skip_set_tid_ok
    
    lea rsi, [rel test_set_tid_ok]
    mov rdx, test_set_tid_ok_len
    call write_string
    
.skip_set_tid_ok:
    
    ; ============ Test 2: TCGETS ioctl ============
    lea rsi, [rel test_tcgets]
    mov rdx, test_tcgets_len
    call write_string
    
    ; ioctl(STDOUT, TCGETS, &term_buffer)
    mov rdi, STDOUT
    mov rsi, TCGETS
    lea rdx, [rel term_buffer]
    mov rax, SYS_IOCTL
    syscall
    
    ; Check result
    cmp rax, 0
    jne .skip_tcgets_ok
    
    lea rsi, [rel test_tcgets_ok]
    mov rdx, test_tcgets_ok_len
    call write_string
    
.skip_tcgets_ok:
    
    ; ============ Test 3: FIOCLEX ioctl ============
    lea rsi, [rel test_fioclex]
    mov rdx, test_fioclex_len
    call write_string
    
    ; ioctl(STDOUT, FIOCLEX, NULL)
    mov rdi, STDOUT
    mov rsi, FIOCLEX
    xor rdx, rdx
    mov rax, SYS_IOCTL
    syscall
    
    ; Check result
    cmp rax, 0
    jne .skip_fioclex_ok
    
    lea rsi, [rel test_fioclex_ok]
    mov rdx, test_fioclex_ok_len
    call write_string
    
.skip_fioclex_ok:
    
    ; ============ Test 4: FIONREAD ioctl ============
    lea rsi, [rel test_fionread]
    mov rdx, test_fionread_len
    call write_string
    
    ; ioctl(STDOUT, FIONREAD, &bytes_avail)
    mov rdi, STDOUT
    mov rsi, FIONREAD
    lea rdx, [rel bytes_avail]
    mov rax, SYS_IOCTL
    syscall
    
    ; Check result
    cmp rax, 0
    jne .skip_fionread_ok
    
    lea rsi, [rel test_fionread_ok]
    mov rdx, test_fionread_ok_len
    call write_string
    
.skip_fionread_ok:
    
    ; ============ Print completion message ============
    lea rsi, [rel test_done]
    mov rdx, test_done_len
    call write_string
    
    ; ============ Exit with exit_group ============
    lea rsi, [rel exit_msg]
    mov rdx, exit_msg_len
    call write_string
    
    ; Use exit_group(0) to exit
    xor rdi, rdi                ; status = 0
    mov rax, SYS_EXIT_GROUP
    syscall
    
    ; Should never reach here
    jmp _start
