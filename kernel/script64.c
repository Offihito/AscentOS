// script64.c - User-defined script/command system implementation
#include "script64.h"
#include "commands64.h"
#include "files64.h"
#include <stddef.h>

// External functions
extern void println64(const char* str, uint8_t color);
extern void print_str64(const char* str, uint8_t color);
extern int execute_command64(const char* input, CommandOutput* output);

// Script storage
static UserScript scripts[MAX_SCRIPTS];
static int script_count = 0;

// String helpers
static int str_starts_with(const char* str, const char* prefix) {
    while (*prefix) {
        if (*str++ != *prefix++) return 0;
    }
    return 1;
}

static void str_trim(char* str) {
    // Remove leading spaces
    while (*str == ' ') str++;
    
    // Remove trailing spaces
    int len = str_len(str);
    while (len > 0 && str[len - 1] == ' ') {
        str[--len] = '\0';
    }
}

// Initialize script system
void init_scripts64(void) {
    for (int i = 0; i < MAX_SCRIPTS; i++) {
        scripts[i].active = 0;
        scripts[i].name[0] = '\0';
        scripts[i].description[0] = '\0';
        scripts[i].line_count = 0;
    }
    script_count = 0;
    
    // Create some example scripts
    script_create("welcome", "Welcome message", SCRIPT_TYPE_SHELL);
    script_add_line("welcome", "echo ========================================");
    script_add_line("welcome", "echo     Welcome to AscentOS 64-bit!");
    script_add_line("welcome", "echo ========================================");
    script_add_line("welcome", "echo Type 'help' for available commands");
    script_add_line("welcome", "echo Type 'script list' to see custom scripts");
    
    script_create("syscheck", "Quick system check", SCRIPT_TYPE_SHELL);
    script_add_line("syscheck", "echo === System Check ===");
    script_add_line("syscheck", "neofetch");
    script_add_line("syscheck", "echo");
    script_add_line("syscheck", "echo === File System ===");
    script_add_line("syscheck", "ls");
    
    script_create("greet", "Personalized greeting", SCRIPT_TYPE_SHELL);
    script_add_line("greet", "echo Hello from AscentOS!");
    script_add_line("greet", "echo Current directory:");
    script_add_line("greet", "pwd");
}

// Create new script
int script_create(const char* name, const char* description, ScriptType type) {
    if (script_count >= MAX_SCRIPTS) {
        return 0;
    }
    
    // Check if script already exists
    for (int i = 0; i < MAX_SCRIPTS; i++) {
        if (scripts[i].active && str_cmp(scripts[i].name, name) == 0) {
            return 0;  // Already exists
        }
    }
    
    // Find empty slot
    for (int i = 0; i < MAX_SCRIPTS; i++) {
        if (!scripts[i].active) {
            scripts[i].active = 1;
            str_cpy(scripts[i].name, name);
            str_cpy(scripts[i].description, description);
            scripts[i].type = type;
            scripts[i].line_count = 0;
            script_count++;
            return 1;
        }
    }
    
    return 0;
}

// Delete script
int script_delete(const char* name) {
    for (int i = 0; i < MAX_SCRIPTS; i++) {
        if (scripts[i].active && str_cmp(scripts[i].name, name) == 0) {
            scripts[i].active = 0;
            scripts[i].name[0] = '\0';
            scripts[i].line_count = 0;
            script_count--;
            return 1;
        }
    }
    return 0;
}

// Add line to script
int script_add_line(const char* script_name, const char* line) {
    UserScript* script = script_get(script_name);
    if (!script) return 0;
    
    if (script->line_count >= MAX_SCRIPT_LINES) {
        return 0;
    }
    
    str_cpy(script->lines[script->line_count], line);
    script->line_count++;
    return 1;
}

// Get script by name
UserScript* script_get(const char* name) {
    for (int i = 0; i < MAX_SCRIPTS; i++) {
        if (scripts[i].active && str_cmp(scripts[i].name, name) == 0) {
            return &scripts[i];
        }
    }
    return NULL;
}

// Parse and execute script line
int interpret_script_line(const char* line, ScriptContext* ctx, void* output) {
    (void)ctx;  // For future variable support
    
    CommandOutput* cmd_output = (CommandOutput*)output;
    
    // Skip empty lines
    if (str_len(line) == 0) {
        return 1;
    }
    
    // Skip comments
    if (line[0] == '#') {
        return 1;
    }
    
    // Trim leading spaces
    const char* trimmed = line;
    while (*trimmed == ' ' || *trimmed == '\t') {
        trimmed++;
    }
    
    // Skip if empty after trim
    if (*trimmed == '\0') {
        return 1;
    }
    
    // Execute as normal command
    return execute_command64(trimmed, cmd_output);
}

// Execute script
int script_execute(const char* script_name, const char* args, void* output) {
    (void)args;  // For future argument support
    
    UserScript* script = script_get(script_name);
    if (!script) {
        return 0;
    }
    
    CommandOutput* cmd_output = (CommandOutput*)output;
    output_init(cmd_output);
    
    ScriptContext ctx = {0};
    
    // Execute each line
    for (int i = 0; i < script->line_count; i++) {
        // Create a temporary output for this line
        CommandOutput line_output;
        output_init(&line_output);
        
        interpret_script_line(script->lines[i], &ctx, &line_output);
        
        // Copy output to main output
        for (int j = 0; j < line_output.line_count; j++) {
            output_add_line(cmd_output, line_output.lines[j], line_output.colors[j]);
        }
    }
    
    return 1;
}

// List all scripts
int script_list(void* output) {
    CommandOutput* cmd_output = (CommandOutput*)output;
    
    if (script_count == 0) {
        output_add_line(cmd_output, "No custom scripts defined.", VGA_YELLOW);
        output_add_line(cmd_output, "Use 'script new <name>' to create one!", VGA_CYAN);
        return 1;
    }
    
    output_add_line(cmd_output, "Custom Scripts:", VGA_CYAN);
    output_add_line(cmd_output, "", VGA_WHITE);
    
    char line[MAX_LINE_LENGTH];
    for (int i = 0; i < MAX_SCRIPTS; i++) {
        if (scripts[i].active) {
            str_cpy(line, "  ");
            str_concat(line, scripts[i].name);
            
            // Pad to 15 chars
            int name_len = str_len(scripts[i].name);
            for (int j = name_len; j < 15; j++) {
                str_concat(line, " ");
            }
            
            str_concat(line, " - ");
            str_concat(line, scripts[i].description);
            
            output_add_line(cmd_output, line, VGA_WHITE);
        }
    }
    
    output_add_empty_line(cmd_output);
    output_add_line(cmd_output, "Run with: script run <name>", VGA_GREEN);
    output_add_line(cmd_output, "Or just: <name> (if name doesn't conflict)", VGA_GREEN);
    
    return 1;
}

// Save script to file
int script_save_to_file(const char* script_name) {
    UserScript* script = script_get(script_name);
    if (!script) return 0;
    
    // Create filename
    char filename[64];
    str_cpy(filename, script_name);
    str_concat(filename, ".sh");
    
    // Build content
    static char content[MAX_SCRIPT_SIZE];
    str_cpy(content, "#!/bin/ascentsh\n");
    str_concat(content, "# Script: ");
    str_concat(content, script->name);
    str_concat(content, "\n# ");
    str_concat(content, script->description);
    str_concat(content, "\n\n");
    
    for (int i = 0; i < script->line_count; i++) {
        str_concat(content, script->lines[i]);
        str_concat(content, "\n");
    }
    
    // Check if file exists, create if not
    const EmbeddedFile64* file = fs_get_file64(filename);
    if (!file) {
        fs_touch_file64(filename);
    }
    
    return fs_write_file64(filename, content);
}

// Load script from file
int script_load_from_file(const char* filename) {
    const EmbeddedFile64* file = fs_get_file64(filename);
    if (!file) return 0;
    
    // Parse script name from filename (remove .sh)
    char script_name[MAX_SCRIPT_NAME];
    int i = 0;
    while (filename[i] && filename[i] != '.' && i < MAX_SCRIPT_NAME - 1) {
        script_name[i] = filename[i];
        i++;
    }
    script_name[i] = '\0';
    
    // Delete existing script with same name
    script_delete(script_name);
    
    // Create script
    if (!script_create(script_name, "Loaded from file", SCRIPT_TYPE_SHELL)) {
        return 0;
    }
    
    // Parse lines
    const char* content = file->content;
    char line[128];
    
    while (*content) {
        // Read line
        int col = 0;
        while (*content && *content != '\n' && col < 127) {
            line[col++] = *content++;
        }
        line[col] = '\0';
        
        if (*content == '\n') content++;
        
        // Skip shebang
        if (line[0] == '#' && line[1] == '!') {
            continue;
        }
        
        // Skip comment lines
        if (line[0] == '#') {
            continue;
        }
        
        // Trim leading/trailing spaces
        char* start = line;
        while (*start == ' ' || *start == '\t') start++;
        
        if (*start == '\0') {
            continue; // Empty line
        }
        
        // Add line to script
        script_add_line(script_name, start);
    }
    
    return 1;
}

// Edit script (opens in nano)
int script_edit(const char* script_name) {
    UserScript* script = script_get(script_name);
    if (!script) return 0;
    
    // Save to temporary file
    char filename[64];
    str_cpy(filename, script_name);
    str_concat(filename, ".sh");
    
    script_save_to_file(script_name);
    
    return 1;  // Return success, nano will be opened by command handler
}

// Show script content
int script_show(const char* script_name, void* output) {
    UserScript* script = script_get(script_name);
    if (!script) return 0;
    
    CommandOutput* cmd_output = (CommandOutput*)output;
    
    char header[MAX_LINE_LENGTH];
    str_cpy(header, "Script: ");
    str_concat(header, script->name);
    output_add_line(cmd_output, header, VGA_CYAN);
    
    str_cpy(header, "Description: ");
    str_concat(header, script->description);
    output_add_line(cmd_output, header, VGA_YELLOW);
    
    output_add_line(cmd_output, "========================================", VGA_DARK_GRAY);
    
    char line_with_num[MAX_LINE_LENGTH];
    for (int i = 0; i < script->line_count; i++) {
        char num[8];
        int_to_str(i + 1, num);
        str_cpy(line_with_num, num);
        str_concat(line_with_num, ": ");
        str_concat(line_with_num, script->lines[i]);
        output_add_line(cmd_output, line_with_num, VGA_WHITE);
    }
    
    output_add_line(cmd_output, "========================================", VGA_DARK_GRAY);
    output_add_line(cmd_output, "", VGA_WHITE);
    
    return 1;
}