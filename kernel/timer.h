// task64.h - AscentOS Timer System
#ifndef TASK64_H
#define TASK64_H

#include <stdint.h>

// Timer functions
uint64_t get_system_ticks(void);
void task_increment_ticks(void);
void scheduler_tick(void);

#endif