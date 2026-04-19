#include <stdio.h>
#include <poll.h>
#include <unistd.h>
#include <string.h>

int main() {
    struct pollfd fds[1];
    
    // stdin (keyboard)
    fds[0].fd = 0; 
    fds[0].events = POLLIN;
    
    printf("\n[POLL_TEST] Polling stdin for 5 seconds...\n");
    printf("[POLL_TEST] Press any key to test wake-up, or wait for timeout.\n");
    
    int ret = poll(fds, 1, 5000);
    
    if (ret < 0) {
        printf("[POLL_TEST] Error in poll()\n");
        return 1;
    } else if (ret == 0) {
        printf("[POLL_TEST] Timeout! No key was pressed within 5 seconds.\n");
    } else {
        if (fds[0].revents & POLLIN) {
            printf("[POLL_TEST] Event POLLIN detected on stdin!\n");
            char buf[32];
            int n = read(0, buf, sizeof(buf) - 1);
            if (n > 0) {
                buf[n] = '\0';
                // Clean up string (remove newline/carriage return for printing)
                for(int i=0; i<n; i++) if(buf[i] == '\r' || buf[i] == '\n') buf[i] = ' ';
                printf("[POLL_TEST] Successfully read input: '%s'\n", buf);
            }
        } else {
            printf("[POLL_TEST] Poll returned %d but POLLIN not set (revents: %d)\n", ret, fds[0].revents);
        }
    }
    
    printf("[POLL_TEST] Test completed.\n");
    return 0;
}
