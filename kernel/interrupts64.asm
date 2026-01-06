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
extern scheduler_select_next  
extern task_get_current
extern current_task

; Global export
global isr_timer
global isr_scheduler

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
    
    ; Note: For now we don't do context switch in timer
    ; Tasks will cooperatively yield with task_yield()
    ; Advanced version can do preemptive switch here
    
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

; Scheduler interrupt (INT 0x80) - Cooperative task switching
isr_scheduler:
    ; This is called by task_yield()
    ; We need to save current task context and load next task
    
    ; Save all registers on stack
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
    
    ; Call scheduler to select next task
    ; Get current task first
    call task_get_current
    test rax, rax
    jz .no_current_task
    
    ; Save current task pointer
    mov r15, rax
    
    ; Get next task
    call scheduler_select_next
    test rax, rax
    jz .no_next_task
    
    ; Check if same task
    cmp r15, rax
    je .same_task
    
    ; Different task - need to switch
    ; r15 = old task
    ; rax = new task
    mov r14, rax
    
    ; Save old task's context
    ; Context structure offset: 0
    ; Registers are already on stack, we need to save them to task structure
    
    ; Get stack pointer with saved registers
    mov rax, rsp
    
    ; Save registers to old task context
    ; The order matches our push order above
    mov rbx, [rax]       ; r15
    mov [r15 + 0], rbx
    
    mov rbx, [rax + 8]   ; r14
    mov [r15 + 8], rbx
    
    mov rbx, [rax + 16]  ; r13
    mov [r15 + 16], rbx
    
    mov rbx, [rax + 24]  ; r12
    mov [r15 + 24], rbx
    
    mov rbx, [rax + 32]  ; r11
    mov [r15 + 32], rbx
    
    mov rbx, [rax + 40]  ; r10
    mov [r15 + 40], rbx
    
    mov rbx, [rax + 48]  ; r9
    mov [r15 + 48], rbx
    
    mov rbx, [rax + 56]  ; r8
    mov [r15 + 56], rbx
    
    mov rbx, [rax + 64]  ; rbp
    mov [r15 + 64], rbx
    
    mov rbx, [rax + 72]  ; rdi
    mov [r15 + 72], rbx
    
    mov rbx, [rax + 80]  ; rsi
    mov [r15 + 80], rbx
    
    mov rbx, [rax + 88]  ; rdx
    mov [r15 + 88], rbx
    
    mov rbx, [rax + 96]  ; rcx
    mov [r15 + 96], rbx
    
    mov rbx, [rax + 104] ; rbx
    mov [r15 + 104], rbx
    
    mov rbx, [rax + 112] ; rax
    mov [r15 + 112], rbx
    
    ; Save RIP (return address after all pushes)
    mov rbx, [rax + 120]
    mov [r15 + 120], rbx
    
    ; Save CS
    mov bx, cs
    mov [r15 + 128], bx
    
    ; Save RFLAGS
    pushfq
    pop rbx
    mov [r15 + 136], rbx
    
    ; Save RSP (stack pointer before interrupt)
    lea rbx, [rax + 128]  ; 15 registers * 8 + return addr
    mov [r15 + 144], rbx
    
    ; Save SS
    mov bx, ss
    mov [r15 + 152], bx
    
    ; Now load new task context (r14 = new task)
    ; Set new task as running
    mov byte [r14 + 264], 1  ; state = RUNNING (offset for state in Task struct)
    
    ; Load registers from new task
    mov r15, [r14 + 0]
    mov r13, [r14 + 16]
    mov r12, [r14 + 24]
    mov r11, [r14 + 32]
    mov r10, [r14 + 40]
    mov r9, [r14 + 48]
    mov r8, [r14 + 56]
    mov rbp, [r14 + 64]
    
    ; Load stack pointer
    mov rsp, [r14 + 144]
    
    ; Load remaining registers
    mov rax, [r14 + 112]
    mov rbx, [r14 + 104]
    mov rcx, [r14 + 96]
    mov rdx, [r14 + 88]
    mov rsi, [r14 + 80]
    mov rdi, [r14 + 72]
    
    ; Load r14 last
    mov r14, [r14 + 8]
    
    ; Return to new task
    iretq

.no_current_task:
.no_next_task:
.same_task:
    ; Restore registers and return to same task
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
