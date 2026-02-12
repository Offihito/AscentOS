#include "timer.h"

// System tick counter
static uint64_t system_ticks = 0;

// Get system tick count
uint64_t get_system_ticks(void) {
    return system_ticks;
}

// Increment system ticks (called by scheduler)
void task_increment_ticks(void) {
    system_ticks++;
}