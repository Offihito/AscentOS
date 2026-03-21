// task.c - Task Management Implementation
// Ring-0 + Ring-3 (SYSRET) + TSS destekli versiyon
// v2: task_create_from_elf() eklendi
// v3: argc/argv SysV ABI stack kurulumu + foreground_pid yönetimi
#include "task.h"
#include "signal64.h"
#include "pmm.h"
#include "heap.h"
#include "elf64.h"
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

// PMM büyük tahsis — vmm identity-map ile (stack'ler için)
extern void* pmm_alloc_pages(uint64_t count);
extern void  pmm_free_pages (void* base, uint64_t count);
// Bayrak parametreli: 0x3=kernel-only, 0x7=user-accessible
extern void* pmm_alloc_pages_flags(uint64_t count, uint64_t map_flags);

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

// ============================================================
// ZOMBIE LİSTESİ
//
// task_exit() çağrıldığında task ready_queue'dan çıkar.
// Ama parent waitpid() ile toplayana kadar TCB kaybolmamalı.
// Bu yüzden TERMINATED yerine ZOMBIE yapıp burada tutuyoruz.
// task_find_by_pid() bu listeye de bakıyor; waitpid bulabilsin.
// ============================================================
static task_t* zombie_list_head = NULL;

static void zombie_list_add(task_t* t) {
    t->next = zombie_list_head;
    t->prev = NULL;
    if (zombie_list_head) zombie_list_head->prev = t;
    zombie_list_head = t;
}

static void zombie_list_remove(task_t* t) {
    if (t->prev) t->prev->next = t->next;
    else         zombie_list_head = t->next;
    if (t->next) t->next->prev = t->prev;
    t->next = NULL;
    t->prev = NULL;
}

// ============================================================
// FOREGROUND TASK YÖNETİMİ
//
// foreground_pid != 0 → bir ELF task ön planda çalışıyor.
// keyboard_unified.c bu değeri okuyarak tuş girdisini
// doğrudan kb_ring'e yönlendirir (shell prompt'una değil).
// task_exit() bu değeri sıfırlar ve shell'i geri getirir.
// ============================================================
volatile uint32_t foreground_pid = 0;

// ===========================================
// YARDIMCI FONKSIYONLAR
// ===========================================
static void* memset_task(void* dest, int val, uint64_t n) {
    uint8_t* d = (uint8_t*)dest;
    while (n--) *d++ = (uint8_t)val;
    return dest;
}

static void* memcpy_task(void* dest, const void* src, uint64_t n) {
    uint8_t* d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;
    while (n--) *d++ = *s++;
    return dest;
}

static void str_copy_safe(char* dest, const char* src, int max_len) {
    int i = 0;
    while (src[i] && i < max_len - 1) { dest[i] = src[i]; i++; }
    dest[i] = '\0';
}

static int str_len_local(const char* s) {
    int n = 0;
    while (s[n]) n++;
    return n;
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
        // uint64_t VMA adresi ondalıkta 20 hane olabilir (0xFFFFFFFF80... = ~1.84e19).
        // Eski char num[8] höher-half'te 20 byte yazarak stack frame'i bozuyordu
        // → tss_init() return'ünde corrupt RIP → triple fault.
        char num[32];
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

    task->pgid = task->pid;
    task->sid  = task->pid;

    // Sinyal tablosunu başlat (v10)
    signal_table_init(&task->signal_table);
    task->signal_trampoline = 0;

    // fd tablosunu başlat — stdin/stdout/stderr serial'a bağlanır
    fd_table_init(task->fd_table);

    // ── Kernel Stack ──────────────────────────────────────────────
    task->kernel_stack_size = KERNEL_STACK_SIZE;
    {
        uint64_t page_count = KERNEL_STACK_SIZE / 4096;
        void* ks = pmm_alloc_pages(page_count);
        if (!ks) {
            serial_print("[TASK WARN] pmm_alloc_pages failed for kernel stack, trying kmalloc\n");
            ks = kmalloc(KERNEL_STACK_SIZE);
            if (!ks) {
                serial_print("[TASK ERROR] Failed to allocate kernel stack\n");
                kfree(task);
                return NULL;
            }
        }
        task->kernel_stack_base = (uint64_t)ks;
    }
    task->kernel_stack_top = task->kernel_stack_base + KERNEL_STACK_SIZE;

    // user_stack: kernel task'ta yok
    task->user_stack_base = 0;
    task->user_stack_top  = 0;
    task->user_stack_size = 0;

    uint64_t* stk = (uint64_t*)task->kernel_stack_top;

    *(--stk) = GDT_KERNEL_DATA;          // SS      = 0x10
    *(--stk) = task->kernel_stack_top;   // RSP     = stack tepesi (referans)
    *(--stk) = 0x202ULL;                 // RFLAGS  = IF=1
    *(--stk) = GDT_KERNEL_CODE;          // CS      = 0x08
    *(--stk) = (uint64_t)entry_point;    // RIP     = entry

    for (int i = 0; i < 15; i++) *(--stk) = 0;

    task->context.rsp    = (uint64_t)stk;
    task->context.rip    = (uint64_t)entry_point;
    task->context.rflags = 0x202;
    task->context.cs     = GDT_KERNEL_CODE;
    task->context.ss     = GDT_KERNEL_DATA;
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
        serial_print(" (size=");
        int_to_str((int)(KERNEL_STACK_SIZE / 1024), buf);
        serial_print(buf);
        serial_print(" KB), ctx.rsp=0x");
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

    task->pgid = task->pid;
    task->sid  = task->pid;

    signal_table_init(&task->signal_table);
    task->signal_trampoline = 0;

    fd_table_init(task->fd_table);

    // ── Kernel Stack ──────────────────────────────────────────────
    task->kernel_stack_size = KERNEL_STACK_SIZE;
    {
        uint64_t page_count = KERNEL_STACK_SIZE / 4096;
        void* ks = pmm_alloc_pages(page_count);
        if (!ks) {
            serial_print("[TASK WARN] pmm_alloc_pages failed for kernel stack, trying kmalloc\n");
            ks = kmalloc(KERNEL_STACK_SIZE);
            if (!ks) {
                kfree(task);
                return NULL;
            }
        }
        task->kernel_stack_base = (uint64_t)ks;
    }
    task->kernel_stack_top = task->kernel_stack_base + KERNEL_STACK_SIZE;

    // ── User Stack ────────────────────────────────────────────────
    task->user_stack_size = USER_STACK_SIZE;
    {
        uint64_t page_count = USER_STACK_SIZE / 4096;
        // 0x7 = PAGE_PRESENT | PAGE_WRITE | PAGE_USER
        void* us = pmm_alloc_pages_flags(page_count, 0x7);
        if (!us) {
            serial_print("[TASK WARN] pmm_alloc_pages_flags failed for user stack, trying kmalloc\n");
            us = kmalloc(USER_STACK_SIZE);
            if (!us) {
                pmm_free_pages((void*)task->kernel_stack_base, KERNEL_STACK_SIZE / 4096);
                kfree(task);
                return NULL;
            }
        }
        task->user_stack_base = (uint64_t)us;
    }
    task->user_stack_top = task->user_stack_base + USER_STACK_SIZE;

    uint64_t* stk = (uint64_t*)task->kernel_stack_top;

    *(--stk) = GDT_USER_DATA_RPL3;      // SS
    *(--stk) = task->user_stack_top;    // RSP (user stack)
    *(--stk) = 0x202ULL;                // RFLAGS = IF=1
    *(--stk) = GDT_USER_CODE_RPL3;      // CS = 0x23
    *(--stk) = (uint64_t)entry_point;   // RIP

    for (int i = 0; i < 15; i++) *(--stk) = 0;

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
        serial_print(" (");
        int_to_str((int)(KERNEL_STACK_SIZE / 1024), buf);
        serial_print(buf);
        serial_print(" KB)  user_stack_top=0x");
        uint64_to_string(task->user_stack_top, buf);
        serial_print(buf);
        serial_print(" (");
        int_to_str((int)(USER_STACK_SIZE / 1024), buf);
        serial_print(buf);
        serial_print(" KB)\n");
        serial_print("[TASK]   CS=0x23 SS=0x1B (Ring-3 selectors)\n");
    }

    return task;
}

// ===========================================
// ELF'TEN USER TASK OLUSTURMA (Ring-3)
// ===========================================
//
// SysV AMD64 ABI — user stack düzeni (task çalışmadan önce):
//
//   [rsp + 0]         argc
//   [rsp + 8]         argv[0]   ← program adı pointer'ı
//   [rsp + 16]        argv[1]   ← 1. argüman pointer'ı (varsa)
//   ...
//   [rsp + (n+1)*8]   NULL      ← argv sonu
//   [rsp + (n+2)*8]   NULL      ← envp sonu
//   ...               string verileri (argv stringleri burada)
//
// argc=0 geçilirse kilo "Usage: kilo <filename>" basıp çıkar.
// argv[0]="KILO.ELF", argv[1]="dosya.txt" şeklinde geçin.
// ===========================================

task_t* task_create_from_elf(const char* name,
                              const ElfImage* img,
                              uint32_t priority,
                              int argc,
                              const char** argv)
{
    if (!name || !img) {
        serial_print("[TASK_ELF] NULL argument!\n");
        return NULL;
    }
    if (!task_system_initialized) {
        serial_print("[TASK_ELF] Task system not initialized!\n");
        return NULL;
    }

    serial_print("[TASK_ELF] Creating ELF task '");
    serial_print(name);
    serial_print("' entry=0x");
    char buf[24];
    uint64_to_string(img->entry, buf);
    serial_print(buf);
    serial_print(" argc=");
    int_to_str(argc, buf);
    serial_print(buf);
    serial_print("\n");

    // ── 1. TCB + stack'leri task_create_user ile oluştur ────────
    // ELF entry'yi void(*)(void) olarak cast ediyoruz.
    // iretq frame'i aşağıda düzeltileceğinden geçici değer.
    void (*elf_entry)(void) = (void(*)(void))img->entry;
    task_t* task = task_create_user(name, elf_entry, priority);
    if (!task) {
        serial_print("[TASK_ELF] task_create_user failed!\n");
        return NULL;
    }

    // ── 2. SysV ABI: argv stringlerini + pointer tablosunu user stack'a yaz ──
    //
    // SysV AMD64 ABI _start çağrı kuralı:
    //   _start girişinde RSP'nin kendisi 16-byte hizalı OLMALI.
    //   (Normal fonksiyonlarda call 8 byte push eder, fonksiyon girişinde
    //    RSP = 16n+8 olur. _start ise doğrudan iretq ile başlar, push yok;
    //    bu yüzden RSP'nin 16-byte hizalı olması gerekir.)
    //
    // Stack düzeni (yüksek → alçak, RSP burayı gösterir):
    //   argc          (8 byte)
    //   argv[0]       (8 byte pointer)
    //   ...
    //   argv[n-1]     (8 byte pointer)
    //   NULL          (argv sonu)
    //   NULL          (envp sonu)
    //   ...string verileri...
    //
    // argc + (argc+1) pointer + 1 NULL envp = (argc + 2) adet 8-byte slot
    // Toplam pointer alanı: (argc + 2) * 8 byte
    // RSP = 16-byte hizalı olmalı:
    //   (argc + 2) çift ise RSP zaten hizalı
    //   (argc + 2) tek  ise 8 byte padding ekle

    #define MAX_ARGV 8
    #define MAX_ARG_LEN 128

    uint64_t sp = task->user_stack_top;

    // Stringleri stack'e kopyala (tepeden aşağı — son argüman önce)
    uint64_t str_ptrs[MAX_ARGV];
    int real_argc = (argc > MAX_ARGV) ? MAX_ARGV : argc;

    for (int i = real_argc - 1; i >= 0; i--) {
        const char* arg = argv[i];
        int len = str_len_local(arg) + 1;
        sp -= (uint64_t)len;
        memcpy_task((void*)sp, arg, (uint64_t)len);
        str_ptrs[i] = sp;
        serial_print("[TASK_ELF]   argv[");
        int_to_str(i, buf); serial_print(buf);
        serial_print("]=\""); serial_print(arg);
        serial_print("\" @ 0x"); uint64_to_string(sp, buf); serial_print(buf);
        serial_print("\n");
    }

    // String bölümünü 8-byte hizala (pointer tablosu 8-byte hizalı başlamalı)
    sp &= ~(uint64_t)0x7;

    // Pointer alanı kaç slot:
    //   argv[0..argc-1]  → real_argc slot
    //   NULL (argv sonu) → 1 slot
    //   NULL (envp sonu) → 1 slot
    //   argc             → 1 slot
    //   Toplam: (real_argc + 3) slot × 8 byte
    //
    // _start girişinde RSP 16-byte hizalı olmalı.
    // (real_argc + 3) tek ise 1 ekstra NULL padding ekle (8 byte).
    int total_slots = real_argc + 3;  // argc + argv[] + NULL + NULL
    if (total_slots % 2 != 0) {
        sp -= 8;
        *((uint64_t*)sp) = 0;   // padding
    }

    // envp sonu = NULL
    sp -= 8;
    *((uint64_t*)sp) = 0;

    // argv sonu = NULL
    sp -= 8;
    *((uint64_t*)sp) = 0;

    // argv pointer'larını yaz (sondan başa)
    for (int i = real_argc - 1; i >= 0; i--) {
        sp -= 8;
        *((uint64_t*)sp) = str_ptrs[i];
    }

    // argc
    sp -= 8;
    *((uint64_t*)sp) = (uint64_t)real_argc;

    // Debug: hizalamayı doğrula
    {
        serial_print("[TASK_ELF] Stack alignment check: sp=0x");
        uint64_to_string(sp, buf); serial_print(buf);
        serial_print(" mod16=");
        int_to_str((int)(sp & 0xF), buf); serial_print(buf);
        serial_print(" (should be 0)\n");
    }

    // ── 3. Kernel stack'teki iretq frame'ini ELF için düzelt ────
    //
    // task_create_user'ın oluşturduğu kernel stack düzeni
    // (yüksekten alçağa, context.rsp = en alt — 15 register'ın başı):
    //
    //   [kernel_stack_top - 8]   SS      (0x1B)
    //   [kernel_stack_top - 16]  RSP     (user_stack_top)   ← argv RSP ile değiştir
    //   [kernel_stack_top - 24]  RFLAGS  (0x202)
    //   [kernel_stack_top - 32]  CS      (0x23)
    //   [kernel_stack_top - 40]  RIP     (entry_point)      ← ELF entry ile değiştir
    //   [context.rsp + 14*8]     r15     (0)
    //   ...
    //   [context.rsp + 0]        rax     (0)

    uint64_t* frame_rip = (uint64_t*)(task->context.rsp + 15 * 8);
    uint64_t* frame_cs  = frame_rip + 1;
    uint64_t* frame_rfl = frame_rip + 2;
    uint64_t* frame_rsp = frame_rip + 3;
    uint64_t* frame_ss  = frame_rip + 4;

    *frame_rip = img->entry;            // ELF entry point

    // ELF yükleme aralığını kaydet — mmap pool çakışma koruması için
    task->elf_load_min = img->load_min;
    task->elf_load_max = img->load_max;
    *frame_cs  = GDT_USER_CODE_RPL3;   // 0x23 — Ring-3 code
    *frame_rfl = 0x202;                 // IF=1
    *frame_rsp = sp;                    // argv kurulmuş RSP
    *frame_ss  = GDT_USER_DATA_RPL3;   // 0x1B — Ring-3 data

    // ── 4. cpu_context güncelle ──────────────────────────────────
    // CRITICAL: context.rsp DEĞİŞMEZ — kernel stack frame pointer'ı!
    task->context.rip    = img->entry;
    task->context.rflags = 0x202;
    task->context.cs     = GDT_USER_CODE_RPL3;
    task->context.ss     = GDT_USER_DATA_RPL3;

    // SysV ABI: _start beklentisi — rdi=argc, rsi=argv, rdx=envp
    // (musl crt0 bunları okur)
    task->context.rdi = (uint64_t)real_argc;
    task->context.rsi = sp + 8;          // argv pointer dizisinin başı
    task->context.rdx = sp + 8 + (uint64_t)(real_argc + 1) * 8; // envp

    serial_print("[TASK_ELF] Task '");
    serial_print(name);
    serial_print("' ready. PID=");
    int_to_str((int)task->pid, buf);
    serial_print(buf);
    serial_print(" user_rsp=0x");
    uint64_to_string(sp, buf);
    serial_print(buf);
    serial_print(" argc=");
    int_to_str(real_argc, buf);
    serial_print(buf);
    serial_print("\n");

    #undef MAX_ARGV
    #undef MAX_ARG_LEN

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
    task->state = TASK_STATE_TERMINATED;

    // Stack'leri serbest bırak
    if (task->kernel_stack_base)
        pmm_free_pages((void*)task->kernel_stack_base, task->kernel_stack_size / 4096);
    if (task->user_stack_base)
        pmm_free_pages((void*)task->user_stack_base, task->user_stack_size / 4096);

    kfree(task);
}

void task_exit(void) {
    if (!current_task || current_task == idle_task) {
        serial_print("[TASK ERROR] Cannot exit idle task!\n");
        return;
    }
    serial_print("[TASK] '");
    serial_print(current_task->name);
    serial_print("' exiting (PID=");
    char _b[12]; int_to_str((int)current_task->pid, _b); serial_print(_b);
    serial_print(")\n");

    // ── Foreground temizle; shell'i geri getir ────────────────
    if (foreground_pid != 0 && foreground_pid == current_task->pid) {
        foreground_pid = 0;
        serial_print("[TASK] foreground_pid cleared, shell restored\n");
        // Klavyeyi shell moduna döndür ve prompt'u yeniden göster
        extern void kb_set_userland_mode(int on);
        kb_set_userland_mode(0);
        shell_restore_prompt();
    }

    // ── ZOMBIE yap: parent waitpid() toplayana kadar TCB'yi koru ─
    // TERMINATED yerine ZOMBIE kullanıyoruz. task_find_by_pid()
    // zombie listesine de baktığından sys_waitpid() görebilir.
    task_t* dying = current_task;
    dying->state = TASK_STATE_ZOMBIE;
    zombie_list_add(dying);

    // scheduler_tick()'in orphan zombie temizliği yapabilmesi için
    // scheduler'ın previous_task'ını da güncelle.
    extern task_t* previous_task;
    previous_task = dying;

    task_t* next = task_get_next();
    if (!next) next = idle_task;
    task_exit_previous = dying;
    current_task  = next;
    next->state   = TASK_STATE_RUNNING;
    next->last_run_time = get_system_ticks();

    tss_set_kernel_stack(next->kernel_stack_top);
    task_load_and_jump_context(&next->context);
    serial_print("[TASK ERROR] task_exit returned!\n");
    while(1) __asm__ volatile("hlt");
}

void task_set_state(task_t* task, uint32_t new_state) {
    if (task) task->state = new_state;
}

// Zombie task'ı listeden çıkar ve kaynaklarını serbest bırak.
// sys_waitpid() child'ı topladıktan sonra çağırır.
void task_reap_zombie(task_t* task) {
    if (!task) return;
    zombie_list_remove(task);

    // Kernel stack: pmm_alloc_pages() ile ayrıldı (sys_fork + task_create_user).
    if (task->kernel_stack_base)
        pmm_free_pages((void*)task->kernel_stack_base,
                       task->kernel_stack_size / 4096);

    // User stack: pmm_alloc_pages_flags() ile ayrıldı.
    if (task->user_stack_base)
        pmm_free_pages((void*)task->user_stack_base,
                       task->user_stack_size / 4096);

    kfree(task);
}

// ── signal64.c için: SIGSTOP/SIGCONT desteği ─────────────────
void task_set_stopped(task_t* t, int stopped) {
    if (!t) return;
    if (stopped) {
        if (t->state == TASK_STATE_RUNNING ||
            t->state == TASK_STATE_READY) {
            t->state = TASK_STATE_STOPPED;
            serial_print("[TASK] Stopped (SIGSTOP): ");
            serial_print(t->name);
            serial_print("\n");
        }
    } else {
        if (t->state == TASK_STATE_STOPPED) {
            t->state = TASK_STATE_READY;
            task_queue_push(&ready_queue, t);
            serial_print("[TASK] Continued (SIGCONT): ");
            serial_print(t->name);
            serial_print("\n");
        }
    }
}

// ── Sleep Yönetimi ────────────────────────────────────────────

void task_sleep(task_t* task, uint64_t ticks) {
    if (!task) return;
    task->wake_tick = get_system_ticks() + ticks;
    task->state     = TASK_STATE_SLEEPING;
}

void task_wakeup_check(void) {
    uint64_t now = get_system_ticks();
    // Ready queue'da sleeping task kontrol et
    task_t* t = ready_queue.head;
    while (t) {
        task_t* next = t->next;
        if (t->state == TASK_STATE_SLEEPING && t->wake_tick <= now) {
            t->state = TASK_STATE_READY;
        }
        t = next;
    }
}

// ============================================================
// Process Group & Session Yönetimi  (v12)
// ============================================================

int task_set_pgid(task_t* t, uint32_t new_pgid) {
    if (!t) return -1;
    t->pgid = new_pgid;
    return 0;
}

int task_do_setsid(task_t* t) {
    if (!t) return -1;
    if (t->pgid == t->pid) return -1;
    t->sid  = t->pid;
    t->pgid = t->pid;
    return (int)t->sid;
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
    return next;
}

uint32_t task_get_count(void) { return ready_queue.count; }

task_t* task_find_by_pid(uint32_t pid) {
    if (current_task && current_task->pid == pid) return current_task;
    task_t* t = ready_queue.head;
    while (t) { if (t->pid == pid) return t; t = t->next; }
    if (idle_task && idle_task->pid == pid) return idle_task;
    // Zombie listesine de bak — waitpid() toplayana kadar burada durur
    t = zombie_list_head;
    while (t) { if (t->pid == pid) return t; t = t->next; }
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

// task_save_fs_base: mevcut task'ın FS.base değerini task->fs_base'e kaydet.
// İSR (timer interrupt) öncesi scheduler tarafından çağrılabilir.
void task_save_fs_base(void) {
    if (!current_task || current_task == idle_task) return;
    uint32_t _lo = 0, _hi = 0;
    __asm__ volatile (
        "rdmsr"
        : "=a"(_lo), "=d"(_hi)
        : "c"(0xC0000100u)   /* MSR_FS_BASE */
    );
    current_task->fs_base = ((uint64_t)_hi << 32) | _lo;
}

// task_restore_fs_base: mevcut task'ın fs_base değerini MSR'a yaz.
// isr_timer iretq öncesi çağrılır — Ring-3'e dönüşte FS_BASE sıfır kalmasın.
void task_restore_fs_base(void) {
    if (!current_task || !current_task->fs_base) return;
    uint32_t _lo = (uint32_t)(current_task->fs_base & 0xFFFFFFFFu);
    uint32_t _hi = (uint32_t)(current_task->fs_base >> 32);
    __asm__ volatile (
        "wrmsr"
        : : "c"(0xC0000100u), "a"(_lo), "d"(_hi)
    );
}

void task_switch(task_t* from, task_t* to) {
    if (!to) {
        serial_print("[TASK ERROR] Cannot switch to NULL task!\n");
        return;
    }

    // Context switch öncesi mevcut task'ın FS.base değerini kaydet.
    // mov fs, 0 CPU'nun MSR_FS_BASE'i sıfırlamasına yol açar; bir sonraki
    // syscall girişinde syscall_dispatch bu değeri wrmsr ile restore eder.
    if (from && from != idle_task) {
        uint32_t _lo = 0, _hi = 0;
        __asm__ volatile (
            "rdmsr"
            : "=a"(_lo), "=d"(_hi)
            : "c"(0xC0000100u)    /* MSR_FS_BASE */
        );
        from->fs_base = ((uint64_t)_hi << 32) | _lo;
    }

    tss_set_kernel_stack(to->kernel_stack_top);

    current_task = to;
    to->state    = TASK_STATE_RUNNING;
    to->last_run_time  = get_system_ticks();
    to->context_switches++;

    // ── İlk çalışma kararı ───────────────────────────────────────────────
    //
    // task_switch_context çalışma prensibi:
    //   context.rip → kernel stack'e push → ret → oraya atlar.
    // Bu mekanizma sadece KERNEL adresleri için geçerli: Ring-0'dan
    // Ring-0'a ret yeterli. Ring-3'e geçmek için iretq şarttır.
    //
    // Ring-3 task'ın ilk çalışmasında (context_switches yeni 1 oldu):
    //   context.rsp → 15 GPR slot + iretq frame ile kurulu.
    //   task_load_and_jump_context: pop×15 → iretq → Ring-3. DOĞRU ✓
    //   task_switch_context: ret → context.rip → Ring-0'dan user-space → #GP. YANLIŞ ✗
    //
    // Kural: from=NULL/idle VEYA hedef Ring-3 task ilk kez çalışıyorsa
    // task_load_and_jump_context kullan.
    //
    // context_switches zaten arttırıldı; yeni task → değer == 1.
    int first_run_user = (to->context_switches == 1 &&
                          to->privilege_level == TASK_PRIVILEGE_USER);

    if (!from || from == idle_task || first_run_user) {
        task_load_and_jump_context(&to->context);
        return;
    }

    task_switch_context(&from->context, &to->context);
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
    const char* msg = "[USER TASK] Hello from Ring-3 via SYSRET!\n";
    uint64_t ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"((uint64_t)8),         // SYS_DEBUG
          "D"((uint64_t)msg)
        : "rcx", "r11", "memory"
    );

    uint64_t pid;
    __asm__ volatile (
        "syscall"
        : "=a"(pid)
        : "a"((uint64_t)4)          // SYS_GETPID
        : "rcx", "r11"
    );
    (void)pid;

    __asm__ volatile (
        "syscall"
        :
        : "a"((uint64_t)3),         // SYS_EXIT
          "D"((uint64_t)0)
        : "rcx", "r11"
    );

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

const char* task_state_name(uint32_t state) {
    switch (state) {
        case TASK_STATE_READY:      return "READY";
        case TASK_STATE_RUNNING:    return "RUNNING";
        case TASK_STATE_BLOCKED:    return "BLOCKED";
        case TASK_STATE_TERMINATED: return "TERMINATED";
        case TASK_STATE_SLEEPING:   return "SLEEPING";
        case TASK_STATE_ZOMBIE:     return "ZOMBIE";
        case TASK_STATE_STOPPED:    return "STOPPED";
        default:                    return "UNKNOWN";
    }
}