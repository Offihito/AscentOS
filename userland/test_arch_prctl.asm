; ── test_arch_prctl.asm ───────────────────────────────────────────────────────
; Tests arch_prctl syscall (SET_FS / GET_FS) for TLS support on AscentOS.
; Build: nasm -f elf64 test_arch_prctl.asm -o test_arch_prctl.o
;        ld -o test_arch_prctl.elf test_arch_prctl.o
; ─────────────────────────────────────────────────────────────────────────────

global _start

; Linux x86_64 syscall numbers
%define SYS_WRITE       1
%define SYS_ARCH_PRCTL  158
%define SYS_EXIT        60

; arch_prctl sub-commands
%define ARCH_SET_FS     0x1002
%define ARCH_GET_FS     0x1003

section .bss
    fs_readback: resq 1     ; 8 bytes to store GET_FS result

section .data
    ; TLS block: simulates what musl would set up
    ; The first qword is the "self pointer" (musl convention: tp points to itself)
    tls_block:
        dq 0                ; slot 0: self-pointer (filled at runtime)
        dq 0xCAFEBABE0000   ; slot 1: test value 1
        dq 0xDEAD00000BEEF  ; slot 2: test value 2

section .text

; ── Helper: write(fd=1, buf, len) ────────────────────────────────────────────
_write_stdout:
    mov rdx, rsi
    mov rsi, rdi
    mov rdi, 1
    mov rax, SYS_WRITE
    syscall
    ret

; ── Helper: print hex number ─────────────────────────────────────────────────
; rdi = number to print
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

    mov rdi, rcx
    mov rsi, r9
    sub rsi, rcx
    call _write_stdout

    add rsp, 24
    pop rbp
    ret

_start:
    ; ═══════════════════════════════════════════════════════════════════════════
    ; TEST 1: ARCH_SET_FS — Set the FS base to our TLS block
    ; ═══════════════════════════════════════════════════════════════════════════
    mov rdi, test1_msg
    mov rsi, test1_msg_len
    call _write_stdout

    ; Store self-pointer in tls_block[0] (musl convention)
    lea rax, [rel tls_block]
    mov [rel tls_block], rax

    ; Print the address we're setting
    mov rdi, setfs_addr_msg
    mov rsi, setfs_addr_msg_len
    call _write_stdout

    lea rdi, [rel tls_block]
    push rdi                   ; save tls_block addr
    call _print_hex

    ; arch_prctl(ARCH_SET_FS, &tls_block)
    mov rax, SYS_ARCH_PRCTL
    mov rdi, ARCH_SET_FS
    pop rsi                    ; rsi = &tls_block
    syscall

    ; Check return value
    test rax, rax
    jnz .set_fs_failed

    mov rdi, setfs_ok_msg
    mov rsi, setfs_ok_msg_len
    call _write_stdout
    jmp .test2

.set_fs_failed:
    mov rdi, setfs_fail_msg
    mov rsi, setfs_fail_msg_len
    call _write_stdout

    ; ═══════════════════════════════════════════════════════════════════════════
    ; TEST 2: Read from fs:0 — should return self pointer (tls_block address)
    ; ═══════════════════════════════════════════════════════════════════════════
.test2:
    mov rdi, test2_msg
    mov rsi, test2_msg_len
    call _write_stdout

    ; Read the self-pointer via FS segment override
    mov rax, [fs:0]
    mov r12, rax          ; save for comparison BEFORE _write_stdout clobbers rax

    ; Print what we got
    mov rdi, fsread_msg
    mov rsi, fsread_msg_len
    call _write_stdout

    mov rdi, r12
    call _print_hex

    ; Compare: fs:0 should == &tls_block
    lea rax, [rel tls_block]
    cmp r12, rax
    jne .read_fs_failed

    mov rdi, fsread_ok_msg
    mov rsi, fsread_ok_msg_len
    call _write_stdout
    jmp .test3

.read_fs_failed:
    mov rdi, fsread_fail_msg
    mov rsi, fsread_fail_msg_len
    call _write_stdout

    ; ═══════════════════════════════════════════════════════════════════════════
    ; TEST 3: Read from fs:8 and fs:16 — test TLS slot access
    ; ═══════════════════════════════════════════════════════════════════════════
.test3:
    mov rdi, test3_msg
    mov rsi, test3_msg_len
    call _write_stdout

    ; Read TLS slot 1 (offset 8)
    mov rax, [fs:8]
    mov r12, rax

    mov rdi, slot1_msg
    mov rsi, slot1_msg_len
    call _write_stdout

    mov rdi, r12
    call _print_hex

    ; Verify slot 1 == 0xCAFEBABE0000
    mov rax, 0xCAFEBABE0000
    cmp r12, rax
    jne .slot_failed

    ; Read TLS slot 2 (offset 16)
    mov rax, [fs:16]
    mov r13, rax

    mov rdi, slot2_msg
    mov rsi, slot2_msg_len
    call _write_stdout

    mov rdi, r13
    call _print_hex

    ; Verify slot 2 == 0xDEAD00000BEEF
    mov rax, 0xDEAD00000BEEF
    cmp r13, rax
    jne .slot_failed

    mov rdi, slot_ok_msg
    mov rsi, slot_ok_msg_len
    call _write_stdout
    jmp .test4

.slot_failed:
    mov rdi, slot_fail_msg
    mov rsi, slot_fail_msg_len
    call _write_stdout

    ; ═══════════════════════════════════════════════════════════════════════════
    ; TEST 4: ARCH_GET_FS — read back the FS base via syscall
    ; ═══════════════════════════════════════════════════════════════════════════
.test4:
    mov rdi, test4_msg
    mov rsi, test4_msg_len
    call _write_stdout

    ; arch_prctl(ARCH_GET_FS, &fs_readback)
    mov rax, SYS_ARCH_PRCTL
    mov rdi, ARCH_GET_FS
    lea rsi, [rel fs_readback]
    syscall

    test rax, rax
    jnz .get_fs_failed

    ; Read the value stored by the kernel
    mov rax, [rel fs_readback]
    mov r12, rax

    mov rdi, getfs_val_msg
    mov rsi, getfs_val_msg_len
    call _write_stdout

    mov rdi, r12
    call _print_hex

    ; Verify it matches our tls_block
    lea rax, [rel tls_block]
    cmp r12, rax
    jne .get_fs_failed

    mov rdi, getfs_ok_msg
    mov rsi, getfs_ok_msg_len
    call _write_stdout
    jmp .done

.get_fs_failed:
    mov rdi, getfs_fail_msg
    mov rsi, getfs_fail_msg_len
    call _write_stdout

    ; ═══════════════════════════════════════════════════════════════════════════
    ; Done
    ; ═══════════════════════════════════════════════════════════════════════════
.done:
    mov rdi, done_msg
    mov rsi, done_msg_len
    call _write_stdout

    mov rax, SYS_EXIT
    xor rdi, rdi
    syscall

; ── String Data ──────────────────────────────────────────────────────────────
section .rodata

test1_msg       db "=== TEST 1: arch_prctl(ARCH_SET_FS) ===", 10
test1_msg_len   equ $ - test1_msg

setfs_addr_msg      db "  Setting FS base to: "
setfs_addr_msg_len  equ $ - setfs_addr_msg

setfs_ok_msg      db "  [PASS] ARCH_SET_FS returned 0", 10
setfs_ok_msg_len  equ $ - setfs_ok_msg

setfs_fail_msg      db "  [FAIL] ARCH_SET_FS returned error", 10
setfs_fail_msg_len  equ $ - setfs_fail_msg

test2_msg       db "=== TEST 2: Read fs:0 (self-pointer) ===", 10
test2_msg_len   equ $ - test2_msg

fsread_msg      db "  fs:0 = "
fsread_msg_len  equ $ - fsread_msg

fsread_ok_msg      db "  [PASS] fs:0 matches TLS block address", 10
fsread_ok_msg_len  equ $ - fsread_ok_msg

fsread_fail_msg      db "  [FAIL] fs:0 does not match", 10
fsread_fail_msg_len  equ $ - fsread_fail_msg

test3_msg       db "=== TEST 3: Read TLS slots (fs:8, fs:16) ===", 10
test3_msg_len   equ $ - test3_msg

slot1_msg       db "  fs:8  (slot 1) = "
slot1_msg_len   equ $ - slot1_msg

slot2_msg       db "  fs:16 (slot 2) = "
slot2_msg_len   equ $ - slot2_msg

slot_ok_msg      db "  [PASS] TLS slot values verified", 10
slot_ok_msg_len  equ $ - slot_ok_msg

slot_fail_msg      db "  [FAIL] TLS slot value mismatch", 10
slot_fail_msg_len  equ $ - slot_fail_msg

test4_msg       db "=== TEST 4: arch_prctl(ARCH_GET_FS) ===", 10
test4_msg_len   equ $ - test4_msg

getfs_val_msg      db "  GET_FS returned: "
getfs_val_msg_len  equ $ - getfs_val_msg

getfs_ok_msg      db "  [PASS] GET_FS matches original address", 10
getfs_ok_msg_len  equ $ - getfs_ok_msg

getfs_fail_msg      db "  [FAIL] GET_FS mismatch or error", 10
getfs_fail_msg_len  equ $ - getfs_fail_msg

done_msg        db 10, "All arch_prctl tests completed.", 10
done_msg_len    equ $ - done_msg
