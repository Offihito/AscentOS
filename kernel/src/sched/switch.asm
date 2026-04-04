global switch_context
global thread_stub

section .text

; void switch_context(uint64_t *old_sp, uint64_t new_sp)
; rdi = pointer to old thread's rsp storage address
; rsi = new thread's rsp value
switch_context:
    ; Push callee-saved registers according to System V AMD64 ABI
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15

    ; Save current stack pointer into old_sp
    mov [rdi], rsp

    ; Load new stack pointer
    mov rsp, rsi

    ; Pop callee-saved registers for the arriving thread
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx

    ; Return to the address left on the new thread's stack
    ret

; This stub is where newly created threads begin execution.
; The switch_context "ret" instruction pops into here.
; 'r12' contains the actual C function entry point (set in sched_create_kernel_thread).
thread_stub:
    ; Ensure interrupts are enabled for the new thread
    sti
    
    ; Call the entry function
    call r12
    
    ; If the entry function returns, it will return into thread_exit() 
    ; because thread_exit was pushed just below the context struct.
    ret
