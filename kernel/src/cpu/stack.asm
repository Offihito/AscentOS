[bits 64]

global cpu_switch_stack
global cpu_jump_to_stack

section .text

; void cpu_switch_stack(uint64_t new_rsp)
; Safely switches the stack and returns into the caller.
cpu_switch_stack:
    cli                     ; Disable interrupts for safe migration
    ; The current stack has the return address at [rsp]
    pop rsi                 ; Pop return address into rsi
    mov rsp, rdi            ; Switch to the new stack top
    push rsi                ; Push return address onto the new stack
    sti                     ; Re-enable interrupts
    ret                     ; Return to caller on the new stack

; void cpu_jump_to_stack(uint64_t new_rsp, void (*target)(void))
; Discards current stack and jumps to a new function on a new stack.
cpu_jump_to_stack:
    cli
    mov rsp, rdi            ; Switch stacks
    sti
    jmp rsi                 ; Jump to target function
