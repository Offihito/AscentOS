global load_idt64
global isr_keyboard
global isr_timer
global syscall_entry
global isr_net
global isr_sb16

global isr_mouse
extern mouse_handler64

extern keyboard_handler64
extern rtl8139_irq_handler
extern sb16_irq_handler
extern scheduler_tick
global isr_apic_timer
global isr_apic_spurious
extern lapic_eoi
extern apic_pic_is_disabled
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
; MAKROLAR
; ============================================================================

; Tüm genel amaçlı yazmaçları kaydet (15 register, 120 byte)
%macro PUSH_ALL 0
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
%endmacro

; Tüm genel amaçlı yazmaçları geri yükle (ters sırada)
%macro POP_ALL 0
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
%endmacro

; Master PIC EOI veya APIC EOI gönder
; Kullanım: SEND_EOI <prefix>
; prefix: etiket isimlerinin çakışmamasını sağlar (ör. kb, mouse, net, timer)
%macro SEND_EOI_MASTER 1
    call apic_pic_is_disabled
    test eax, eax
    jz %%pic_eoi
    call lapic_eoi
    jmp %%eoi_done
%%pic_eoi:
    mov al, 0x20
    out 0x20, al        ; Master PIC EOI
%%eoi_done:
%endmacro

; Slave + Master PIC EOI veya APIC EOI gönder (IRQ8-15 için)
%macro SEND_EOI_SLAVE 1
    call apic_pic_is_disabled
    test eax, eax
    jz %%pic_eoi
    call lapic_eoi
    jmp %%eoi_done
%%pic_eoi:
    mov al, 0x20
    out 0xA0, al        ; Slave PIC EOI
    out 0x20, al        ; Master PIC EOI
%%eoi_done:
%endmacro

; Context switch mantığı (isr_timer ve isr_apic_timer ortak)
; Kullanım: TIMER_CONTEXT_SWITCH <no_switch_label>
%macro TIMER_CONTEXT_SWITCH 1
    call task_needs_switch
    test rax, rax
    jz %1

    mov rdi, rsp
    call task_save_current_stack

    call task_get_next_context
    test rax, rax
    jz %1

    push rax
    mov rdi, rax
    call tss_update_rsp0_from_context
    pop rax

    mov rsp, [rax + 56]     ; load RSP from cpu_context_t.rsp
%endmacro

; Basit ISR: kaydet → handler çağır → master EOI → geri yükle → iretq
; Kullanım: SIMPLE_ISR_MASTER <isim>, <handler>
%macro SIMPLE_ISR_MASTER 2
global %1
%1:
    PUSH_ALL
    call %2
    SEND_EOI_MASTER %1
    POP_ALL
    iretq
%endmacro

; Basit ISR: kaydet → handler çağır → slave+master EOI → geri yükle → iretq
; Kullanım: SIMPLE_ISR_SLAVE <isim>, <handler>
%macro SIMPLE_ISR_SLAVE 2
global %1
%1:
    PUSH_ALL
    call %2
    SEND_EOI_SLAVE %1
    POP_ALL
    iretq
%endmacro

; ============================================================================

load_idt64:
    lidt [rdi]
    ret

; ============================================================================
; KEYBOARD INTERRUPT (IRQ1) — Master PIC
; ============================================================================
SIMPLE_ISR_MASTER isr_keyboard, keyboard_handler64

; ============================================================================
; MOUSE INTERRUPT (IRQ12 → INT 0x2C) — PS/2 Mouse, Slave PIC
; IRQ12 slave PIC üzerinde; EOI sırası: slave (0xA0) → master (0x20)
; ============================================================================
SIMPLE_ISR_SLAVE isr_mouse, mouse_handler64

; ============================================================================
; NETWORK INTERRUPT (IRQ11 → INT 0x2B) — RTL8139, Slave PIC
; Installed at IDT vector 0x2B via idt_set(43,...) in init_interrupts64().
; ============================================================================
SIMPLE_ISR_SLAVE isr_net, rtl8139_irq_handler

; ============================================================================
; SB16 INTERRUPT (IRQ5 → INT 0x25) — Sound Blaster 16, Master PIC
; sb16_irq_handler() kendi EOI'sini içinde halleder.
; ============================================================================
isr_sb16:
    PUSH_ALL
    call sb16_irq_handler       ; DSP acknowledge + g_sb16.playing=false
                                ; EOI is handled inside sb16_irq_handler()
                                ; to allow it to read IRQ status first.
    POP_ALL
    iretq

; ============================================================================
; TIMER INTERRUPT (IRQ0) — PIC tabanlı, context switch desteği
; ============================================================================
isr_timer:
    PUSH_ALL

    call scheduler_tick

    TIMER_CONTEXT_SWITCH .no_switch

.no_switch:
    SEND_EOI_MASTER timer

    ; Restore FS_BASE before iretq.
    ; The timer can fire while a Ring-3 task is running (even inside a syscall).
    ; Write the current task's fs_base to MSR_FS_BASE regardless of whether a
    ; switch occurred; otherwise musl pthread_self() -> %fs:0 -> zero -> #GP.
    call task_restore_fs_base

    POP_ALL
    iretq

; ============================================================================
; APIC TIMER ISR (INT 0x40)
;
; PIC-tabanlı isr_timer'dan farkı:
;   - EOI olarak lapic_eoi() kullanır (outb 0x20 DEĞİL)
;   - Aynı zamanlayıcı mantığını çalıştırır: scheduler_tick + bağlam değiştirme
; ============================================================================
isr_apic_timer:
    PUSH_ALL

    call scheduler_tick

    TIMER_CONTEXT_SWITCH .no_switch

.no_switch:
    ; APIC interrupt acknowledge
    call lapic_eoi

    ; Always restore FS_BASE before iretq.
    ; Timer can hit while returning to userland; stale/zero FS_BASE breaks TLS.
    call task_restore_fs_base

    POP_ALL
    iretq

; ============================================================================
; APIC SPURIOUS ISR (INT 0xFF)
;
; LAPIC, bir kesmeyi teslim edemediğinde bu vektörü kullanır.
; x86 kılavuzu: spurious ISR'dan EOI gönderilmemeli.
; ============================================================================
isr_apic_spurious:
    ; Spurious kesmeler için EOI gerekmez (Intel SDM Vol. 3A §10.9)
    iretq

global task_switch_context
task_switch_context:
    ; RDI = old_ctx, RSI = new_ctx
    test rdi, rdi
    jz .load_new

    push rdi                 ; save real rdi on stack

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
    POP_ALL
    iretq


; ============================================================================
; IST (Interrupt Stack Table) TAHSİSATI
; ============================================================================
;
; x86-64 TSS, 7 bağımsız IST yuvası sunar (IST1..IST7).
; Her IST, IDT gate'inin IST alanına (bit 2:0) yazılarak aktif edilir.
; CPU o gate'i tetiklendiğinde TSS.ISTn'den RSP'yi yükler — mevcut RSP
; ne olursa olsun (bozuk, kullanıcı alanı, Ring-3 vs.).
;
; Tahsisat (init_interrupts64() içinde IDT gate'lerine yansıtılmalı):
;
;   IST1 → #DF  (Double Fault,       vektör  8) — bozuk RSP'ye karşı zorunlu
;   IST2 → #NMI (Non-Maskable Int,   vektör  2) — NMI iç içe geçme sorununu önler
;   IST3 → #MC  (Machine Check,      vektör 18) — donanım hatası, yığın güvenilmez
;   IST4 → #SS  (Stack-Segment Fault,vektör 12) — #SS zaten yığın bozukluğu
;   IST5 → #GP  (General Protection, vektör 13) — kernel #GP'de RSP geçersiz olabilir
;   IST6 → #PF  (Page Fault,         vektör 14) — sayfa hatası + bozuk RSP → #DF
;   IST7 → (rezerve / gelecek kullanım)
;
; .bss'teki stack tamponları: IST_STACK_SIZE (16 KB) × 6 adet
; tss_init() içinde: tss.ist1 = ist1_stack_top, tss.ist2 = ist2_stack_top, ...
; ============================================================================

%define IST_STACK_SIZE  16384   ; 16 KB — her IST yığını için

; ── Exception makroları ─────────────────────────────────────────────────────

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

; IST'li exception'lar için ayrı giriş noktası makrosu.
; ISR_NOERRCODE/ISR_ERRCODE ile aynı işi yapar; tek farkı yorum + global isim
; kuralıdır — asıl IST anahtarlaması IDT gate'inde yapılır (IST alanı).
; Bu makro, IST'li gate'lerin hangi etikete atlandığını netleştirir.
%macro ISR_NOERRCODE_IST 2          ; %1=vektör, %2=IST numarası (belgeleme amaçlı)
global isr%1
isr%1:
    push 0
    push %1
    jmp isr_panic_common            ; IST yığını CPU tarafından zaten seçildi
%endmacro

%macro ISR_ERRCODE_IST 2            ; %1=vektör, %2=IST numarası
global isr%1
isr%1:
    push %1
    jmp isr_panic_common
%endmacro

; ── Ortak panic giriş noktası ───────────────────────────────────────────────
;
; Yığın düzeni bu noktada (IST veya normal, fark yok):
;   [RSP+0 .. +112]  PUSH_ALL  (15 × 8 = 120 byte)
;   [RSP+120]        isr_num   (push %1)
;   [RSP+128]        err_code  (CPU veya pseudo)
;   [RSP+136]        RIP       } CPU'nun otomatik
;   [RSP+144]        CS        } iretq çerçevesi
;   [RSP+152]        RFLAGS    }
;   [RSP+160]        RSP       } (CPL değişiminde)
;   [RSP+168]        SS        }
;
; kernel_panic_handler(panic_frame_t *frame) → RDI = RSP
isr_panic_common:
    PUSH_ALL
    mov  rdi, rsp
    call kernel_panic_handler

    cli
.hang:
    hlt
    jmp .hang

; ── Exception tablosu ───────────────────────────────────────────────────────

ISR_NOERRCODE 0    ; #DE  Divide Error
ISR_NOERRCODE 1    ; #DB  Debug

; #NMI — IST2
; NMI, başka bir NMI işlenirken de gelebilir. Normal yığında iç içe NMI
; çerçevesi IRET bloğunu bozar (Intel SDM Vol.3 §6.7.1).
; IST2 ile her NMI temiz, bağımsız bir yığıla girer.
ISR_NOERRCODE_IST 2, 2  ; #NMI Non-Maskable Interrupt  → IST2

ISR_NOERRCODE 3    ; #BP  Breakpoint
ISR_NOERRCODE 4    ; #OF  Overflow
ISR_NOERRCODE 5    ; #BR  Bound Range Exceeded
ISR_NOERRCODE 6    ; #UD  Invalid Opcode
ISR_NOERRCODE 7    ; #NM  Device Not Available

; #DF — IST1
; Çift hata, genellikle bozuk RSP ile tetiklenir. IST1 olmadan CPU
; aynı bozuk RSP'yi kullanır → ikinci fault → triple fault.
;
; Gerekli kurulum (init_interrupts64):
;   tss.ist1 = ist1_stack_top
;   IDT[8].ist = 1
global isr8_df
isr8_df:
    ; CPU IST1 yığınına geçti; RSP temiz.
    ; #DF err_code = 0 (CPU tarafından zaten push edildi).
    push 8
    jmp isr_panic_common

ISR_NOERRCODE 9    ; #09  Coprocessor Segment Overrun
ISR_ERRCODE   10   ; #TS  Invalid TSS
ISR_ERRCODE   11   ; #NP  Segment Not Present

; #SS — IST4
; Yığın-segment hatası zaten yığın bozukluğunu işaret eder.
; Aynı yığını kullanmak güvenilmez; IST4 bağımsız yığın sağlar.
;
; Gerekli kurulum:
;   tss.ist4 = ist4_stack_top
;   IDT[12].ist = 4
ISR_ERRCODE_IST 12, 4   ; #SS  Stack-Segment Fault      → IST4

; #GP — IST5
; Kernel modunda #GP patlıyor ve RSP geçersizse (ör. yanlış yığın
; pointer'ı) normal yığında handler çalışamaz → #DF zinciri.
; IST5 bu zinciri kırar.
;
; Gerekli kurulum:
;   tss.ist5 = ist5_stack_top
;   IDT[13].ist = 5
ISR_ERRCODE_IST 13, 5   ; #GP  General Protection        → IST5

; #PF — IST6
; Sayfa hatası + bozuk RSP kombinasyonu #DF'ye yol açar.
; IST6, #PF handler'ını RSP durumundan bağımsız kılar.
; Not: CR2 = fault address (okunması geciktirilmemeli).
;
; Gerekli kurulum:
;   tss.ist6 = ist6_stack_top
;   IDT[14].ist = 6
ISR_ERRCODE_IST 14, 6   ; #PF  Page Fault (CR2=fault addr) → IST6

ISR_NOERRCODE 15   ; #15  Reserved
ISR_NOERRCODE 16   ; #MF  x87 FP Error
ISR_ERRCODE   17   ; #AC  Alignment Check

; #MC — IST3
; Makine hatası asenkrondur ve herhangi bir komut sınırında oluşabilir.
; Yığın durumu tamamen belirsiz; IST3 garantili temiz yığın sağlar.
;
; Gerekli kurulum:
;   tss.ist3 = ist3_stack_top
;   IDT[18].ist = 3
ISR_NOERRCODE_IST 18, 3 ; #MC  Machine Check               → IST3

ISR_NOERRCODE 19   ; #XF  SIMD FP Exception
ISR_NOERRCODE 20   ; #VE  Virtualization Exception
ISR_NOERRCODE 21   ; #CP  Control Protection
ISR_NOERRCODE 22
ISR_NOERRCODE 23
ISR_NOERRCODE 24
ISR_NOERRCODE 25
ISR_NOERRCODE 26
ISR_NOERRCODE 27
ISR_NOERRCODE 28
ISR_NOERRCODE 29
ISR_ERRCODE   30   ; #SX  Security Exception
ISR_NOERRCODE 31

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
; IST YIĞIN TAMPONLARI (.bss)
; ============================================================================
;
; Her IST yığını 16 KB, 16-byte hizalı.
; tss_init() içinde şu atamaları yap:
;
;   tss.ist1 = (uint64_t)ist1_stack_top   ; #DF
;   tss.ist2 = (uint64_t)ist2_stack_top   ; #NMI
;   tss.ist3 = (uint64_t)ist3_stack_top   ; #MC
;   tss.ist4 = (uint64_t)ist4_stack_top   ; #SS
;   tss.ist5 = (uint64_t)ist5_stack_top   ; #GP
;   tss.ist6 = (uint64_t)ist6_stack_top   ; #PF
;
; init_interrupts64() içinde IDT gate IST alanlarını güncelle:
;
;   idt_set_ist(8,  1)   ; #DF  → IST1
;   idt_set_ist(2,  2)   ; #NMI → IST2
;   idt_set_ist(18, 3)   ; #MC  → IST3
;   idt_set_ist(12, 4)   ; #SS  → IST4
;   idt_set_ist(13, 5)   ; #GP  → IST5
;   idt_set_ist(14, 6)   ; #PF  → IST6
; ============================================================================

section .bss
align 16

; IST1 — #DF Double Fault
global ist1_stack_top
ist1_stack_bottom: resb IST_STACK_SIZE
ist1_stack_top:

; IST2 — #NMI Non-Maskable Interrupt
global ist2_stack_top
ist2_stack_bottom: resb IST_STACK_SIZE
ist2_stack_top:

; IST3 — #MC Machine Check
global ist3_stack_top
ist3_stack_bottom: resb IST_STACK_SIZE
ist3_stack_top:

; IST4 — #SS Stack-Segment Fault
global ist4_stack_top
ist4_stack_bottom: resb IST_STACK_SIZE
ist4_stack_top:

; IST5 — #GP General Protection
global ist5_stack_top
ist5_stack_bottom: resb IST_STACK_SIZE
ist5_stack_top:

; IST6 — #PF Page Fault
global ist6_stack_top
ist6_stack_bottom: resb IST_STACK_SIZE
ist6_stack_top:

; IST7 — rezerve (gelecek kullanım için)
global ist7_stack_top
ist7_stack_bottom: resb IST_STACK_SIZE
ist7_stack_top: