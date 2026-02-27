// calculator.c - AscentOS Calculator v1.1 (newlib uyumlu)
//
// Düzeltmeler (v1.0 → v1.1):
//   - readline(): read() EAGAIN/-1 dönünce sched_yield() çağır.
//     Böylece CPU'yu meşgul etmeden (busy-wait) diğer task'lara
//     zaman dilimi bırakılır. Scheduler CALC.ELF ↔ idle arasında
//     gereksiz döngüye girmez.
//   - main() sonunda explicit exit(0): bazı newlib _start
//     implementasyonlarında exit çağrısı eksik olabilir; güvenlik
//     için açık çağrı eklendi.
//   - EOF / kalıcı okuma hatası: break ile döngüden çık, exit(0).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#define BUF_SIZE 64

// ── sched_yield: kernel'e CPU bırak (SYS_YIELD = 5) ─────────────
// Newlib'in sched_yield'i yoksa inline syscall.
static inline void yield_cpu(void) {
    __asm__ volatile (
        "movq $5, %%rax\n\t"   // SYS_YIELD = 5
        "syscall\n\t"
        ::: "rax", "rcx", "r11", "memory"
    );
}

// ── Basit string → int ────────────────────────────────────────────
static int parse_int(const char* s, int* out) {
    int neg = 0, val = 0, i = 0;
    if (s[i] == '-') { neg = 1; i++; }
    if (s[i] == '\0') return -1;
    for (; s[i] >= '0' && s[i] <= '9'; i++)
        val = val * 10 + (s[i] - '0');
    if (s[i] != '\0') return -1;
    *out = neg ? -val : val;
    return i;
}

// ── İfadeyi ayrıştır ve hesapla ──────────────────────────────────
static int calculate(const char* expr, int* result) {
    char left_s[32], right_s[32];
    char op = 0;
    int i = 0, j = 0;

    while (expr[i] == ' ') i++;
    if (expr[i] == '-') left_s[j++] = expr[i++];
    while (expr[i] >= '0' && expr[i] <= '9' && j < 31)
        left_s[j++] = expr[i++];
    left_s[j] = '\0';
    while (expr[i] == ' ') i++;

    if (expr[i] == '+' || expr[i] == '-' ||
        expr[i] == '*' || expr[i] == '/') {
        op = expr[i++];
    } else {
        return -1;
    }

    while (expr[i] == ' ') i++;
    j = 0;
    if (expr[i] == '-') right_s[j++] = expr[i++];
    while (expr[i] >= '0' && expr[i] <= '9' && j < 31)
        right_s[j++] = expr[i++];
    right_s[j] = '\0';
    while (expr[i] == ' ') i++;
    if (expr[i] != '\0') return -1;

    int a = 0, b = 0;
    if (parse_int(left_s,  &a) < 0) return -1;
    if (parse_int(right_s, &b) < 0) return -1;

    switch (op) {
    case '+': *result = a + b; break;
    case '-': *result = a - b; break;
    case '*': *result = a * b; break;
    case '/':
        if (b == 0) return -2;
        *result = a / b;
        break;
    }
    return 0;
}

// ── Satır oku — bloklayan, yield'li ──────────────────────────────
// read() EAGAIN / -1 dönerse yield_cpu() ile scheduler'a bırakır.
// EOF veya kalıcı hata durumunda -1 döner.
static int readline(char* buf, int max) {
    int i = 0;
    int consecutive_empty = 0;     // art arda boş okuma sayacı

    while (i < max - 1) {
        char c = 0;
        ssize_t n = read(STDIN_FILENO, &c, 1);

        if (n == 1) {
            consecutive_empty = 0;
            if (c == '\r') continue;
            if (c == '\n') break;
            if (c == '\b' || c == 127) {
                if (i > 0) {
                    i--;
                    write(STDOUT_FILENO, "\b \b", 3);
                }
                continue;
            }
            buf[i++] = c;
        } else if (n == 0) {
            // EOF — bağlantı kapandı
            buf[i] = '\0';
            return -1;
        } else {
            // n < 0: EAGAIN veya geçici hata
            // CPU'yu boşa harcamak yerine scheduler'a bırak
            consecutive_empty++;
            yield_cpu();

            // 10000 art arda boş okuma = gerçek hata / kapalı fd
            if (consecutive_empty > 10000) {
                buf[i] = '\0';
                return -1;
            }
        }
    }

    buf[i] = '\0';
    return i;
}

int main(void) {
    printf("================================\n");
    printf("  AscentOS Calculator v1.1\n");
    printf("  Islemler: + - * /\n");
    printf("  Cikis: q\n");
    printf("================================\n");
    fflush(stdout);

    char buf[BUF_SIZE];

    while (1) {
        printf("> ");
        fflush(stdout);

        int len = readline(buf, BUF_SIZE);

        // EOF veya kalıcı okuma hatası → temiz çıkış
        if (len < 0) {
            printf("\nEOF, cikiliyor...\n");
            break;
        }

        printf("\n");
        if (len == 0) continue;

        if (buf[0] == 'q' || buf[0] == 'Q') {
            printf("Cikiliyor...\n");
            break;
        }

        int result = 0;
        int ret = calculate(buf, &result);

        if (ret == -1) {
            printf("Hata: Gecersiz ifade. Ornek: 12+34\n");
        } else if (ret == -2) {
            printf("Hata: Sifira bolme!\n");
        } else {
            printf("= %d\n", result);
        }

        fflush(stdout);
    }

    fflush(stdout);
    exit(0);   // Açık exit: SYS_EXIT syscall'ını tetikler → task_exit()
    return 0;  // unreachable, derleyiciyi memnun eder
}