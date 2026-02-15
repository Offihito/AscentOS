; interrupts64.asm - WITH PROPER CONTEXT SAVE/RESTORE

global load_idt64
global isr_keyboard
global isr_timer

%ifndef TEXT_MODE_BUILD
global isr_mouse
extern mouse_handler64
%endif

extern keyboard_handler64
extern scheduler_tick
extern task_needs_switch
extern task_get_current_context
extern task_save_current_stack
extern task_get_next_context

section .text
bits 64

load_idt64:
    lidt [rdi]
    ret

isr_keyboard:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    
    call keyboard_handler64
    
    mov al, 0x20
    out 0x20, al
    
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    
    iretq

%ifndef TEXT_MODE_BUILD
isr_mouse:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    
    call mouse_handler64
    
    mov al, 0x20
    out 0xA0, al
    out 0x20, al
    
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    
    iretq
%endif

; ===========================================================================
; TIMER INTERRUPT WITH PROPER CONTEXT SAVE
; ===========================================================================

isr_timer:
    ; Save all registers on current task's stack
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    
    ; Call scheduler tick
    call scheduler_tick
    
    ; Check if context switch needed
    call task_needs_switch
    test rax, rax
    jz .no_switch
    
    ; IMPORTANT: Save current task's stack pointer
    mov rdi, rsp
    call task_save_current_stack
    
    ; Get new task's context
    call task_get_next_context
    test rax, rax
    jz .no_switch
    
    ; Switch to new task's stack
    mov rsp, [rax + 56]  ; Load RSP from context
    
.no_switch:
    ; Send EOI
    mov al, 0x20
    out 0x20, al
    
    ; Restore registers (from current stack - might be different task now!)
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    
    iretq

; ===========================================================================
; CONTEXT SWITCH FUNCTIONS
; ===========================================================================

global task_switch_context
task_switch_context:
    test rdi, rdi
    jz .load_new
    
    mov [rdi + 0], rax
    mov [rdi + 8], rbx
    mov [rdi + 16], rcx
    mov [rdi + 24], rdx
    mov [rdi + 32], rsi
    mov [rdi + 40], rdi
    mov [rdi + 48], rbp
    mov [rdi + 56], rsp
    mov [rdi + 64], r8
    mov [rdi + 72], r9
    mov [rdi + 80], r10
    mov [rdi + 88], r11
    mov [rdi + 96], r12
    mov [rdi + 104], r13
    mov [rdi + 112], r14
    mov [rdi + 120], r15
    
    mov rax, [rsp]
    mov [rdi + 128], rax
    
    pushfq
    pop rax
    mov [rdi + 136], rax
    
.load_new:
    mov rax, [rsi + 0]
    mov rbx, [rsi + 8]
    mov rcx, [rsi + 16]
    mov rdx, [rsi + 24]
    mov rdi, [rsi + 40]
    mov rbp, [rsi + 48]
    mov rsp, [rsi + 56]
    mov r8, [rsi + 64]
    mov r9, [rsi + 72]
    mov r10, [rsi + 80]
    mov r11, [rsi + 88]
    mov r12, [rsi + 96]
    mov r13, [rsi + 104]
    mov r14, [rsi + 112]
    mov r15, [rsi + 120]
    
    mov r10, [rsi + 136]
    push r10
    popfq
    
    mov r10, [rsi + 128]
    push r10
    
    mov rsi, [rsi + 32]
    ret

global task_save_current_context
task_save_current_context:
    mov [rdi + 0], rax
    mov [rdi + 8], rbx
    mov [rdi + 16], rcx
    mov [rdi + 24], rdx
    mov [rdi + 32], rsi
    mov [rdi + 40], rdi
    mov [rdi + 48], rbp
    mov [rdi + 56], rsp
    mov [rdi + 64], r8
    mov [rdi + 72], r9
    mov [rdi + 80], r10
    mov [rdi + 88], r11
    mov [rdi + 96], r12
    mov [rdi + 104], r13
    mov [rdi + 112], r14
    mov [rdi + 120], r15
    
    mov rax, [rsp]
    mov [rdi + 128], rax
    
    pushfq
    pop rax
    mov [rdi + 136], rax
    ret

global task_load_and_jump_context
task_load_and_jump_context:
    mov rax, [rdi + 0]
    mov rbx, [rdi + 8]
    mov rcx, [rdi + 16]
    mov rdx, [rdi + 24]
    mov rsi, [rdi + 32]
    mov rbp, [rdi + 48]
    mov rsp, [rdi + 56]
    mov r8, [rdi + 64]
    mov r9, [rdi + 72]
    mov r10, [rdi + 80]
    mov r11, [rdi + 88]
    mov r12, [rdi + 96]
    mov r13, [rdi + 104]
    mov r14, [rdi + 112]
    mov r15, [rdi + 120]
    
    mov r10, [rdi + 136]
    push r10
    popfq
    
    mov r10, [rdi + 128]
    push r10
    
    mov rdi, [rdi + 40]
    ret

global jump_to_usermode
jump_to_usermode:
    cli
    push 0x23
    push rsi
    pushfq
    pop rax
    or rax, 0x200
    push rax
    push 0x1B
    push rdi
    mov ax, 0x23
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    iretq
    ud2

global get_current_ring
get_current_ring:
    mov ax, cs
    and ax, 0x3
    movzx rax, ax
    ret

; CPU Exceptions
%macro ISR_NOERRCODE 1
global isr%1
isr%1:
    push 0
    push %1
    jmp isr_common
%endmacro

%macro ISR_ERRCODE 1
global isr%1
isr%1:
    push %1
    jmp isr_common
%endmacro

isr_common:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    mov rdi, rsp
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    add rsp, 16
    iretq

ISR_NOERRCODE 0
ISR_NOERRCODE 1
ISR_NOERRCODE 2
ISR_NOERRCODE 3
ISR_NOERRCODE 4
ISR_NOERRCODE 5
ISR_NOERRCODE 6
ISR_NOERRCODE 7
ISR_ERRCODE   8
ISR_NOERRCODE 9
ISR_ERRCODE   10
ISR_ERRCODE   11
ISR_ERRCODE   12
ISR_ERRCODE   13
ISR_ERRCODE   14
ISR_NOERRCODE 15
ISR_NOERRCODE 16
ISR_ERRCODE   17
ISR_NOERRCODE 18
ISR_NOERRCODE 19
ISR_NOERRCODE 20
ISR_NOERRCODE 21
ISR_NOERRCODE 22
ISR_NOERRCODE 23
ISR_NOERRCODE 24
ISR_NOERRCODE 25
ISR_NOERRCODE 26
ISR_NOERRCODE 27
ISR_NOERRCODE 28
ISR_NOERRCODE 29
ISR_ERRCODE   30
ISR_NOERRCODE 31