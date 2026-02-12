; 64-bit Interrupt Descriptor Table - WITH CONDITIONAL MOUSE SUPPORT

global load_idt64
global isr_keyboard

%ifndef TEXT_MODE_BUILD
global isr_mouse
extern mouse_handler64
%endif

extern keyboard_handler64

section .text
bits 64

; IDT'yi yükle
load_idt64:
    lidt [rdi]
    ret

; Keyboard interrupt handler (IRQ1)
isr_keyboard:
    ; Register'ları sakla
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
    
    ; C handler'ı çağır
    call keyboard_handler64
    
    ; PIC'e EOI gönder
    mov al, 0x20
    out 0x20, al
    
    ; Register'ları geri yükle
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
; Mouse interrupt handler (IRQ12) - Only for GUI mode
isr_mouse:
    ; Register'ları sakla
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
    
    ; C handler'ı çağır
    call mouse_handler64
    
    ; PIC'e EOI gönder (slave PIC için)
    mov al, 0x20
    out 0xA0, al  ; Slave PIC
    out 0x20, al  ; Master PIC
    
    ; Register'ları geri yükle
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

; IDT entry'si oluşturma makrosu
%macro ISR_NOERRCODE 1
global isr%1
isr%1:
    push 0              ; Dummy error code
    push %1             ; Interrupt number
    jmp isr_common
%endmacro

%macro ISR_ERRCODE 1
global isr%1
isr%1:
    push %1             ; Interrupt number
    jmp isr_common
%endmacro

; Ortak interrupt handler
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
    ; C handler çağrılabilir (şimdilik yok)
    
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
    
    add rsp, 16  ; Error code ve interrupt number'ı temizle
    iretq

; CPU exception'ları
ISR_NOERRCODE 0   ; Divide by zero
ISR_NOERRCODE 1   ; Debug
ISR_NOERRCODE 2   ; NMI
ISR_NOERRCODE 3   ; Breakpoint
ISR_NOERRCODE 4   ; Overflow
ISR_NOERRCODE 5   ; Bound range exceeded
ISR_NOERRCODE 6   ; Invalid opcode
ISR_NOERRCODE 7   ; Device not available
ISR_ERRCODE   8   ; Double fault
ISR_NOERRCODE 9   ; Coprocessor segment overrun
ISR_ERRCODE   10  ; Invalid TSS
ISR_ERRCODE   11  ; Segment not present
ISR_ERRCODE   12  ; Stack fault
ISR_ERRCODE   13  ; General protection fault
ISR_ERRCODE   14  ; Page fault
ISR_NOERRCODE 15  ; Reserved
ISR_NOERRCODE 16  ; x87 FPU error
ISR_ERRCODE   17  ; Alignment check
ISR_NOERRCODE 18  ; Machine check
ISR_NOERRCODE 19  ; SIMD floating point
ISR_NOERRCODE 20  ; Virtualization
ISR_NOERRCODE 21  ; Reserved
ISR_NOERRCODE 22  ; Reserved
ISR_NOERRCODE 23  ; Reserved
ISR_NOERRCODE 24  ; Reserved
ISR_NOERRCODE 25  ; Reserved
ISR_NOERRCODE 26  ; Reserved
ISR_NOERRCODE 27  ; Reserved
ISR_NOERRCODE 28  ; Reserved
ISR_NOERRCODE 29  ; Reserved
ISR_ERRCODE   30  ; Security exception
ISR_NOERRCODE 31  ; Reserved

; External fonksiyonlar
extern scheduler_tick

; Global export
global isr_timer

; Timer interrupt handler (IRQ0 -> INT 32)
isr_timer:
    ; Save all registers
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
    
    ; Call scheduler tick to update system time
    call scheduler_tick
    
    ; Send EOI to PIC
    mov al, 0x20
    out 0x20, al
    
    ; Restore registers
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
; CONTEXT SWITCHING FUNCTIONS FOR MULTITASKING
; ===========================================================================

; task_switch_context(cpu_context_t* old_ctx, cpu_context_t* new_ctx)
; Switch from one task to another
global task_switch_context
task_switch_context:
    ; RDI = old_ctx (can be NULL for first task)
    ; RSI = new_ctx
    
    ; If old_ctx is not NULL, save current context
    test rdi, rdi
    jz .load_new_context
    
    ; Save registers to old_ctx
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
    
    ; Save RIP (return address from stack)
    mov rax, [rsp]
    mov [rdi + 128], rax
    
    ; Save RFLAGS
    pushfq
    pop rax
    mov [rdi + 136], rax
    
.load_new_context:
    ; Load new context from new_ctx (RSI)
    mov rax, [rsi + 0]
    mov rbx, [rsi + 8]
    mov rcx, [rsi + 16]
    mov rdx, [rsi + 24]
    ; Skip RSI for now
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
    
    ; Load RIP to stack for return
    mov r10, [rsi + 128]
    push r10
    
    ; Load RFLAGS
    mov r10, [rsi + 136]
    push r10
    popfq
    
    ; Finally load RSI
    mov rsi, [rsi + 32]
    
    ret

; task_save_current_context(cpu_context_t* ctx)
; Save current CPU context to structure
global task_save_current_context
task_save_current_context:
    ; RDI = ctx pointer
    
    ; Save general purpose registers
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
    
    ; Save return address as RIP
    mov rax, [rsp]
    mov [rdi + 128], rax
    
    ; Save RFLAGS
    pushfq
    pop rax
    mov [rdi + 136], rax
    
    ret

; task_load_and_jump_context(cpu_context_t* ctx)
; Load context and jump to it (doesn't return)
global task_load_and_jump_context
task_load_and_jump_context:
    ; RDI = ctx pointer
    
    ; Load all registers from context
    mov rax, [rdi + 0]
    mov rbx, [rdi + 8]
    mov rcx, [rdi + 16]
    mov rdx, [rdi + 24]
    mov rsi, [rdi + 32]
    ; Skip RDI for now
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
    
    ; Load RFLAGS
    mov r10, [rdi + 136]
    push r10
    popfq
    
    ; Load RIP to stack for return
    mov r10, [rdi + 128]
    push r10
    
    ; Finally load RDI
    mov rdi, [rdi + 40]
    
    ; Jump to RIP
    ret