#include <unistd.h>
#include <stdlib.h>
#include <termios.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    struct termios orig_termios, raw;
    unsigned char c;
    
    // Get current terminal settings
    if (tcgetattr(STDIN_FILENO, &orig_termios) == -1) {
        perror("tcgetattr");
        return 1;
    }
    
    raw = orig_termios;
    // Disable canonical mode and echoing
    raw.c_lflag &= ~(14); // ICANON | ECHO
    raw.c_cc[6] = 0;    // VMIN = 0 (non-blocking)
    raw.c_cc[5] = 1;    // VTIME = 1 (100ms timeout)
    
    if (tcsetattr(STDIN_FILENO, 0, &raw) == -1) {
        perror("tcsetattr");
        return 1;
    }
    
    printf("Press keys (ESC to quit, numbers will be printed)...\r\n");
    printf("If arrow keys work, you'll see ESC [ A/B/C/D\r\n");
    fflush(stdout);
    
    while (1) {
        int n = read(STDIN_FILENO, &c, 1);
        if (n > 0) {
            if (c == 27) { // ESC
                printf("ESC pressed, exiting\r\n");
                break;
            }
            printf("Got byte: 0x%02x (%d) ", c, c);
            if (c >= 32 && c < 127) {
                printf("'%c'", c);
            }
            printf("\r\n");
            fflush(stdout);
        }
    }
    
    // Restore original terminal settings
    tcsetattr(STDIN_FILENO, 0, &orig_termios);
    return 0;
}
