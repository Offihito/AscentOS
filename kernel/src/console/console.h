#ifndef CONSOLE_CONSOLE_H
#define CONSOLE_CONSOLE_H

#include <limine.h>
#include <stdbool.h>

void console_init(struct limine_framebuffer *framebuffer);
void console_putchar(char c);
void console_puts(const char *s);
void console_clear(void);
void console_update_cursor(bool visible);
void console_scroll_view(int delta);

#endif
