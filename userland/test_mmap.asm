; ── test_mmap.asm ─────────────────────────────────────────────────────────────
; Tests mmap, munmap, and brk syscalls on AscentOS.
; Build: nasm -f elf64 test_mmap.asm -o test_mmap.o
;        ld -o test_mmap.elf test_mmap.o
; ─────────────────────────────────────────────────────────────────────────────

global _start

; Linux x86_64 syscall numbers
%define SYS_WRITE   1
%define SYS_MMAP    9
%define SYS_MUNMAP 11
%define SYS_BRK    12
%define SYS_EXIT   60

; mmap constants
%define PROT_READ   0x1
%define PROT_WRITE  0x2
%define MAP_PRIVATE   0x02
%define MAP_ANONYMOUS 0x20

section .text

; ── Helper: write(fd=1, buf, len) ────────────────────────────────────────────
; rdi = buf, rsi = len
_write_stdout:
    mov rdx, rsi        ; count
    mov rsi, rdi         ; buf
    mov rdi, 1           ; fd = stdout
    mov rax, SYS_WRITE
    syscall
    ret

; ── Helper: print a 64-bit hex number ───────────────────────────────────────
; rdi = number to print
_print_hex:
    push rbp
    mov rbp, rsp
    sub rsp, 24          ; local buffer on stack (18 chars max: "0x" + 16 hex + newline)

    ; Build "0x" prefix
    lea rcx, [rbp - 24]
    mov byte [rcx], '0'
    mov byte [rcx + 1], 'x'

    ; Convert 64-bit value to hex, stored starting at rcx+2
    mov rax, rdi
    mov r8, 16            ; 16 nibbles
    lea r9, [rcx + 2]    ; write pointer

.hex_loop:
    dec r8
    mov rdx, rax
    shr rdx, cl           ; We'll use a different approach
    ; Actually let's just shift and mask nibble by nibble from MSB
    jmp .hex_method2

.hex_method2:
    ; Reset — do it the simple way: extract each nibble from high to low
    mov rax, rdi
    mov r8, 16
    lea r9, [rcx + 2]

.hex_loop2:
    rol rax, 4            ; rotate left 4 bits — MSB nibble now in low 4 bits
    mov rdx, rax
    and rdx, 0x0F
    cmp rdx, 10
    jl .hex_digit
    add rdx, 'a' - 10
    jmp .hex_store
.hex_digit:
    add rdx, '0'
.hex_store:
    mov [r9], dl
    inc r9
    dec r8
    jnz .hex_loop2

    ; Append newline
    mov byte [r9], 10
    inc r9

    ; Write it out: buf = rcx, len = r9 - rcx
    mov rdi, rcx
    mov rsi, r9
    sub rsi, rcx
    call _write_stdout

    add rsp, 24
    pop rbp
    ret

_start:
    ; ═══════════════════════════════════════════════════════════════════════════
    ; TEST 1: mmap — allocate 4096 bytes of anonymous memory
    ; ═══════════════════════════════════════════════════════════════════════════
    mov rdi, test1_banner
    mov rsi, test1_banner_len
    call _write_stdout

    mov rax, SYS_MMAP
    xor rdi, rdi                              ; addr = NULL (let kernel choose)
    mov rsi, 4096                             ; length = 4096
    mov rdx, PROT_READ | PROT_WRITE           ; prot = RW
    mov r10, MAP_PRIVATE | MAP_ANONYMOUS      ; flags
    mov r8, -1                                ; fd = -1 (anonymous)
    xor r9, r9                                ; offset = 0
    syscall

    ; rax = mapped address (or -1 on failure)
    mov r12, rax          ; Save mmap result in r12

    ; Print "  mmap returned: 0x..."
    mov rdi, mmap_result_msg
    mov rsi, mmap_result_msg_len
    call _write_stdout

    mov rdi, r12
    call _print_hex

    ; Check for MAP_FAILED
    cmp r12, -1
    je .mmap_failed

    ; Write a test pattern into the mapped memory
    mov dword [r12], 0xDEADBEEF
    mov dword [r12 + 4], 0xCAFEBABE

    ; Verify the pattern
    cmp dword [r12], 0xDEADBEEF
    jne .verify_failed

    mov rdi, mmap_ok_msg
    mov rsi, mmap_ok_msg_len
    call _write_stdout
    jmp .test2

.mmap_failed:
    mov rdi, mmap_fail_msg
    mov rsi, mmap_fail_msg_len
    call _write_stdout
    jmp .test2

.verify_failed:
    mov rdi, verify_fail_msg
    mov rsi, verify_fail_msg_len
    call _write_stdout

    ; ═══════════════════════════════════════════════════════════════════════════
    ; TEST 2: munmap — unmap the memory we just mapped
    ; ═══════════════════════════════════════════════════════════════════════════
.test2:
    mov rdi, test2_banner
    mov rsi, test2_banner_len
    call _write_stdout

    mov rax, SYS_MUNMAP
    mov rdi, r12          ; addr from mmap
    mov rsi, 4096         ; length
    syscall

    ; rax = 0 on success
    mov r13, rax

    mov rdi, munmap_result_msg
    mov rsi, munmap_result_msg_len
    call _write_stdout

    mov rdi, r13
    call _print_hex

    test r13, r13
    jnz .munmap_failed

    mov rdi, munmap_ok_msg
    mov rsi, munmap_ok_msg_len
    call _write_stdout
    jmp .test3

.munmap_failed:
    mov rdi, munmap_fail_msg
    mov rsi, munmap_fail_msg_len
    call _write_stdout

    ; ═══════════════════════════════════════════════════════════════════════════
    ; TEST 3: brk — query and expand the program break
    ; ═══════════════════════════════════════════════════════════════════════════
.test3:
    mov rdi, test3_banner
    mov rsi, test3_banner_len
    call _write_stdout

    ; Query current brk
    mov rax, SYS_BRK
    xor rdi, rdi          ; addr = 0 → query
    syscall
    mov r12, rax           ; Save initial brk in r12

    mov rdi, brk_initial_msg
    mov rsi, brk_initial_msg_len
    call _write_stdout

    mov rdi, r12
    call _print_hex

    ; Expand brk by 8192 bytes
    mov rax, SYS_BRK
    lea rdi, [r12 + 8192] ; new break = old + 8192
    syscall
    mov r13, rax           ; Save new brk in r13

    mov rdi, brk_expanded_msg
    mov rsi, brk_expanded_msg_len
    call _write_stdout

    mov rdi, r13
    call _print_hex

    ; Verify we can write to the newly allocated heap
    mov dword [r12], 0x12345678
    cmp dword [r12], 0x12345678
    jne .brk_verify_failed

    mov rdi, brk_ok_msg
    mov rsi, brk_ok_msg_len
    call _write_stdout
    jmp .test4

.brk_verify_failed:
    mov rdi, brk_fail_msg
    mov rsi, brk_fail_msg_len
    call _write_stdout

    ; ═══════════════════════════════════════════════════════════════════════════
    ; TEST 4: brk — shrink back to original
    ; ═══════════════════════════════════════════════════════════════════════════
.test4:
    mov rdi, test4_banner
    mov rsi, test4_banner_len
    call _write_stdout

    mov rax, SYS_BRK
    mov rdi, r12           ; Shrink back to initial brk
    syscall
    mov r14, rax

    mov rdi, brk_shrunk_msg
    mov rsi, brk_shrunk_msg_len
    call _write_stdout

    mov rdi, r14
    call _print_hex

    ; Verify brk returned to initial value
    cmp r14, r12
    jne .shrink_failed

    mov rdi, shrink_ok_msg
    mov rsi, shrink_ok_msg_len
    call _write_stdout
    jmp .done

.shrink_failed:
    mov rdi, shrink_fail_msg
    mov rsi, shrink_fail_msg_len
    call _write_stdout

    ; ═══════════════════════════════════════════════════════════════════════════
    ; All done — exit cleanly
    ; ═══════════════════════════════════════════════════════════════════════════
.done:
    mov rdi, done_msg
    mov rsi, done_msg_len
    call _write_stdout

    mov rax, SYS_EXIT
    xor rdi, rdi           ; status = 0
    syscall

; ── String Data ──────────────────────────────────────────────────────────────
section .rodata

test1_banner     db "=== TEST 1: mmap (anonymous, 4096 bytes) ===", 10
test1_banner_len equ $ - test1_banner

mmap_result_msg     db "  mmap returned: "
mmap_result_msg_len equ $ - mmap_result_msg

mmap_ok_msg     db "  [PASS] mmap: write + verify OK", 10
mmap_ok_msg_len equ $ - mmap_ok_msg

mmap_fail_msg     db "  [FAIL] mmap returned MAP_FAILED", 10
mmap_fail_msg_len equ $ - mmap_fail_msg

verify_fail_msg     db "  [FAIL] mmap memory verify failed", 10
verify_fail_msg_len equ $ - verify_fail_msg

test2_banner     db "=== TEST 2: munmap (unmap 4096 bytes) ===", 10
test2_banner_len equ $ - test2_banner

munmap_result_msg     db "  munmap returned: "
munmap_result_msg_len equ $ - munmap_result_msg

munmap_ok_msg     db "  [PASS] munmap succeeded", 10
munmap_ok_msg_len equ $ - munmap_ok_msg

munmap_fail_msg     db "  [FAIL] munmap returned error", 10
munmap_fail_msg_len equ $ - munmap_fail_msg

test3_banner     db "=== TEST 3: brk (query + expand by 8192) ===", 10
test3_banner_len equ $ - test3_banner

brk_initial_msg     db "  Initial brk: "
brk_initial_msg_len equ $ - brk_initial_msg

brk_expanded_msg     db "  Expanded brk: "
brk_expanded_msg_len equ $ - brk_expanded_msg

brk_ok_msg     db "  [PASS] brk: expand + write OK", 10
brk_ok_msg_len equ $ - brk_ok_msg

brk_fail_msg     db "  [FAIL] brk heap verify failed", 10
brk_fail_msg_len equ $ - brk_fail_msg

test4_banner     db "=== TEST 4: brk (shrink back) ===", 10
test4_banner_len equ $ - test4_banner

brk_shrunk_msg     db "  Shrunk brk: "
brk_shrunk_msg_len equ $ - brk_shrunk_msg

shrink_ok_msg     db "  [PASS] brk: shrink OK", 10
shrink_ok_msg_len equ $ - shrink_ok_msg

shrink_fail_msg     db "  [FAIL] brk: shrink mismatch", 10
shrink_fail_msg_len equ $ - shrink_fail_msg

done_msg     db 10, "All memory syscall tests completed.", 10
done_msg_len equ $ - done_msg
