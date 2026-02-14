// commands_gui.h - GUI command processing header
#ifndef COMMANDS_GUI_H
#define COMMANDS_GUI_H

#include "../apps/commands64.h"

// GUI'de bir komut işler (şu an serial'a log yapar)
// Gelecekte GUI penceresinde sonuç gösterebilir
void process_command(const char* cmd);

#endif