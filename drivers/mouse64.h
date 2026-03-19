#ifndef MOUSE64_H
#define MOUSE64_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    int x, y;           
    bool left_button;     
    bool right_button;     
    bool middle_button;   
} MouseState;

void init_mouse64(void);
void mouse_handler64(void);
void mouse_get_state(MouseState* state);

#endif 