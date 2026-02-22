// mini_shell.c
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define MAX_ARGS 8
#define BUF_SIZE 128

typedef void (*cmd_func_t)(int argc, char** argv);

typedef struct {
    const char* name;
    cmd_func_t  func;
} command_t;

// Komut fonksiyonları
static void cmd_help(int argc, char** argv) {
    puts("Komutlar: help, echo, clear, ver");
}
static void cmd_echo(int argc, char** argv) {
    for (int i = 1; i < argc; i++) printf("%s ", argv[i]);
    puts("");
}
static void cmd_clear(int argc, char** argv) { puts("\033[2J\033[H"); }
static void cmd_ver(int argc, char** argv)   { puts("AscentOS MiniShell v0.1"); }

static const command_t commands[] = {
    {"help",  cmd_help},
    {"echo",  cmd_echo},
    {"clear", cmd_clear},
    {"ver",   cmd_ver},
    {NULL, NULL}
};

static int tokenize(char* buf, char** argv, int max) {
    int argc = 0;
    char* p = buf;
    while (*p && argc < max-1) {
        while (*p == ' ') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = '\0';
    }
    argv[argc] = NULL;
    return argc;
}

int main(void) {
    char buf[BUF_SIZE];
    char* argv[MAX_ARGS];

    puts("\nMini Shell – AscentOS");
    puts("  help → komutları göster\n");

    while (1) {
        printf("> ");
        fflush(stdout);

        if (readlink(buf, BUF_SIZE) <= 0) continue;   // senin readline fonksiyonun

        int argc = tokenize(buf, argv, MAX_ARGS);
        if (argc == 0) continue;

        if (strcmp(argv[0], "exit") == 0) {
            puts("Cikiliyor...");
            break;
        }

        const command_t* cmd = commands;
        while (cmd->name) {
            if (strcmp(argv[0], cmd->name) == 0) {
                cmd->func(argc, argv);
                goto next;
            }
            cmd++;
        }
        printf("Komut bulunamadi: %s\n", argv[0]);

    next:;
    }
    return 0;
}