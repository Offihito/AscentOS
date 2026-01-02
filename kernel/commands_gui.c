#include "commands_gui.h"
#include "commands64.h"
#include <stdint.h>

// CommandOutput'u Terminal'e render eden yardımcı fonksiyon
static void render_command_output(Terminal* term, const CommandOutput* output)
{
    for (int i = 0; i < output->line_count; i++) {
        const char* line = output->lines[i];
        uint8_t color = output->colors[i];

        // Özel clear komutu kontrolü
        if (line[0] == '_' && line[1] == '_' && 
            line[2] == 'C' && line[3] == 'L' && 
            line[4] == 'E' && line[5] == 'A' && 
            line[6] == 'R' && line[7] == '_') {
            terminal_clear(term);
            continue;
        }

        // Renk mapping: VGA renk kodlarını GUI terminal renklerine çevir
        // (terminal64.h'deki renk sistemi neyse ona göre uyarla)
        // Basitlik için şimdilik sabit renk kullanalım, sonra genişletebilirsin
        uint32_t gui_color = 0xFFFFFFFF; // Beyaz varsayılan

        switch (color) {
            case VGA_WHITE:     gui_color = 0xFFFFFFFF; break;
            case VGA_GREEN:     gui_color = 0xFF00FF00; break;
            case VGA_RED:       gui_color = 0xFFFF0000; break;
            case VGA_YELLOW:    gui_color = 0xFFFFFF00; break;
            case VGA_CYAN:      gui_color = 0xFF00FFFF; break;
            case VGA_MAGENTA:   gui_color = 0xFFFF00FF; break;
            case VGA_DARK_GRAY: gui_color = 0xFF888888; break;
            default:            gui_color = 0xFFFFFFFF; break;
        }

        terminal_println_colored(term, line, gui_color);
    }
}

void process_command(Terminal* term, const char* cmd)
{
    // Boş satır
    if (!cmd || cmd[0] == '\0') {
        terminal_println(term, "");
        return;
    }

    // Leading space'leri atla
    while (*cmd == ' ') cmd++;

    CommandOutput output;
    int success = execute_command64(cmd, &output);

    if (success) {
        render_command_output(term, &output);
    } else {
        // execute_command64 zaten hata mesajını output'a ekler
        render_command_output(term, &output);
    }

    // Her komuttan sonra boş satır (güzel görünüm için)
    terminal_println(term, "");
}