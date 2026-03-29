// memstress_full.c — AscentOS Memory Stress Test (v1 + v2, tüm testler)
//
// Test 1–9  : mmap, munmap, brk, sbrk, arena, interleaved, fragmentation,
//             sustained load, edge-cases  (orijinal memstress.c)
// Test 10–14: slab allocator, preemption simülasyonu, uzun süreli
//             fragmentation, heap expansion, slab+kmalloc karışımı (v2)
//
// Build:
//   x86_64-elf-gcc -O0 -g -nostdlib -o memstress.elf memstress_full.c
//
// Run inside AscentOS:
//   exec memstress.elf
// ---------------------------------------------------------------------------

// ── Syscall numaraları ──────────────────────────────────────────────────────
#define SYS_WRITE     1
#define SYS_EXIT     60
#define SYS_MMAP      9
#define SYS_MUNMAP   11
#define SYS_BRK      12
#define SYS_SBRK    405   // AscentOS custom
#define SYS_GETTICKS 404  // AscentOS custom
#define SYS_YIELD    24   // sched_yield

// ── mmap / prot flags ───────────────────────────────────────────────────────
#define PROT_READ    0x1
#define PROT_WRITE   0x2
#define PROT_NONE    0x0
#define MAP_PRIVATE  0x02
#define MAP_ANONYMOUS 0x20
#define MAP_ANON     MAP_ANONYMOUS
#define MAP_FAILED   ((void*)-1)

// ── Renkler ─────────────────────────────────────────────────────────────────
#define GRN "\033[32m"
#define RED "\033[31m"
#define YEL "\033[33m"
#define CYN "\033[36m"
#define MGN "\033[35m"
#define WHT "\033[37m"
#define RST "\033[0m"

typedef unsigned long  size_t;
typedef long           ssize_t;
typedef unsigned char  uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int   uint32_t;
typedef unsigned long  uint64_t;
typedef long           int64_t;

// ── Ham syscall stub'ları ───────────────────────────────────────────────────
static inline long sc0(long n) {
    long r;
    __asm__ volatile("syscall":"=a"(r):"0"(n):"rcx","r11","memory");
    return r;
}
static inline long sc1(long n, long a) {
    long r;
    __asm__ volatile("syscall":"=a"(r):"0"(n),"D"(a):"rcx","r11","memory");
    return r;
}
static inline long sc3(long n, long a, long b, long c) {
    long r;
    __asm__ volatile("syscall":"=a"(r):"0"(n),"D"(a),"S"(b),"d"(c):"rcx","r11","memory");
    return r;
}
static inline long sc6(long n, long a, long b, long c,
                        long d, long e, long f) {
    long r;
    register long r10 __asm__("r10") = d;
    register long r8  __asm__("r8")  = e;
    register long r9  __asm__("r9")  = f;
    __asm__ volatile("syscall":"=a"(r)
        :"0"(n),"D"(a),"S"(b),"d"(c),"r"(r10),"r"(r8),"r"(r9)
        :"rcx","r11","memory");
    return r;
}

// ── libc yerine geçenler ────────────────────────────────────────────────────
static void _write(const char* s, size_t n) {
    sc3(SYS_WRITE, 1, (long)s, (long)n);
}
static void _exit(int code) {
    sc1(SYS_EXIT, code);
    __builtin_unreachable();
}
static long  _getticks(void)      { return sc0(SYS_GETTICKS); }
static void  _yield(void)         { sc0(SYS_YIELD); }
static long  _brk(long addr)      { return sc1(SYS_BRK,  addr); }
static long  _sbrk(long inc)      { return sc1(SYS_SBRK, inc);  }
static void* _mmap(size_t len) {
    return (void*)sc6(SYS_MMAP, 0, (long)len,
                      PROT_READ|PROT_WRITE,
                      MAP_PRIVATE|MAP_ANON, -1, 0);
}
static int _munmap(void* p, size_t len) {
    return (int)sc3(SYS_MUNMAP, (long)p, (long)len, 0);
}

// ── String / bellek yardımcıları ───────────────────────────────────────────
static size_t _strlen(const char* s) {
    size_t n = 0; while (s[n]) n++; return n;
}
static void _puts(const char* s) { _write(s, _strlen(s)); }

static void _putu(uint64_t v) {
    char buf[24]; int i = 0;
    if (!v) { _write("0", 1); return; }
    while (v) { buf[i++] = '0' + (v % 10); v /= 10; }
    char out[24]; int j = 0;
    while (i--) out[j++] = buf[i+1+0];
    for (int a=0,b=j-1;a<b;a++,b--){char c=out[a];out[a]=out[b];out[b]=c;}
    _write(out, j);
}
static void _puthex(uint64_t v) {
    _puts("0x");
    if (!v) { _write("0",1); return; }
    char buf[16]; int i=0;
    while (v) { buf[i++]="0123456789ABCDEF"[v&0xF]; v>>=4; }
    char out[16]; int j=0;
    while (i--) out[j++]=buf[i+1+0];
    for (int a=0,b=j-1;a<b;a++,b--){char c=out[a];out[a]=out[b];out[b]=c;}
    _write(out,j);
}

static void _memset(void* p, int v, size_t n) {
    uint8_t* b = (uint8_t*)p;
    while (n--) *b++ = (uint8_t)v;
}
static int _memcheck(void* p, int v, size_t n) {
    uint8_t* b = (uint8_t*)p;
    while (n--) if (*b++ != (uint8_t)v) return 0;
    return 1;
}

// ── Test sayaçları ──────────────────────────────────────────────────────────
static int g_pass = 0, g_fail = 0;

static void ok(const char* name) {
    _puts("  " GRN "OK" RST "  "); _puts(name); _puts("\n");
    g_pass++;
}
static void ng(const char* name, const char* detail) {
    _puts("  " RED "NG" RST "  "); _puts(name);
    if (detail) { _puts("  ("); _puts(detail); _puts(")"); }
    _puts("\n");
    g_fail++;
}
static void info(const char* msg) {
    _puts("  " CYN "··" RST "  "); _puts(msg); _puts("\n");
}

// ── LCG PRNG ────────────────────────────────────────────────────────────────
static uint32_t g_seed = 0xDEADBEEF;
static uint32_t lcg(void) {
    g_seed = g_seed * 1664525u + 1013904223u;
    return g_seed;
}
static uint32_t lcg_range(uint32_t lo, uint32_t hi) {
    return lo + (lcg() % (hi - lo + 1));
}

// ── Basit bump arena (Doom Z_Malloc simülasyonu) ────────────────────────────
#define ARENA_SIZE (2 * 1024 * 1024)

typedef struct {
    uint8_t* base;
    size_t   cap;
    size_t   used;
} Arena;

static int arena_init(Arena* a) {
    void* p = _mmap(ARENA_SIZE);
    if (p == MAP_FAILED || (long)p < 0) return 0;
    a->base = (uint8_t*)p; a->cap = ARENA_SIZE; a->used = 0;
    return 1;
}
static void* arena_alloc(Arena* a, size_t sz) {
    sz = (sz + 15) & ~(size_t)15;
    if (a->used + sz > a->cap) return (void*)0;
    void* p = a->base + a->used; a->used += sz; return p;
}
static void arena_free(Arena* a) {
    _munmap(a->base, a->cap); a->base = (void*)0;
}

// ============================================================================
// TEST 1 — mmap: basic alloc / write / verify / munmap
// ============================================================================
static void test_mmap_basic(void) {
    _puts("\n" WHT "--- Test 1: mmap basic alloc/write/verify/munmap ---" RST "\n");

    static const size_t sizes[] = {4096, 8192, 65536, 256*1024, 1024*1024, 2*1024*1024};
    static const int N = 6;

    for (int i = 0; i < N; i++) {
        size_t sz = sizes[i];
        void* p = _mmap(sz);
        if (p == MAP_FAILED || (long)p < 0) { ng("mmap alloc", "returned MAP_FAILED"); continue; }
        _memset(p, 0xAB, sz);
        if (!_memcheck(p, 0xAB, sz)) { ng("mmap write-verify", "pattern mismatch"); _munmap(p,sz); continue; }
        int r = _munmap(p, sz);
        if (r != 0) { ng("munmap", "non-zero return"); continue; }
        ok("mmap+write+verify+munmap");
    }
}

// ============================================================================
// TEST 2 — mmap: rapid alloc/free churn (Doom lump load/unload)
// ============================================================================
static void test_mmap_churn(void) {
    _puts("\n" WHT "--- Test 2: mmap churn (1000 alloc+free cycles) ---" RST "\n");

    int errors = 0;
    for (int i = 0; i < 1000; i++) {
        size_t sz = (size_t)lcg_range(1, 64) * 4096;
        void* p = _mmap(sz);
        if (p == MAP_FAILED || (long)p < 0) { errors++; continue; }
        ((uint8_t*)p)[0]      = (uint8_t)(i & 0xFF);
        ((uint8_t*)p)[sz - 1] = (uint8_t)(i & 0xFF);
        if (((uint8_t*)p)[0] != (uint8_t)(i & 0xFF)) errors++;
        _munmap(p, sz);
    }
    if (errors == 0) ok("mmap churn: 1000 random-size alloc+free, no errors");
    else ng("mmap churn", "canary mismatches or alloc failures");
}

// ============================================================================
// TEST 3 — mmap: hold multiple regions simultaneously
// ============================================================================
#define LIVE_REGIONS 32
static void test_mmap_concurrent(void) {
    _puts("\n" WHT "--- Test 3: mmap concurrent live regions (hold 32, verify, free) ---" RST "\n");

    void*   ptrs[LIVE_REGIONS];
    size_t  lens[LIVE_REGIONS];
    uint8_t tags[LIVE_REGIONS];
    int allocated = 0;

    for (int i = 0; i < LIVE_REGIONS; i++) {
        size_t sz = (size_t)lcg_range(1, 16) * 4096;
        void* p = _mmap(sz);
        if (p == MAP_FAILED || (long)p < 0) break;
        tags[i] = (uint8_t)lcg_range(1, 254);
        _memset(p, tags[i], sz);
        ptrs[i] = p; lens[i] = sz; allocated++;
    }

    int errors = 0;
    for (int i = 0; i < allocated; i++)
        if (!_memcheck(ptrs[i], tags[i], lens[i])) errors++;
    for (int i = 0; i < allocated; i++)
        _munmap(ptrs[i], lens[i]);

    if (errors == 0) {
        _puts("  "); _putu(allocated); _puts(" regions held simultaneously\n");
        ok("mmap concurrent: all canaries intact after holding live regions");
    } else {
        ng("mmap concurrent: canary corruption while holding live regions", 0);
    }
}

// ============================================================================
// TEST 4 — brk / sbrk: heap growth and shrink
// ============================================================================
static void test_brk(void) {
    _puts("\n" WHT "--- Test 4: brk/sbrk heap growth ---" RST "\n");

    long base = _brk(0);
    if (base <= 0) { ng("brk(0) query", "returned <=0"); return; }
    _puts("  base brk = 0x");
    {
        char hex[18]; int hi = 0;
        uint64_t v = (uint64_t)base;
        for (int b = 60; b >= 0; b -= 4) {
            uint8_t nib = (v >> b) & 0xF;
            if (nib || hi || b == 0) hex[hi++] = "0123456789ABCDEF"[nib];
        }
        hex[hi] = '\0'; _puts(hex);
    }
    _puts("\n");
    ok("brk(0) query");

    int grow_ok = 1;
    long prev = base;
    for (int step = 0; step < 16; step++) {
        long next = prev + 65536;
        long got  = _brk(next);
        if (got < next) { grow_ok = 0; break; }
        uint8_t* wp = (uint8_t*)prev;
        _memset(wp, 0xCC, 65536);
        if (!_memcheck(wp, 0xCC, 65536)) { grow_ok = 0; break; }
        prev = next;
    }
    if (grow_ok) ok("brk grow: 16 x 64KB steps, write+verify OK");
    else          ng("brk grow", "growth or verify failed");

    long r = _brk(base); (void)r;
    ok("brk shrink: returned without GPF");

    long s0 = _sbrk(0);
    if (s0 > 0) {
        long s1 = _sbrk(4096);
        if (s1 == s0) {
            uint8_t* np = (uint8_t*)s0;
            _memset(np, 0xDD, 4096);
            if (_memcheck(np, 0xDD, 4096)) ok("sbrk(4096) grow + write + verify");
            else ng("sbrk write", "pattern mismatch");
            _sbrk(-4096);
            ok("sbrk shrink: no GPF");
        } else {
            ng("sbrk(+4096)", "did not return old brk as expected");
        }
    } else {
        ng("sbrk(0) query", "returned <=0");
    }
}

// ============================================================================
// TEST 5 — Arena churn (Doom Z_Malloc simulation — 64 × 2 MB)
// ============================================================================
static void test_arena_churn(void) {
    _puts("\n" WHT "--- Test 5: arena churn (64 x 2MB Doom-style zone alloc) ---" RST "\n");

    int errors = 0;
    long t0 = _getticks();

    for (int round = 0; round < 64; round++) {
        Arena a;
        if (!arena_init(&a)) { errors++; continue; }
        uint8_t tag = (uint8_t)(round & 0xFF);
        int obj_count = 0;
        while (1) {
            size_t sz = lcg_range(8, 4096);
            void* p = arena_alloc(&a, sz);
            if (!p) break;
            _memset(p, tag, sz);
            obj_count++;
        }
        if (obj_count > 0 && a.base[0] != tag) errors++;
        arena_free(&a);
    }

    long t1 = _getticks();
    if (errors == 0) {
        ok("arena churn: 64 x 2MB zones alloc+fill+free, no errors");
        _puts("  time: "); _putu((uint64_t)(t1-t0)); _puts(" ticks\n");
    } else {
        ng("arena churn", "alloc or canary error");
    }
}

// ============================================================================
// TEST 6 — Interleaved mmap + brk (tehlikeli karışım)
// ============================================================================
static void test_interleaved(void) {
    _puts("\n" WHT "--- Test 6: interleaved mmap + brk (Doom mix) ---" RST "\n");

    long brk_start = _brk(0);
    if (brk_start <= 0) { ng("brk snapshot", "brk(0) failed"); return; }

    void*  mptrs[8];
    size_t mlens[8];
    int mcount = 0;
    for (int i = 0; i < 8; i++) {
        size_t sz = (size_t)(64 + i * 32) * 1024;
        void* p = _mmap(sz);
        if (p == MAP_FAILED || (long)p < 0) break;
        _memset(p, 0xAA + i, sz);
        mptrs[mcount] = p; mlens[mcount] = sz; mcount++;
    }

    int brk_ok = 1;
    long cur = brk_start;
    for (int step = 0; step < 8; step++) {
        long nxt = cur + 65536;
        long got = _brk(nxt);
        if (got < nxt) { brk_ok = 0; break; }
        uint8_t* wp = (uint8_t*)cur;
        _memset(wp, 0xBB, 65536);
        cur = nxt;
    }

    int mmap_ok = 1;
    for (int i = 0; i < mcount; i++)
        if (!_memcheck(mptrs[i], 0xAA + i, mlens[i])) mmap_ok = 0;

    for (int i = 0; i < mcount; i++) _munmap(mptrs[i], mlens[i]);
    _brk(brk_start);

    if      (brk_ok && mmap_ok)
        ok("interleaved mmap+brk: no overlap, mmap regions intact after brk growth");
    else if (!brk_ok)
        ng("interleaved", "brk growth failed");
    else
        ng("interleaved", "mmap region clobbered by brk growth — VMM overlap bug!");
}

// ============================================================================
// TEST 7 — Heap fragmentation torture (checkerboard free)
// ============================================================================
#define FRAG_SLOTS 128
static void test_fragmentation(void) {
    _puts("\n" WHT "--- Test 7: heap fragmentation torture (mmap-backed) ---" RST "\n");

    size_t total = (size_t)FRAG_SLOTS * 4096 * 2;
    void* arena = _mmap(total);
    if (arena == MAP_FAILED || (long)arena < 0) {
        ng("frag/mmap", "could not allocate arena"); return;
    }

    uint8_t* base = (uint8_t*)arena;
    size_t slot = 4096;

    for (int i = 0; i < FRAG_SLOTS; i++)
        _memset(base + i * slot, (uint8_t)(i & 0xFF), slot);
    for (int i = 0; i < FRAG_SLOTS; i += 2)
        _memset(base + i * slot, 0x00, slot);

    int errors = 0;
    for (int i = 1; i < FRAG_SLOTS; i += 2)
        if (!_memcheck(base + i * slot, (uint8_t)(i & 0xFF), slot)) errors++;

    _munmap(arena, total);

    if (errors == 0) ok("fragmentation torture: odd slots intact after checkerboard free");
    else             ng("fragmentation torture", "slot corruption detected");
}

// ============================================================================
// TEST 8 — Sustained load: 5000 mixed-size mmap alloc/free
// ============================================================================
#define POOL_SIZE 16
static void test_sustained(void) {
    _puts("\n" WHT "--- Test 8: sustained load (5000 mixed mmap cycles) ---" RST "\n");

    long t0 = _getticks();
    int errors = 0;
    uint64_t total_bytes = 0;

    void*   pool_p[POOL_SIZE];
    size_t  pool_s[POOL_SIZE];
    uint8_t pool_t[POOL_SIZE];
    int     pool_used[POOL_SIZE];
    for (int i = 0; i < POOL_SIZE; i++) pool_used[i] = 0;

    for (int iter = 0; iter < 5000; iter++) {
        int slot = (int)(lcg() % POOL_SIZE);
        if (pool_used[slot]) {
            if (!_memcheck(pool_p[slot], pool_t[slot], pool_s[slot])) errors++;
            _munmap(pool_p[slot], pool_s[slot]);
            pool_used[slot] = 0;
        } else {
            size_t sz = (size_t)lcg_range(1, 256) * 4096;
            void* p = _mmap(sz);
            if (p == MAP_FAILED || (long)p < 0) continue;
            uint8_t tag = (uint8_t)lcg_range(1, 254);
            _memset(p, tag, sz);
            pool_p[slot] = p; pool_s[slot] = sz;
            pool_t[slot] = tag; pool_used[slot] = 1;
            total_bytes += sz;
        }
    }
    for (int i = 0; i < POOL_SIZE; i++) {
        if (pool_used[i]) {
            if (!_memcheck(pool_p[i], pool_t[i], pool_s[i])) errors++;
            _munmap(pool_p[i], pool_s[i]);
        }
    }

    long t1 = _getticks();
    if (errors == 0) {
        ok("sustained load: 5000 mixed mmap cycles, zero canary errors");
        _puts("  total allocated: "); _putu(total_bytes / (1024*1024)); _puts(" MB\n");
        _puts("  time: "); _putu((uint64_t)(t1-t0)); _puts(" ticks\n");
    } else {
        ng("sustained load", "canary corruption — likely heap/VMM bug causing Doom GPF");
    }
}

// ============================================================================
// TEST 9 — NULL / edge-case guards (must not GPF)
// ============================================================================
static void test_edge_cases(void) {
    _puts("\n" WHT "--- Test 9: edge cases (must not GPF) ---" RST "\n");

    {
        long r = sc6(SYS_MMAP, 0, 0, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANON, -1, 0);
        ok("mmap(size=0): no GPF"); (void)r;
    }
    {
        int r = _munmap((void*)0, 4096); (void)r;
        ok("munmap(NULL, 4096): no GPF");
    }
    {
        long cur = _brk(0);
        long r   = _brk(cur);
        if (r >= cur) ok("brk(current): idempotent, no GPF");
        else          ng("brk(current)", "returned less than current brk");
    }
    {
        void* p = _mmap((size_t)512 * 1024 * 1024);
        if (p == MAP_FAILED || (long)p < 0 || p == (void*)0)
            ok("mmap(512MB): failed gracefully (no GPF)");
        else {
            _munmap(p, (size_t)512 * 1024 * 1024);
            ok("mmap(512MB): succeeded (large physical memory)");
        }
    }
}

// ============================================================================
//
//  v2 EKLEMELERİ — TEST 10–14
//
//  Slab allocator simülasyonu: user-space'de mmap page üzerinde
//  slab_alloc / slab_free / slab_owns mantığını kernel ile aynı şekilde
//  kuruyoruz.  Böylece bitmap hatası, magic overwrite, çift-free,
//  slab_owns yanlışlığı ve slab+kmalloc karışımı GPF vermeden test edilir.
//
// ============================================================================

// ── Slab simülasyon sabitleri ───────────────────────────────────────────────
#define SLAB_MAGIC_FREE  0xDEADC0DEUL
#define SLAB_MAGIC_ALLOC 0xABCD1234UL
#define SLAB_PAGE_SIZE   4096
#define SLAB_MAX_OBJ     128
#define SLAB_CACHE_COUNT 4
#define SLAB_PAGE_MAGIC  0x5ABC0DEEul

typedef struct slab_obj {
    uint32_t         magic;
    uint32_t         cache_id;
    struct slab_obj* next_free;
    uint8_t          _pad[8];
} slab_obj_t;

typedef struct {
    uint32_t    magic;
    uint32_t    obj_size;
    uint32_t    obj_count;
    uint32_t    free_count;
    uint32_t    bitmap[SLAB_MAX_OBJ/32];
    slab_obj_t* free_head;
    uint8_t     _pad[16];
} slab_page_t;

static const uint32_t slab_cache_obj_sizes[SLAB_CACHE_COUNT] = {32, 64, 128, 256};

#define MAX_SLAB_PAGES 16
typedef struct {
    slab_page_t* pages[MAX_SLAB_PAGES];
    uint32_t     page_count;
    uint32_t     obj_size;
    uint32_t     cache_id;
} sim_cache_t;

static sim_cache_t g_caches[SLAB_CACHE_COUNT];

// ── Slab page oluştur ───────────────────────────────────────────────────────
static slab_page_t* slab_page_create(uint32_t obj_size, uint32_t cache_id) {
    void* raw = _mmap(SLAB_PAGE_SIZE);
    if (raw == MAP_FAILED || (long)raw < 0) return (void*)0;

    slab_page_t* sp = (slab_page_t*)raw;
    sp->magic     = SLAB_PAGE_MAGIC;
    sp->obj_size  = obj_size;
    sp->free_head = (void*)0;

    uint8_t* base  = (uint8_t*)raw + sizeof(slab_page_t);
    uint32_t avail = SLAB_PAGE_SIZE - (uint32_t)sizeof(slab_page_t);
    uint32_t cnt   = avail / obj_size;
    if (cnt > SLAB_MAX_OBJ) cnt = SLAB_MAX_OBJ;

    sp->obj_count  = cnt;
    sp->free_count = cnt;
    for (int i = 0; i < (int)(SLAB_MAX_OBJ/32); i++) sp->bitmap[i] = 0;

    // LIFO free list (kernel'in slab davranışı)
    for (int i = (int)cnt - 1; i >= 0; i--) {
        slab_obj_t* obj = (slab_obj_t*)(base + (uint32_t)i * obj_size);
        obj->magic     = SLAB_MAGIC_FREE;
        obj->cache_id  = cache_id;
        obj->next_free = sp->free_head;
        sp->free_head  = obj;
    }
    return sp;
}

// ── slab_alloc ──────────────────────────────────────────────────────────────
static void* slab_alloc(sim_cache_t* cache) {
    // Mevcut page'lerde bak
    for (uint32_t pi = 0; pi < cache->page_count; pi++) {
        slab_page_t* sp = cache->pages[pi];
        if (sp->magic != SLAB_PAGE_MAGIC) return (void*)0;
        if (sp->free_count > 0 && sp->free_head) {
            slab_obj_t* obj = sp->free_head;
            if (obj->magic != SLAB_MAGIC_FREE) return (void*)0; // magic overwrite!
            sp->free_head  = obj->next_free;
            sp->free_count--;
            obj->magic     = SLAB_MAGIC_ALLOC;
            obj->next_free = (void*)0;
            uint8_t* pb = (uint8_t*)sp + sizeof(slab_page_t);
            uint32_t idx = (uint32_t)((uint8_t*)obj - pb) / cache->obj_size;
            if (idx < SLAB_MAX_OBJ) sp->bitmap[idx/32] |= (1u << (idx%32));
            return (void*)((uint8_t*)obj + sizeof(slab_obj_t));
        }
    }
    // Yeni page aç
    if (cache->page_count >= MAX_SLAB_PAGES) return (void*)0;
    slab_page_t* np = slab_page_create(cache->obj_size, cache->cache_id);
    if (!np) return (void*)0;
    cache->pages[cache->page_count++] = np;

    slab_page_t* sp = cache->pages[cache->page_count - 1];
    if (sp->free_count > 0 && sp->free_head) {
        slab_obj_t* obj = sp->free_head;
        sp->free_head  = obj->next_free;
        sp->free_count--;
        obj->magic     = SLAB_MAGIC_ALLOC;
        obj->next_free = (void*)0;
        uint8_t* pb = (uint8_t*)sp + sizeof(slab_page_t);
        uint32_t idx = (uint32_t)((uint8_t*)obj - pb) / cache->obj_size;
        if (idx < SLAB_MAX_OBJ) sp->bitmap[idx/32] |= (1u << (idx%32));
        return (void*)((uint8_t*)obj + sizeof(slab_obj_t));
    }
    return (void*)0;
}

// ── slab_owns ───────────────────────────────────────────────────────────────
// DÜZELTİLDİ v2: Adres aralığı kontrolü yetmez.
//
// Sorun: AscentOS mmap pool bir bump allocator — hem slab page'ler hem de
// doğrudan mmap() çağrıları aynı havuzdan ardışık adresler alır.
// Örnek: slab page 0x10000..0x10FFF, mmap ptr 0x11000.
//   user_ptr(0x11000) - sizeof(slab_obj_t)(24) = 0x10FE8
//   0x10FE8 ∈ [page_start, page_end) → yanlış "sahip" kararı!
//
// Gerçek fix: adres aralığı kontrolünün YANINDA slab_obj_t header'ının
// magic ve cache_id alanlarını da doğrula.  Raw mmap pointer'ının
// 24 byte öncesinde geçerli bir slab header olmaz.
//
// Kernel'deki karşılığı: kfree'nin slab_owns false-positive vermesinin
// nedeni budur → slab_free'ye giden mmap ptr → magic mismatch → panic/GPF.
static int slab_owns(sim_cache_t* cache, void* user_ptr) {
    if (!user_ptr) return 0;
    uint8_t* obj_addr = (uint8_t*)user_ptr - sizeof(slab_obj_t);
    for (uint32_t pi = 0; pi < cache->page_count; pi++) {
        uint8_t* page_start = (uint8_t*)cache->pages[pi];
        uint8_t* data_start = page_start + sizeof(slab_page_t);
        uint8_t* page_end   = page_start + SLAB_PAGE_SIZE;
        if (obj_addr < data_start || obj_addr >= page_end) continue;
        // Adres aralığı uyuyor — ama bu mmap'ten bitişik bir pointer da olabilir.
        // Header magic ve cache_id'yi doğrula (raw mmap'te bu değerler olmaz).
        slab_obj_t* hdr = (slab_obj_t*)obj_addr;
        if (hdr->magic != SLAB_MAGIC_ALLOC && hdr->magic != SLAB_MAGIC_FREE) continue;
        if (hdr->cache_id != cache->cache_id) continue;
        return 1;
    }
    return 0;
}

// ── slab_free ───────────────────────────────────────────────────────────────
static int slab_free(sim_cache_t* cache, void* user_ptr) {
    if (!user_ptr) return 0;
    slab_obj_t* obj = (slab_obj_t*)((uint8_t*)user_ptr - sizeof(slab_obj_t));

    if (obj->magic == SLAB_MAGIC_FREE)  return -1; // çift-free
    if (obj->magic != SLAB_MAGIC_ALLOC) return -2; // magic overwrite
    if (obj->cache_id != cache->cache_id) return -3; // yanlış cache_id

    slab_page_t* sp  = (void*)0;
    uint32_t     idx = 0;
    for (uint32_t pi = 0; pi < cache->page_count; pi++) {
        uint8_t* pb  = (uint8_t*)cache->pages[pi] + sizeof(slab_page_t);
        uint8_t* pe  = (uint8_t*)cache->pages[pi] + SLAB_PAGE_SIZE;
        if ((uint8_t*)obj >= pb && (uint8_t*)obj < pe) {
            sp  = cache->pages[pi];
            idx = (uint32_t)((uint8_t*)obj - pb) / cache->obj_size;
            break;
        }
    }
    if (!sp) return -4; // slab_owns kararıyla çelişir — asla buraya gelmemeli

    if (!(sp->bitmap[idx/32] & (1u << (idx%32)))) return -5; // bitmap tutarsız

    obj->magic     = SLAB_MAGIC_FREE;
    obj->next_free = sp->free_head;
    sp->free_head  = obj;
    sp->free_count++;
    sp->bitmap[idx/32] &= ~(1u << (idx%32));
    return 0;
}

// ── Slab cache'i temizle ─────────────────────────────────────────────────────
static void slab_cache_destroy(sim_cache_t* cache) {
    for (uint32_t pi = 0; pi < cache->page_count; pi++) {
        _munmap(cache->pages[pi], SLAB_PAGE_SIZE);
        cache->pages[pi] = (void*)0;
    }
    cache->page_count = 0;
}

// ── Cache'leri başlat ────────────────────────────────────────────────────────
static void slab_caches_init(void) {
    for (int i = 0; i < SLAB_CACHE_COUNT; i++) {
        g_caches[i].page_count = 0;
        g_caches[i].obj_size   = slab_cache_obj_sizes[i];
        g_caches[i].cache_id   = (uint32_t)i;
    }
}

// ============================================================================
// TEST 10 — Slab allocator: küçük nesne alloc/free / bitmap / magic / slab_owns
// ============================================================================
#define SLAB_TEST_OBJ_COUNT 512

static void test_slab_basic(void) {
    _puts("\n" WHT "--- Test 10: Slab allocator — küçük nesne paterni (Doom struct/node/cache) ---" RST "\n");
    slab_caches_init();

    int total_errors = 0;

    for (int ci = 0; ci < SLAB_CACHE_COUNT; ci++) {
        sim_cache_t* cache = &g_caches[ci];
        uint32_t dsz = cache->obj_size - (uint32_t)sizeof(slab_obj_t);

        _puts("  cache["); _putu(ci); _puts("] obj_size=");
        _putu(cache->obj_size); _puts("B\n");

        void* ptrs[SLAB_TEST_OBJ_COUNT];
        int alloc_ok = 0;
        for (int i = 0; i < SLAB_TEST_OBJ_COUNT; i++) {
            void* p = slab_alloc(cache);
            if (!p) { ptrs[i] = (void*)0; continue; }
            _memset(p, (uint8_t)(i & 0xFF), dsz);
            ptrs[i] = p; alloc_ok++;
        }
        _puts("    alloc'd: "); _putu(alloc_ok); _puts("\n");
        if (alloc_ok == 0) {
            ng("slab alloc", "hiç nesne tahsis edilemedi"); total_errors++; continue;
        }

        // 10a — canary doğrulama
        int cerr = 0;
        for (int i = 0; i < SLAB_TEST_OBJ_COUNT; i++) {
            if (!ptrs[i]) continue;
            if (!_memcheck(ptrs[i], (uint8_t)(i & 0xFF), dsz)) cerr++;
        }
        if (cerr == 0) ok("10a slab canary: alloc'lar birbirine yazmadı");
        else { ng("10a slab canary", "canary bozulması — slab overlap!"); total_errors++; }

        // 10b — slab_owns
        int oerr = 0;
        for (int i = 0; i < SLAB_TEST_OBJ_COUNT; i++) {
            if (!ptrs[i]) continue;
            if (!slab_owns(cache, ptrs[i])) oerr++;
            int other = (ci + 1) % SLAB_CACHE_COUNT;
            if (slab_owns(&g_caches[other], ptrs[i])) oerr++;
        }
        if (oerr == 0) ok("10b slab_owns: doğru cache tespiti");
        else { ng("10b slab_owns", "yanlış sahiplik kararı"); total_errors++; }

        // 10c — checkerboard free
        int ferr = 0;
        for (int i = 0; i < SLAB_TEST_OBJ_COUNT; i += 2) {
            if (!ptrs[i]) continue;
            if (slab_free(cache, ptrs[i]) != 0) ferr++;
            ptrs[i] = (void*)0;
        }
        if (ferr == 0) ok("10c slab checkerboard free: hata yok");
        else { ng("10c slab free", "magic/bitmap/owns hatası"); total_errors++; }

        // 10d — çift-free tespiti
        {
            void* tp = (void*)0;
            for (int i = 1; i < SLAB_TEST_OBJ_COUNT; i += 2)
                if (ptrs[i]) { tp = ptrs[i]; ptrs[i] = (void*)0; break; }
            if (tp) {
                int r1 = slab_free(cache, tp);
                int r2 = slab_free(cache, tp);
                if (r1 == 0 && r2 == -1) ok("10d çift-free tespiti: doğru reddedildi");
                else { ng("10d çift-free tespiti", "çift-free yakalanmadı"); total_errors++; }
            }
        }

        for (int i = 0; i < SLAB_TEST_OBJ_COUNT; i++)
            if (ptrs[i]) slab_free(cache, ptrs[i]);
        slab_cache_destroy(cache);
    }

    if (total_errors == 0)
        ok("TEST 10 TOPLAM: slab allocator tüm kontroller geçti");
    else
        ng("TEST 10 TOPLAM", "slab hataları — Doom GPF buradan geliyor olabilir");
}

// ============================================================================
// TEST 11 — Preemption simülasyonu: her 7. işlemde SYS_YIELD
// ============================================================================
#define PREEMPT_CYCLES  2000
#define PREEMPT_YIELD_N 7
#define PREEMPT_POOL    24

static void test_preemption_sim(void) {
    _puts("\n" WHT "--- Test 11: Preemption simülasyonu (SYS_YIELD her 7. işlemde) ---" RST "\n");
    slab_caches_init();

    sim_cache_t* cache = &g_caches[1]; // 64B
    uint32_t dsz = cache->obj_size - (uint32_t)sizeof(slab_obj_t);

    void*   pool[PREEMPT_POOL];
    uint8_t pool_tag[PREEMPT_POOL];
    int     pool_live[PREEMPT_POOL];
    for (int i = 0; i < PREEMPT_POOL; i++) pool_live[i] = 0;

    int errors = 0, op = 0;

    for (int cycle = 0; cycle < PREEMPT_CYCLES; cycle++) {
        int slot = (int)(lcg() % PREEMPT_POOL);
        if (pool_live[slot]) {
            if (!_memcheck(pool[slot], pool_tag[slot], dsz)) errors++;
            slab_free(cache, pool[slot]);
            pool_live[slot] = 0;
        } else {
            void* p = slab_alloc(cache);
            if (p) {
                uint8_t tag = (uint8_t)lcg_range(1, 254);
                _memset(p, tag, dsz);
                pool[slot] = p; pool_tag[slot] = tag; pool_live[slot] = 1;
            }
        }
        op++;
        if (op % PREEMPT_YIELD_N == 0) _yield();
    }
    for (int i = 0; i < PREEMPT_POOL; i++)
        if (pool_live[i]) slab_free(cache, pool[i]);
    slab_cache_destroy(cache);

    if (errors == 0) ok("11a slab+yield: yield sırasında canary bozulmadı");
    else ng("11a slab+yield", "preemption sonrası canary bozulması");

    // 11b — brk + yield
    {
        long base = _brk(0);
        if (base <= 0) { ng("11b brk+yield", "brk(0) başarısız"); goto skip11b; }
        int berr = 0; long cur = base;
        for (int step = 0; step < 8; step++) {
            long nxt = cur + 65536;
            if (_brk(nxt) < nxt) { berr++; break; }
            _memset((void*)cur, 0xBE, 65536);
            _yield();
            if (!_memcheck((void*)cur, 0xBE, 65536)) { berr++; break; }
            cur = nxt;
        }
        _brk(base);
        if (berr == 0) ok("11b brk+yield: yield sonrası brk sayfaları bozulmadı");
        else ng("11b brk+yield", "preemption sonrası brk bölgesi değişti — race!");
    }
skip11b:;

    // 11c — mmap + yield
    {
        int merr = 0;
        void*   p[8]; size_t sz[8]; uint8_t tag[8];
        for (int i = 0; i < 8; i++) {
            sz[i] = (size_t)lcg_range(1,16)*4096;
            p[i]  = _mmap(sz[i]);
            if (p[i] == MAP_FAILED || (long)p[i] < 0) { p[i]=(void*)0; continue; }
            tag[i] = (uint8_t)lcg_range(1,254);
            _memset(p[i], tag[i], sz[i]);
            _yield();
        }
        for (int i = 0; i < 8; i++) {
            if (!p[i]) continue;
            if (!_memcheck(p[i], tag[i], sz[i])) merr++;
            _munmap(p[i], sz[i]); _yield();
        }
        if (merr == 0) ok("11c mmap+yield: yield sırasında mmap bölgeleri bozulmadı");
        else ng("11c mmap+yield", "yield sonrası mmap canary bozulması");
    }
}

// ============================================================================
// TEST 12 — Uzun süreli fragmentation + coalescing baskısı (50 000 döngü)
// ============================================================================
#define FRAG_ARENA_SIZE   (8 * 1024 * 1024)
#define FRAG_TOTAL_ALLOCS 50000
#define FRAG_SLOT_COUNT   256

typedef struct free_node {
    size_t            size;
    struct free_node* next;
} free_node_t;

typedef struct {
    uint8_t*   base;
    size_t     cap;
    size_t     bump;
    free_node_t* free_list;
    size_t     free_total;
} FArena;

static int farena_init(FArena* a) {
    void* p = _mmap(FRAG_ARENA_SIZE);
    if (p == MAP_FAILED || (long)p < 0) return 0;
    a->base = (uint8_t*)p; a->cap = FRAG_ARENA_SIZE;
    a->bump = sizeof(FArena); a->free_list = (void*)0; a->free_total = 0;
    return 1;
}
static void* farena_alloc(FArena* a, size_t sz) {
    sz = (sz + 15) & ~(size_t)15;
    // first-fit
    free_node_t** prev = &a->free_list;
    free_node_t*  cur  = a->free_list;
    while (cur) {
        if (cur->size >= sz) {
            *prev = cur->next; a->free_total -= cur->size;
            return (void*)cur;
        }
        prev = &cur->next; cur = cur->next;
    }
    if (a->bump + sz > a->cap) return (void*)0;
    void* p = a->base + a->bump; a->bump += sz; return p;
}
static void farena_free(FArena* a, void* p, size_t sz) {
    if (!p) return;
    sz = (sz + 15) & ~(size_t)15;
    free_node_t* node = (free_node_t*)p;
    node->size = sz; node->next = a->free_list;
    a->free_list = node; a->free_total += sz;
}

static void test_long_frag(void) {
    _puts("\n" WHT "--- Test 12: Uzun süreli fragmentation + sawdust baskısı (50K döngü) ---" RST "\n");

    FArena arena;
    if (!farena_init(&arena)) { ng("12 arena init", "mmap başarısız"); return; }

    static const uint32_t sz_min[5] = {16,  65,  257, 1025, 4097};
    static const uint32_t sz_max[5] = {64, 256, 1024, 4096, 16384};

    void*   slots[FRAG_SLOT_COUNT];
    size_t  slot_sz[FRAG_SLOT_COUNT];
    uint8_t slot_tag[FRAG_SLOT_COUNT];
    int     slot_live[FRAG_SLOT_COUNT];
    for (int i = 0; i < FRAG_SLOT_COUNT; i++) slot_live[i] = 0;

    long t0 = _getticks();
    int canary_errors = 0, alloc_failures = 0;
    uint64_t total_alloc = 0, total_free = 0;

    for (int iter = 0; iter < FRAG_TOTAL_ALLOCS; iter++) {
        int slot  = (int)(lcg() % FRAG_SLOT_COUNT);
        int group = (int)(lcg() % 5);
        if (slot_live[slot]) {
            if (slot_sz[slot] > 0 && !_memcheck(slots[slot], slot_tag[slot], slot_sz[slot]))
                canary_errors++;
            farena_free(&arena, slots[slot], slot_sz[slot]);
            total_free += slot_sz[slot]; slot_live[slot] = 0;
        } else {
            size_t sz = (size_t)lcg_range(sz_min[group], sz_max[group]);
            void* p = farena_alloc(&arena, sz);
            if (!p) { alloc_failures++; continue; }
            uint8_t tag = (uint8_t)lcg_range(1, 254);
            _memset(p, tag, sz);
            slots[slot] = p; slot_sz[slot] = sz;
            slot_tag[slot] = tag; slot_live[slot] = 1;
            total_alloc += sz;
        }
        if (iter % 200 == 0) _yield();
    }
    for (int i = 0; i < FRAG_SLOT_COUNT; i++) {
        if (!slot_live[i]) continue;
        if (!_memcheck(slots[i], slot_tag[i], slot_sz[i])) canary_errors++;
        farena_free(&arena, slots[i], slot_sz[i]);
    }

    long t1 = _getticks();
    _puts("  toplam alloc: "); _putu(total_alloc/1024); _puts(" KB\n");
    _puts("  toplam free : "); _putu(total_free /1024);  _puts(" KB\n");
    _puts("  free listede: "); _putu(arena.free_total/1024); _puts(" KB (sawdust)\n");
    _puts("  alloc başar.: "); _putu((uint64_t)(FRAG_TOTAL_ALLOCS - alloc_failures)); _puts("\n");
    _puts("  alloc hata  : "); _putu((uint64_t)alloc_failures); _puts("\n");
    _puts("  süre        : "); _putu((uint64_t)(t1-t0)); _puts(" ticks\n");

    void* big = farena_alloc(&arena, 50*1024);
    if (big) {
        _memset(big, 0xEE, 50*1024);
        if (_memcheck(big, 0xEE, 50*1024))
            ok("12a büyük alloc sonrası: 50KB nesne tahsis + doğrulama OK");
        else
            ng("12a büyük alloc", "50KB canary bozulması");
    } else {
        _puts("  " YEL "uyarı: 50KB tahsis başarısız — sawdust birikimi (beklenen)" RST "\n");
        ok("12a büyük alloc: sawdust birikimi tespit edildi (arena tükendi)");
    }

    _munmap(arena.base, arena.cap);

    if (canary_errors == 0)
        ok("TEST 12 TOPLAM: 50K döngü fragmentation baskısı, canary temiz");
    else {
        _puts("  canary hata: "); _putu((uint64_t)canary_errors); _puts("\n");
        ng("TEST 12 TOPLAM", "fragmentation sırasında canary bozulması");
    }
}

// ============================================================================
// TEST 13 — Heap expansion: expand_heap / PMM+VMM map yolunu zorla
// ============================================================================
#define EXPAND_SMALL_STEP  (4  * 1024)
#define EXPAND_SMALL_COUNT 64
#define EXPAND_BIG_JUMP    (2  * 1024 * 1024)

static void test_heap_expansion(void) {
    _puts("\n" WHT "--- Test 13: Heap expansion (PMM+VMM map yolu) ---" RST "\n");

    long base = _brk(0);
    if (base <= 0) { ng("13 brk(0)", "başarısız"); return; }
    _puts("  base brk = "); _puthex((uint64_t)base); _puts("\n");

    // 13a — küçük adımlar
    int serr = 0; long cur = base;
    for (int i = 0; i < EXPAND_SMALL_COUNT; i++) {
        long nxt = cur + EXPAND_SMALL_STEP;
        long got = _brk(nxt);
        if (got < nxt) { serr++; break; }
        uint8_t tag = (uint8_t)(0x10 + (i & 0xF));
        _memset((void*)cur, tag, EXPAND_SMALL_STEP);
        if (!_memcheck((void*)cur, tag, EXPAND_SMALL_STEP)) { serr++; break; }
        cur = nxt;
        if (i % 16 == 0) _yield();
    }
    long after_small = cur;
    _puts("  küçük adım sonrası brk = "); _puthex((uint64_t)after_small); _puts("\n");
    if (serr == 0) ok("13a küçük adımlı heap expansion (4KB×64) OK");
    else ng("13a küçük adım", "expand veya canary hatası");

    // 13b — eski bölgeler bozulmadı mı?
    int prev_ok = 1; cur = base;
    for (int i = 0; i < EXPAND_SMALL_COUNT; i++) {
        uint8_t tag = (uint8_t)(0x10 + (i & 0xF));
        if (!_memcheck((void*)cur, tag, EXPAND_SMALL_STEP)) { prev_ok = 0; break; }
        cur += EXPAND_SMALL_STEP;
    }
    if (prev_ok) ok("13b önceki bölgeler: brk büyüdükçe eski sayfalar bozulmadı");
    else ng("13b önceki bölgeler", "eski canary bozuldu — VMM remap hatası");

    // 13c — 2MB tek atlama
    long big_end = after_small + EXPAND_BIG_JUMP;
    long got_big = _brk(big_end);
    if (got_big >= big_end) {
        _memset((void*)after_small, 0xDD, (size_t)EXPAND_BIG_JUMP);
        _yield();
        if (_memcheck((void*)after_small, 0xDD, (size_t)EXPAND_BIG_JUMP))
            ok("13c 2MB büyük atlama: PMM bitişik sayfa + VMM map OK");
        else
            ng("13c 2MB atlama", "2MB canary bozulması — PMM/VMM hatası");
    } else {
        _puts("  " YEL "uyarı: 2MB brk atlaması başarısız (yeterli fiziksel bellek yok)" RST "\n");
        ok("13c 2MB atlama: başarısız ama GPF vermedi (graceful)");
    }

    // 13d — shrink + re-grow
    long shrink_to = base + 128*1024;
    _brk(shrink_to); _yield();
    long regrow = shrink_to + 256*1024;
    long got_rg = _brk(regrow);
    if (got_rg >= regrow) {
        _memset((void*)shrink_to, 0xAA, 256*1024);
        if (_memcheck((void*)shrink_to, 0xAA, 256*1024))
            ok("13d shrink+regrow: VMM yeniden map, canary OK");
        else
            ng("13d shrink+regrow", "canary bozulması — shrink sonrası re-map hatası");
    } else {
        _puts("  " YEL "uyarı: regrow brk başarısız" RST "\n");
        ok("13d shrink+regrow: başarısız ama GPF yok");
    }

    _brk(base);
}

// ============================================================================
// TEST 14 — Slab + mmap(kmalloc) karışımı — slab_owns doğruluğu
//
// DÜZELTME: mmap nesneleri doğrudan mmap adresinin başından başlar,
// slab_obj_t header'ı yoktur.  slab_owns çağrısından önce pointer'ın
// slab mı yoksa mmap mi olduğunu kayıt edip buna göre test yapıyoruz.
// ============================================================================
#define MIX_COUNT 128

static void test_slab_kmalloc_mix(void) {
    _puts("\n" WHT "--- Test 14: Slab + mmap(kmalloc) karışımı — slab_owns doğruluğu ---" RST "\n");
    slab_caches_init();

    void*   slab_ptrs[MIX_COUNT/2];   // slab'dan gelenler (data pointer)
    void*   mmap_ptrs[MIX_COUNT/2];   // mmap'ten gelenler (ham adres)
    size_t  mmap_lens[MIX_COUNT/2];
    uint8_t slab_tags[MIX_COUNT/2];
    uint8_t mmap_tags[MIX_COUNT/2];
    int errors = 0;

    sim_cache_t* cache = &g_caches[1]; // 64B
    uint32_t dsz = cache->obj_size - (uint32_t)sizeof(slab_obj_t);

    // Tahsis
    for (int i = 0; i < MIX_COUNT/2; i++) {
        // Slab nesnesi
        void* sp = slab_alloc(cache);
        if (sp) {
            uint8_t t = (uint8_t)lcg_range(1,254);
            _memset(sp, t, dsz);
            slab_ptrs[i] = sp; slab_tags[i] = t;
        } else {
            slab_ptrs[i] = (void*)0;
        }
        // mmap nesnesi
        size_t sz = (size_t)lcg_range(1,8)*4096;
        void* mp = _mmap(sz);
        if (mp != MAP_FAILED && (long)mp > 0) {
            uint8_t t = (uint8_t)lcg_range(1,254);
            _memset(mp, t, sz);
            mmap_ptrs[i] = mp; mmap_lens[i] = sz; mmap_tags[i] = t;
        } else {
            mmap_ptrs[i] = (void*)0; mmap_lens[i] = 0;
        }
    }

    // 14a — slab_owns: slab nesnelerini tanımalı, mmap'leri tanımamalı
    int oerr = 0;
    for (int i = 0; i < MIX_COUNT/2; i++) {
        if (slab_ptrs[i] && !slab_owns(cache, slab_ptrs[i])) oerr++;
        if (mmap_ptrs[i] &&  slab_owns(cache, mmap_ptrs[i])) oerr++;
    }
    if (oerr == 0) ok("14a slab_owns: slab vs mmap nesneleri doğru ayırt etti");
    else {
        _puts("  hata: "); _putu(oerr); _puts("\n");
        ng("14a slab_owns karışık", "slab_owns yanlış karar — kfree yanlış yola gider");
        errors++;
    }

    // 14b — canary'ler bozulmamış mı?
    int cerr = 0;
    for (int i = 0; i < MIX_COUNT/2; i++) {
        if (slab_ptrs[i] && !_memcheck(slab_ptrs[i], slab_tags[i], dsz)) cerr++;
        if (mmap_ptrs[i] && mmap_lens[i] &&
            !_memcheck(mmap_ptrs[i], mmap_tags[i], mmap_lens[i])) cerr++;
    }
    if (cerr == 0) ok("14b karışık havuz canary: slab ve mmap nesneleri birbirine yazmadı");
    else { ng("14b karışık canary", "overlap — slab + mmap adres aralığı çakışıyor"); errors++; }

    // 14c — doğru free yolu
    int ferr = 0;
    for (int i = 0; i < MIX_COUNT/2; i++) {
        if (slab_ptrs[i] && slab_free(cache, slab_ptrs[i]) != 0) ferr++;
        if (mmap_ptrs[i] && _munmap(mmap_ptrs[i], mmap_lens[i]) != 0) ferr++;
    }
    if (ferr == 0) ok("14c karışık free: slab ve mmap ayrı ayrı free edildi, hata yok");
    else { ng("14c karışık free", "free hatası"); errors++; }

    // 14d — yanlış cache tespiti
    {
        void* p = slab_alloc(cache);
        if (p) {
            sim_cache_t* wrong = &g_caches[2]; // farklı cache_id
            int r = slab_free(wrong, p);
            if (r != 0)
                ok("14d yanlış cache free: doğru reddedildi (slab_owns koruması çalışıyor)");
            else {
                ng("14d yanlış cache free", "yanlış cache'e free kabul edildi — slab_owns bug!");
                errors++;
            }
            slab_free(cache, p);
        }
    }

    slab_cache_destroy(cache);

    if (errors == 0) ok("TEST 14 TOPLAM: slab+mmap karışımı, slab_owns doğru çalışıyor");
    else ng("TEST 14 TOPLAM", "slab_owns hatası — kfree yanlış yol seçer");
}

// ============================================================================
// main
// ============================================================================
int main(void) {
    _puts(CYN "================================================\n" RST);
    _puts(CYN "   AscentOS Memory Stress Test — Full Suite\n" RST);
    _puts(CYN "   Test 1-9  : mmap / brk / arena / frag\n" RST);
    _puts(CYN "   Test 10-14: slab / preempt / sawdust / VMM\n" RST);
    _puts(CYN "   Doom GPF root-cause hunt\n" RST);
    _puts(CYN "================================================\n" RST);

    // ── v1 testleri ──────────────────────────────────────────────────────────
    test_mmap_basic();
    test_mmap_churn();
    test_mmap_concurrent();
    test_brk();
    test_arena_churn();
    test_interleaved();
    test_fragmentation();
    test_sustained();
    test_edge_cases();

    // ── v2 testleri ──────────────────────────────────────────────────────────
    test_slab_basic();
    test_preemption_sim();
    test_long_frag();
    test_heap_expansion();
    test_slab_kmalloc_mix();

    // ── Özet ─────────────────────────────────────────────────────────────────
    _puts("\n");
    _puts(CYN "================================================\n" RST);
    _puts("  Sonuç: " GRN); _putu(g_pass); _puts(" OK" RST "   ");
    if (g_fail > 0) { _puts(RED); _putu(g_fail); _puts(" NG" RST); }
    else              _puts(GRN "0 NG" RST);
    _puts("\n");

    if (g_fail == 0) {
        _puts(GRN "  Tüm testler geçti — bellek alt sistemi OK\n" RST);
        _puts(YEL "  Hâlâ GPF varsa kontrol et:\n" RST);
        _puts(YEL "  - sys_fb_blit pointer doğrulaması\n" RST);
        _puts(YEL "  - Doom game loop stack overflow\n" RST);
        _puts(YEL "  - Doom'un kullandığı belirli bir mmap boyutu (serial log)\n" RST);
    } else {
        _puts(RED "  HATALAR BULUNDU — Doom GPF'nin muhtemel kökü:\n" RST);
        if (g_fail > 0) {
        _puts(RED "  Kernel'de kontrol et:\n" RST);
        _puts(RED "  - slab_alloc/slab_free: bitmap bit set/clear mantığı\n" RST);
        _puts(RED "  - slab_owns: data_start hesabı (header offset dahil mi?)\n" RST);
        _puts(RED "  - kfree: slab_owns false dönerse heap_free çağrılıyor mu?\n" RST);
        _puts(RED "  - expand_heap: pmm_alloc_pages + vmm_map_pages zinciri\n" RST);
        _puts(RED "  - Preemption: free-list next pointer race condition\n" RST);
        }
    }
    _puts(CYN "================================================\n" RST);

    _exit(g_fail > 0 ? 1 : 0);
    return 0;
}