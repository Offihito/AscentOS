// script64.h - User-defined script/command system
#ifndef SCRIPT64_H
#define SCRIPT64_H

#include <stdint.h>

#define MAX_SCRIPTS 32
#define MAX_SCRIPT_NAME 32
#define MAX_SCRIPT_SIZE 4096
#define MAX_SCRIPT_LINES 128

// Script types
typedef enum {
    SCRIPT_TYPE_SHELL,    // Simple shell-like scripts
    SCRIPT_TYPE_COMMAND   // Single command scripts
} ScriptType;

// Script structure
typedef struct {
    char name[MAX_SCRIPT_NAME];
    char description[64];
    ScriptType type;
    int active;
    int line_count;
    char lines[MAX_SCRIPT_LINES][128];
} UserScript;

// Script commands
typedef struct {
    char command[64];
    char args[128];
} ScriptCommand;

// Script execution context
typedef struct {
    char variables[16][64];  // Simple variable storage
    int var_count;
    int return_code;
} ScriptContext;

// Functions
void init_scripts64(void);
int script_create(const char* name, const char* description, ScriptType type);
int script_delete(const char* name);
int script_add_line(const char* script_name, const char* line);
int script_execute(const char* script_name, const char* args, void* output);
int script_list(void* output);
int script_show(const char* script_name, void* output);
UserScript* script_get(const char* name);
int script_save_to_file(const char* script_name);
int script_load_from_file(const char* filename);
int script_edit(const char* script_name);

// Script interpreter
int interpret_script_line(const char* line, ScriptContext* ctx, void* output);

#endif // SCRIPT64_H