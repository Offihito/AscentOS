// mouse64.h - PS/2 Mouse Driver Header
#ifndef MOUSE64_H
#define MOUSE64_H

#include <stdint.h>
#include <stdbool.h>

// Mouse durumu
typedef struct {
    int x, y;              // Ekran koordinatları
    bool left_button;      // Sol tuş
    bool right_button;     // Sağ tuş
    bool middle_button;    // Orta tuş (scroll wheel)
} MouseState;

// Mouse fonksiyonları
void init_mouse64(void);
void mouse_handler64(void);
void mouse_get_state(MouseState* state);

#endif // MOUSE64_H