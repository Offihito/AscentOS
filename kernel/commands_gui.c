// commands_gui.c - GUI command processing without terminal window
// This is a placeholder for future GUI command interface

#include "commands_gui.h"
#include "../apps/commands64.h"
#include <stdint.h>

// External serial output for debugging
extern void serial_print(const char* str);

// Process command in GUI mode (currently just logs to serial)
// In the future, this could display results in a GUI window
void process_command(const char* cmd)
{
    // Boş satır
    if (!cmd || cmd[0] == '\0') {
        return;
    }

    // Leading space'leri atla
    while (*cmd == ' ') cmd++;

    // Log command to serial for debugging
    serial_print("[GUI CMD] Executing: ");
    serial_print(cmd);
    serial_print("\n");

    // Execute command using the command64 infrastructure
    CommandOutput output;
    int success = execute_command64(cmd, &output);

    // Log results to serial
    if (success) {
        serial_print("[GUI CMD] Success, output:\n");
        for (int i = 0; i < output.line_count; i++) {
            serial_print("  ");
            serial_print(output.lines[i]);
            serial_print("\n");
        }
    } else {
        serial_print("[GUI CMD] Command failed or not recognized\n");
        for (int i = 0; i < output.line_count; i++) {
            serial_print("  ");
            serial_print(output.lines[i]);
            serial_print("\n");
        }
    }
}