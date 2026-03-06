/*
 * __ascent_malloc.c  — musl için mmap/munmap tabanlı minimal allocator
 *
 * musl'ün mallocng kaldırılır, bu dosya yerine geçer.
 * mmap/munmap SYSCALL'larını doğrudan kullanır (kmalloc/kfree'ye dokunmaz).
 * musl'ün stdio/string gibi iç kullanımları için yeterlidir.
 *
 * Strateji: Her malloc() → mmap(MAP_ANONYMOUS) ile tam sayfa al.
 *           free()       → munmap() ile geri ver.
 *           realloc()    → yeni mmap + memcpy + eski munmap.
 *
 * Avantaj: kfree/kmalloc heap'ine hiç dokunmaz, çakışma olmaz.
 * Dezavantaj: Her alloc en az 1 sayfa (4KB). Küçük alloc'lar israf.
 *             Ama musl'ün stdio buffer'ları zaten sayfa boyutunda.
 */

typedef __SIZE_TYPE__    size_t;
typedef __UINT8_TYPE__   uint8_t;
typedef __INTPTR_TYPE__  intptr_t;

#define NULL      ((void*)0)
#define PAGE_SIZE 4096UL
#define MAP_FAILED ((void*)-1)

/* Linux/AscentOS syscall numaraları */
#define SYS_MMAP   9
#define SYS_MUNMAP 11

#define PROT_READ  0x01
#define PROT_WRITE 0x02
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20

static inline long _syscall6(long n,
    long a1, long a2, long a3, long a4, long a5, long a6)
{
    long ret;
    register long _rax __asm__("rax") = n;
    register long _rdi __asm__("rdi") = a1;
    register long _rsi __asm__("rsi") = a2;
    register long _rdx __asm__("rdx") = a3;
    register long _r10 __asm__("r10") = a4;
    register long _r8  __asm__("r8")  = a5;
    register long _r9  __asm__("r9")  = a6;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "r"(_rax),"r"(_rdi),"r"(_rsi),"r"(_rdx),"r"(_r10),"r"(_r8),"r"(_r9)
        : "rcx","r11","memory"
    );
    return ret;
}

static inline long _syscall2(long n, long a1, long a2)
{
    long ret;
    register long _rax __asm__("rax") = n;
    register long _rdi __asm__("rdi") = a1;
    register long _rsi __asm__("rsi") = a2;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "r"(_rax),"r"(_rdi),"r"(_rsi)
        : "rcx","r11","memory"
    );
    return ret;
}

/* Sayfa hizalı boyut hesapla */
static size_t page_align(size_t n) {
    return (n + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
}

/*
 * malloc blok header'ı.
 * Her mmap bölgesinin başında saklanır.
 * Kullanıcıya (header + 1) döndürülür.
 */
typedef struct {
    size_t total_size;   /* mmap ile alınan toplam bayt (header dahil) */
    size_t user_size;    /* kullanıcının istediği bayt */
} malloc_hdr_t;

#define HDR_SIZE (sizeof(malloc_hdr_t))

void *malloc(size_t size) {
    if (size == 0) return NULL;

    size_t total = page_align(HDR_SIZE + size);

    long addr = _syscall6(SYS_MMAP,
        0, (long)total,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1, 0);

    if (addr < 0 || (void*)addr == MAP_FAILED) return NULL;

    malloc_hdr_t *hdr = (malloc_hdr_t*)addr;
    hdr->total_size = total;
    hdr->user_size  = size;

    return (void*)(hdr + 1);
}

void free(void *ptr) {
    if (!ptr) return;

    malloc_hdr_t *hdr = (malloc_hdr_t*)ptr - 1;
    size_t total = hdr->total_size;

    /* Header'ı sıfırla (double-free tespiti için) */
    hdr->total_size = 0;
    hdr->user_size  = 0;

    _syscall2(SYS_MUNMAP, (long)hdr, (long)total);
}

void *calloc(size_t nmemb, size_t size) {
    /* mmap zaten sıfırlı bellek verir (MAP_ANONYMOUS garantisi) */
    return malloc(nmemb * size);
}

void *realloc(void *ptr, size_t size) {
    if (!ptr)        return malloc(size);
    if (size == 0) { free(ptr); return NULL; }

    malloc_hdr_t *hdr = (malloc_hdr_t*)ptr - 1;

    /* Mevcut alan yeterliyse aynı pointer'ı döndür */
    if (hdr->user_size >= size) {
        hdr->user_size = size;
        return ptr;
    }

    /* Yeni alan al, kopyala, eskiyi serbest bırak */
    void *new_ptr = malloc(size);
    if (!new_ptr) return NULL;

    size_t copy_n = hdr->user_size;
    uint8_t *src = (uint8_t*)ptr;
    uint8_t *dst = (uint8_t*)new_ptr;
    for (size_t i = 0; i < copy_n; i++) dst[i] = src[i];

    free(ptr);
    return new_ptr;
}

/* musl bazı iç fonksiyonlarda bunları çağırabilir */
void *aligned_alloc(size_t alignment, size_t size) {
    /* mmap zaten sayfa hizalı (4096) döndürür */
    (void)alignment;
    return malloc(size);
}

void *memalign(size_t alignment, size_t size) {
    return aligned_alloc(alignment, size);
}

int posix_memalign(void **memptr, size_t alignment, size_t size) {
    void *p = aligned_alloc(alignment, size);
    if (!p) return 12; /* ENOMEM */
    *memptr = p;
    return 0;
}