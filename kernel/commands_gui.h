#ifndef COMMANDS_GUI_H
#define COMMANDS_GUI_H

#include "terminal64.h"
#include "../apps/commands64.h"  // Artık commands64 altyapısını kullanıyoruz

// GUI terminalde bir komut işler
void process_command(Terminal* term, const char* cmd);

#endif