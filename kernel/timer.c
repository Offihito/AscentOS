#include "timer.h"

static uint64_t system_ticks = 0;

uint64_t get_system_ticks(void) {
    return system_ticks;
}

void task_increment_ticks(void) {
    system_ticks++;
}