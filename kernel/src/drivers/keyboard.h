#ifndef KEYBOARD_H
#define KEYBOARD_H

void keyboard_init(void);

// Halts execution until a character is typed, then returns it.
char keyboard_get_char(void);

#endif
