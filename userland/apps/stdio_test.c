#include "../libc/stdio.h"
#include "../libc/unistd.h"

// ─────────────────────────────────────────────
//  AscentOS — stdio_test.c
//  libc stdio.h için kapsamlı test programı
// ─────────────────────────────────────────────

// Test sonucunu raporla
static inline void test(const char* name, int ok) {
    if (ok)
        fprintf(STDOUT, "[PASS] %s\n", name);
    else
        fprintf(STDERR, "[FAIL] %s\n", name);
}

int main(void) {
    puts("=== AscentOS stdio.h Test ===\n");

    // ── %s ───────────────────────────────────
    puts("-- %s / puts / putchar --");
    printf("merhaba %s\n", "dunya");
    printf("null ptr: %s\n", (char*)0);
    putchar('A'); putchar('B'); putchar('C'); putchar('\n');

    // ── %d ───────────────────────────────────
    puts("\n-- %d (signed int) --");
    printf("sifir:    %d\n", 0);
    printf("pozitif:  %d\n", 12345);
    printf("negatif:  %d\n", -9876);
    printf("INT_MAX:  %d\n", 2147483647);
    printf("INT_MIN:  %d\n", -2147483648);

    // ── %u ───────────────────────────────────
    puts("\n-- %u (unsigned int) --");
    printf("sifir:    %u\n", 0u);
    printf("buyuk:    %u\n", 4294967295u);  // UINT_MAX

    // ── %x / %X ──────────────────────────────
    puts("\n-- %x / %X (hex) --");
    printf("kucuk:    %x\n",  0xdeadbeef);
    printf("buyuk:    %X\n",  0xdeadbeef);
    printf("prefix:   %#x\n", 0xcafe);
    printf("prefix:   %#X\n", 0xcafe);
    printf("sifir:    %x\n",  0u);

    // ── %o ───────────────────────────────────
    puts("\n-- %o (octal) --");
    printf("8:        %o\n",  8u);
    printf("255:      %o\n",  255u);
    printf("prefix:   %#o\n", 255u);

    // ── %p ───────────────────────────────────
    puts("\n-- %p (pointer) --");
    int x = 42;
    printf("adres:    %p\n", (void*)&x);
    printf("null:     %p\n", (void*)0);
    printf("sabit:    %p\n", (void*)0xffffffff80001000UL);

    // ── %c ───────────────────────────────────
    puts("\n-- %c (char) --");
    printf("harf:     %c\n", 'Z');
    printf("rakam:    %c\n", '7');

    // ── %% ───────────────────────────────────
    puts("\n-- %% (literal) --");
    printf("yuzdesi:  %%100\n");

    // ── fprintf / dprintf / stderr ───────────
    puts("\n-- fprintf / dprintf --");
    fprintf(STDOUT, "fprintf STDOUT: %d\n", 1);
    fprintf(STDERR, "fprintf STDERR: hata mesaji\n");
    dprintf(STDOUT, "dprintf STDOUT: %x\n", 0xabcd);

    // ── snprintf ─────────────────────────────
    puts("\n-- snprintf --");
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "pid=%d hex=%#x", 42, 0xff);
    printf("sonuc:    '%s' (%d karakter)\n", buf, n);

    // Taşma testi: küçük buffer
    char small[8];
    snprintf(small, sizeof(small), "123456789");
    printf("taşma:    '%s' (max 7 karakter beklenir)\n", small);

    // ── sprintf ──────────────────────────────
    puts("\n-- sprintf --");
    sprintf(buf, "sprintf: %s %d %#x", "test", -1, 0xbeef);
    printf("%s\n", buf);

    // ── Doğrulama testleri ───────────────────
    puts("\n-- Dogrulama --");
    char tmp[32];

    snprintf(tmp, sizeof(tmp), "%d", 0);
    test("%d sifir",    tmp[0] == '0' && tmp[1] == '\0');

    snprintf(tmp, sizeof(tmp), "%d", -1);
    test("%d negatif",  tmp[0] == '-' && tmp[1] == '1');

    snprintf(tmp, sizeof(tmp), "%u", 0u);
    test("%u sifir",    tmp[0] == '0' && tmp[1] == '\0');

    snprintf(tmp, sizeof(tmp), "%x", 0xffu);
    test("%x ff",       tmp[0] == 'f' && tmp[1] == 'f' && tmp[2] == '\0');

    snprintf(tmp, sizeof(tmp), "%X", 0xABu);
    test("%X AB",       tmp[0] == 'A' && tmp[1] == 'B' && tmp[2] == '\0');

    snprintf(tmp, sizeof(tmp), "%#x", 0x1u);
    test("%#x prefix",  tmp[0]=='0' && tmp[1]=='x' && tmp[2]=='1');

    snprintf(tmp, sizeof(tmp), "%o", 8u);
    test("%o 8->10",    tmp[0] == '1' && tmp[1] == '0' && tmp[2] == '\0');

    snprintf(tmp, sizeof(tmp), "%p", (void*)0);
    test("%p nil",      tmp[0]=='(' && tmp[1]=='n'); // "(nil)"

    // snprintf null-terminate garantisi
    char nb[4];
    snprintf(nb, 4, "ABCDEFGH");
    test("snprintf null-term", nb[3] == '\0');

    puts("\n=== Test tamamlandi ===");
    return 0;
}