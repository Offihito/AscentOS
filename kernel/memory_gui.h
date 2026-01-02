// memory_gui.h - Memory management for GUI mode
#ifndef MEMORY_GUI_H
#define MEMORY_GUI_H

#include <stdint.h>

void init_memory_gui(void);
uint64_t get_total_memory(void);
void* malloc_gui(uint64_t size);
void free_gui(void* ptr);

#endif