#include "../libc/stdio.h"
#include "../libc/string.h"
#include "../libc/unistd.h"

// ─────────────────────────────────────────────
//  AscentOS — calculator.c
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
    if (s[i] != '\0') return -1; // beklenmedik karakter
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

    // Boşlukları atla
    while (expr[i] == ' ') i++;

    // Sol sayıyı oku (negatif olabilir)
    if (expr[i] == '-') left_s[j++] = expr[i++];
    while (expr[i] >= '0' && expr[i] <= '9' && j < 31)
        left_s[j++] = expr[i++];
    left_s[j] = '\0';

    // Boşlukları atla
    while (expr[i] == ' ') i++;

    // Operatörü oku
    if (expr[i] == '+' || expr[i] == '-' ||
        expr[i] == '*' || expr[i] == '/') {
        op = expr[i++];
    } else {
        return -1;
    }

    // Boşlukları atla
    while (expr[i] == ' ') i++;

    // Sağ sayıyı oku
    j = 0;
    if (expr[i] == '-') right_s[j++] = expr[i++];
    while (expr[i] >= '0' && expr[i] <= '9' && j < 31)
        right_s[j++] = expr[i++];
    right_s[j] = '\0';

    // Sona kadar boşluk olabilir
    while (expr[i] == ' ') i++;
    if (expr[i] != '\0') return -1;

    // Parse
    int a = 0, b = 0;
    if (parse_int(left_s,  &a) < 0) return -1;
    if (parse_int(right_s, &b) < 0) return -1;

    // Hesapla
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
// Kernel handler zaten VGA'ya echo yapıyor.
// Biz sadece ring buffer'dan okuyoruz.
static int readline(char* buf, int max) {
    int i = 0;
    while (i < max - 1) {
        char c = 0;
        ssize_t n = read(STDIN, &c, 1);
        if (n <= 0) { yield(); continue; }
        if (c == '\r') continue;
        if (c == '\n') break;
        if (c == '\b' || c == 127) {
            if (i > 0) {
                i--;
                // VGA'da da geri al (kernel handler echo yapmıyor backspace için)
                write(STDOUT, "\b \b", 3);
            }
            continue;
        }
        buf[i++] = c;
        // VGA echo kernel handler tarafından yapılıyor
    }
    buf[i] = '\0';
    return i;
}

int main(void) {
    puts("================================");
    puts("  AscentOS Calculator v1.0");
    puts("  Islemler: + - * /");
    puts("  Cikis: q");
    puts("================================");

    char buf[BUF_SIZE];

    while (1) {
        write(STDOUT, "> ", 2);

        int len = readline(buf, BUF_SIZE);
        // newline'ı kernel handler basmadığı için biz basalım
        write(STDOUT, "\n", 1);
        if (len == 0) continue;

        // Çıkış
        if (buf[0] == 'q' || buf[0] == 'Q') {
            puts("Cikiliyor...");
            break;
        }



        int result = 0;
        int ret = calculate(buf, &result);

        if (ret == -1) {
            puts("Hata: Gecersiz ifade. Ornek: 12+34");
        } else if (ret == -2) {
            puts("Hata: Sifira bolme!");
        } else {
            // printf %d bozuk olabilir, itoa + write kullan
            char resbuf[16];
            itoa(result, resbuf);
            write(STDOUT, "= ", 2);
            write(STDOUT, resbuf, strlen(resbuf));
            write(STDOUT, "\n", 1);
        }
    }

    return 0;
}