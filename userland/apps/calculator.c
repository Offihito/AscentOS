#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// ─────────────────────────────────────────────
//  AscentOS — calculator.c  (newlib uyumlu)
//  VGA text mode, tek satır hesap makinesi
//  Desteklenen: + - * /
//  Kullanım: 12+34  →  = 46
//            100/4  →  = 25
//            q      →  çıkış
// ─────────────────────────────────────────────

#define BUF_SIZE 64

// ── Basit string → int ────────────────────────
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

// ── İfadeyi ayrıştır ve hesapla ──────────────
// Format: <sayi><op><sayi>
// Döner: 0=ok, -1=sözdizimi hatası, -2=sıfıra bölme
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

// ── Satır oku ────────────────────────────────
static int readline(char* buf, int max) {
    int i = 0;
    while (i < max - 1) {
        char c = 0;
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n <= 0) continue;
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
    }
    buf[i] = '\0';
    return i;
}

int main(void) {
    printf("================================\n");
    printf("  AscentOS Calculator v1.0\n");
    printf("  Islemler: + - * /\n");
    printf("  Cikis: q\n");
    printf("================================\n");

    char buf[BUF_SIZE];

    while (1) {
        printf("> ");
        fflush(stdout);

        int len = readline(buf, BUF_SIZE);
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
    }

    return 0;
}