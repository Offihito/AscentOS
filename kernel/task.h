// task.h - Task Management System for AscentOS 64-bit
// PHASE 2: WITH USERMODE SUPPORT
#ifndef TASK_H
#define TASK_H

#include <stdint.h>

// Task states
#define TASK_STATE_READY      0  // Çalışmaya hazır
#define TASK_STATE_RUNNING    1  // Şu anda çalışıyor
#define TASK_STATE_BLOCKED    2  // I/O bekliyor
#define TASK_STATE_TERMINATED 3  // Sonlandı

// Task privilege levels
#define TASK_PRIVILEGE_KERNEL   0  // Ring 0 (kernel mode)
#define TASK_PRIVILEGE_USER     3  // Ring 3 (user mode)

// Default stack sizes
#define KERNEL_STACK_SIZE  0x4000  // 16KB kernel stack
#define USER_STACK_SIZE    0x4000  // 16KB user stack

// CPU Context - Tüm register'ları saklar
typedef struct {
    // Genel amaçlı register'lar
    uint64_t rax, rbx, rcx, rdx;
    uint64_t rsi, rdi, rbp, rsp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    
    // Program counter ve flags
    uint64_t rip;
    uint64_t rflags;
    
    // Segment register'ları
    uint64_t cs, ss, ds, es, fs, gs;
    
    // Page table (her task kendi adres alanına sahip olabilir)
    uint64_t cr3;
} cpu_context_t;

// Task Control Block (TCB)
typedef struct task {
    // Task kimliği
    uint32_t pid;                    // Process ID
    char name[32];                   // Task adı (debug için)
    
    // Task durumu
    uint32_t state;                  // TASK_STATE_*
    uint32_t priority;               // Öncelik (0-255, yüksek = önemli)
    uint32_t privilege_level;        // Ring 0 (kernel) or Ring 3 (user)
    
    // CPU context
    cpu_context_t context;           // Kaydedilmiş CPU durumu
    
    // Stack bilgileri
    uint64_t kernel_stack_base;      // Kernel stack başlangıcı
    uint64_t kernel_stack_size;      // Kernel stack boyutu
    uint64_t user_stack_base;        // User stack başlangıcı (Ring 3 için)
    uint64_t user_stack_size;        // User stack boyutu
    
    // Zamanlama bilgileri
    uint64_t time_slice;             // Bu task'a ayrılan zaman dilimi (ticks)
    uint64_t time_used;              // Kullanılan CPU zamanı
    uint64_t last_run_time;          // Son çalıştırıldığı zaman (system ticks)
    
    // Linked list için
    struct task* next;               // Sıradaki task
    struct task* prev;               // Önceki task (çift bağlı liste)
    
    // Debug/istatistik
    uint64_t context_switches;       // Kaç kez context switch yapıldı
    uint64_t total_runtime;          // Toplam çalışma süresi
} task_t;

// Task Queue - Görev kuyruğu
typedef struct {
    task_t* head;                    // İlk task
    task_t* tail;                    // Son task
    uint32_t count;                  // Queue'daki task sayısı
} task_queue_t;

// ===========================================
// TASK MANAGEMENT FUNCTIONS
// ===========================================

// Task sistemi başlatma
void task_init(void);

// Yeni kernel task oluştur (Ring 0)
task_t* task_create(const char* name, void (*entry_point)(void), uint32_t priority);

// Yeni usermode task oluştur (Ring 3) - NEW FOR PHASE 2
task_t* task_create_user(const char* name, void (*entry_point)(void), uint32_t priority);

// Task'ı başlat (queue'ya ekle)
int task_start(task_t* task);

// Task'ı sonlandır
void task_terminate(task_t* task);

// Mevcut task'ı sonlandır
void task_exit(void);

// Task durumunu değiştir
void task_set_state(task_t* task, uint32_t new_state);

// ===========================================
// TASK QUEUE OPERATIONS
// ===========================================

// Queue başlatma
void task_queue_init(task_queue_t* queue);

// Task'ı queue'ya ekle (sonuna)
void task_queue_push(task_queue_t* queue, task_t* task);

// Task'ı queue'dan çıkar (başından)
task_t* task_queue_pop(task_queue_t* queue);

// Task'ı queue'dan kaldır (herhangi bir yerden)
void task_queue_remove(task_queue_t* queue, task_t* task);

// Queue boş mu?
int task_queue_is_empty(task_queue_t* queue);

// ===========================================
// CURRENT TASK MANAGEMENT
// ===========================================

// Şu anda çalışan task'ı al
task_t* task_get_current(void);

// Bir sonraki task'ı seç (scheduler kullanır)
task_t* task_get_next(void);

// Task sayısını al
uint32_t task_get_count(void);

// PID ile task bul
task_t* task_find_by_pid(uint32_t pid);

// ===========================================
// CONTEXT SWITCHING
// ===========================================

// Context'i kaydet (assembly'de implement edilecek)
void task_save_context(cpu_context_t* context);

// Context'i yükle (assembly'de implement edilecek)
void task_load_context(cpu_context_t* context);

// İki task arasında geçiş yap
void task_switch(task_t* from, task_t* to);

// ===========================================
// USERMODE TRANSITION - NEW FOR PHASE 2
// ===========================================

// Jump to usermode (Ring 3) - implemented in assembly
extern void jump_to_usermode(uint64_t entry_point, uint64_t stack_pointer);

// ===========================================
// UTILITY FUNCTIONS
// ===========================================

// Task bilgilerini yazdır
void task_print_info(task_t* task);

// Tüm task'ları listele
void task_list_all(void);

// Task istatistikleri
void task_print_stats(void);

// ===========================================
// IDLE TASK
// ===========================================

// Idle task (hiçbir iş yokken çalışır)
void idle_task_entry(void);

// Idle task'ı oluştur
task_t* task_create_idle(void);

// ===========================================
// TEST TASKS
// ===========================================

// Demo task - Ekrana saniyede bir sayaç yazdırır
void demo_counter_task(void);

// Test için basit task fonksiyonları
void test_task_a(void);
void test_task_b(void);
void test_task_c(void);

// Offihito demo task
void offihito_task(void);

// PHASE 2: Usermode test tasks
void usermode_test_task(void);      // Simple usermode task
void usermode_syscall_task(void);   // Tests syscalls from usermode

#endif // TASK_H