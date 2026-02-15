; usermode_transition.asm - Ring 3 Transition Code
; This file contains assembly routines for transitioning to usermode (Ring 3)

global jump_to_usermode

section .text
bits 64

; ============================================================================
; jump_to_usermode - Transition from Ring 0 to Ring 3
; ============================================================================
; Parameters:
;   rdi = entry_point (user code address)
;   rsi = stack_pointer (user stack)
;
; This function uses IRET to transition to Ring 3
; IRET expects the following on the stack (in order):
;   SS      (user stack segment)
;   RSP     (user stack pointer)
;   RFLAGS  (flags register)
;   CS      (user code segment)
;   RIP     (user code address)

jump_to_usermode:
    ; Save parameters
    mov rcx, rdi        ; Entry point
    mov rdx, rsi        ; Stack pointer
    
    ; Disable interrupts during transition
    cli
    
    ; Setup user mode segments
    ; GDT layout: 0x00=null, 0x08=kernel_code, 0x10=kernel_data,
    ;             0x18=user_data, 0x20=user_code
    mov ax, 0x18 | 3    ; User data segment with RPL=3
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    
    ; Build IRET frame on kernel stack
    ; We're currently on kernel stack, so we can push to it
    
    ; Push SS (user stack segment)
    push qword 0x18 | 3     ; User data segment | RPL=3
    
    ; Push RSP (user stack pointer)
    push rdx                ; User stack from parameter
    
    ; Push RFLAGS
    pushfq                  ; Push current flags
    pop rax
    or rax, 0x200           ; Set IF (enable interrupts in usermode)
    push rax
    
    ; Push CS (user code segment)
    push qword 0x20 | 3     ; User code segment | RPL=3
    
    ; Push RIP (user entry point)
    push rcx                ; Entry point from parameter
    
    ; Clear all general purpose registers for security
    ; (Don't leak kernel data to usermode)
    xor rax, rax
    xor rbx, rbx
    xor rcx, rcx
    xor rdx, rdx
    xor rsi, rsi
    xor rdi, rdi
    xor r8, r8
    xor r9, r9
    xor r10, r10
    xor r11, r11
    xor r12, r12
    xor r13, r13
    xor r14, r14
    xor r15, r15
    
    ; IRET will:
    ; 1. Pop RIP -> Jump to user code
    ; 2. Pop CS  -> Switch to user code segment
    ; 3. Pop RFLAGS -> Restore flags (with IF set)
    ; 4. Pop RSP -> Switch to user stack
    ; 5. Pop SS  -> Switch to user data segment
    ; 6. Set CPL to 3 (Ring 3)
    
    iretq
    
    ; Should never reach here
    ud2

; ============================================================================
; Helper function to get current privilege level
; ============================================================================
global get_current_ring

get_current_ring:
    mov rax, cs
    and rax, 3      ; Extract RPL bits (lowest 2 bits)
    ret

; ============================================================================
; Test if we're in usermode
; ============================================================================
global is_usermode

is_usermode:
    mov rax, cs
    and rax, 3
    cmp rax, 3
    je .yes
    xor rax, rax    ; Return 0 (not usermode)
    ret
.yes:
    mov rax, 1      ; Return 1 (is usermode)
    ret