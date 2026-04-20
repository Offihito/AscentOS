#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

int main() {
    printf("TSC Manual Test starting...\n");
    printf("Measuring cycles for 1 second...\n");

    uint64_t start = rdtsc();
    sleep(1);
    uint64_t end = rdtsc();

    uint64_t diff = end - start;
    uint64_t mhz = diff / 1000000;
    uint64_t mhz_frac = (diff % 1000000) / 1000;

    printf("TSC Start: %lu\n", start);
    printf("TSC End:   %lu\n", end);
    printf("TSC Diff:  %lu\n", diff);
    
    printf("\nManual Calculation:\n");
    printf("Frequency: %lu.%03lu MHz\n", mhz, mhz_frac);

    return 0;
}
