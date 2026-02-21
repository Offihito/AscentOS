#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    // ── Temel çıktı ───────────────────────────────
    printf("Hello from ring3 AscentOS :3\n");
    printf("PID: %d\n", getpid());

    // ── malloc / free testi ───────────────────────
    char* buf = malloc(64);
    if (!buf) {
        printf("HATA: malloc basarisiz!\n");
        return 1;
    }
    strcpy(buf, "newlib malloc calisiyor!");
    printf("malloc : %s\n", buf);
    free(buf);

    // ── sprintf testi ─────────────────────────────
    char msg[32];
    sprintf(msg, "pid=%d fmt ok", getpid());
    printf("sprintf: %s\n", msg);

    printf("--- Tum testler gecti ---\n");
    return 0;
}