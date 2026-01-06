; task_switch.asm - Context Switching for Multitasking
; Pure context switch function - interrupt handlers are in interrupts64.asm

global task_switch_asm

section .text
bits 64

; void task_switch_asm(Task* old_task, Task* new_task)
; RDI = old_task (pointer to old task's CPUContext structure)
; RSI = new_task (pointer to new task's CPUContext structure)
;
; CPUContext structure offsets:
; 0:   r15, r14, r13, r12, r11, r10, r9, r8 (64 bytes)
; 64:  rbp, rdi, rsi, rdx, rcx, rbx, rax (56 bytes)
; 120: rip (8 bytes)
; 128: cs (8 bytes)
; 136: rflags (8 bytes)
; 144: rsp (8 bytes)
; 152: ss (8 bytes)

task_switch_asm:
    ; Check if old_task is NULL
    test rdi, rdi
    jz .load_new_task
    
    ; Save old task context
    ; Save callee-saved registers
    mov [rdi + 0], r15
    mov [rdi + 8], r14
    mov [rdi + 16], r13
    mov [rdi + 24], r12
    mov [rdi + 32], r11
    mov [rdi + 40], r10
    mov [rdi + 48], r9
    mov [rdi + 56], r8
    mov [rdi + 64], rbp
    
    ; Save caller-saved registers
    ; Note: RDI and RSI are function parameters, save them too
    mov rax, rdi
    mov [rax + 72], rax    ; Save original RDI
    mov [rdi + 80], rsi    ; Save original RSI
    mov [rdi + 88], rdx
    mov [rdi + 96], rcx
    mov [rdi + 104], rbx
    
    ; Save RAX last (we've been using it)
    mov rax, [rsp + 8]     ; Get original RAX from caller
    mov [rdi + 112], rax
    
    ; Save return address as RIP
    mov rax, [rsp]
    mov [rdi + 120], rax
    
    ; Save CS
    mov ax, cs
    mov [rdi + 128], ax
    
    ; Save RFLAGS
    pushfq
    pop rax
    mov [rdi + 136], rax
    
    ; Save RSP (pointing to return address)
    lea rax, [rsp + 8]
    mov [rdi + 144], rax
    
    ; Save SS
    mov ax, ss
    mov [rdi + 152], ax

.load_new_task:
    ; Check if new_task is NULL
    test rsi, rsi
    jz .done
    
    ; Load new task context
    ; Restore stack pointer
    mov rsp, [rsi + 144]
    
    ; Restore segment registers (if needed)
    ; mov ax, [rsi + 128]
    ; mov cs, ax    ; Can't directly modify CS
    ; mov ax, [rsi + 152]
    ; mov ss, ax    ; Can't directly modify SS
    
    ; Restore callee-saved registers
    mov r15, [rsi + 0]
    mov r14, [rsi + 8]
    mov r13, [rsi + 16]
    mov r12, [rsi + 24]
    mov r11, [rsi + 32]
    mov r10, [rsi + 40]
    mov r9, [rsi + 48]
    mov r8, [rsi + 56]
    mov rbp, [rsi + 64]
    
    ; Restore caller-saved registers
    mov rax, [rsi + 112]
    mov rbx, [rsi + 104]
    mov rcx, [rsi + 96]
    mov rdx, [rsi + 88]
    
    ; Restore RFLAGS
    push qword [rsi + 136]
    popfq
    
    ; Restore RDI and RSI last
    mov rdi, [rsi + 72]
    ; Save RSI value before we overwrite it
    push qword [rsi + 80]
    
    ; Jump to saved RIP
    push qword [rsi + 120]
    
    ; Restore RSI
    pop rsi     ; Get saved RSI value
    xchg rsi, [rsp]  ; Swap with return address
    push rsi    ; Push return address back
    pop rsi     ; Get RSI back
    
    ; Actually, let's do this more simply:
    ; We'll use RET to jump to the saved RIP
    mov rsi, [rsi + 80]  ; Restore RSI last
    
.done:
    ret


; Simplified version that works better with the task structure
; This version assumes the context is saved/restored by interrupt handlers
; and we just need to switch the stack pointer

global task_switch_simple

task_switch_simple:
    ; RDI = old_task (pointer to Task structure)
    ; RSI = new_task (pointer to Task structure)
    
    ; If old_task is NULL, just load new task
    test rdi, rdi
    jz .load_new
    
    ; Save current RSP to old task's context (offset 144 in CPUContext)
    mov [rdi + 144], rsp
    
.load_new:
    ; Load new task's RSP
    mov rsp, [rsi + 144]
    
    ret