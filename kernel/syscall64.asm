; syscall64.asm - SYSCALL/SYSRET Entry Point for AscentOS
; 
; This file implements the low-level syscall entry point.
; When userspace executes SYSCALL instruction:
;   - RIP is saved to RCX
;   - RFLAGS is saved to R11
;   - CPU jumps here (address set in IA32_LSTAR MSR)
;   - CPU switches to ring 0 (CS/SS from IA32_STAR MSR)

[BITS 64]

global syscall_entry
extern syscall_handler
extern serial_print

section .text

; ===========================================
; SYSCALL ENTRY POINT
; ===========================================
; On entry from userspace:
;   RAX = syscall number
;   RDI = arg1
;   RSI = arg2
;   RDX = arg3
;   R10 = arg4 (NOT RCX! SYSCALL uses RCX for return address)
;   R8  = arg5
;   R9  = arg6
;   RCX = user RIP (return address)
;   R11 = user RFLAGS
;
; We need to:
;   1. Switch to kernel stack
;   2. Save all registers
;   3. Call syscall_handler
;   4. Restore registers
;   5. Return to userspace with SYSRET

syscall_entry:
    ; At this point:
    ;   - We're in ring 0
    ;   - Stack is still user stack (DANGEROUS!)
    ;   - RCX = user RIP
    ;   - R11 = user RFLAGS
    
    ; First, we need to switch to kernel stack
    ; This is tricky because we can't use stack yet
    
    ; Save user RSP in a scratch register temporarily
    ; We'll use the GS segment for per-CPU data (for now, we'll use a global)
    swapgs              ; Swap GS with kernel GS base (contains kernel data)
    
    ; Save user RSP in kernel per-CPU area
    ; For simplicity, we'll use a global variable for now
    ; In a real system, GS would point to per-CPU data structure
    mov [rel user_rsp_save], rsp
    
    ; Load kernel stack from per-CPU area
    ; For now, we'll use a static kernel stack
    mov rsp, [rel kernel_syscall_stack_top]
    
    ; Now we have a valid kernel stack, push everything
    push qword 0x1B     ; User SS (segment selector for usermode data)
    push qword [rel user_rsp_save]  ; User RSP
    push r11            ; User RFLAGS (saved by SYSCALL)
    push qword 0x23     ; User CS (segment selector for usermode code)
    push rcx            ; User RIP (return address, saved by SYSCALL)
    
    ; Save all general purpose registers
    push rax            ; Syscall number
    push rbx
    push rcx            ; User RIP (duplicate, but useful)
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11            ; User RFLAGS (duplicate)
    push r12
    push r13
    push r14
    push r15
    
    ; Save segment registers (mostly for completeness)
    mov ax, ds
    push rax
    mov ax, es
    push rax
    mov ax, fs
    push rax
    ; GS is already swapped, don't push it
    
    ; Set up kernel segments
    mov ax, 0x10        ; Kernel data segment
    mov ds, ax
    mov es, ax
    mov fs, ax
    ; GS stays swapped (kernel GS)
    
    ; Prepare arguments for syscall_handler(syscall_num, arg1, arg2, arg3, arg4, arg5)
    ; Arguments are already in registers, we just need to ensure they're correct
    ; syscall_handler expects:
    ;   RDI = syscall_num (we need to move RAX to RDI)
    ;   RSI = arg1 (currently RDI)
    ;   RDX = arg2 (currently RSI)
    ;   RCX = arg3 (currently RDX)
    ;   R8  = arg4 (currently R10)
    ;   R9  = arg5 (currently R8)
    
    mov rdi, rax        ; syscall_num (was in RAX)
    mov rsi, rdi        ; arg1 (was in RDI) - WAIT, this overwrites! Need to be careful
    
    ; Actually, let's reorganize. We saved everything, so load from stack:
    ; Stack layout (top to bottom):
    ; [rsp+0]  = GS (not pushed)
    ; [rsp+0]  = FS
    ; [rsp+8]  = ES
    ; [rsp+16] = DS
    ; [rsp+24] = R15
    ; [rsp+32] = R14
    ; [rsp+40] = R13
    ; [rsp+48] = R12
    ; [rsp+56] = R11
    ; [rsp+64] = R10
    ; [rsp+72] = R9
    ; [rsp+80] = R8
    ; [rsp+88] = RBP
    ; [rsp+96] = RDI (arg1)
    ; [rsp+104] = RSI (arg2)
    ; [rsp+112] = RDX (arg3)
    ; [rsp+120] = RCX (user RIP)
    ; [rsp+128] = RBX
    ; [rsp+136] = RAX (syscall number)
    
    ; Let's do this more carefully
    ; We need to call: syscall_handler(rax, rdi, rsi, rdx, r10, r8)
    ; As:              syscall_handler(rdi, rsi, rdx, rcx, r8,  r9)
    
    mov rdi, [rsp+136]  ; syscall_num (RAX)
    mov rsi, [rsp+96]   ; arg1 (RDI)
    mov rdx, [rsp+104]  ; arg2 (RSI)
    mov rcx, [rsp+112]  ; arg3 (RDX)
    mov r8,  [rsp+64]   ; arg4 (R10)
    mov r9,  [rsp+80]   ; arg5 (R8)
    ; arg6 (R9) would be on stack as 7th argument if needed
    
    ; Align stack to 16 bytes (required by System V ABI)
    ; RSP should be 16-byte aligned before CALL
    mov rbp, rsp
    and rsp, ~0xF       ; Align to 16 bytes
    
    ; Call the C handler
    call syscall_handler
    
    ; Restore stack
    mov rsp, rbp
    
    ; Result is in RAX, preserve it
    mov [rsp+136], rax  ; Store result back to saved RAX
    
    ; Restore segment registers
    pop rax
    mov fs, ax
    pop rax
    mov es, ax
    pop rax
    mov ds, ax
    
    ; Restore general purpose registers
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11             ; User RFLAGS (will be restored by SYSRET)
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx             ; User RIP (will be restored by SYSRET)
    pop rbx
    pop rax             ; Return value (modified by handler)
    
    ; Get return frame
    pop rcx             ; User RIP
    pop r11             ; User CS (ignored, SYSRET uses STAR)
    pop r11             ; User RFLAGS
    pop rsp             ; User RSP (DANGEROUS! But we're about to switch)
    ; User SS is ignored by SYSRET
    
    ; At this point:
    ;   RAX = return value
    ;   RCX = user RIP (where to return)
    ;   R11 = user RFLAGS (will be restored)
    ;   RSP = user RSP
    
    ; Swap GS back to user GS
    swapgs
    
    ; Return to userspace
    ; SYSRET will:
    ;   - Restore RIP from RCX
    ;   - Restore RFLAGS from R11
    ;   - Set CS/SS from IA32_STAR MSR
    ;   - Switch to ring 3
    sysret

; ===========================================
; HELPER: Save user RSP temporarily
; ===========================================
section .data
align 8
user_rsp_save:
    dq 0

; Kernel syscall stack (8KB for now)
section .bss
align 16
kernel_syscall_stack:
    resb 8192
kernel_syscall_stack_top: