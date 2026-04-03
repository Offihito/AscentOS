#ifndef CONSOLE_CONSOLE_H
#define CONSOLE_CONSOLE_H

#include <limine.h>

void console_init(struct limine_framebuffer *framebuffer);
void console_putchar(char c);
void console_puts(const char *s);
void console_clear(void);

#endif
