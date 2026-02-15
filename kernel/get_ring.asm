; get_ring.asm - Get current CPU privilege ring level

section .text
bits 64

; uint8_t get_current_ring(void)
; Returns the current privilege level (CPL) from CS register
global get_current_ring
get_current_ring:
    ; CS register'ın son 2 biti CPL'i verir (Current Privilege Level)
    ; CS & 0x3 = Ring level (0=kernel, 3=user)
    
    ; Get CS register value
    mov ax, cs
    
    ; Mask to get RPL (Requested Privilege Level) bits [1:0]
    and ax, 0x3
    
    ; Return in RAX (already there from AX)
    movzx rax, ax
    ret