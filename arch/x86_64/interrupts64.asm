; interrupts64.asm - Interrupt & Syscall Handlers (Ring-3 SYSRET destekli)

global load_idt64
global isr_keyboard
global isr_timer
global syscall_entry

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

; Syscall dispatcher (syscall.c)
extern syscall_dispatch

section .text
bits 64

; ============================================================================
; IDT Yukle
; ============================================================================
load_idt64:
    lidt [rdi]
    ret

; ============================================================================
; KLAVYE INTERRUPT (IRQ1)
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

; ============================================================================
; MOUSE INTERRUPT (IRQ12) - Sadece GUI mode
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

; ============================================================================
; TIMER INTERRUPT (IRQ0) - Context switch destekli
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

    mov rsp, [rax + 56]     ; Load RSP from cpu_context_t.rsp

.no_switch:
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

; ============================================================================
; CONTEXT SWITCH FONKSIYONLARI
; ============================================================================
;
; cpu_context_t offset haritasi (task.h ile eslesmeli):
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

    ; Mevcut context'i kaydet
    ; BUG FIX: rdi hem pointer (old_ctx) hem kaydedilmesi gereken register.
    ; "mov [rdi+40], rdi" yazilirsa ctx->rdi'ye pointer adresi gider, asil
    ; deger kaybolur. Cozum: rdi'yi onceden stack'e push et.
    push rdi                 ; rdi gercek degerini stack'te sakla
    ; Simdi rsp 8 byte kaymis durumda. Tum kayitlar buna gore yapilacak:
    ;   - rdi: [rsp+0]'dan alinacak
    ;   - rsp: push oncesi deger = rsp+8 (cagiran fonksiyonun RSP'si)
    ;   - rip: [rsp+8] (return address, push rdi'nin ustunde)
    mov [rdi + 0],   rax
    mov [rdi + 8],   rbx
    mov [rdi + 16],  rcx
    mov [rdi + 24],  rdx
    mov [rdi + 32],  rsi
    mov rax, [rsp]           ; rax = gercek rdi degeri (stack'ten)
    mov [rdi + 40],  rax     ; ctx->rdi = dogru deger
    mov [rdi + 48],  rbp
    lea rax, [rsp + 8]       ; rsp + 8 = push rdi oncesindeki RSP (gercek RSP)
    mov [rdi + 56],  rax     ; ctx->rsp = gercek RSP
    mov [rdi + 64],  r8
    mov [rdi + 72],  r9
    mov [rdi + 80],  r10
    mov [rdi + 88],  r11
    mov [rdi + 96],  r12
    mov [rdi + 104], r13
    mov [rdi + 112], r14
    mov [rdi + 120], r15

    mov rax, [rsp + 8]       ; return address ([rsp+8] = push rdi ustundeki ret addr)
    mov [rdi + 128], rax     ; rip

    pushfq
    pop rax
    mov [rdi + 136], rax     ; rflags

    pop rdi                  ; rdi'yi geri al (stack temizle)
    mov rax, [rdi + 0]       ; rax'i orijinal degerine geri yukle

.load_new:
    ; Yeni context'i yukle (RSI = new_ctx)
    mov rax, [rsi + 0]
    mov rbx, [rsi + 8]
    mov rcx, [rsi + 16]
    mov rdx, [rsi + 24]
    ; rsi'yi en son yukle (adres kaynagidir)
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

    mov rsi, [rsi + 32]      ; rsi'yi en son yukle
    ret

global task_save_current_context
task_save_current_context:
    ; RDI = cpu_context_t*
    ; BUG FIX: rdi bug'i burada da ayni sekilde mevcut, ayni cozum uygulanir.
    push rdi
    mov [rdi + 0],   rax
    mov [rdi + 8],   rbx
    mov [rdi + 16],  rcx
    mov [rdi + 24],  rdx
    mov [rdi + 32],  rsi
    mov rax, [rsp]           ; gercek rdi degeri
    mov [rdi + 40],  rax
    mov [rdi + 48],  rbp
    lea rax, [rsp + 8]       ; gercek RSP (push rdi oncesi)
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

    pop rdi                  ; stack temizle
    mov rax, [rdi + 0]       ; rax'i orijinaline geri al
    ret

global task_load_and_jump_context
task_load_and_jump_context:
    ; RDI = cpu_context_t*
    ;
    ; Her iki privilege level (Ring-0 ve Ring-3) icin ayni mekanizma:
    ; context.rsp -> kernel stack tepesi
    ;   [dusuk adr]  15 x register (pop edilir)
    ;   [yuksek adr] iretq frame: RIP / CS / RFLAGS / RSP* / SS*
    ;                (* = Ring-3'te user RSP/SS; Ring-0'da CPU bunlari almaz)
    ;
    ; Adimlar:
    ;   1. DS/ES/FS/GS'yi dogru segment'e set et
    ;   2. RSP = context.rsp
    ;   3. 15 pop
    ;   4. iretq
    ;
    ; Ring-0 iretq: CS.DPL=0 == CPL=0 -> privilege degismez, RSP/SS alinmaz.
    ;   Stack'te 3 kelime (RIP/CS/RFLAGS) okunur.
    ; Ring-3 iretq: CS.DPL=3 != CPL=0 -> privilege gecisi, RSP/SS alinir.
    ;   Stack'te 5 kelime (RIP/CS/RFLAGS/RSP/SS) okunur.
    ;
    ; Her iki durumda da stack yapisi ayni formatta hazirlanmis olmali
    ; (task_create ve task_create_user bunu saglar).

    ; ── DS/ES/FS/GS ayarla ───────────────────────────────────────
    ; context.cs'ye bakarak Ring belirle
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
    ; Ring-3: user data segment (iretq CS/SS'yi ayarlar ama DS/ES/FS/GS'ye dokunmaz)
    mov ax, 0x1B
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

.load_stack:
    ; ── RSP yukle ─────────────────────────────────────────────────
    mov rsp, [rdi + 56]     ; context.rsp -> kernel stack tepesi

    ; ── 15 register pop ───────────────────────────────────────────
    ; task_create/task_create_user dongusu: for(0..14) *(--stk)=0
    ; Stack'te en dusuk adres = son push = ilk pop
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi     ; rdi artik adres kaynagi degil, guvenle pop edilir
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    ; ── iretq ─────────────────────────────────────────────────────
    ; Stack'te simdi iretq frame:
    ;   Ring-0: [RSP+0]=RIP [RSP+8]=CS(0x08) [RSP+16]=RFLAGS
    ;           CPU RSP/SS'yi stack'ten ALMAZ (ayni ring)
    ;   Ring-3: [RSP+0]=RIP [RSP+8]=CS(0x23) [RSP+16]=RFLAGS [RSP+24]=RSP [RSP+32]=SS
    ;           CPU user RSP ve SS'yi stack'ten alir
    iretq

; ============================================================================
; CPU EXCEPTION HANDLER'LAR
; ============================================================================
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

; ============================================================================
; SYSCALL ENTRY POINT
; ============================================================================
;
; Bu handler hem kernel-mode hem user-mode (Ring-3) syscall'larini destekler.
;
; SYSCALL donanimsal olarak su islemleri yapar (MSR konfigurasyonuyla):
;   RCX    <- RIP        (donus adresi — syscall'dan sonraki instruction)
;   R11    <- RFLAGS     (caller'in RFLAGS'i)
;   RIP    <- LSTAR      (bu fonksiyon)
;   CS     <- STAR[47:32]      = 0x08 (Kernel Code)
;   SS     <- STAR[47:32]+8    = 0x10 (Kernel Data)
;   RFLAGS &= ~FMASK           (IF ve DF sifirlanir)
;   RSP    -> DEGISMEZ         (caller'in RSP'si hala gecerli!)
;
; ONEMLI: Ring-3 task SYSCALL yaptiginda RSP hala user stack'i gosteriyor.
; Bu nedenle ilk is kernel stack'e gecmektir.
; TSS RSP0, task_switch()'te tss_set_kernel_stack() ile her zaman guncellenmis.
;
; Ring-3 -> Ring-0 gecisi icin stack degisimi:
;   1. RSP'yi gecici olarak sakla (user RSP)
;   2. TSS RSP0'dan kernel RSP'yi al (swapgs + gs:[kernel_rsp_offset] veya
;      dogrudan TSS'ten okuma — biz burada basit yaklasim kullaniyoruz)
;
; BASIT YAKLASIM (single-CPU, kernel-only sayfa tablosu):
;   TSS RSP0 her context switch'te kernel_stack_top ile guncelleniyor.
;   SYSCALL geldiginde CPU otomatik olarak Ring-3'ten Ring-0'a
;   IRETQ framiyle gecmez — bu sadece interrupt'ta olur.
;   SYSCALL'da RSP degismez; manuel olarak kernel stack'e gecmeliyiz.
;
; ADIMLAR:
;   1. User RSP'yi gecici bir yere kaydet
;   2. Kernel RSP'yi TSS'ten al (kernel_tss.rsp0)
;   3. Kernel stack'e gec
;   4. User RSP ve diger register'lari kaydet
;   5. syscall_dispatch() cagir
;   6. User RSP'yi geri yukle
;   7. SYSRET ile Ring-3'e don
;
; syscall_frame_t layout (syscall.h ile eslesmeli):
;   +0:  rax  syscall num / return value
;   +8:  rdi  arg1
;   +16: rsi  arg2
;   +24: rdx  arg3
;   +32: r10  arg4
;   +40: r8   arg5
;   +48: r9   arg6
;   +56: rcx  saved RIP  (SYSCALL'in kaydettigi, SYSRET icin gerekli)
;   +64: r11  saved RFLAGS (SYSRET icin gerekli)

; Kernel TSS (task.h extern tss_t kernel_tss; -> boot64_unified.asm'de tanimli)
extern kernel_tss

; ============================================================================
; SYSCALL ENTRY — Kernel-mode ve User-mode SYSCALL ayirimi
;
; SYSCALL donanimiyla (MSR):
;   RCX  <- RIP        donus adresi
;   R11  <- RFLAGS     caller RFLAGS
;   RIP  <- LSTAR      bu fonksiyon
;   CS   <- 0x08       Kernel Code (STAR[47:32])
;   SS   <- 0x10       Kernel Data (STAR[47:32]+8)
;   RFLAGS &= ~FMASK   IF+DF sifirlanir
;   RSP  -> DEGISMEZ   caller RSP
;
; CPL TESPITI: CS register'inin RPL field'i (bit[1:0]) CPL'i verir:
;   CS = 0x08 -> RPL=0 -> Ring-0'dan gelmis   -> kernel path
;   SS = 0x1B -> RPL=3 -> Ring-3'ten gelmis   -> user path
;
; KERNEL-MODE PATH (CPL=0):
;   RSP zaten kernel stack'te -> direkt kullan.
;   Donus: RFLAGS'i R11'den restore et, RIP=RCX, dogrudan ret-benzeri donus.
;   NOT: iretq KULLANMA — kernel-mode SYSCALL'da iretq icin tam 5-kelimelik
;   (RIP/CS/RFLAGS/RSP/SS) frame lazim; eksik frame triple fault'a yol acar.
;   Bunun yerine: push r11 / popfq ile RFLAGS restore, sonra jmp rcx.
;
; USER-MODE PATH (CPL=3):
;   RSP = user stack -> kernel stack'e gec (TSS RSP0).
;   dispatch -> o64 sysret ile Ring-3'e don.
; ============================================================================

syscall_entry:
    ; ── CPL tespiti: CS RPL field'ine bak ──────────────────────
    ; SYSCALL sonrasi CS = STAR[47:32] = 0x08 (kernel, RPL=0) olarak set edilir.
    ; Fakat biz SYSCALL'dan onceki CPL'i bilmek istiyoruz.
    ; Cozum: SYSCALL aninda SS'nin eski degeri korunur:
    ;   CPL=0 -> SS=0x10, bit[1:0]=0  -> kernel path
    ;   CPL=3 -> SS=0x1B, bit[1:0]=3  -> user path
    ;
    ; RAX'in yalnizca alt 16 bitini bozarak SS'yi kontrol ediyoruz.
    ; RAX'i stack'e push etmeden yapiyoruz: movzx ax, ss yeterli.
    push rax            ; RAX'i gecici kaydet
    mov ax, ss
    test ax, 3          ; RPL=3 -> user-mode; RPL=0 -> kernel-mode
    pop rax             ; RAX'i geri al (flags degismez, test sonucu korunur)
    jnz .user_syscall   ; Ring-3'ten gelmis -> user path

; ============================================================
; KERNEL-MODE SYSCALL PATH (CPL=0 -> CPL=0)
;
; Donus mekanizmasi: iretq DEGIL, RFLAGS/RIP manuel restore.
; Sebep: iretq, kernel stack'te tam 5-kelimelik (RIP, CS, RFLAGS, RSP, SS)
; frame bekler. Bu path'te RSP/SS push edilmemis olur ve eksik frame
; General Protection Fault -> Double Fault -> Triple Fault zinciri baslatir.
;
; Dogru yaklasim:
;   1. Dispatch'ten sonra callee-saved + frame register'larini geri al.
;   2. push r11 / popfq  -> RFLAGS'i (IF dahil) geri yukle.
;   3. jmp rcx           -> RCX = SYSCALL'in kaydettigi donus RIP.
; ============================================================
.kernel_syscall:
    ; Callee-saved register'lari kaydet (ABI geregi)
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15

    ; syscall_frame_t insa et (syscall.h ile eslesmeli, +0..+64)
    push r11        ; frame.r11  (+64) — saved RFLAGS
    push rcx        ; frame.rcx  (+56) — saved RIP (donus adresi)
    push r9         ; frame.r9   (+48)
    push r8         ; frame.r8   (+40)
    push r10        ; frame.r10  (+32)
    push rdx        ; frame.rdx  (+24)
    push rsi        ; frame.rsi  (+16)
    push rdi        ; frame.rdi  (+8)
    push rax        ; frame.rax  (+0)  — syscall numarasi

    ; syscall_dispatch(frame)
    mov rdi, rsp
    call syscall_dispatch

    ; Donus degerini al (frame.rax), frame'i temizle
    pop rax         ; donus degeri
    pop rdi
    pop rsi
    pop rdx
    pop r10
    pop r8
    pop r9
    pop rcx         ; RCX = saved RIP  (donus adresi)
    pop r11         ; R11 = saved RFLAGS

    ; Callee-saved register'lari geri al
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx

    ; RFLAGS'i restore et: IF'yi (bit 9) geri ac, sonra jmp rcx
    push r11
    or qword [rsp], (1 << 9)    ; IF=1 — interrupt'lari yeniden ac
    popfq                        ; RFLAGS = R11 | IF
    jmp rcx                      ; RIP = saved return address (iretq YOK)

; ============================================================
; USER-MODE SYSCALL PATH (CPL=3 -> CPL=3, SYSRET ile don)
; ============================================================
.user_syscall:
    ; RSP = user stack -> kernel stack'e gec
    mov r15, rsp            ; r15 = user RSP (callee-saved, henuz bozulmadi)

    ; TSS RSP0 = kernel_stack_top
    ; tss_t packed: +0=reserved0(4B), +4=rsp0(8B)
    mov rsp, [rel kernel_tss + 4]

    ; Callee-saved + user RSP'yi kernel stack'e kaydet
    push r15        ; user RSP (adim 9'da geri alinacak)
    push rbx
    push rbp
    push r12
    push r13
    push r14
    ; r15 zaten push edildi

    ; syscall_frame_t insa et
    push r11        ; frame.r11  (+64)
    push rcx        ; frame.rcx  (+56)
    push r9         ; frame.r9   (+48)
    push r8         ; frame.r8   (+40)
    push r10        ; frame.r10  (+32)
    push rdx        ; frame.rdx  (+24)
    push rsi        ; frame.rsi  (+16)
    push rdi        ; frame.rdi  (+8)
    push rax        ; frame.rax  (+0)

    ; dispatch
    mov rdi, rsp
    call syscall_dispatch

    ; donus degerini al, frame temizle
    pop rax
    pop rdi
    pop rsi
    pop rdx
    pop r10
    pop r8
    pop r9
    pop rcx         ; RCX = saved RIP  (SYSRET icin)
    pop r11         ; R11 = saved RFLAGS (SYSRET icin)

    ; callee-saved geri yukle (r15 dahil = user RSP)
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    pop r15         ; r15 = user RSP

    ; DS/ES/FS/GS -> Ring-3 (SYSRET CS/SS'yi ayarlar ama bunlara dokunmaz)
    mov ax, 0x1B
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; User RSP'ye don
    mov rsp, r15

    ; 64-bit SYSRET -> Ring-3
    ; CS = STAR[63:48]+16|3 = 0x23, SS = STAR[63:48]+8|3 = 0x1B
    o64 sysret