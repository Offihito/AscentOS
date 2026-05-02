#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

#define AC97_DEV "/dev/ac97"
#define STATS_DEV "/dev/interrupts"

void print_irq_stats() {
    printf("\n--- Current Kernel IRQ Stats ---\n");
    int fd = open(STATS_DEV, O_RDONLY);
    if (fd < 0) {
        perror("Failed to open /dev/interrupts");
        return;
    }
    char buf[1024];
    int bytes = read(fd, buf, sizeof(buf)-1);
    if (bytes > 0) {
        buf[bytes] = '\0';
        printf("%s", buf);
    }
    close(fd);
    printf("--------------------------------\n");
}

int main() {
    printf("════════════════════════════════════════════════════\n");
    printf("   AscentOS Shared Interrupt Verification Tool      \n");
    printf("════════════════════════════════════════════════════\n");

    // 1. Show the baseline
    printf("[INIT] Checking initial interrupt counts...\n");
    print_irq_stats();

    // 2. Trigger Audio Activity
    printf("\n[STEP 1] Stressing Audio (/dev/ac97) to fire interrupts...\n");
    int audio_fd = open(AC97_DEV, O_WRONLY);
    if (audio_fd < 0) {
        perror("Failed to open audio device");
    } else {
        char noise[4096];
        memset(noise, 0xAA, sizeof(noise));
        for(int i = 0; i < 50; i++) {
            write(audio_fd, noise, sizeof(noise));
            if (i % 10 == 0) printf("  Writing buffers... %d/50\n", i);
            usleep(1000);
        }
        close(audio_fd);
        printf("[DONE] Audio activity finished.\n");
    }

    // 3. Show the results
    printf("\n[STEP 2] Verifying Shared Dispatching...\n");
    printf("Note: You should see Hits increasing for BOTH handlers on IRQ 11!\n");
    print_irq_stats();

    printf("\n[SUCCESS] If you see hits on IRQ 11 for multiple entries, sharing is working!\n");

    return 0;
}
