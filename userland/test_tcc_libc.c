/*
 * test_tcc_libc.c - Test program for TCC on AscentOS using musl libc
 * 
 * This uses standard libc (musl) - the recommended way to use TCC.
 * 
 * Compile with TCC:
 *   tcc -o test_tcc_libc test_tcc_libc.c
 * 
 * Run on AscentOS:
 *   ./test_tcc_libc
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    printf("Hello from TCC with musl libc on AscentOS!\n");
    
    // Test malloc/free
    char *buf = malloc(100);
    if (buf) {
        strcpy(buf, "Dynamic memory works!\n");
        printf("%s", buf);
        free(buf);
    }
    
    // Test string functions
    const char *s = "AscentOS";
    printf("String length of '%s' = %zu\n", s, strlen(s));
    
    return 0;
}
