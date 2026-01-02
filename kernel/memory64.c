// 64-bit Memory Manager

#include <stdint.h>
#include <stddef.h>

extern void println64(const char* str, uint8_t color);
extern void print_str64(const char* str, uint8_t color);

#define VGA_WHITE 0x0F
#define VGA_GREEN 0x0A
#define VGA_YELLOW 0x0E

// Sayfa boyutu
#define PAGE_SIZE 4096

// Bellek blok yapısı
typedef struct memory_block {
    uint64_t address;
    uint64_t size;
    int is_free;
    struct memory_block* next;
} memory_block_t;

// Basit heap başlangıcı (kernel sonrası)
uint8_t* heap_start = (uint8_t*)0x200000;  // 2MB'den başla
uint8_t* heap_current = (uint8_t*)0x200000;

static memory_block_t* first_block = NULL;

// Heap'ten bellek ayır
void* kmalloc(size_t size) {
    if (size == 0) return NULL;
    
    // 8 byte hizalama
    size = (size + 7) & ~7;
    
    // İlk ayırma
    if (first_block == NULL) {
        first_block = (memory_block_t*)heap_current;
        first_block->address = (uint64_t)heap_current + sizeof(memory_block_t);
        first_block->size = size;
        first_block->is_free = 0;
        first_block->next = NULL;
        
        heap_current += sizeof(memory_block_t) + size;
        return (void*)first_block->address;
    }
    
    // Boş blok ara
    memory_block_t* block = first_block;
    memory_block_t* prev = NULL;
    
    while (block != NULL) {
        if (block->is_free && block->size >= size) {
            block->is_free = 0;
            return (void*)block->address;
        }
        prev = block;
        block = block->next;
    }
    
    // Yeni blok oluştur
    memory_block_t* new_block = (memory_block_t*)heap_current;
    new_block->address = (uint64_t)heap_current + sizeof(memory_block_t);
    new_block->size = size;
    new_block->is_free = 0;
    new_block->next = NULL;
    
    if (prev) prev->next = new_block;
    
    heap_current += sizeof(memory_block_t) + size;
    
    return (void*)new_block->address;
}

// Belleği serbest bırak
void kfree(void* ptr) {
    if (ptr == NULL) return;
    
    memory_block_t* block = first_block;
    
    while (block != NULL) {
        if (block->address == (uint64_t)ptr) {
            block->is_free = 1;
            return;
        }
        block = block->next;
    }
}

// Sayfa tablosu yapısı
typedef struct {
    uint64_t entries[512];
} page_table_t __attribute__((aligned(4096)));

static page_table_t* pml4 = NULL;

// Fiziksel adres -> Sanal adres mapping
void* map_page(uint64_t physical, uint64_t virtual __attribute__((unused))) {
    // Basitleştirilmiş - sadece identity mapping
    return (void*)physical;
}

// Bellek bilgisini göster
void show_memory_info(void) {
    uint64_t total_allocated = 0;
    uint64_t total_free = 0;
    int block_count = 0;
    
    memory_block_t* block = first_block;
    while (block != NULL) {
        block_count++;
        if (block->is_free) {
            total_free += block->size;
        } else {
            total_allocated += block->size;
        }
        block = block->next;
    }
    
    print_str64("Memory Statistics:\n", VGA_YELLOW);
    print_str64("  Total blocks: ", VGA_WHITE);
    
    // Sayı yazdır (basit)
    char num_str[32];
    int i = 0;
    int temp = block_count;
    if (temp == 0) {
        num_str[i++] = '0';
    } else {
        while (temp > 0) {
            num_str[i++] = '0' + (temp % 10);
            temp /= 10;
        }
    }
    num_str[i] = '\0';
    
    // Ters çevir
    for (int j = 0; j < i / 2; j++) {
        char c = num_str[j];
        num_str[j] = num_str[i - j - 1];
        num_str[i - j - 1] = c;
    }
    
    println64(num_str, VGA_GREEN);
}

// Memory manager başlat
void init_memory64(void) {
    heap_current = heap_start;
    first_block = NULL;
    
    // Basit test
    void* test1 = kmalloc(64);
    kfree(test1);
    (void)test1; // Suppress warning
    
    // PML4'ü al (CR3'ten)
    __asm__ volatile ("mov %%cr3, %0" : "=r"(pml4));
}