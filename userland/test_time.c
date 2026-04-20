#include <stdio.h>
#include <time.h>
#include <stdint.h>

int main() {
    struct timespec ts_real, ts_mono;

    if (clock_gettime(CLOCK_REALTIME, &ts_real) == 0) {
        printf("CLOCK_REALTIME:  %ld seconds, %ld nanoseconds\n", ts_real.tv_sec, ts_real.tv_nsec);
    } else {
        perror("clock_gettime(CLOCK_REALTIME)");
    }

    if (clock_gettime(CLOCK_MONOTONIC, &ts_mono) == 0) {
        printf("CLOCK_MONOTONIC: %ld seconds, %ld nanoseconds\n", ts_mono.tv_sec, ts_mono.tv_nsec);
    } else {
        perror("clock_gettime(CLOCK_MONOTONIC)");
    }

    return 0;
}
