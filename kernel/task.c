// task.c - Task Management Implementation
// Ring-0 + Ring-3 (SYSRET) + TSS destekli versiyon
#include "task.h"
#include "memory_unified.h"
#include <stddef.h>

// External helpers
extern void str_cpy(char* dest, const char* src);
extern int str_len(const char* str);
extern void println64(const char* str, uint8_t color);
extern void print_str64(const char* str, uint8_t color);
extern void int_to_str(int num, char* str);
extern void uint64_to_string(uint64_t num, char* str);
extern void serial_print(const char* str);
extern uint64_t get_system_ticks(void);

// GDT pointer (boot64_unified.asm'de tanimli)
// gdt64_pointer: [0..1]=limit (uint16), [2..9]=base (uint64)
extern uint8_t  gdt64[];          // GDT tablosunun baslangici
extern uint8_t  gdt64_pointer[];  // lgdt pointer struct'i

// kernel_tss boot64_unified.asm'de tanimli, burada sadece extern (task.h'da bildirildi)

// VGA colors
#define VGA_WHITE  0x0F
#define VGA_GREEN  0x0A
#define VGA_CYAN   0x03
#define VGA_YELLOW 0x0E

// ===========================================
// GLOBAL DEGISKENLER
// ===========================================
static task_t*      current_task            = NULL;
static task_queue_t ready_queue;
static task_t*      idle_task               = NULL;
static uint32_t     next_pid                = 1;
static int          task_system_initialized = 0;

// task_exit() icin: onceki task (static — scheduler'daki previous_task ile catismasin)
static task_t* task_exit_previous = NULL;

// ===========================================
// YARDIMCI FONKSIYONLAR
// ===========================================
static void* memset_task(void* dest, int val, uint64_t n) {
    uint8_t* d = (uint8_t*)dest;
    while (n--) *d++ = (uint8_t)val;
    return dest;
}

static void str_copy_safe(char* dest, const char* src, int max_len) {
    int i = 0;
    while (src[i] && i < max_len - 1) { dest[i] = src[i]; i++; }
    dest[i] = '\0';
}

// ============================================================
// GDT YONETIMI
// ============================================================

// 8-byte normal segment descriptor yaz (GDT'nin offset byte'ina)
static void gdt_write_descriptor(uint32_t offset,
                                  uint32_t base,  uint32_t limit,
                                  uint8_t  access, uint8_t  flags_limit_hi)
{
    uint8_t* e = gdt64 + offset;
    e[0] = (uint8_t)(limit & 0xFF);
    e[1] = (uint8_t)((limit >> 8) & 0xFF);
    e[2] = (uint8_t)(base & 0xFF);
    e[3] = (uint8_t)((base >> 8) & 0xFF);
    e[4] = (uint8_t)((base >> 16) & 0xFF);
    e[5] = access;
    e[6] = flags_limit_hi;
    e[7] = (uint8_t)((base >> 24) & 0xFF);
}

// 16-byte TSS system descriptor yaz (gdt64'teki offset'e)
// Descriptor iki ardisik 8-byte slot kaplar: offset ve offset+8
static void gdt_write_tss_descriptor(uint32_t offset,
                                      uint64_t tss_addr,
                                      uint32_t tss_limit)
{
    uint8_t* e = gdt64 + offset;

    // Low 8 bytes:
    //   [1:0]   Limit[15:0]
    //   [4:2]   Base[23:0]
    //   [5]     Access: P=1, DPL=0, Type=0x9 (64-bit TSS Available)
    //   [6]     Flags + Limit[19:16]: G=0, Limit hi nibble=0
    //   [7]     Base[31:24]
    e[0] = (uint8_t)(tss_limit & 0xFF);
    e[1] = (uint8_t)((tss_limit >> 8) & 0xFF);
    e[2] = (uint8_t)(tss_addr & 0xFF);
    e[3] = (uint8_t)((tss_addr >> 8) & 0xFF);
    e[4] = (uint8_t)((tss_addr >> 16) & 0xFF);
    e[5] = 0x89;    // P=1, DPL=0, S=0 (system), Type=1001 (64-bit TSS Avail)
    e[6] = 0x00;    // G=0, limit high=0
    e[7] = (uint8_t)((tss_addr >> 24) & 0xFF);

    // High 8 bytes:
    //   [3:0]   Base[63:32]
    //   [7:4]   Reserved (sifir)
    uint8_t* e2 = e + 8;
    e2[0] = (uint8_t)((tss_addr >> 32) & 0xFF);
    e2[1] = (uint8_t)((tss_addr >> 40) & 0xFF);
    e2[2] = (uint8_t)((tss_addr >> 48) & 0xFF);
    e2[3] = (uint8_t)((tss_addr >> 56) & 0xFF);
    e2[4] = 0x00;
    e2[5] = 0x00;
    e2[6] = 0x00;
    e2[7] = 0x00;
}

void gdt_install_user_segments(void)
{
    serial_print("[GDT] Installing Ring-3 + TSS descriptors...\n");

    // ── 0x18: User Data Segment (Ring 3) ─────────────────────────────
    // Access byte = 0xF2:
    //   P=1  (Present)
    //   DPL=11 (Ring 3)
    //   S=1  (code/data, not system)
    //   Type=0010 (data, expand-up, writable)
    // Flags+LimitHigh = 0xCF:
    //   G=1  (4KB granularity)
    //   D=1  (32-bit default operand)
    //   L=0  (NOT 64-bit code; data descriptor icin L anlamsiz)
    //   AVL=0
    //   Limit[19:16] = 0xF
    gdt_write_descriptor(GDT_USER_DATA,
                          0x00000000,  // base = 0 (flat memory)
                          0x000FFFFF,  // limit
                          0xF2,        // access
                          0xCF);       // flags + limit high

    serial_print("[GDT] User Data (0x18) written\n");

    // ── 0x20: User Code Segment (Ring 3, 64-bit) ─────────────────────
    // Access byte = 0xFA:
    //   P=1, DPL=11, S=1, Type=1010 (code, exec, readable)
    // Flags+LimitHigh = 0xAF:
    //   G=1, D=0 (64-bit modda D=0 olmali), L=1 (64-bit code!), AVL=0
    //   Limit[19:16] = 0xF
    gdt_write_descriptor(GDT_USER_CODE,
                          0x00000000,
                          0x000FFFFF,
                          0xFA,        // access
                          0xAF);       // flags: G=1, L=1 (64-bit)

    serial_print("[GDT] User Code (0x20) written\n");

    // ── 0x28 + 0x30: TSS Descriptor (16-byte) ────────────────────────
    // tss_addr = kernel_tss'in sanal adresi (higher half'te)
    // tss_limit = sizeof(tss_t) - 1 = 103
    gdt_write_tss_descriptor(GDT_TSS_SELECTOR,
                              (uint64_t)&kernel_tss,
                              (uint32_t)(sizeof(tss_t) - 1));

    serial_print("[GDT] TSS descriptor (0x28/0x30) written\n");

    // ── GDT pointer'i guncelle ve lgdt cagir ─────────────────────────
    // gdt64_pointer formati: [uint16_t limit][uint64_t base]
    // Yeni limit = GDT_TSS_SELECTOR + 16 - 1 = 0x28 + 16 - 1 = 0x37
    uint16_t new_limit = (uint16_t)(GDT_TSS_SELECTOR + 16 - 1);  // 0x37

    // gdt64_pointer[0..1] = limit (little-endian)
    gdt64_pointer[0] = (uint8_t)(new_limit & 0xFF);
    gdt64_pointer[1] = (uint8_t)((new_limit >> 8) & 0xFF);
    // gdt64_pointer[2..9] = base — gdt64 adresi degismez, dokunmuyoruz

    __asm__ volatile (
        "lgdt (%0)"
        :
        : "r"((uint64_t)gdt64_pointer)
        : "memory"
    );

    serial_print("[GDT] lgdt reloaded. New limit=0x37\n");
    serial_print("[GDT] Segments: KCode=0x08 KData=0x10 "
                 "UData=0x18 UCode=0x20 TSS=0x28\n");
}

// ============================================================
// TSS YONETIMI
// ============================================================

void tss_init(void)
{
    serial_print("[TSS] Initializing TSS (64-bit)...\n");

    // TSS'yi tamamen sifirla
    memset_task(&kernel_tss, 0, sizeof(tss_t));

    // IOPB offset: sizeof(tss_t) => I/O Permission Bitmap yok
    // Ring-0 tum I/O portlarina eriSebilir
    kernel_tss.iopb_offset = (uint16_t)sizeof(tss_t);

    // RSP0: ilk deger olarak 0; ilk context switch'te
    // tss_set_kernel_stack(task->kernel_stack_top) cagirilir.
    kernel_tss.rsp0 = 0;

    // IST1: kritik interrupt'lar (NMI, Double Fault) icin ayri stack
    // Simdilik 0 (kernel stack yeterli)
    kernel_tss.ist1 = 0;

    // TR (Task Register) register'ini yukle
    // ltr: GDT'deki TSS selector'i TR'ye yazar
    // NOT: GDT'de TSS "Available" (type=9) olmali — ltr onu "Busy" (type=B) yapar
    //      Bu nedenle tss_init(), gdt_install_user_segments()'dan SONRA cagirilmali.
    __asm__ volatile (
        "ltr %0"
        :
        : "r"((uint16_t)GDT_TSS_SELECTOR)
        : "memory"
    );

    serial_print("[TSS] TR loaded with selector 0x28\n");

    {
        char num[8];
        serial_print("[TSS] kernel_tss @ 0x");
        uint64_to_string((uint64_t)&kernel_tss, num);
        serial_print(num);
        serial_print(", size=");
        int_to_str((int)sizeof(tss_t), num);
        serial_print(num);
        serial_print(" bytes\n");
        serial_print("[TSS] iopb_offset=");
        int_to_str(kernel_tss.iopb_offset, num);
        serial_print(num);
        serial_print("\n");
    }
}

// ===========================================
// TASK QUEUE ISLEMLERI
// ===========================================

void task_queue_init(task_queue_t* queue) {
    queue->head  = NULL;
    queue->tail  = NULL;
    queue->count = 0;
}

void task_queue_push(task_queue_t* queue, task_t* task) {
    if (!task) return;
    task->next = NULL;
    task->prev = queue->tail;
    if (queue->tail) queue->tail->next = task;
    else             queue->head = task;
    queue->tail = task;
    queue->count++;
}

task_t* task_queue_pop(task_queue_t* queue) {
    if (!queue->head) return NULL;
    task_t* task  = queue->head;
    queue->head   = task->next;
    if (queue->head) queue->head->prev = NULL;
    else             queue->tail = NULL;
    task->next = NULL;
    task->prev = NULL;
    queue->count--;
    return task;
}

void task_queue_remove(task_queue_t* queue, task_t* task) {
    if (!task) return;
    if (task->prev) task->prev->next = task->next;
    else            queue->head = task->next;
    if (task->next) task->next->prev = task->prev;
    else            queue->tail = task->prev;
    task->next = NULL;
    task->prev = NULL;
    queue->count--;
}

int task_queue_is_empty(task_queue_t* queue) {
    return queue->count == 0;
}

// ===========================================
// TASK INIT
// ===========================================

void task_init(void) {
    if (task_system_initialized) return;

    serial_print("[TASK] Initializing task management system...\n");

    task_queue_init(&ready_queue);
    task_system_initialized = 1;

    idle_task = task_create_idle();
    if (idle_task) {
        serial_print("[TASK] Idle task created (PID=0)\n");
    } else {
        serial_print("[TASK ERROR] Failed to create idle task!\n");
        task_system_initialized = 0;
        return;
    }

    current_task = idle_task;

    // Idle task'in kernel_stack_top'unu TSS RSP0'a yaz
    // (Ilk interrupt geldiginde CPU bunu kullanir)
    tss_set_kernel_stack(idle_task->kernel_stack_top);

    serial_print("[TASK] Task system initialized\n");
}

// ===========================================
// KERNEL TASK OLUSTURMA (Ring-0)
// ===========================================

task_t* task_create(const char* name, void (*entry_point)(void), uint32_t priority) {
    if (!task_system_initialized) {
        serial_print("[TASK ERROR] Task system not initialized!\n");
        return NULL;
    }

    task_t* task = (task_t*)kmalloc(sizeof(task_t));
    if (!task) {
        serial_print("[TASK ERROR] Failed to allocate task structure\n");
        return NULL;
    }
    memset_task(task, 0, sizeof(task_t));

    task->pid             = next_pid++;
    str_copy_safe(task->name, name, 32);
    task->state           = TASK_STATE_READY;
    task->priority        = priority;
    task->privilege_level = TASK_PRIVILEGE_KERNEL;  // Ring 0

    // ── Kernel Stack ──────────────────────────────────────────────
    task->kernel_stack_size = KERNEL_STACK_SIZE;
    task->kernel_stack_base = (uint64_t)kmalloc(KERNEL_STACK_SIZE);
    if (!task->kernel_stack_base) {
        serial_print("[TASK ERROR] Failed to allocate kernel stack\n");
        kfree(task);
        return NULL;
    }
    memset_task((void*)task->kernel_stack_base, 0, KERNEL_STACK_SIZE);
    // Stack asagi buyur: top = base + size
    task->kernel_stack_top = task->kernel_stack_base + KERNEL_STACK_SIZE;

    // user_stack: kernel task'ta yok
    task->user_stack_base = 0;
    task->user_stack_top  = 0;
    task->user_stack_size = 0;

    // ── Kernel Stack Frame ────────────────────────────────────────
    // Timer ISR (isr_timer) context switch mekanizmasi:
    //   1. Calisan task'in register'larini push eder (15 adet)
    //   2. task_save_current_stack(rsp) -> context.rsp buraya yazilir
    //   3. Sonraki task'in context.rsp'si RSP'ye yuklenir
    //   4. 15 pop + iretq -> yeni task'a atlar
    //
    // Yani task_create() ile ilk kez olusturulan bir task icin
    // context.rsp, timer ISR'in iretq yapabilmesi icin dogru formatta
    // hazir bir stack'e isaret etmeli:
    //
    //   [yuksek]  SS      = 0x10  (Kernel Data)   \
    //             RSP     = kernel_stack_top        |  iretq frame
    //             RFLAGS  = 0x202 (IF=1)            |  (5 qword)
    //             CS      = 0x08  (Kernel Code)     |
    //             RIP     = entry_point             /
    //             r15..rax = 0  (15 register)      <- context.rsp buraya
    //   [dusuk]
    //
    // task_load_and_jump_context() Ring-0 path'i de (context_switches==0
    // durumunda task_switch tarafindan cagirilir) bu formati destekler:
    // struct'tan register yukleme yerine RSP'yi set edip ayni pop+iretq
    // yapar -- HAYIR, o path hala ret kullaniyor.
    //
    // En tutarli cozum: her iki path icin de (timer ISR ve ilk baslatma)
    // ayni stack formati. task_load_and_jump_context Ring-0 path'ini de
    // pop+iretq yapacak sekilde guncelliyoruz (interrupts64.asm'de).
    // Burada sadece stack'i hazirliyoruz.

    uint64_t* stk = (uint64_t*)task->kernel_stack_top;

    // iretq frame (Ring-0 -> Ring-0, ayni privilege seviyesi)
    // Ring-0 iretq: CPU SS ve RSP'yi stack'ten ALMAZ (privilege degismedi).
    // Sadece RIP, CS, RFLAGS alinir. Ama stack formati tutarlilik icin
    // 5 kelime yaziyoruz; timer ISR her zaman 5 kelimelik iretq frame bekler.
    *(--stk) = GDT_KERNEL_DATA;          // SS      = 0x10
    *(--stk) = task->kernel_stack_top;   // RSP     = stack tepesi (referans)
    *(--stk) = 0x202ULL;                 // RFLAGS  = IF=1
    *(--stk) = GDT_KERNEL_CODE;          // CS      = 0x08
    *(--stk) = (uint64_t)entry_point;    // RIP     = entry

    // 15 genel amacli register (timer ISR pop sirasi: r15..rax)
    for (int i = 0; i < 15; i++) *(--stk) = 0;

    // ── CPU Context ───────────────────────────────────────────────
    task->context.rsp    = (uint64_t)stk;        // stack tepesi (15 reg alani)
    task->context.rip    = (uint64_t)entry_point;
    task->context.rflags = 0x202;
    task->context.cs     = GDT_KERNEL_CODE;       // 0x08
    task->context.ss     = GDT_KERNEL_DATA;       // 0x10
    task->context.ds     = GDT_KERNEL_DATA;
    task->context.es     = GDT_KERNEL_DATA;
    task->context.fs     = 0;
    task->context.gs     = 0;
    task->context.cr3    = 0;
    task->context.rax    = 0;
    task->context.rbx    = 0;
    task->context.rcx    = 0;
    task->context.rdx    = 0;
    task->context.rsi    = 0;
    task->context.rdi    = 0;
    task->context.rbp    = 0;
    task->context.r8     = 0;
    task->context.r9     = 0;
    task->context.r10    = 0;
    task->context.r11    = 0;
    task->context.r12    = 0;
    task->context.r13    = 0;
    task->context.r14    = 0;
    task->context.r15    = 0;

    task->time_slice     = 10;

    {
        char buf[20];
        serial_print("[TASK] Created kernel task '");
        serial_print(name);
        serial_print("' (PID=");
        int_to_str(task->pid, buf);
        serial_print(buf);
        serial_print(", Ring 0, entry=0x");
        uint64_to_string((uint64_t)entry_point, buf);
        serial_print(buf);
        serial_print(")\n");
        serial_print("[TASK]   kernel_stack_top=0x");
        uint64_to_string(task->kernel_stack_top, buf);
        serial_print(buf);
        serial_print(", ctx.rsp=0x");
        uint64_to_string(task->context.rsp, buf);
        serial_print(buf);
        serial_print("\n");
    }

    return task;
}

// ===========================================
// USER TASK OLUSTURMA (Ring-3)
// ===========================================

task_t* task_create_user(const char* name, void (*entry_point)(void), uint32_t priority)
{
    if (!task_system_initialized) {
        serial_print("[TASK ERROR] Task system not initialized!\n");
        return NULL;
    }

    task_t* task = (task_t*)kmalloc(sizeof(task_t));
    if (!task) {
        serial_print("[TASK ERROR] Failed to allocate user task structure\n");
        return NULL;
    }
    memset_task(task, 0, sizeof(task_t));

    task->pid             = next_pid++;
    str_copy_safe(task->name, name, 32);
    task->state           = TASK_STATE_READY;
    task->priority        = priority;
    task->privilege_level = TASK_PRIVILEGE_USER;   // Ring 3

    // ── Kernel Stack ──────────────────────────────────────────────
    // SYSCALL/interrupt geldiginde CPU, TSS RSP0'dan bu adresi yukler.
    // task_switch() sirasinda tss_set_kernel_stack(task->kernel_stack_top)
    // cagirilarak TSS her task degisiminde guncellenir.
    task->kernel_stack_size = KERNEL_STACK_SIZE;
    task->kernel_stack_base = (uint64_t)kmalloc(KERNEL_STACK_SIZE);
    if (!task->kernel_stack_base) {
        kfree(task);
        return NULL;
    }
    memset_task((void*)task->kernel_stack_base, 0, KERNEL_STACK_SIZE);
    task->kernel_stack_top = task->kernel_stack_base + KERNEL_STACK_SIZE;

    // ── User Stack ────────────────────────────────────────────────
    // SYSRET sonrasi RSP bu degere ayarlanir.
    // Ring-3 task'in butun calisma suresi bu stack'i kullanir.
    task->user_stack_size = USER_STACK_SIZE;
    task->user_stack_base = (uint64_t)kmalloc(USER_STACK_SIZE);
    if (!task->user_stack_base) {
        kfree((void*)task->kernel_stack_base);
        kfree(task);
        return NULL;
    }
    memset_task((void*)task->user_stack_base, 0, USER_STACK_SIZE);
    task->user_stack_top = task->user_stack_base + USER_STACK_SIZE;

    // ── Kernel Stack'e iretq Frame ────────────────────────────────
    // task_load_and_jump_context ve timer ISR ayni stack formatini bekler:
    //   [yuksek]  SS     = 0x1B  (User Data)
    //             RSP    = user_stack_top
    //             RFLAGS = 0x202 (IF=1)
    //             CS     = 0x23  (User Code, DPL=3)
    //             RIP    = entry_point
    //             r15..rax = 0  (15 adet)  <- context.rsp buraya
    //   [dusuk]

    uint64_t* stk = (uint64_t*)task->kernel_stack_top;

    *(--stk) = GDT_USER_DATA_RPL3;      // SS
    *(--stk) = task->user_stack_top;    // RSP (user stack)
    *(--stk) = 0x202ULL;                // RFLAGS = IF=1
    *(--stk) = GDT_USER_CODE_RPL3;      // CS = 0x23
    *(--stk) = (uint64_t)entry_point;   // RIP

    for (int i = 0; i < 15; i++) *(--stk) = 0;

    // ── CPU Context ───────────────────────────────────────────────
    task->context.rsp    = (uint64_t)stk;
    task->context.rip    = (uint64_t)entry_point;
    task->context.rflags = 0x202;
    task->context.cs     = GDT_USER_CODE_RPL3;   // 0x23
    task->context.ss     = GDT_USER_DATA_RPL3;   // 0x1B
    task->context.ds     = GDT_USER_DATA_RPL3;
    task->context.es     = GDT_USER_DATA_RPL3;
    task->context.fs     = 0;
    task->context.gs     = 0;
    task->context.cr3    = 0;

    task->time_slice = 10;

    {
        char buf[20];
        serial_print("[TASK] Created user task '");
        serial_print(name);
        serial_print("' (PID=");
        int_to_str(task->pid, buf);
        serial_print(buf);
        serial_print(", Ring 3, entry=0x");
        uint64_to_string((uint64_t)entry_point, buf);
        serial_print(buf);
        serial_print(")\n");
        serial_print("[TASK]   kernel_stack_top=0x");
        uint64_to_string(task->kernel_stack_top, buf);
        serial_print(buf);
        serial_print("  user_stack_top=0x");
        uint64_to_string(task->user_stack_top, buf);
        serial_print(buf);
        serial_print("\n");
        serial_print("[TASK]   CS=0x23 SS=0x1B (Ring-3 selectors)\n");
    }

    return task;
}

// ===========================================
// TASK YASAM DONGUSU
// ===========================================

int task_start(task_t* task) {
    if (!task) return -1;
    if (task->context_switches == 0) {
        serial_print("[TASK] Starting task '");
        serial_print(task->name);
        serial_print("'\n");
    }
    task->state = TASK_STATE_READY;
    task_queue_push(&ready_queue, task);
    return 0;
}

void task_terminate(task_t* task) {
    if (!task) return;
    serial_print("[TASK] Terminating task '");
    serial_print(task->name);
    serial_print("'\n");
    task->state = TASK_STATE_TERMINATED;
    task_queue_remove(&ready_queue, task);
    if (task->kernel_stack_base)
        kfree((void*)task->kernel_stack_base);
    if (task->user_stack_base)
        kfree((void*)task->user_stack_base);
    kfree(task);
}

void task_exit(void) {
    if (!current_task || current_task == idle_task) {
        serial_print("[TASK ERROR] Cannot exit idle task!\n");
        return;
    }
    serial_print("[TASK] Task '");
    serial_print(current_task->name);
    serial_print("' exiting\n");
    current_task->state = TASK_STATE_TERMINATED;
    task_t* next = task_get_next();
    if (!next) next = idle_task;
    task_exit_previous = current_task;
    current_task  = next;
    next->state   = TASK_STATE_RUNNING;
    next->last_run_time = get_system_ticks();

    // Yeni task'in kernel stack'ini TSS'e bildir
    tss_set_kernel_stack(next->kernel_stack_top);

    // Direkt idle_task_entry() cagrilmaz — IF restore edilmez, klavye olur.
    // Her zaman task_load_and_jump_context() uzerinden gec.
    serial_print("[TASK] Restoring next task context\n");
    task_load_and_jump_context(&next->context);
    serial_print("[TASK ERROR] task_exit returned!\n");
    while(1) __asm__ volatile("hlt");
}

void task_set_state(task_t* task, uint32_t new_state) {
    if (task) task->state = new_state;
}

// ===========================================
// MEVCUT TASK YONETIMI
// ===========================================

task_t* task_get_current(void) { return current_task; }

void task_set_current(task_t* task) {
    if (!task) return;
    current_task = task;
    task->last_run_time = get_system_ticks();
}

task_t* task_get_next(void) {
    task_t* next = task_queue_pop(&ready_queue);
    if (!next) return idle_task;
    serial_print("[TASK] Getting next: ");
    serial_print(next->name);
    serial_print(" (RSP=0x");
    char addr[20];
    uint64_to_string(next->context.rsp, addr);
    serial_print(addr);
    serial_print(")\n");
    return next;
}

uint32_t task_get_count(void) { return ready_queue.count; }

task_t* task_find_by_pid(uint32_t pid) {
    if (current_task && current_task->pid == pid) return current_task;
    task_t* t = ready_queue.head;
    while (t) { if (t->pid == pid) return t; t = t->next; }
    if (idle_task && idle_task->pid == pid) return idle_task;
    return NULL;
}

// ===========================================
// CONTEXT SWITCH
// ===========================================

void task_save_context(cpu_context_t* context) {
    task_save_current_context(context);
}

void task_load_context(cpu_context_t* context) {
    task_load_and_jump_context(context);
}

void task_switch(task_t* from, task_t* to) {
    if (!to) {
        serial_print("[TASK ERROR] Cannot switch to NULL task!\n");
        return;
    }

    serial_print("[TASK] Switching from '");
    serial_print(from ? from->name : "NULL");
    serial_print("' to '");
    serial_print(to->name);
    serial_print("'\n");

    // ── TSS RSP0 Guncelle ──────────────────────────────────────────
    // Yeni task'in kernel_stack_top'u TSS RSP0'a yazilir.
    // SYSCALL veya interrupt geldiginde CPU bu adresi RSP olarak kullanir.
    // Hem Ring-0 hem Ring-3 task'lar icin calisiyor:
    //   - Ring-0 task: kernel_stack_top, task'in kendi kernel stack tepesi
    //   - Ring-3 task: kernel_stack_top, SYSCALL handler'in kullanacagi kernel stack
    tss_set_kernel_stack(to->kernel_stack_top);

    current_task = to;
    to->state    = TASK_STATE_RUNNING;
    to->last_run_time  = get_system_ticks();
    to->context_switches++;

    if (!from || from == idle_task) {
        serial_print("[TASK] Jumping to task (no save)\n");
        task_load_and_jump_context(&to->context);
        return;
    }

    // Normal context switch: from'u kaydet, to'yu yukle
    task_switch_context(&from->context, &to->context);
    serial_print("[TASK] Context switch returned\n");
}

// ===========================================
// IDLE TASK
// ===========================================

void idle_task_entry(void) {
    serial_print("[IDLE] Idle task started\n");
    while (1) __asm__ volatile("hlt");
}

task_t* task_create_idle(void) {
    task_t* idle = task_create("idle", idle_task_entry, 0);
    if (idle) {
        idle->pid   = 0;
        idle->state = TASK_STATE_RUNNING;
    }
    return idle;
}

// ===========================================
// TEST TASK'LAR (Ring-0)
// ===========================================

void test_task_a(void) {
    serial_print("[TASK A] Started (Ring 0)\n");
    for (int i = 0; i < 5; i++) {
        serial_print("[TASK A] Iteration ");
        char num[16]; int_to_str(i, num); serial_print(num);
        serial_print("\n");
        for (volatile uint64_t j = 0; j < 1000000; j++);
    }
    serial_print("[TASK A] Exiting\n");
    task_exit();
}

void test_task_b(void) {
    serial_print("[TASK B] Started (Ring 0)\n");
    for (int i = 0; i < 5; i++) {
        serial_print("[TASK B] Iteration ");
        char num[16]; int_to_str(i, num); serial_print(num);
        serial_print("\n");
        for (volatile uint64_t j = 0; j < 1000000; j++);
    }
    serial_print("[TASK B] Exiting\n");
    task_exit();
}

void test_task_c(void) {
    serial_print("[TASK C] Started (Ring 0)\n");
    for (int i = 0; i < 5; i++) {
        serial_print("[TASK C] Iteration ");
        char num[16]; int_to_str(i, num); serial_print(num);
        serial_print("\n");
        for (volatile uint64_t j = 0; j < 1000000; j++);
    }
    serial_print("[TASK C] Exiting\n");
    task_exit();
}

void offihito_task(void) {
    serial_print("[OFFIHITO] Task started\n");
    uint64_t counter = 0;
    while (1) {
        counter++;
        if (counter >= 100000) {
            println64("Offihito", 0x0D);
            serial_print("[OFFIHITO] Printed\n");
            counter = 0;
        }
        for (volatile int i = 0; i < 1000; i++);
    }
}

// ===========================================
// USER MODE TEST TASK (Ring-3)
// ===========================================

void user_mode_test_task(void) {
    // Bu fonksiyon SYSRET ile Ring-3'te baslar.
    // Buradaki kod user privilege level'da calisir.
    // SYSCALL ile kernel'e gecebilir.

    // SYS_DEBUG syscall'i ile kernel'e mesaj gonder
    // RAX=8 (SYS_DEBUG), RDI=msg_ptr
    const char* msg = "[USER TASK] Hello from Ring-3 via SYSRET!\n";
    uint64_t ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"((uint64_t)8),         // SYS_DEBUG
          "D"((uint64_t)msg)
        : "rcx", "r11", "memory"
    );

    // SYS_GETPID
    uint64_t pid;
    __asm__ volatile (
        "syscall"
        : "=a"(pid)
        : "a"((uint64_t)4)          // SYS_GETPID
        : "rcx", "r11"
    );
    (void)pid;

    // SYS_EXIT ile cik
    __asm__ volatile (
        "syscall"
        :
        : "a"((uint64_t)3),         // SYS_EXIT
          "D"((uint64_t)0)          // exit code = 0
        : "rcx", "r11"
    );

    // Buraya hic gelinmemeli
    while (1) __asm__ volatile("hlt");
}

// ===========================================
// UTILITY
// ===========================================

void task_print_info(task_t* task) {
    if (!task) return;
#ifdef TEXT_MODE
    print_str64("Task '", VGA_CYAN);
    print_str64(task->name, VGA_YELLOW);
    print_str64("' (PID=", VGA_CYAN);
    char pid_str[16];
    int_to_str(task->pid, pid_str);
    print_str64(pid_str, VGA_WHITE);
    print_str64(", Ring ", VGA_CYAN);
    char ring[2]; ring[0] = '0' + task->privilege_level; ring[1] = '\0';
    print_str64(ring, VGA_WHITE);
    print_str64(", State=", VGA_CYAN);
    const char* s;
    switch (task->state) {
        case TASK_STATE_READY:      s = "READY";      break;
        case TASK_STATE_RUNNING:    s = "RUNNING";    break;
        case TASK_STATE_BLOCKED:    s = "BLOCKED";    break;
        case TASK_STATE_TERMINATED: s = "TERMINATED"; break;
        default:                    s = "UNKNOWN";    break;
    }
    print_str64(s, VGA_GREEN);
    println64(")", VGA_CYAN);
#endif
}

void task_list_all(void) {
#ifdef TEXT_MODE
    println64("=== Task List ===", VGA_CYAN);
    if (current_task) {
        print_str64("* CURRENT: ", VGA_GREEN);
        task_print_info(current_task);
    }
    println64("READY QUEUE:", VGA_YELLOW);
    task_t* t = ready_queue.head;
    while (t) { print_str64("  - ", VGA_WHITE); task_print_info(t); t = t->next; }
    if (idle_task) { print_str64("IDLE: ", VGA_CYAN); task_print_info(idle_task); }
#endif
}

void task_print_stats(void) {
#ifdef TEXT_MODE
    println64("=== Task Statistics ===", VGA_CYAN);
    char num[16];
    serial_print("Total tasks: ");
    int_to_str(ready_queue.count + (current_task ? 1 : 0), num);
    serial_print(num); serial_print("\n");
    serial_print("Next PID: ");
    int_to_str(next_pid, num);
    serial_print(num); serial_print("\n");
#endif
}