#ifndef CPU64_H
#define CPU64_H

#include <stdint.h>

// I/O Port Access
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// CPU Feature Flags 
#define CPU_FEAT_FPU   (1 << 0)   
#define CPU_FEAT_TSC   (1 << 1)  
#define CPU_FEAT_PAE   (1 << 2) 
#define CPU_FEAT_MMX   (1 << 3)   
#define CPU_FEAT_SSE   (1 << 4)  
#define CPU_FEAT_SSE2  (1 << 5)   
#define CPU_FEAT_SSE3  (1 << 6)   
#define CPU_FEAT_SSSE3 (1 << 7)   
#define CPU_FEAT_SSE41 (1 << 8)   
#define CPU_FEAT_SSE42 (1 << 9)   
#define CPU_FEAT_AVX   (1 << 10)  
#define CPU_FEAT_AES   (1 << 11)  
#define CPU_FEAT_RDRAND (1 << 12) 
#define CPU_FEAT_LONG  (1 << 13)  

// Cache Info
typedef struct {
    uint32_t l1d_kb;   
    uint32_t l1i_kb;    
    uint32_t l2_kb;     
    uint32_t l3_kb;    
} CacheInfo;

// CPU Model Info
typedef struct {
    uint8_t  stepping;   
    uint8_t  model;     
    uint16_t family;     
    uint8_t  cpu_type;  
} CPUStepping;

// Function declarations
void     sse_init(void);
void     get_cpu_info(char* vendor_out);   
void     uint64_to_hex(uint64_t n, char* buf);  
uint64_t cpu_get_cr2(void);                  
uint32_t cpu_get_features(void);              
void     cpu_get_model_name(char* out);      
void     cpu_get_cache_info(CacheInfo* out);   
uint32_t cpu_get_freq_estimate(void);       
void     cpu_get_stepping(CPUStepping* out);    


static inline void cpu_enable_interrupts(void) {
    __asm__ volatile ("sti" ::: "memory");
}

static inline void cpu_disable_interrupts(void) {
    __asm__ volatile ("cli" ::: "memory");
}

static inline uint64_t cpu_save_flags(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags) :: "memory");
    __asm__ volatile ("cli" ::: "memory");
    return flags;
}

static inline void cpu_restore_flags(uint64_t flags) {
    __asm__ volatile ("pushq %0; popfq" :: "r"(flags) : "memory", "cc");
}

// TLB Management
static inline void cpu_invlpg(uint64_t virtual_addr) {
    __asm__ volatile ("invlpg (%0)" :: "r"(virtual_addr) : "memory");
}


static inline void cpu_relax(void) {
    __asm__ volatile ("pause" ::: "memory");
}

//  TSC (Time Stamp Counter)
static inline uint64_t cpu_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}


// Performance Calculation Structure
typedef struct {
    uint64_t start_tsc; 
    uint64_t end_tsc;    
    uint64_t elapsed;     
    uint32_t cpu_mhz;      
} PerfCounter;

// Performance Calculation Tests
void     perf_start(PerfCounter* pc);            
void     perf_stop(PerfCounter* pc);             
uint64_t perf_cycles(const PerfCounter* pc);   
uint64_t perf_ns(const PerfCounter* pc);         
uint32_t perf_us(const PerfCounter* pc);         
void     perf_print(const PerfCounter* pc,       
const char* label);
#endif 