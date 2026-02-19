#include "../libc/stdio.h"
#include "../libc/math.h"

// ─────────────────────────────────────────────
//  AscentOS — math_test.c
//  libc math.h için test programı
// ─────────────────────────────────────────────

static int pass = 0, fail = 0;

static inline void test(const char* name, int ok) {
    if (ok) {
        fprintf(STDOUT, "[PASS] %s\n", name);
        pass++;
    } else {
        fprintf(STDERR, "[FAIL] %s\n", name);
        fail++;
    }
}

int main(void) {
    puts("=== AscentOS math.h Test ===\n");

    // ── abs ──────────────────────────────────
    puts("-- abs --");
    printf("abs(0)        = %d\n", abs(0));
    printf("abs(42)       = %d\n", abs(42));
    printf("abs(-42)      = %d\n", abs(-42));
    printf("abs(INT_MIN+1)= %d\n", abs(-2147483647));

    test("abs(0)==0",          abs(0)          == 0);
    test("abs(42)==42",        abs(42)         == 42);
    test("abs(-42)==42",       abs(-42)        == 42);
    test("abs(-2147483647)",   abs(-2147483647) == 2147483647);

    // ── min ──────────────────────────────────
    puts("\n-- min --");
    printf("min(3,5)      = %d\n", min(3, 5));
    printf("min(5,3)      = %d\n", min(5, 3));
    printf("min(-1,1)     = %d\n", min(-1, 1));
    printf("min(7,7)      = %d\n", min(7, 7));

    test("min(3,5)==3",     min(3,5)  == 3);
    test("min(5,3)==3",     min(5,3)  == 3);
    test("min(-1,1)==-1",   min(-1,1) == -1);
    test("min(7,7)==7",     min(7,7)  == 7);

    // ── max ──────────────────────────────────
    puts("\n-- max --");
    printf("max(3,5)      = %d\n", max(3, 5));
    printf("max(5,3)      = %d\n", max(5, 3));
    printf("max(-1,1)     = %d\n", max(-1, 1));
    printf("max(7,7)      = %d\n", max(7, 7));

    test("max(3,5)==5",     max(3,5)  == 5);
    test("max(5,3)==5",     max(5,3)  == 5);
    test("max(-1,1)==1",    max(-1,1) == 1);
    test("max(7,7)==7",     max(7,7)  == 7);

    // ── clamp ────────────────────────────────
    puts("\n-- clamp --");
    printf("clamp(5,0,10) = %d\n", clamp(5,  0, 10));
    printf("clamp(-5,0,10)= %d\n", clamp(-5, 0, 10));
    printf("clamp(15,0,10)= %d\n", clamp(15, 0, 10));
    printf("clamp(0,0,10) = %d\n", clamp(0,  0, 10));
    printf("clamp(10,0,10)= %d\n", clamp(10, 0, 10));

    test("clamp(5,0,10)==5",   clamp(5,  0, 10) == 5);
    test("clamp(-5,0,10)==0",  clamp(-5, 0, 10) == 0);
    test("clamp(15,0,10)==10", clamp(15, 0, 10) == 10);
    test("clamp(0,0,10)==0",   clamp(0,  0, 10) == 0);
    test("clamp(10,0,10)==10", clamp(10, 0, 10) == 10);

    // ── labs ─────────────────────────────────
    puts("\n-- labs --");
    printf("labs(-1L)     = %d\n", (int)labs(-1L));
    printf("labs(0L)      = %d\n", (int)labs(0L));

    test("labs(-1)==1",   labs(-1L) == 1L);
    test("labs(0)==0",    labs(0L)  == 0L);

    // ── lmin / lmax / lclamp ─────────────────
    puts("\n-- lmin / lmax / lclamp --");
    test("lmin(3,5)==3",       lmin(3L,5L)       == 3L);
    test("lmax(3,5)==5",       lmax(3L,5L)       == 5L);
    test("lclamp(15,0,10)==10",lclamp(15L,0L,10L)== 10L);

    // ── smin / smax / sclamp ─────────────────
    puts("\n-- smin / smax / sclamp --");
    test("smin(3,5)==3",       smin(3,5)       == 3);
    test("smax(3,5)==5",       smax(3,5)       == 5);
    test("sclamp(15,0,10)==10",sclamp(15,0,10) == 10);

    // ── Özet ─────────────────────────────────
    printf("\n=== Sonuc: %d PASS, %d FAIL ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}