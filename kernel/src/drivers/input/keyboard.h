#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdbool.h>
#include <stdint.h>

void keyboard_init(void);

// Returns true if there is a character in the keyboard buffer.
bool keyboard_has_char(void);

void keyboard_push_bytes(const char *bytes, uint32_t len);

// Halts execution until a character is typed, then returns it.
char keyboard_get_char(void);


#define KEY_UP    0xE0
#define KEY_DOWN  0xE1
#define KEY_LEFT  0xE2
#define KEY_RIGHT 0xE3
#define KEY_PGUP  0xE4
#define KEY_PGDN  0xE5

#endif
