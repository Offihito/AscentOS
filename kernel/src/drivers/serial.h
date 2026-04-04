#ifndef DRIVERS_SERIAL_H
#define DRIVERS_SERIAL_H

#include <stdint.h>

void serial_init(void);
void serial_putchar(char c);
int serial_received(void);
char serial_get_char(void);


#endif
