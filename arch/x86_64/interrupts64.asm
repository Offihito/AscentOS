; interrupts64.asm - Interrupt & Syscall Handlers (Ring-3 SYSRET support)
; CPU exceptions are forwarded to kernel_panic_handler.

global load_idt64
global isr_keyboard
global isr_timer
global syscall_entry
global isr_net

%ifndef TEXT_MODE_BUILD
global isr_mouse
extern mouse_handler64
%endif

extern keyboard_handler64
extern rtl8139_irq_handler
extern scheduler_tick
extern task_needs_switch
extern task_get_current_context
extern task_save_current_stack
extern task_get_next_context
extern tss_update_rsp0_from_context
extern task_restore_fs_base

extern syscall_dispatch
extern kernel_panic_handler

section .text
bits 64

; ============================================================================
; Load IDT
; ============================================================================
load_idt64:
    lidt [rdi]
    ret

; ============================================================================
; KEYBOARD INTERRUPT (IRQ1)
; ============================================================================
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
    out 0x20, al        ; Master PIC EOI

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

; ============================================================================
; MOUSE INTERRUPT (IRQ12) - GUI mode only
; ============================================================================
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
    out 0xA0, al        ; Slave PIC EOI
    out 0x20, al        ; Master PIC EOI

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

; ============================================================================
; NETWORK INTERRUPT (IRQ11 → INT 0x2B) — RTL8139
; IRQ11 is on the slave PIC (IRQ8-15); EOI order: slave (0xA0) then master (0x20).
; Installed at IDT vector 0x2B via idt_set(43,...) in init_interrupts64().
; ============================================================================
isr_net:
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

    call rtl8139_irq_handler

    mov al, 0x20
    out 0xA0, al        ; Slave PIC EOI  (IRQ11 -> slave)
    out 0x20, al        ; Master PIC EOI

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

; ============================================================================
; TIMER INTERRUPT (IRQ0) - Context switch support
; ============================================================================
isr_timer:
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

    call scheduler_tick

    call task_needs_switch
    test rax, rax
    jz .no_switch

    mov rdi, rsp
    call task_save_current_stack

    call task_get_next_context
    test rax, rax
    jz .no_switch

    ; Update TSS.RSP0 for the incoming task.
    ; When the new task takes an interrupt/syscall from Ring-3, the CPU
    ; loads the kernel stack pointer from TSS.RSP0.
    ; Stale RSP0 -> wrong stack -> #DF -> triple fault.
    ; rax = cpu_context_t* (returned by task_get_next_context)
    push rax
    mov rdi, rax
    call tss_update_rsp0_from_context
    pop rax

    mov rsp, [rax + 56]     ; load RSP from cpu_context_t.rsp

.no_switch:
    mov al, 0x20
    out 0x20, al            ; Master PIC EOI

    ; Restore FS_BASE before iretq.
    ; The timer can fire while a Ring-3 task is running (even inside a syscall).
    ; Write the current task's fs_base to MSR_FS_BASE regardless of whether a
    ; switch occurred; otherwise musl pthread_self() -> %fs:0 -> zero -> #GP.
    call task_restore_fs_base

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

; ============================================================================
; CONTEXT SWITCH FUNCTIONS
; ============================================================================
;
; cpu_context_t offset map (must match task.h):
;   +0    rax    +8    rbx    +16   rcx    +24   rdx
;   +32   rsi    +40   rdi    +48   rbp    +56   rsp
;   +64   r8     +72   r9     +80   r10    +88   r11
;   +96   r12    +104  r13    +112  r14    +120  r15
;   +128  rip    +136  rflags
;   +144  cs     +152  ss     +160  ds     +168  es
;   +176  fs     +184  gs
;   +192  cr3

global task_switch_context
task_switch_context:
    ; RDI = old_ctx, RSI = new_ctx
    test rdi, rdi
    jz .load_new

    ; Save current context.
    ; BUG FIX: rdi is both the pointer (old_ctx) and a register to be saved.
    ; Writing "mov [rdi+40], rdi" would store the pointer address, not the
    ; original register value. Fix: push rdi first so the real value is on stack.
    push rdi                 ; save real rdi on stack
    ; RSP is now shifted by 8 bytes. Offsets accounted for:
    ;   - rdi: read back from [rsp+0]
    ;   - rsp: pre-push value = rsp+8
    ;   - rip: [rsp+8] (return address, above the pushed rdi)
    mov [rdi + 0],   rax
    mov [rdi + 8],   rbx
    mov [rdi + 16],  rcx
    mov [rdi + 24],  rdx
    mov [rdi + 32],  rsi
    mov rax, [rsp]           ; real rdi value from stack
    mov [rdi + 40],  rax     ; ctx->rdi = correct value
    mov [rdi + 48],  rbp
    lea rax, [rsp + 8]       ; rsp+8 = RSP before push rdi
    mov [rdi + 56],  rax     ; ctx->rsp = real RSP
    mov [rdi + 64],  r8
    mov [rdi + 72],  r9
    mov [rdi + 80],  r10
    mov [rdi + 88],  r11
    mov [rdi + 96],  r12
    mov [rdi + 104], r13
    mov [rdi + 112], r14
    mov [rdi + 120], r15

    mov rax, [rsp + 8]       ; return address (above pushed rdi)
    mov [rdi + 128], rax     ; rip

    pushfq
    pop rax
    mov [rdi + 136], rax     ; rflags

    pop rdi                  ; restore rdi, clean stack
    mov rax, [rdi + 0]       ; restore rax to original value

.load_new:
    ; Load new context (RSI = new_ctx)
    mov rax, [rsi + 0]
    mov rbx, [rsi + 8]
    mov rcx, [rsi + 16]
    mov rdx, [rsi + 24]
    ; load rsi last (it is the source pointer)
    mov rdi, [rsi + 40]
    mov rbp, [rsi + 48]
    mov rsp, [rsi + 56]
    mov r8,  [rsi + 64]
    mov r9,  [rsi + 72]
    mov r10, [rsi + 80]
    mov r11, [rsi + 88]
    mov r12, [rsi + 96]
    mov r13, [rsi + 104]
    mov r14, [rsi + 112]
    mov r15, [rsi + 120]

    mov r10, [rsi + 136]     ; rflags
    push r10
    popfq

    mov r10, [rsi + 128]     ; rip (return address)
    push r10

    mov rsi, [rsi + 32]      ; load rsi last
    ret

global task_save_current_context
task_save_current_context:
    ; RDI = cpu_context_t*
    ; Same rdi bug fix as in task_switch_context.
    push rdi
    mov [rdi + 0],   rax
    mov [rdi + 8],   rbx
    mov [rdi + 16],  rcx
    mov [rdi + 24],  rdx
    mov [rdi + 32],  rsi
    mov rax, [rsp]           ; real rdi value from stack
    mov [rdi + 40],  rax
    mov [rdi + 48],  rbp
    lea rax, [rsp + 8]       ; real RSP (before push rdi)
    mov [rdi + 56],  rax
    mov [rdi + 64],  r8
    mov [rdi + 72],  r9
    mov [rdi + 80],  r10
    mov [rdi + 88],  r11
    mov [rdi + 96],  r12
    mov [rdi + 104], r13
    mov [rdi + 112], r14
    mov [rdi + 120], r15

    mov rax, [rsp + 8]       ; return address
    mov [rdi + 128], rax     ; rip

    pushfq
    pop rax
    mov [rdi + 136], rax     ; rflags

    pop rdi                  ; clean stack
    mov rax, [rdi + 0]       ; restore rax
    ret

global task_load_and_jump_context
task_load_and_jump_context:
    ; RDI = cpu_context_t*
    ;
    ; Works for both privilege levels (Ring-0 and Ring-3):
    ;   context.rsp -> kernel stack top
    ;     [low  addr]  15 x registers  (popped)
    ;     [high addr]  iretq frame: RIP / CS / RFLAGS / RSP* / SS*
    ;                  (* only present for Ring-3; CPU ignores them for Ring-0)
    ;
    ; Steps:
    ;   1. Set DS/ES/FS/GS to the correct segment
    ;   2. RSP = context.rsp
    ;   3. 15 pops
    ;   4. iretq
    ;
    ; Ring-0 iretq: CS.DPL=0 == CPL=0 -> no privilege change, RSP/SS not read.
    ;   Stack must have 3 words: RIP / CS / RFLAGS.
    ; Ring-3 iretq: CS.DPL=3 != CPL=0 -> privilege change, RSP/SS are read.
    ;   Stack must have 5 words: RIP / CS / RFLAGS / RSP / SS.
    ;
    ; Both cases share the same stack layout (prepared by task_create / task_create_user).

    ; Determine ring from context.cs
    mov r10, [rdi + 144]    ; context.cs
    cmp r10, 0x23
    je  .set_user_segs

    ; Ring-0: kernel data segment
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    jmp .load_stack

.set_user_segs:
    ; Ring-3: set DS/ES, do NOT touch FS/GS.
    ; "mov fs, reg" zeroes MSR_FS_BASE -> corrupts TLS -> #GP.
    ; FS_BASE is restored via wrmsr in syscall_dispatch.
    mov ax, 0x1B
    mov ds, ax
    mov es, ax
    ; mov fs, ax  <- REMOVED: would zero FS_BASE/TLS
    ; mov gs, ax  <- REMOVED

.load_stack:
    mov rsp, [rdi + 56]     ; context.rsp -> kernel stack top

    ; Pop 15 registers (pushed in order by task_create / task_create_user)
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi     ; rdi is no longer the context pointer, safe to pop
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    ; iretq frame now at stack top:
    ;   Ring-0: [RSP+0]=RIP  [RSP+8]=CS(0x08)  [RSP+16]=RFLAGS
    ;   Ring-3: [RSP+0]=RIP  [RSP+8]=CS(0x23)  [RSP+16]=RFLAGS  [RSP+24]=RSP  [RSP+32]=SS
    iretq

; ============================================================================
; CPU EXCEPTION HANDLERS — PANIC SUPPORT
;
; Stack layout on entry to isr_panic_common:
;
;   ISR_NOERRCODE — CPU push order:
;     [RSP+0]  = isr_num    (pushed by macro)
;     [RSP+8]  = 0          (pseudo err_code, pushed by macro)
;     [RSP+16] = RIP        \
;     [RSP+24] = CS          | CPU iretq frame
;     [RSP+32] = RFLAGS     /
;     [RSP+40] = RSP*        \ (only pushed on privilege change)
;     [RSP+48] = SS*         /
;
;   ISR_ERRCODE — CPU pushes err_code first, then macro pushes isr_num.
;   Layout is otherwise identical.
;
;   After isr_panic_common pushes all GPRs:
;     [RSP+0]   r15  \
;     [RSP+8]   r14   |
;     [RSP+16]  r13   |
;     [RSP+24]  r12   |  exception_frame_t*  (rdi = rsp)
;     [RSP+32]  r11   |
;     [RSP+40]  r10   |
;     [RSP+48]  r9    |
;     [RSP+56]  r8    |
;     [RSP+64]  rbp   |
;     [RSP+72]  rdi   |
;     [RSP+80]  rsi   |
;     [RSP+88]  rdx   |
;     [RSP+96]  rcx   |
;     [RSP+104] rbx   |
;     [RSP+112] rax  /
;     [RSP+120] err_code
;     [RSP+128] isr_num
;     [RSP+136] RIP    \
;     [RSP+144] CS      | CPU iretq frame
;     [RSP+152] RFLAGS  |
;     [RSP+160] RSP*    |
;     [RSP+168] SS*    /
;
;   This layout must match exception_frame_t in panic64.c exactly.
; ============================================================================

%macro ISR_NOERRCODE 1
global isr%1
isr%1:
    push 0      ; pseudo err_code (CPU does not push one for this exception)
    push %1     ; isr_num
    jmp isr_panic_common
%endmacro

%macro ISR_ERRCODE 1
global isr%1
isr%1:
    ; CPU already pushed err_code for this exception
    push %1     ; isr_num
    jmp isr_panic_common
%endmacro

; Common panic entry point
isr_panic_common:
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

    mov  rdi, rsp
    call kernel_panic_handler   ; kernel_panic_handler(exception_frame_t*) — does not return

    cli
.hang:
    hlt
    jmp .hang

; Exception table
ISR_NOERRCODE 0    ; #DE Divide Error
ISR_NOERRCODE 1    ; #DB Debug
ISR_NOERRCODE 2    ; #NMI Non-Maskable Interrupt
ISR_NOERRCODE 3    ; #BP Breakpoint
ISR_NOERRCODE 4    ; #OF Overflow
ISR_NOERRCODE 5    ; #BR Bound Range Exceeded
ISR_NOERRCODE 6    ; #UD Invalid Opcode
ISR_NOERRCODE 7    ; #NM Device Not Available
; #DF handled by isr8_df below (IST1 stack)

; #DF Double Fault — uses IST1 stack.
; If #DF fires with a corrupt RSP the CPU would generate a second fault ->
; triple fault. Fix: set IDT gate IST=1 so the CPU switches to TSS.IST1,
; an independent clean stack unaffected by the corrupted RSP.
;
; Required setup:
;   tss.ist1 = (uint64_t)df_stack_top   <- in tss_init()
;   idt[8] gate IST field = 1           <- in init_interrupts64()
global isr8_df
isr8_df:
    ; CPU switched to IST1 stack; RSP is clean.
    ; #DF err_code is always 0 (already pushed by CPU).
    push 8          ; isr_num
    jmp isr_panic_common

ISR_NOERRCODE 9    ; #09 Coprocessor Segment Overrun
ISR_ERRCODE   10   ; #TS Invalid TSS
ISR_ERRCODE   11   ; #NP Segment Not Present
ISR_ERRCODE   12   ; #SS Stack-Segment Fault
ISR_ERRCODE   13   ; #GP General Protection
ISR_ERRCODE   14   ; #PF Page Fault          <- CR2 = fault address
ISR_NOERRCODE 15   ; #15 Reserved
ISR_NOERRCODE 16   ; #MF x87 FP Error
ISR_ERRCODE   17   ; #AC Alignment Check
ISR_NOERRCODE 18   ; #MC Machine Check
ISR_NOERRCODE 19   ; #XF SIMD FP Exception
ISR_NOERRCODE 20   ; #VE Virtualization Exception
ISR_NOERRCODE 21   ; #CP Control Protection
ISR_NOERRCODE 22
ISR_NOERRCODE 23
ISR_NOERRCODE 24
ISR_NOERRCODE 25
ISR_NOERRCODE 26
ISR_NOERRCODE 27
ISR_NOERRCODE 28
ISR_NOERRCODE 29
ISR_ERRCODE   30   ; #SX Security Exception
ISR_NOERRCODE 31

; ============================================================================
; SYSCALL ENTRY POINT
; ============================================================================
;
; Supports both kernel-mode (CPL=0) and user-mode (CPL=3) syscalls.
;
; Hardware SYSCALL actions (via MSR configuration):
;   RCX    <- RIP        (return address — instruction after syscall)
;   R11    <- RFLAGS     (caller's RFLAGS)
;   RIP    <- LSTAR      (this function)
;   CS     <- STAR[47:32]       = 0x08 (Kernel Code)
;   SS     <- STAR[47:32]+8     = 0x10 (Kernel Data)
;   RFLAGS &= ~FMASK            (IF and DF cleared)
;   RSP    -> UNCHANGED         (caller's RSP is still active!)
;
; When a Ring-3 task issues SYSCALL, RSP still points to the user stack.
; The first thing to do is switch to the kernel stack.
; TSS RSP0 is always up-to-date (updated by tss_set_kernel_stack() on task_switch).
;
; CPL DETECTION: the RPL field of SS (bits[1:0]) holds the CPL at the
; time of the SYSCALL:
;   SS = 0x10 -> RPL=0 -> came from Ring-0 -> kernel path
;   SS = 0x1B -> RPL=3 -> came from Ring-3 -> user path
;
; KERNEL-MODE PATH (CPL=0):
;   RSP is already on the kernel stack — use it directly.
;   Return: restore RFLAGS from R11 via pushfq, then jmp rcx.
;   Do NOT use iretq — kernel-mode SYSCALL has no RSP/SS on the stack;
;   a partial iretq frame causes #GP -> #DF -> triple fault.
;
; USER-MODE PATH (CPL=3):
;   RSP = user stack -> switch to kernel stack (TSS RSP0).
;   Dispatch -> return to Ring-3 with o64 sysret.
;
; syscall_frame_t layout (must match syscall.h):
;   +0:  rax  syscall number / return value
;   +8:  rdi  arg1
;   +16: rsi  arg2
;   +24: rdx  arg3
;   +32: r10  arg4
;   +40: r8   arg5
;   +48: r9   arg6
;   +56: rcx  saved RIP   (saved by SYSCALL hardware, needed for SYSRET)
;   +64: r11  saved RFLAGS (needed for SYSRET)

extern kernel_tss

syscall_entry:
    ; Detect CPL via SS RPL field.
    ; After SYSCALL, CS is set to 0x08 (kernel, RPL=0) by hardware.
    ; To know the *previous* CPL we check SS, which is preserved:
    ;   CPL=0 -> SS=0x10, bits[1:0]=0 -> kernel path
    ;   CPL=3 -> SS=0x1B, bits[1:0]=3 -> user path
    push rax            ; save RAX temporarily
    mov ax, ss
    test ax, 3          ; RPL=3 -> user-mode; RPL=0 -> kernel-mode
    pop rax             ; restore RAX (flags preserved, test result intact)
    jnz .user_syscall

; ============================================================
; KERNEL-MODE SYSCALL PATH (CPL=0 -> CPL=0)
;
; Return mechanism: manual RFLAGS/RIP restore, NOT iretq.
; iretq expects a full 5-word frame (RIP/CS/RFLAGS/RSP/SS) on the stack.
; This path has no RSP/SS pushed, so iretq would fault.
;
; Correct approach:
;   1. Restore callee-saved registers and frame.
;   2. push r11 / popfq -> restore RFLAGS (including IF).
;   3. jmp rcx          -> RCX = return RIP saved by SYSCALL.
; ============================================================
.kernel_syscall:
    ; Save callee-saved registers (System V ABI)
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15

    ; Build syscall_frame_t (must match syscall.h, offsets +0..+64)
    push r11        ; frame.r11  (+64) — saved RFLAGS
    push rcx        ; frame.rcx  (+56) — saved RIP (return address)
    push r9         ; frame.r9   (+48)
    push r8         ; frame.r8   (+40)
    push r10        ; frame.r10  (+32)
    push rdx        ; frame.rdx  (+24)
    push rsi        ; frame.rsi  (+16)
    push rdi        ; frame.rdi  (+8)
    push rax        ; frame.rax  (+0)  — syscall number

    mov rdi, rsp
    call syscall_dispatch

    ; Retrieve return value (frame.rax) and unwind frame
    pop rax
    pop rdi
    pop rsi
    pop rdx
    pop r10
    pop r8
    pop r9
    pop rcx         ; RCX = saved RIP
    pop r11         ; R11 = saved RFLAGS

    ; Restore callee-saved registers
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx

    ; Restore RFLAGS: re-enable interrupts (IF bit 9), then jump to return address
    push r11
    or qword [rsp], (1 << 9)    ; IF=1
    popfq
    jmp rcx                      ; RIP = saved return address (no iretq)

; ============================================================
; USER-MODE SYSCALL PATH (CPL=3 -> CPL=3, return via SYSRET)
; ============================================================
.user_syscall:
    ; RSP = user stack -> switch to kernel stack
    mov r15, rsp            ; r15 = user RSP (callee-saved, not yet clobbered)

    ; TSS RSP0 = kernel_stack_top
    ; tss_t packed layout: +0=reserved0(4B), +4=rsp0(8B)
    mov rsp, [rel kernel_tss + 4]

    ; Save callee-saved registers and user RSP on kernel stack
    push r15        ; user RSP (retrieved in step 9)
    push rbx
    push rbp
    push r12
    push r13
    push r14
    ; r15 already pushed

    ; Build syscall_frame_t (must match syscall.h, offsets +0..+72)
    ; frame.user_rsp (+72): real user RSP for sys_fork; execve may override it
    push r15        ; frame.user_rsp (+72)
    push r11        ; frame.r11  (+64)
    push rcx        ; frame.rcx  (+56)
    push r9         ; frame.r9   (+48)
    push r8         ; frame.r8   (+40)
    push r10        ; frame.r10  (+32)
    push rdx        ; frame.rdx  (+24)
    push rsi        ; frame.rsi  (+16)
    push rdi        ; frame.rdi  (+8)
    push rax        ; frame.rax  (+0)

    mov rdi, rsp
    call syscall_dispatch

    ; Check for user_rsp override (execve may have written a new RSP)
    mov r14, [rsp + 72]     ; r14 = frame.user_rsp (callee-saved)

    ; Unwind frame: rax..r11 (10 slots = 80 bytes)
    pop rax         ; +0  return value
    pop rdi         ; +8
    pop rsi         ; +16
    pop rdx         ; +24
    pop r10         ; +32
    pop r8          ; +40
    pop r9          ; +48
    pop rcx         ; +56 saved RIP (for SYSRET)
    pop r11         ; +64 saved RFLAGS (for SYSRET)
    add rsp, 8      ; +72 skip user_rsp slot

    ; Restore callee-saved registers
    ; r14 was used for user_rsp; restore its real value from stack now
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    pop r15         ; r15 = original user RSP

    ; Set DS/ES to Ring-3; do NOT touch FS/GS.
    ; "mov fs, reg" zeroes MSR_FS_BASE -> corrupts TLS -> #GP.
    ; FS_BASE was already restored via wrmsr in syscall_dispatch.
    mov ax, 0x1B
    mov ds, ax
    mov es, ax

    ; If execve wrote a new RSP (non-zero), use it; otherwise restore original
    test r14, r14
    jnz .use_new_rsp
    mov rsp, r15    ; normal path: original user RSP
    jmp .do_sysret

.use_new_rsp:
    mov rsp, r14    ; execve path: new user stack

.do_sysret:
    ; 64-bit SYSRET -> Ring-3
    ; CS = STAR[63:48]+16|3 = 0x23, SS = STAR[63:48]+8|3 = 0x1B
    o64 sysret

; ============================================================================
; IST1 stack for #DF — 16 KB, must be set in TSS.IST1
; ============================================================================
section .bss
align 16
df_stack_bottom:
    resb 16384          ; 16 KB
global df_stack_top
df_stack_top: